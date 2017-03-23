#include <linux/module.h>
#include "kcheck.h"
#include <net/netlink.h>
#include <net/genetlink.h>

#include "nl.h"
#include "unlproto.h"
#include "ktf.h"

/* The global map from name to ktf_case */
DEFINE_KTF_MAP(test_cases);

/* a lock to protect this datastructure */
DEFINE_MUTEX(tc_lock);

#define MAX_PRINTF 4096


/* Current total number of test cases defined */
size_t ktf_case_count()
{
	return ktf_map_size(&test_cases);
}

struct ktf_case *ktf_case_create(const char *name)
{
	struct ktf_case *tc = kmalloc(sizeof(*tc), GFP_KERNEL);
	int ret;

	if (!tc)
		return tc;

	INIT_LIST_HEAD(&tc->fun_list);
	ret = ktf_map_elem_init(&tc->kmap, name);
	if (ret) {
		kfree(tc);
		return NULL;
	}
	DM(T_DEBUG, printk(KERN_INFO "ktf: Added test set %s\n", name));
	return tc;
}

struct ktf_case *ktf_case_find(const char *name)
{
	return ktf_map_find_entry(&test_cases, name, struct ktf_case, kmap);
}

struct ktf_case *ktf_case_find_create(const char *name)
{
	struct ktf_case *tc;
	int ret = 0;

	tc = ktf_case_find(name);
	if (!tc) {
		tc = ktf_case_create(name);
		ret = ktf_map_insert(&test_cases, &tc->kmap);
		if (ret) {
			kfree(tc);
			tc = NULL;
		}
	}
	return tc;
}

/* TBD: access to 'test_cases' should be properly synchronized
 * to avoid crashes if someone tries to unload while a test in progress
 */

u32 assert_cnt = 0;

void flush_assert_cnt(struct sk_buff* skb)
{
	if (assert_cnt) {
		tlog(T_DEBUG, "update: %d asserts", assert_cnt);
		nla_put_u32(skb, KTF_A_STAT, assert_cnt);
		assert_cnt = 0;
	}
}


long _fail_unless (struct sk_buff* skb, int result, const char *file,
			int line, const char *fmt, ...)
{
	int len;
	va_list ap;
	char* buf = "";
	if (result)
		assert_cnt++;
	else {
		flush_assert_cnt(skb);
		nla_put_u32(skb, KTF_A_STAT, result);
		buf = (char*)kmalloc(MAX_PRINTF, GFP_KERNEL);
		nla_put_string(skb, KTF_A_FILE, file);
		nla_put_u32(skb, KTF_A_NUM, line);

		va_start(ap,fmt);
		len = vsnprintf(buf,MAX_PRINTF-1,fmt,ap);
		buf[len] = 0;
		va_end(ap);
		nla_put_string(skb, KTF_A_STR, buf);
		tlog(T_ERROR, "file %s line %d: result %d (%s)",
			file, line, result, buf);
		kfree(buf);
	}
	return result;
}
EXPORT_SYMBOL(_fail_unless);


/* Add a test to a testcase:
 * Tests are represented by fun_hook objects that are linked into
 * two lists: fun_hook::flist in TCase::fun_list and
 *            fun_hook::hlist in ktf_handle::test_list
 *
 * TCase::fun_list is used for iterating through the tests.
 * ktf_handle::test_list is needed for cleanup
 */
void  _tcase_add_test (struct __test_desc td,
				struct ktf_handle *th,
				int _signal,
				int allowed_exit_value,
				int start, int end)
{
	struct ktf_case *tc;
	struct fun_hook *fc = (struct fun_hook *)
		kmalloc(sizeof(struct fun_hook), GFP_KERNEL);
	if (!fc)
		return;

	mutex_lock(&tc_lock);
	tc = ktf_case_find_create(td.tclass);
	if (!tc) {
		printk(KERN_INFO "ERROR: Failed to add test %s from %s to test case \"%s\"",
			td.name, td.file, td.tclass);
		kfree(fc);
		goto out;
	}
	fc->name = td.name;
	fc->tclass = td.tclass;
	fc->fun = td.fun;
	fc->start = start;
	fc->end = end;
	fc->handle = th;

	DM(T_LIST, printk(KERN_INFO "ktf: Added test \"%s.%s\""
		" start = %d, end = %d\n",
		td.tclass, td.name, start, end));
	list_add(&fc->flist, &tc->fun_list);
	list_add(&fc->hlist, &th->test_list);
out:
	mutex_unlock(&tc_lock);
}
EXPORT_SYMBOL(_tcase_add_test);


/* Clean up all tests associated with a ktf_handle */

void _tcase_cleanup(struct ktf_handle *th)
{
	struct fun_hook *fh;
	struct list_head *pos, *n;

	mutex_lock(&tc_lock);
	list_for_each_safe(pos, n, &th->test_list) {
		fh = list_entry(pos, struct fun_hook, hlist);
		DM(T_LIST, printk(KERN_INFO "ktf: delete test %s.%s\n", fh->tclass, fh->name));
		list_del(&fh->flist);
		list_del(&fh->hlist);
		kfree(fh);
	}
	mutex_unlock(&tc_lock);
}
EXPORT_SYMBOL(_tcase_cleanup);




int ktf_cleanup(void)
{
	struct fun_hook *fh;
	struct ktf_case *tc;

	mutex_lock(&tc_lock);
	ktf_map_for_each_entry(tc, &test_cases, kmap) {
		struct list_head *pos;
		list_for_each(pos, &tc->fun_list) {
			fh = list_entry(pos, struct fun_hook, flist);
			printk(KERN_WARNING
				"ktf: (memory leak) test set %s still active with test %s at unload!\n",
				tc_name(tc), fh->name);
			return -EBUSY;
		}
	}
	ktf_map_delete_all(&test_cases, struct ktf_case, kmap);
	mutex_unlock(&tc_lock);
	return 0;
}
