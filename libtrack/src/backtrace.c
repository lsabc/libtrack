/*
 * backtrace.c
 * Copyright (C) 2013 Jeremy C. Andrus <jeremya@cs.columbia.edu>
 *
 * Glue to wrap up a library (including possibly libc) and trace certain
 * functions within that library.
 */
#include <dlfcn.h>
#include <unwind.h>
#include <sys/types.h>

#include <asm/wrap_lib.h>
#include "wrap_lib.h"
#include "backtrace.h"
#include "java_backtrace.h"

//#define OUTPUT_CACHE_STATS

extern void wrap_symbol_mod(struct tls_info *tls);

/*
 * Unwound stack frame:
 *	<TRACE_FUNC (isn't counted)>
 *	unwind_backtrace
 *	log_backtrace
 *	wrapped_tracer
 * skip them all
 */
#define FRAMES_TO_SKIP 2

#define TLS_LOGBUF_SZ (LOG_BUFFER_SIZE \
		       + (4 * sizeof(int)) \
		       + (MAX_BT_FRAMES * sizeof(void *)))
_static char main_logbuffer[TLS_LOGBUF_SZ];

_static struct bt_line_cache main_btcache;


/*
 * TODO: add some locking, and make the cache global!
 */
_static struct bt_line_cache *get_bt_line_cache(void)
{
	struct tls_info *tls;
	struct bt_line_cache *cache;

	tls = get_tls();
	if (!tls)
		return NULL;

	if (tls->btcache)
		return (struct bt_line_cache *)(tls->btcache);

	if (is_main())
		cache = &main_btcache;
	else
		cache = (struct bt_line_cache *)libc.malloc(sizeof(*cache));
	if (!cache)
		return NULL;
	libc.memset(cache, 0, sizeof(*cache));
	tls->btcache = (void *)cache;
	return cache;
}

void __hidden tls_release_btcache(struct tls_info *tls)
{
	if (!tls)
		return;

	if (tls->btcache && tls->btcache != &main_btcache)
		libc.free(tls->btcache);
	tls->btcache = NULL;
}

struct bt_line __hidden
*bt_cache_fetch(void *sym, struct bt_line_cache **cache_out)
{
	struct bt_line_cache *cache;
	struct bt_line *l;
	uint16_t idx;

	cache = get_bt_line_cache();
	if (!cache)
		return NULL;

	idx = bt_hash(sym);
	if (cache_out)
		*cache_out = cache;

	l = cache->c[idx];
	if (l[0].sym == sym) {
		cache->hit++;
		l[0].usage++;
		return &l[0];
	}
	if (l[1].sym == sym) {
		cache->hit++;
		l[1].usage++;
		return &l[1];
	}

	cache->miss++;
	if (!l[0].sym) {
		cache->usage++;
		return &l[0];
	}
	if (!l[1].sym) {
		cache->usage++;
		return &l[1];
	}

	/* both levels of cache are full - evict the least recently used */
	if (l[0].usage < l[1].usage) {
		l[0].usage++;
		return &l[0];
	}
	l[1].usage++;
	return &l[1];
}

_static int bt_setup_logbuffer(struct tls_info *tls, struct log_info *info)
{
	char *buf;

	if (!tls)
		return -1;

	buf = (char *)tls->logbuffer;

	if (buf == (void *)1)
		return -1;

	if (!buf) {

		if (is_main()) {
			buf = main_logbuffer;
		} else {
			tls->logbuffer = (void *)1;
			buf = (char *)libc.malloc(TLS_LOGBUF_SZ);
		}

		if (!buf) {
			tls->logbuffer = NULL;
			return -1;
		}

		libc.memset(buf, 0, TLS_LOGBUF_SZ);
		tls->logbuffer = (void *)buf;

		bt_flush(tls, info);
		if (tls->logfile)
			log_flush(tls->logfile);
	}

	if (info->log_pos) {
		/* already setup: do a quick sanity check */
		if (info->log_pos == __bt_logpos(buf))
			return 0;
		/*
		 * hrm... not sure how this would happen, but we'll just reset
		 * the values knowing that we probably lost some log data.
		 */
		bt_flush(tls, info);
	}

	info->log_pos = __bt_logpos(buf);
	info->log_buffer = __bt_logbuf(buf);
	info->last_stack_depth = __bt_last_stack_depth(buf);
	info->last_stack_cnt = __bt_last_stack_cnt(buf);
	info->last_stack = __bt_last_stack(buf);

	return 0;
}

void __hidden tls_release_logbuffer(struct tls_info *tls)
{
	char *buf;
	int  *pos;

	if (!tls)
		return;

	buf = tls->logbuffer;
	if (!buf || buf == (void *)1)
		return;

	tls->logbuffer = NULL;

	pos = __bt_logpos(buf);
	if (tls->logfile && *pos > 0) {
		__bt_flush(tls->logfile, __bt_logbuf(buf), pos);
		log_flush(tls->logfile);
	}

	if (buf != main_logbuffer)
		libc.free(buf);
	else
		libc.memset(buf, 0, TLS_LOGBUF_SZ);

	tls->info.log_pos = NULL;
	tls->info.log_buffer = NULL;
	tls->info.last_stack_depth = NULL;
	tls->info.last_stack_cnt = NULL;
	tls->info.last_stack = NULL;
}

_static void print_info(struct tls_info *tls, int count, void *sym)
{
	unsigned long ofst;
	char c;
	struct bt_line_cache *cache = NULL;
	struct bt_line *cline;
	struct log_info *info;
	const char *symname;
	Dl_info dli;

	info = &tls->info;

	cline = bt_cache_fetch(sym, &cache);
	if (!cline)
		goto do_lookup;

#ifdef OUTPUT_CACHE_STATS
	if (cache &&
	    ((cache->hit + cache->miss) % 1024 == 0))
		__bt_printf(tls, " :CACHE_STATS:U[%d]:H[%d]:M[%d]",
			    cache->usage, cache->hit, cache->miss);
#endif

	if (cline->sym == sym){ /* cache hit! */
		/* because we dynamically change the name of the calling
		 * symbol based on input, we can't cache that part of the
		 * string. For the last entry in the backtrace we just cache
		 * the dladdr results
		 */
		if (count > 0)
			__bt_printf(tls, ":%d:%s", count, cline->str);
		else
			__bt_printf(tls, ":0:%x:%s:%s",
				    (unsigned int)sym, info->symbol, cline->str);
		return;
	}

	cline->sym = sym;

do_lookup:
	libc.memset(&dli, 0, sizeof(dli));
	dladdr(sym, &dli);

	if (!dli.dli_saddr) {
		c = '+';
		ofst = (unsigned long)sym -
			(unsigned long)dli.dli_fbase;
	} else if ((unsigned long)sym >
		   (unsigned long)dli.dli_saddr) {
		c = '+';
		ofst = (unsigned long)sym -
			(unsigned long)dli.dli_saddr;
	} else {
		c = '-';
		ofst = (unsigned long)dli.dli_saddr -
			(unsigned long)sym;
	}

	symname = dli.dli_sname ? dli.dli_sname : "??";
	if (count == 0)
		symname = info->symbol;

	if (cline) {
		if (count > 0) {
			libc.snprintf(cline->str, MAX_LINE_LEN-1,
				      "%x:%s:%c0x%x:%s(%p):",
				      (unsigned int)sym, symname,
				      c, ofst, dli.dli_fname, dli.dli_fbase);
			__bt_printf(tls, ":%d:%s", count, cline->str);
		} else {
			/* only cache the dladdr() lookup results
			 * for the last symbol in the BT stack - this allows
			 * us to properly dynamically rename based on input
			 */
			libc.snprintf(cline->str, MAX_LINE_LEN,
				      "%c0x%x:%s(%p):",
				      c, ofst, dli.dli_fname, dli.dli_fbase);
			__bt_printf(tls, ":0:%x:%s:%s",
				    (unsigned int)sym, symname, cline->str);
		}
		return;
	}

	__bt_printf(tls, ":%d:0x%x:%s:%c0x%x:%s(%p):",
		    count, (unsigned int)sym, symname,
		    c, ofst, dli.dli_fname, dli.dli_fbase);
}

_static void print_bt_state(struct tls_info *tls, struct bt_state *state)
{
	int count;
	struct bt_frame *frame;
	unsigned long ofst;
	char c;
	const char *sym;

	bt_printf(tls, "BT:START:%d:", state->count);
	for (count = 0; count < state->count; count++) {
		frame = &state->frame[count];
		print_info(tls, count, frame->pc);
#ifdef VERBOSE_FRAME_INFO
		__br_printf(tls, " : :R0=0x%08x,R1=0x%08x,R2=0x%08x,"
			     "R3=0x%08x,SP=0x%08x,LR=0x%08x:",
			     frame->reg[0], frame->reg[1], frame->reg[2],
			     frame->reg[3], frame->sp, frame->lr);
#endif
	}

#ifndef NO_DVM_BACKTRACE
	if (state->dvm_bt && state->dvm_bt->count > 0)
		print_dvm_bt(tls, &dvm, state->dvm_bt);
#endif
	/* bt_printf(tls, "BT:END:"); */
}

_static void __attribute__((noinline))
std_backtrace(struct tls_info *tls)
{
	struct bt_state state;
	void *frames[MAX_BT_FRAMES];
	int count;

	if (!tls)
		return;

	libc.memset(&state, 0, sizeof(state));
	state.f = tls->logfile;

	state.count = libc.backtrace(frames, MAX_BT_FRAMES);

	for (count = 0; count < state.count; count++)
		state.frame[count].pc = frames[count];

	print_bt_state(tls, &state);
}

#ifdef __arm__
_static inline uintptr_t __Unwind_GetIP(__unwind_context *ctx)
{
	uint32_t val;
	if (!libc._Unwind_VRS_Get)
		_BUG(0xA0);
	libc._Unwind_VRS_Get(ctx, _UVRSC_CORE, 15,/* 15 == pc (UNWIND_IP_REG) */
			     _UVRSD_UINT32, &val);
	return val; /* & ~1; */ /* thumb bit */
}
#else
#define __Unwind_GetIP(ctx) \
	(uintptr_t)(libc._Unwind_GetIP(ctx))
#endif

/*
 * unwind backtrace code adapted from:
 * 	bionic/libc/bionic/debug_stacktrace.cpp
 *
 */
_static _Unwind_Reason_Code trace_func(__unwind_context* context, void* arg)
{
	struct bt_state *state = (struct bt_state *)arg;
	struct bt_frame *frame;
	uintptr_t ip;

	ip = __Unwind_GetIP(context);

	if (ip != 0 && state->nskip < FRAMES_TO_SKIP) {
		state->nskip++;
		return _URC_NO_REASON;
	}
	if (state->count >= MAX_BT_FRAMES)
		return _URC_END_OF_STACK;

	frame = &state->frame[state->count];

#ifdef __arm__
	/*
	 * The instruction pointer is pointing at the instruction after the
	 * bl(x), and the _Unwind_Backtrace routine already masks the Thumb
	 * mode indicator (LSB in PC). So we need to do a quick check here
	 * to find out if the previous instruction is a Thumb-mode BLX(2).
	 * If so subtract 2 otherwise 4 from PC.
	 */
	/* on ARM, we fixed GetIP to _not_ mask the thumb bit! */
	if (ip & 0x1)
		ip = (ip & ~0x1) - 2;
	else
		ip -= 4;
#ifdef VERBOSE_FRAME_INFO
	libc._Unwind_VRS_Get(context, _UVRSC_CORE, 0,
			     _UVRSD_UINT32, &frame->regs[0]);
	libc._Unwind_VRS_Get(context, _UVRSC_CORE, 1,
			     _UVRSD_UINT32, &frame->regs[1]);
	libc._Unwind_VRS_Get(context, _UVRSC_CORE, 2,
			     _UVRSD_UINT32, &frame->regs[2]);
	libc._Unwind_VRS_Get(context, _UVRSC_CORE, 3,
			     _UVRSD_UINT32, &frame->regs[3]);
	libc._Unwind_VRS_Get(context, _UVRSC_CORE, 14,
			     _UVRSD_UINT32, &frame->lr);
#endif
	/* always save the stack pointer */
	libc._Unwind_VRS_Get(context, _UVRSC_CORE, 13,
			     _UVRSD_UINT32, &frame->sp);
#endif

	if (state->count > 0) {
		if (state->frame[state->count - 1].pc == (void *)ip) {
			/* don't continue indefinitely! */
			if (++(state->recursion) > MAX_RECURSIVE_DEPTH) {
				frame->pc = (void *)-1;
				state->count++;
				return _URC_END_OF_STACK;
			}
			/* recursion: skip this frame! */
			return _URC_NO_REASON;
		}
	}

	frame->pc = (void *)ip;

	state->recursion = 0;
	state->count++;

	if (state->count >= MAX_BT_FRAMES)
		return _URC_END_OF_STACK;
	return _URC_NO_REASON;
}

_static inline int is_same_stack(struct bt_frame *current, void **last, int count)
{
	int i;
	for (i = 0; i < count; i++) {
		if (current[i].pc != last[i])
			return 0;
	}
	return 1;
}


_static void __attribute__((noinline))
unwind_backtrace(struct tls_info *tls)
{
	Dl_info dli;
	int i;
	struct log_info *info;
	unsigned stack_sz;
	struct bt_state state;
	struct dvm_bt dvm_bt;

	if (!tls)
		return;

	libc.memset(&state, 0, sizeof(state));
	libc.memset(&dvm_bt, 0, sizeof(dvm_bt));

	info = &tls->info;

	state.f = tls->logfile;
	state.dvm_bt = &dvm_bt;

	libc._Unwind_Backtrace(trace_func, &state);

	/* check the backtrace to see if it's the same as the previous one */
	if (*(info->last_stack_depth) == state.count) {
		if (is_same_stack(state.frame, info->last_stack, state.count)) {
			/*
			 * these stacks are the same - bump a counter and
			 * don't print anything yet
			 */
			*(info->last_stack_cnt) += 1;

			/*
			 * TODO: here we are making the assumption that an
			 * identical native backtrace is equivalent to an
			 * identical Java/Dalvik backtrace.
			 * THIS MAY NOT BE TRUE!
			 */
			return;
		}
	}
	/*
	 * the current stack is a new one:
	 *     check the last stack count - if it's > 1, print a repeat message
	 *     reset the stats and keep the PCs for next time
	 */
	if (*(info->last_stack_cnt) > 1)
		bt_printf(tls, "BT:REPEAT:%d:", *(info->last_stack_cnt));

	*(info->last_stack_cnt) = 1;
	*(info->last_stack_depth) = state.count;
	for (i = 0; i < state.count; i++)
		info->last_stack[i] = state.frame[i].pc;

#ifndef NO_DVM_BACKTRACE
	get_dvm_backtrace(tls, &dvm, &state, &dvm_bt);
#endif
	print_bt_state(tls, &state);
	/* print stack usage! */
	stack_sz = (unsigned)state.frame[state.count-1].sp
			+ WRAPPER_STACK_SZ
			- (unsigned)info->stack;
	bt_printf(tls, "BT:STACKMEM:%d:", stack_sz);
}

void __hidden __attribute__((noinline))
log_backtrace(struct tls_info *tls)
{
	if (bt_setup_logbuffer(tls, &tls->info) < 0)
		return;

	/*
	 * Use a cache/table to modify info->symbol based on the function
	 * called and its arguments. This allows us to track FDs through the
	 * system and mark their access as network, FS, pipe, etc. by
	 * translating the symbol name to, e.g., read_N (for network read).
	 */
	wrap_symbol_mod(tls);

	/* TODO: maybe print out function arguments? */
	if (libc.backtrace)
		std_backtrace(tls);
	else if (libc._Unwind_Backtrace)
		unwind_backtrace(tls);
	else if (tls->logfile)
		__log_print(&tls->info.tv, tls->logfile, "CALL", "%s", tls->info.symbol);
}

uint8_t __hidden *
__bt_raw_print_start(struct tls_info *tls, int prlen, int *remain_r)
{
	int remain;
	int *log_pos;
	if (!tls)
		return NULL;
	log_pos = tls->info.log_pos;
	if (!log_pos)
		return NULL;

	remain = LOG_BUFFER_SIZE - *log_pos - 1;
	if (remain < prlen) {
		bt_flush(tls, &tls->info);
		log_pos = tls->info.log_pos;
		remain = LOG_BUFFER_SIZE - *log_pos - 1;
		if (remain < prlen) { /* never going to fit... */
			log_print(tls->logfile, LOG, "E:TRUNCATED!");
			return NULL;
		}
	}

	if (remain_r)
		*remain_r = remain;
	return (uint8_t *)(tls->info.log_buffer) + *log_pos;
}

int __bt_raw_maybe_finish(struct tls_info *tls, int len, int remain)
{
	int *log_pos = tls->info.log_pos;

	if (len > remain) {
		if (!(*log_pos)) {
			/* this buffer just doesn't fit! */
			__bt_raw_print_end(tls, 0);
			bt_flush(tls, &tls->info);
			log_flush(tls->logfile);
			log_print(tls->logfile, LOG, "E:TRUNCATED!");
			return -1;
		}
		bt_flush(tls, &tls->info);
		return 1;
	}
	__bt_raw_print_end(tls, len);
	/* flush if we exactly filled the buffer */
	if (len == remain)
		bt_flush(tls, &tls->info);
	return 0;
}


void __hidden
__bt_raw_print_end(struct tls_info *tls, int prlen)
{
	*(tls->info.log_pos) += prlen;
	BT_EXTRA_FLUSH(tls, &tls->info);
}


void __hidden
__bt_raw_print(struct tls_info *tls, const char *str, int slen)
{
	uint8_t *buf;
	int prlen;

	if (slen <= 0)
		slen = local_strlen(str);

	prlen = slen + tls->info.tv_strlen;

	/* make sure there's enough room for us to put a terminating NULL */
	buf = __bt_raw_print_start(tls, prlen + 1, NULL);

	if (buf) {
		libc.memcpy(buf, tls->info.tv_str, tls->info.tv_strlen);
		libc.memcpy(buf + tls->info.tv_strlen, str, slen);
		buf[prlen] = 0; /* make sure it's always NULL terminated */
		__bt_raw_print_end(tls, prlen);
	}
}
