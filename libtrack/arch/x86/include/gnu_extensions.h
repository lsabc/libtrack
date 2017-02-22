int dup3(int oldfd, int newfd, int flags);
int getopt_long(int argc, char * const argv[], const char *optstring, const struct option *longopts, int *longindex);
pid_t wait3(int *status, int options, struct rusage *rusage);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage);
int argvccept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flagss);
struct mallinfo mallinfo(void);
int pipe2(int pipefd[2], int flags);
int fcloseall(void);
int execvpe(const char *file, char *const argv[], char *const envp[]);
#void error(int status, int errnum, const char *format, ...);
#void error_at_line(int status, int errnum, const char *filename, unsigned int linenum, const charhar *format, ...);
int ppoll(struct pollfd *fds, nfds_t nfds, const structtruct timespec *timeout_ts, const sigset_t *sigmask);
int clone(int (*func)(void *), void *child_stack, int flags, void *arg, ...);
int mallopt(int param, int value);
int mkostemp(char *template, int suffixlen);
void *mempcpy(void *dest, const void *src, size_t n);
int versionsort(const void *a, const void *b);
int pthread_sigqueue(pthread_t *thread, int sig, const union sigval value);
int cacheflush(char *cacheflushaddr, int nbytes, int cache);