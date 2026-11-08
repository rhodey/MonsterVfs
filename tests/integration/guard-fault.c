/* Fault the VFS's own OFD guard, which bypasses SQLite's syscall table. */
#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>

static _Thread_local int armed;
static _Atomic int hits;
void mvfs_arm_guard(int release) { armed=release ? 2 : 1; }
int mvfs_guard_hits(void) { return atomic_load(&hits); }
static int fail_guard(int command, uintptr_t argument) {
  if(command!=F_OFD_SETLK && command!=F_OFD_SETLKW) return 0;
  struct flock *lock=(void *)argument;
  if(armed && lock->l_start==8191 && lock->l_len==1 &&
      lock->l_type==(armed==2 ? F_UNLCK : F_WRLCK)) {
    int failure=armed; armed=0; atomic_fetch_add(&hits,1); return failure;
  }
  return 0;
}
/* Only lock commands carry pointers. Forward the other unix-VFS commands with
 * their real argument type, including commands which have no third argument. */
static int argument_kind(int command) {
  switch(command) {
    case F_GETFD: case F_GETFL: case F_GETOWN:
#ifdef F_FULLFSYNC
    case F_FULLFSYNC:
#endif
#ifdef F_GETSIG
    case F_GETSIG: case F_GETLEASE: case F_GETPIPE_SZ: case F_GET_SEALS:
#endif
      return 0;
    case F_DUPFD: case F_DUPFD_CLOEXEC: case F_SETFD: case F_SETFL: case F_SETOWN:
#ifdef F_NOCACHE
    case F_NOCACHE:
#endif
#ifdef F_SETSIG
    case F_SETSIG: case F_SETLEASE: case F_SETPIPE_SZ: case F_ADD_SEALS:
#endif
      return 1;
    default: return 2;
  }
}
static uintptr_t argument(int kind, va_list *args) {
  if(kind==0) return 0;
  return kind==1 ? (uintptr_t)va_arg(*args,int) : (uintptr_t)va_arg(*args,void *);
}
static int forward(int (*next)(int,int,...),int fd,int command,int kind,uintptr_t arg) {
  int failure=fail_guard(command,arg);
  if(failure==1) { errno=EIO; return -1; }
  int rc=kind==0 ? next(fd,command) :
    kind==1 ? next(fd,command,(int)arg) : next(fd,command,(void *)arg);
  /* Report release failure after releasing the OS lock, so the test does not
   * strand unrelated connections behind an artificial permanently held lock. */
  if(failure==2) { errno=EIO; return -1; }
  return rc;
}
#ifdef __APPLE__
static int intercepted_fcntl(int fd,int command,...) {
  int kind=argument_kind(command);
  va_list args; va_start(args,command); uintptr_t arg=argument(kind,&args); va_end(args);
  return forward(fcntl,fd,command,kind,arg);
}
__attribute__((used)) static struct { const void *replacement,*original; } interpose
__attribute__((section("__DATA,__interpose"))) = {(void *)intercepted_fcntl,(void *)fcntl};
#else
#define INTERCEPT(name) \
int name(int fd,int command,...) { \
  int (*next)(int,int,...)=dlsym(RTLD_NEXT,#name); \
  int kind=argument_kind(command); \
  va_list args; va_start(args,command); uintptr_t arg=argument(kind,&args); va_end(args); \
  return forward(next,fd,command,kind,arg); \
}
INTERCEPT(fcntl)
INTERCEPT(fcntl64)
#endif
