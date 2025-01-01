struct stat;
struct rtcdate;
struct proc;
struct reetrantlock;

// system calls
int fork(void);
int exit(void) __attribute__((noreturn));
int wait(void);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
int sleep(int);
int uptime(void);
int sort_syscalls(int pid);
int get_process_by_pid(int, struct proc**);
int get_most_invoked_syscall(int);
int list_all_processes(void);
int move_file(const char *src_file, const char *dest_dir);
void find_palindrome(int);
void show_process_info(void);
int change_sched_Q(int , int);
//int set_proc_sjf_params(int, int, int);
int getcount(void);
int initreentrantlock(struct reetrantlock *rlk, char *name);
int acquirereentrantlock(struct reetrantlock *rlk);
int releasereentrantlock(struct reetrantlock *rlk) ;




// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void printf(int, const char*, ...);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
void* malloc(uint);
void free(void*);
int atoi(const char*);
