/*
 * java_backtrace.cpp
 * Copyright (C) 2013 Jeremy C. Andrus <jeremya@cs.columbia.edu>
 *
 */
#pragma GCC diagnostic ignored "-fpermissive"

#include <string>

#include <dlfcn.h>
#include <stdio.h>
#include <sys/types.h>

#include "wrap_lib.h"
#include "java_backtrace.h"

/*
 * TODO: use libgccdemangle.so to auto-mangle these names?
 */

/*
 * gcc mangled names of:
 * 	void dvmCallMethod(Thread* self, const Method* method,
 * 			   Object* obj, JValue* pResult, ...);
 * and:
 *	void dvmCallMethodA(Thread* self, const Method* method,
 *			    Object* obj,bool fromJni, JValue* pResult,
 *			    const jvalue* args);
 */
#define DVM_CALL_METHOD   "_Z13dvmCallMethodP6ThreadPK6MethodP6ObjectP6JValuez"
#define DVM_CALL_METHOD_A "_Z14dvmCallMethodAP6ThreadPK6MethodP6ObjectbP6JValuePK6jvalue"
#define DVM_CALL_METHOD_V "_Z14dvmCallMethodVP6ThreadPK6MethodP6ObjectbP6JValueSt9__va_list"

/*
 *
 * gcc mangled names of:
 * 	Thread *dvmThreadSelf();
 * 	int dvmComputeExactFrameDepth(const void* fp);
 * 	void dvmFillStackTraceArray(const void* fp,
 * 				    const struct Method** array,
 * 				    size_t length);
 * 	std::string dvmGetThreadName(struct Thread* thread);
 * 	std::string dvmHumanReadableMethod(const struct Method* method,
 * 					   bool withSignature);
 */
#define sym_DVM_ThreadSelf		_Z13dvmThreadSelfv
#define sym_DVM_ComputeExactFrameDepth	_Z25dvmComputeExactFrameDepthPKv
#define sym_DVM_FillStackTraceArray	_Z22dvmFillStackTraceArrayPKvPPK6Methodj
#define sym_DVM_HumanReadableMethod	_Z22dvmHumanReadableMethodPK6Methodb
#define sym_DVM_GetThreadName		_Z16dvmGetThreadNameP6Thread

extern struct libc_iface libc;

struct dvm_iface dvm __attribute__((visibility("hidden")));

static pthread_key_t s_dvm_thread_name_key = (pthread_key_t)(-1);
static pthread_key_t s_dvm_stack_key = (pthread_key_t)(-1);


static void *_find_symbol_end(void *sym_start)
{
	Dl_info dli;
	void *sym_end = sym_start;

	/*
	 * This is a bit of a hack: try to find the end of a symbol by
	 * repeatedly calling dladdr() until the symbol name changes...
	 */
	while (1) {
		void *sym = (void *)((char *)sym_end + 4);
		if (dladdr(sym, &dli) == 0)
			break;
		if (dli.dli_saddr != sym_start)
			break;
		sym_end = sym;
	}

	return sym_end;
}

extern "C"
void init_dvm_iface(struct dvm_iface *dvm, const char *dso_path)
{
	int ii;

	if (dvm->dso)
		return;

	dvm->dso = dlopen(dso_path, RTLD_NOW | RTLD_LOCAL);
	if (!dvm->dso) {
		libc_log("E:Could not open libdvm (%s)!", dso_path);
		return;
	}

	/* initialize the minimal Dalvik debug interface */
#define __init_sym(iface,sym,mem) \
	do { \
		if ((iface)->mem) \
			break; \
		(iface)->mem = dlsym((iface)->dso, #sym); \
		if ((iface)->mem) \
			break; \
		(iface)->mem = dlsym((iface)->dso, "_" #sym); \
		if ((iface)->mem) \
			break; \
		BUG(0xD0); \
	} while (0)
#define _init_sym(iface,sym,mem) \
	__init_sym(iface, sym, mem)
#define init_sym(IFACE,SYM) \
	_init_sym(IFACE, sym_DVM_ ## SYM, dvm ## SYM)

	init_sym(dvm, ThreadSelf);
	init_sym(dvm, ComputeExactFrameDepth);
	init_sym(dvm, FillStackTraceArray);
	init_sym(dvm, GetThreadName);
	init_sym(dvm, HumanReadableMethod);

#undef init_sym
#undef _init_sym
#undef __init_sym

	/*
	 * Now find the functions that indicate
	 * we are within the Java call stack
	 */
	dvm->dvmCallMethodSym[0][0] = dlsym(dvm->dso, DVM_CALL_METHOD);
	dvm->dvmCallMethodSym[1][0] = dlsym(dvm->dso, DVM_CALL_METHOD_A);
	dvm->dvmCallMethodSym[2][0] = dlsym(dvm->dso, DVM_CALL_METHOD_V);

	/*
	 * note: if either of these fail, then we won't ever find a
	 * java/dalvik backtrace, but we won't risk instability either
	 */
	for (ii = 0; ii < (int)ARRAY_SZ(dvm->dvmCallMethodSym); ii++) {
		if (!dvm->dvmCallMethodSym[ii][0]) {
			libc_log("W:Couldn't find dvmCallMethod[%d] in libdvm.so", ii);
		} else {
			void *end;
			end = _find_symbol_end(dvm->dvmCallMethodSym[ii][0]);
			dvm->dvmCallMethodSym[ii][1] = end;
			/*
			libc_log("I:dvmCallMethod[%d][%p-%p]:", ii,
				 dvm->dvmCallMethodSym[ii][0],
				 dvm->dvmCallMethodSym[ii][1]);
			 */
		}
	}

	if (s_dvm_thread_name_key == (pthread_key_t)(-1)) {
		libc.pthread_key_create(&s_dvm_thread_name_key, NULL);
		libc.pthread_setspecific(s_dvm_thread_name_key, NULL);
	}

	dvm->valid = 1;
	return;
}

extern "C"
void close_dvm_iface(struct dvm_iface *dvm)
{
	void *val;
	void *dso;

	dso = dvm->dso;
	if (!dvm->dso)
		goto out;
	dvm->valid = 0;
	dvm->dso = NULL;
	dlclose(dso);

	val = (char *)libc.pthread_getspecific(s_dvm_thread_name_key);
	if (val) {
		libc.free(val);
		libc.pthread_setspecific(s_dvm_thread_name_key, NULL);
	}

	val = libc.pthread_getspecific(s_dvm_stack_key);
	if (val) {
		libc.free(val);
		libc.pthread_setspecific(s_dvm_stack_key, NULL);
	}

out:
	libc.memset(dvm, 0, sizeof(*dvm));
}

static inline int _in_range(void *v, void *range[])
{
	if (v >= range[0] && v <= range[1])
		return 1;
	return 0;
}

extern "C"
void get_dvm_backtrace(struct dvm_iface *dvm,
			      struct bt_state *bt_state,
			      struct dvm_bt *dvm_bt)
{
	int cnt;

	if (!dvm->valid)
		return;

	dvm_bt->count = 0;

	/*
	 * Search through the native backtrace for the java stack indicators.
	 * It should be faster to search up from the bottom of the stack
	 * because the dvmCallMethod function is generally invoked from
	 * platform-specific assembly quite early in the backtrace.
	 */
	cnt = bt_state->count;
	while (--cnt >= 0) {
		int ii;
		void *addr = bt_state->frame[cnt].pc;
		for (ii = 0; ii < (int)ARRAY_SZ(dvm->dvmCallMethodSym); ii++)
			if (_in_range(addr, dvm->dvmCallMethodSym[ii]))
				goto do_dvm_bt;
	}
	return;

	/*
	 * We have a call stack that's from a Java/Dalvik thread:
	 * get the backtrace!
	 */
do_dvm_bt:
	{
		struct Thread *self;
		uint32_t *fp = NULL;
		char *tname;
		int r;

		self = dvm->dvmThreadSelf();
		if (!self)
			return;
		fp = self->interpSave.curFrame;

		tname = (char *)libc.pthread_getspecific(s_dvm_thread_name_key);
		if (!tname) {
			std::string nm = dvm->dvmGetThreadName(self);
			tname = (char *)libc.malloc(nm.length() + 1);
			libc.memset(tname, 0, nm.length() + 1);
			libc.memcpy(tname, nm.c_str(), nm.length());
			libc.pthread_setspecific(s_dvm_thread_name_key, tname);
			libc_log("I:DalvikThreadName:%s:", tname);
		}

		dvm_bt->count = dvm->dvmComputeExactFrameDepth(fp);
		if (dvm_bt->count > MAX_BT_FRAMES)
			dvm_bt->count = MAX_BT_FRAMES;

		dvm->dvmFillStackTraceArray(fp, dvm_bt->mlist, dvm_bt->count);
	}

	return;
}

static int compare_traces(struct dvm_iface *dvm, struct dvm_bt *dvm_bt)
{
	char *last_stack = NULL;
	int ii, ret;
	int *last_depth, *last_repeat;
	struct Method **last_meth;

	if (s_dvm_stack_key == (pthread_key_t)(-1)) {
		if (libc.pthread_key_create(&s_dvm_stack_key, NULL) != 0)
			return 0;
		libc.pthread_setspecific(s_dvm_stack_key, NULL);
	}

	last_stack = libc.pthread_getspecific(s_dvm_stack_key);
	if (!last_stack) {
		int sz = ((MAX_BT_FRAMES) * sizeof(struct Method *))
				+ (2 * sizeof(int));
		last_stack = (char *)libc.malloc(sz);
		if (!last_stack)
			return 0;
		libc.memset(last_stack, 0, sz);
		libc.pthread_setspecific(s_dvm_stack_key, (void *)last_stack);
	}
	last_depth = (int *)last_stack;
	last_repeat = (int *)(last_stack + sizeof(int));
	last_meth = (struct Method **)(last_stack + 2*sizeof(int));

	if (*last_depth != dvm_bt->count) {
		/* this is _not_ a repeated trace, save it for next time */
		ret = *last_repeat;
		goto save_current_stack;
	}

	for (ii = 0; ii < *last_depth; ii++) {
		if (last_meth[ii] != dvm_bt->mlist[ii]) {
			/*
			 * It's not a repeated trace, but we had a previously
			 * repeating trace. Save the current stack and return
			 * the number of times the previous stack repeated.
			 */
			ret = *last_repeat;
			goto save_current_stack;
		}
	}

	*last_repeat += 1;
	return -1; /* repeat frame, don't print anything yet */

save_current_stack:
	*last_depth = dvm_bt->count;
	for (ii = 0; ii < *last_depth; ii++)
		last_meth[ii] = dvm_bt->mlist[ii];
	*last_repeat = 1;
	return ret;
}

static void print_dvm_sym(struct log_info *info, struct dvm_iface *dvm,
			  int count, struct Method *m)
{
	struct bt_line_cache *cache = NULL;
	struct bt_line *cline;

	if (!dvm->valid)
		return;

	cline = bt_cache_fetch((void *)m, &cache);
	if (!cline)
		goto do_lookup;

	if (cline->sym == (void *)m) {
		__bt_printf(info, " :%d:%s", count, cline->str);
		return;
	}

	cline->sym = (void *)m;

do_lookup:
	std::string name;
	name = dvm->dvmHumanReadableMethod(m, DVM_BT_GET_SIGNATURE);
	if (cline) {
		libc.snprintf(cline->str, MAX_LINE_LEN, "%s:\n", name.c_str());
		__bt_printf(info, " :%d:%s", count, cline->str);
		return;
	}

	__bt_printf(info, " :%d:%s:\n", count, name.c_str());
}

extern "C"
void print_dvm_bt(struct dvm_iface *dvm, struct dvm_bt *dvm_bt,
		  struct log_info *info)
{
	int ii;

	ii = compare_traces(dvm, dvm_bt);
	if (ii < 0) {
		__bt_printf(info, "DVM:BT_REPEAT:1:\n");
		return;
	}
	/* else if (ii > 1)
		__bt_printf(info, "DVM:BT_REPEAT:%d:\n", ii);
	*/

	bt_printf(info, "DVM:BT_START:%d:\n", dvm_bt->count);
	for (ii = 0; ii < dvm_bt->count; ii++) {
		print_dvm_sym(info, dvm, ii, dvm_bt->mlist[ii]);
	}

	/* bt_printf(info, "DVM:BT_END:\n"); */
	return;
}