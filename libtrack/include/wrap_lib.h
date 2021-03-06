/*
 * wrap_lib.h
 * Copyright (C) 2013 Jeremy C. Andrus
 */
#ifndef WRAP_LIB_H
#define WRAP_LIB_H

/*
 * Uncomment the following line to flush after almost every print.
 * NOTE: this is only recommended for debugging!
 */
//#define AGGRESIVE_FLUSHING

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>
#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/stat.h>

__BEGIN_DECLS

/* easily export / hide all non-wrapped functionality */
#ifdef EXPORT_ALL_SYMS
#define _static
#define __hidden
#else
#define _static static
#define __hidden __attribute__((visibility("hidden")))
#endif

#include "libz.h"

#define ___str(x) #x
#define __str(x) ___str(x)
#define _str(x) __str(x)

#ifndef container_of
#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})
#endif

#define ARRAY_SZ(arr) \
	(sizeof(arr) / sizeof((arr)[0]))

#define EN_LOG_FILE "enable_trace_logs"

#if defined(ANDROID)
#  define LIB_PATH "/system/lib"
#  if defined(_LIBC) && _LIBC == 1
#    define LIBC_NAME "_ibc_so"
#  else
#    define LIBC_NAME "libc.so"
#  endif
#  define LOGFILE_PATH "/data/trace_logs"
#  define ENABLE_LOG_PATH "/data/" EN_LOG_FILE
#elif defined(__APPLE__)
#  define LIB_PATH "/usr/lib/system"
#  if defined(_LIBC) && _LIBC == 1
#    define LIBC_NAME "_ibc_dylib"
#  else
#    define LIBC_NAME "libc.dylib"
#  endif
#  define LOGFILE_PATH "/tmp/trace_logs"
#  define ENABLE_LOG_PATH "/tmp/" EN_LOG_FILE
#else
#  define LIB_PATH "/lib"
#  if defined(_LIBC) && _LIBC == 1
#    define LIBC_NAME "_ibc_so"
#  else
#    define LIBC_NAME "_ibc_so"
#  endif
#  define LOGFILE_PATH "/tmp/trace_logs"
#  define ENABLE_LOG_PATH "/tmp/" EN_LOG_FILE
#endif

#define LIBC_PATH LIB_PATH "/" LIBC_NAME

#if defined(HAVE_UNWIND_CONTEXT_STRUCT) || defined(__clang__)
typedef struct _Unwind_Context __unwind_context;
#else
typedef _Unwind_Context __unwind_context;
#endif
typedef _Unwind_Reason_Code (*bt_func)(__unwind_context* context, void* arg);

struct libc_iface {
	void *dso;
	int   wrap_cache;
	int   forking;

	FILE *(*fopen)(const char *pathname, const char *mode);
	int (*fclose)(FILE *f);
	ssize_t (*fwrite)(const void *buf, size_t size, size_t nitems, FILE *f);
	ssize_t (*fread)(void *buf, size_t size, size_t nitems, FILE *f);
	int (*fflush)(FILE *f);
	int (*fno)(FILE *f);
	int (*fchmod)(int fd, mode_t mode);
	int (*access)(const char *path, int amode);
	int (*stat)(const char *path, struct stat *buf);
	int (*dirfd)(DIR *dirp);

	pid_t (*getpid)(void);
	uint32_t (*gettid)(void);
	int (*nanosleep)(const struct timespec *req, struct timespec *rem);

	int (*pthread_key_create)(pthread_key_t *key, void (*destructor)(void *));
	int (*pthread_key_delete)(pthread_key_t key);
	void *(*pthread_getspecific)(pthread_key_t key);
	int (*pthread_setspecific)(pthread_key_t key, const void *val);

#ifdef ANDROID
	void (*__pthread_cleanup_push)(__pthread_cleanup_t *c,
				       __pthread_cleanup_func_t func,
				       void *arg);
	void (*__pthread_cleanup_pop)(__pthread_cleanup_t *c, int execute);
	ssize_t (*readlink)(const char *path, char *buf, size_t bufsize);
#endif

	int (*pthread_mutex_lock)(pthread_mutex_t *mutex);
	int (*pthread_mutex_unlock)(pthread_mutex_t *mutex);

	int (*snprintf)(char *str, size_t size, const char *format, ...);
	int (*printf)(const char *fmt, ...);
	int (*fprintf)(FILE *f, const char *fmt, ...);
	long (*strtol)(const char *str, char **endptr, int base);

	void *(*memset)(void *b, int c, size_t len);
	void *(*memcpy)(void *dst, const void *src, size_t len);
	void *(*malloc)(size_t size);
	void (*free)(void *ptr);

	char *(*getenv)(const char *name);
	int (*setenv)(const char *name, const char *value, int overwrite);

	int (*gettimeofday)(struct timeval *tp, void *tzp);
	int (*clock_gettime)(int , struct timespec *tm);

	int (*sigaction)(int sig, const struct sigaction *act,
			 struct sigaction *oact);
	char *(*strsignal)(int sig);

	int (*getsockname)(int sockfd, struct sockaddr *addr, int* addrlen);

	void (*__cxa_finalize)(void *dso);
	int (*raise)(int sig);
	void (*abort)(void);

	/* backtrace interface */
	int (*backtrace)(void **buffer, int size);
	char ** (*backtrace_symbols)(void *const *buffer, int size);

	/* unwind interface */
	uintptr_t (*_Unwind_GetIP)(__unwind_context *ctx);
#ifdef __arm__
	_Unwind_VRS_Result (*_Unwind_VRS_Get)(__unwind_context *context,
					      _Unwind_VRS_RegClass regclass,
					      uint32_t regno,
					      _Unwind_VRS_DataRepresentation representation,
					      void *valuep);
#endif
	uintptr_t (*_Unwind_Backtrace)(bt_func func, void *arg);
};

extern struct libc_iface libc;

extern int cached_pid;
extern int log_timing;

struct tls_info;

extern int  __set_wrapping(void);
extern void __clear_wrapping(void);

extern int  __get_libc(struct tls_info *tls, const char *symbol);
extern void __put_libc(void);

extern int init_libc_iface(struct libc_iface *iface, const char *dso_path);

extern void *wrapped_dlsym(const char *libpath, void **lib_handle, const char *symbol);

extern int wrapped_tracer(const char *symbol, void *symptr, void *regs, void *stack);

extern void *get_log(int release);
extern void *__get_log(int release);

extern const char *local_strrchr(const char *str, int c);
extern int local_strcmp(const char *s1, const char *s2);
extern int local_strncmp(const char *s1, const char *s2, size_t n);
extern int local_strlen(const char *s);

extern void libc_close_log(void);

static inline int should_log(void)
{
	int err;
	char buf[32];
	FILE *f;

	err = libc.access(ENABLE_LOG_PATH, F_OK);
	if (err < 0) {
		if (cached_pid)
			libc_close_log();
		cached_pid = 0;
		log_timing = 0;
		return 0;
	}
	if (!cached_pid &&
	    (f = libc.fopen(ENABLE_LOG_PATH, "r")) != NULL) {
		libc.memset(buf, 0, sizeof(buf));
		if (libc.fread(buf, 1, sizeof(buf), f) > 0) {
			char *tptr = buf;
			int maybe_log_timing = 0;
			while (*tptr && *tptr != ':'
			       && tptr < buf + sizeof(buf))
				tptr++;
			while (*tptr == ':') {
				maybe_log_timing = 1;
				*tptr++ = '\0';
			}
			cached_pid = libc.strtol(buf, NULL, 10);
			if (maybe_log_timing && tptr < buf + sizeof(buf))
				log_timing = libc.strtol(tptr, NULL, 10);
		}
		if (!cached_pid)
			cached_pid = -1;
		libc.fclose(f);
	}

	if (!cached_pid)
		return 0;
	if (cached_pid > 0)
		return libc.getpid() == cached_pid;
	/* if cached_pid < 0, then always log */
	return 1;
}

extern volatile int*  __errno(void);

#define LOG_BUFFER_SIZE (32 * 1024)

struct log_info {
	const char *symbol;
	void *func;
	uint32_t *regs;
	void *stack;
	struct timeval tv;
	char tv_str[32];
	int  tv_strlen;

	int log_time;

	uint8_t should_log;
	uint8_t should_handle;
	uint8_t should_mod_sym;
	uint8_t symhash;
	void *symcache;

	void **last_stack;
	int   *last_stack_depth;
	int   *last_stack_cnt;

	char  *log_buffer;
	int   *log_pos;
};

/*
 * TODO: this should probably be platform/arch specific...
 */
#define MAX_SYMBOL_LEN 256
struct ret_ctx {
	/* for now we'll have space for 4 32-bit registers */
	union {
		uint64_t u64[2];
		uint32_t u32[4];
		uint16_t u16[8];
		uint8_t  u8[16];
	} u;
	int _errno;
	char symmod[MAX_SYMBOL_LEN];
	const char *sym;
	struct timespec posix_start;
};

extern struct ret_ctx *get_retmem(struct tls_info *tls);

#define is_main() \
	((uint32_t)libc.getpid() == libc.gettid())

#define mtx_lock(mtx) \
	if (libc.pthread_mutex_lock && libc.pthread_mutex_unlock) \
		libc.pthread_mutex_lock(mtx)

#define mtx_unlock(mtx) \
	if (libc.pthread_mutex_lock && libc.pthread_mutex_unlock) \
		libc.pthread_mutex_unlock(mtx)

#define __log_print_raw(tvptr, f, fmt, ...) \
	if (f) { \
	if (zlib.valid) \
		zlib.gzprintf((struct gzFile *)(f), "%lu.%lu:" fmt, \
			     (unsigned long)(tvptr)->tv_sec, \
			     (unsigned long)(tvptr)->tv_usec, ## __VA_ARGS__ ); \
	else \
		libc.fprintf((FILE *)(f), "%lu.%lu:" fmt, \
			     (unsigned long)(tvptr)->tv_sec, \
			     (unsigned long)(tvptr)->tv_usec, ## __VA_ARGS__ ); \
	}

#define __log_print(tvptr, f, key, fmt, ...) \
	__log_print_raw(tvptr, f, key ":" fmt "\n", ## __VA_ARGS__ )

#define log_print(f, key, fmt, ...) \
	if (f) { \
		struct timeval tv; \
		libc.gettimeofday(&tv, NULL); \
		__log_print(&tv, f, #key, fmt, ## __VA_ARGS__ ); \
	}

#define libc_log(fmt, ...) \
	do { \
		struct tls_info *tls = peek_tls(); \
		if (!tls) \
			break; \
		if (zlib.valid && tls->info.log_pos) { \
			bt_printf(tls, fmt, ## __VA_ARGS__); \
			bt_flush(tls, &(tls->info)); \
		} else { \
			log_print(tls->logfile, LOG, fmt, ## __VA_ARGS__ ); \
		} \
		log_flush(tls->logfile); \
	} while (0)

#define log_flush(f) \
	if (f) { \
		if (zlib.valid) \
			; /* zlib.gzflush((struct gzFile *)(f), Z_SYNC_FLUSH); */ \
		else \
			libc.fflush((FILE *)f); \
	}

#define log_close(f) \
	if (f) { \
		if (zlib.valid) { \
			zlib.gzflush((struct gzFile *)f, Z_FINISH); \
			zlib.gzclose((struct gzFile *)f); \
		} else \
			libc.fclose((FILE *)(f)); \
	}

#define lnk_dbg(msg) \
	do { \
		extern void *real_libc_dso; \
		(void)dlsym(real_libc_dso, "LOG:" msg); \
	} while (0)


#define __bt_flush(logfile, logbuffer, pos) \
do { \
	int prlen = *(pos); \
	((uint8_t *)(logbuffer))[prlen] = 0; \
	if (zlib.valid) { \
		zlib.gzwrite((struct gzFile *)(logfile), (logbuffer), prlen); \
		/* I swear there's a bug in libz's gzwrite() - this extra call \
		 * seems to make it better, but still not OK... */ \
		zlib.gzwrite((struct gzFile *)(logfile), " ", 1); /* why?!?! */ \
	} else \
		libc.fwrite((logbuffer), *(pos), 1, (FILE *)(logfile)); \
	*(pos) = 0; \
} while (0)

#define bt_flush(tls, info) \
	if ((tls) && (tls)->logfile && (info)->log_pos && *((info)->log_pos) > 0) \
		__bt_flush((tls)->logfile, (info)->log_buffer, (info)->log_pos); \

#ifdef AGGRESIVE_FLUSHING
#define BT_EXTRA_FLUSH(tls,info) \
	{ \
		bt_flush(tls, info); \
		if (tls) { log_flush((tls)->logfile); } \
	}
#else
#define BT_EXTRA_FLUSH(tls,info)
#endif

extern uint8_t * __bt_raw_print_start(struct tls_info *tls,
				      int prlen, int *remain_r);
extern int __bt_raw_maybe_finish(struct tls_info *tls, int len, int remain);
extern void __bt_raw_print_end(struct tls_info *tls, int prlen);
extern void __bt_raw_print(struct tls_info *tls,
			   const char *str, int slen);

/*
 * I'm keeping this as a macro to avoid vararg processing
 */
#define __bt_printf(____tls, fmt, ...) \
	do { \
		int __ret, __len, __remain = 0; \
		uint8_t *__logpos; \
		__len = (____tls)->info.tv_strlen; \
		__logpos = __bt_raw_print_start(____tls, __len + 64, &__remain); \
		if (!__logpos) \
			break; \
		libc.memcpy(__logpos, (____tls)->info.tv_str, __len); \
		__len += libc.snprintf((char *)__logpos + __len, \
				       __remain - __len, fmt "\n ", ## __VA_ARGS__ ); \
		__ret = __bt_raw_maybe_finish(____tls, __len, __remain); \
		if (__ret > 0) \
			continue; \
		break; \
	} while (1)

/*
 * this separate macro used to be useful... it's still here so
 * I don't have to change other code now :-)
 */
#define bt_printf(__tls, fmt, ...) \
	__bt_printf(__tls, fmt, ## __VA_ARGS__ )

#define _BUG(X) \
	do { \
		*((volatile int *)(0xFFFF0000 | (uint32_t)(X))) = X; \
	} while (0)

#define BUG(X) \
	do { \
		void *f; \
		f = __get_log(1); \
		if (f) { \
			log_print(f, _BUG_, "(0x%x) at %s:%d", X, __FILE__, __LINE__); \
			log_flush(f); \
			log_close(f); \
		} \
		_BUG(X); \
	} while (0)

#define BUG_MSG(X,fmt,...) \
	do { \
		void *f; \
		f = __get_log(1); \
		if (f) { \
			log_print(f, _BUG_, "(0x%x) at %s:%d", X, __FILE__, __LINE__); \
			log_print(f, _BUG_, fmt, ## __VA_ARGS__ ); \
			log_flush(f); \
			log_close(f); \
		} \
		_BUG(X); \
	} while (0)

#ifdef _LIBC
#define SYMERR(X) \
	_BUG(X)
#else
#define SYMERR(X) \
	BUG(X)
#endif

__END_DECLS
#endif
