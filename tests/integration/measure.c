#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif

/* Instrument the host SQLite's actual OS calls. The VFS delegates payload I/O
 * to this host. Directory barriers issued directly by Rust are not counted;
 * creation/setup finish before any measured stage begins. */
static _Atomic uint64_t writes, write_bytes, syncs, data_syncs;
static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*real_pwrite64)(int, const void *, size_t, off_t);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_fcntl)(int, int, ...);
static ssize_t counted_pwrite(int fd, const void *bytes, size_t n, off_t offset) {
  atomic_fetch_add(&writes, 1);
  ssize_t result = real_pwrite(fd, bytes, n, offset);
  if (result > 0) atomic_fetch_add(&write_bytes, (uint64_t)result);
  return result;
}
static ssize_t counted_pwrite64(int fd, const void *bytes, size_t n, off_t offset) {
  atomic_fetch_add(&writes, 1);
  ssize_t result = real_pwrite64(fd, bytes, n, offset);
  if (result > 0) atomic_fetch_add(&write_bytes, (uint64_t)result);
  return result;
}
static ssize_t counted_write(int fd, const void *bytes, size_t n) {
  atomic_fetch_add(&writes, 1);
  ssize_t result = real_write(fd, bytes, n);
  if (result > 0) atomic_fetch_add(&write_bytes, (uint64_t)result);
  return result;
}
/* A full sync bypasses fsync, and both VFSes measure the same barrier. */
static int counted_fcntl(int fd, int command, ...) {
  va_list arguments;
  va_start(arguments, command);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
#ifdef F_FULLFSYNC
  if (command == F_FULLFSYNC) atomic_fetch_add(&syncs, 1);
#endif
  return real_fcntl(fd, command, argument);
}
int mvfs_measure_fsync(int fd) {
  atomic_fetch_add(&syncs, 1);
  return fsync(fd);
}
int mvfs_measure_fdatasync(int fd) {
  atomic_fetch_add(&data_syncs, 1);
#ifdef __APPLE__
  return fsync(fd);
#else
  return fdatasync(fd);
#endif
}
static void check(int code, sqlite3 *db, const char *operation) {
  if (code != SQLITE_OK) {
    fprintf(stderr, "%s: %d %s\n", operation, code, db ? sqlite3_errmsg(db) : "");
    exit(1);
  }
}
static void sql(sqlite3 *db, const char *text) {
  check(sqlite3_exec(db, text, NULL, NULL, NULL), db, text);
}
static double now(void) {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time)) { perror("clock_gettime"); exit(1); }
  return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}
static void reset(void) {
  atomic_store(&writes, 0); atomic_store(&write_bytes, 0);
  atomic_store(&syncs, 0); atomic_store(&data_syncs, 0);
}
static void file_size(const char *path, const char *suffix, long long *size, long long *allocated) {
  char name[8192];
  if (snprintf(name, sizeof(name), "%s%s", path, suffix) >= (int)sizeof(name)) exit(1);
  struct stat info;
  if (stat(name, &info)) {
    if (errno != ENOENT) { perror("stat"); exit(1); }
    *size = 0; *allocated = 0; return;
  }
  *size = info.st_size; *allocated = (long long)info.st_blocks * 512;
}
static void report(const char *stage, const char *path, double start) {
  double elapsed = now() - start;
  long long main, wal, shm, main_allocated, wal_allocated, shm_allocated;
  file_size(path, "", &main, &main_allocated);
  file_size(path, "-wal", &wal, &wal_allocated);
  file_size(path, "-shm", &shm, &shm_allocated);
  printf("{\"stage\":\"%s\",\"elapsed_ms\":%.3f,\"write_calls\":%llu,\"write_bytes\":%llu,"
         "\"sqlite_fsync_calls\":%llu,\"sqlite_fdatasync_calls\":%llu,\"main_bytes\":%lld,"
         "\"wal_bytes\":%lld,\"shm_bytes\":%lld,\"allocated_bytes\":%lld}\n", stage, elapsed * 1000,
         (unsigned long long)atomic_load(&writes), (unsigned long long)atomic_load(&write_bytes),
         (unsigned long long)atomic_load(&syncs), (unsigned long long)atomic_load(&data_syncs),
         main, wal, shm, main_allocated + wal_allocated + shm_allocated);
}
static void install_counts(sqlite3_vfs *native) {
  real_pwrite = (ssize_t(*)(int,const void*,size_t,off_t))native->xGetSystemCall(native, "pwrite");
  real_pwrite64 = (ssize_t(*)(int,const void*,size_t,off_t))native->xGetSystemCall(native, "pwrite64");
  real_write = (ssize_t(*)(int,const void*,size_t))native->xGetSystemCall(native, "write");
  real_fcntl = (int(*)(int,int,...))native->xGetSystemCall(native, "fcntl");
  if (!real_pwrite && !real_pwrite64 && !real_write) { fputs("write hooks unavailable\n", stderr); exit(1); }
  check(native->xSetSystemCall(native, "fcntl", (sqlite3_syscall_ptr)counted_fcntl), NULL, "hook fcntl");
  if (real_pwrite) check(native->xSetSystemCall(native, "pwrite", (sqlite3_syscall_ptr)counted_pwrite), NULL, "hook pwrite");
  if (real_pwrite64) check(native->xSetSystemCall(native, "pwrite64", (sqlite3_syscall_ptr)counted_pwrite64), NULL, "hook pwrite64");
  if (real_write) check(native->xSetSystemCall(native, "write", (sqlite3_syscall_ptr)counted_write), NULL, "hook write");
}
static void insert(sqlite3 *db, sqlite3_stmt *statement, int key, int bytes, unsigned char *payload) {
  check(sqlite3_bind_int(statement, 1, key), db, "bind key");
  check(sqlite3_bind_blob(statement, 2, payload, bytes, SQLITE_STATIC), db, "bind payload");
  int code = sqlite3_step(statement);
  if (code != SQLITE_DONE) check(code, db, "insert");
  check(sqlite3_reset(statement), db, "reset insert");
}
int main(int argc, char **argv) {
  if (argc != 6) { fputs("usage: measure EXTENSION DATABASE VFS COMMITS BATCH_ROWS\n", stderr); return 2; }
  int commits = atoi(argv[4]), batch = atoi(argv[5]);
  if (commits < 1 || commits > 100000 || batch < 1 || batch > 100000) return 2;
  const char *path = argv[2], *vfs = argv[3];
  int (*stop)(void) = NULL;
  if (strcmp(vfs, MVFS_NAME) == 0) {
    sqlite3 *loader = NULL;
    check(sqlite3_open(":memory:", &loader), loader, "loader open");
    check(sqlite3_enable_load_extension(loader, 1), loader, "enable extension");
    char *error = NULL;
    int code = sqlite3_load_extension(loader, argv[1], MVFS_ENTRY, &error);
    if (code) { fprintf(stderr, "load extension: %s\n", error ? error : sqlite3_errmsg(loader)); return 1; }
    sqlite3_free(error);
    check(sqlite3_close(loader), NULL, "loader close");
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    stop = (int(*)(void))dlsym(library, "monstervfs_shutdown");
    if (!stop) return 1;
  }
  sqlite3_vfs *native = sqlite3_vfs_find("unix");
  if (!native) return 1;
  install_counts(native);
  sqlite3 *db = NULL;
  check(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs), db, "database open");
  sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
          "PRAGMA fullfsync=ON; PRAGMA wal_autocheckpoint=0; PRAGMA mmap_size=0; PRAGMA cache_size=-8192;"
          "CREATE TABLE t(id INTEGER PRIMARY KEY, value BLOB); PRAGMA wal_checkpoint(TRUNCATE);");
  int psow = -1;
  check(sqlite3_file_control(db, "main", SQLITE_FCNTL_POWERSAFE_OVERWRITE, &psow), db, "read powersafe-overwrite setting");
  sqlite3_file *file = NULL;
  check(sqlite3_file_control(db, "main", SQLITE_FCNTL_FILE_POINTER, &file), db, "read file pointer");
  int sector = file->pMethods->xSectorSize(file);
  struct statfs filesystem;
  if (statfs(path, &filesystem)) { perror("statfs"); return 1; }
#ifdef __APPLE__
  printf("{\"sqlite\":\"%s\",\"filesystem\":\"%s\",\"vfs\":\"%s\",\"powersafe_overwrite\":%d,\"sector_bytes\":%d}\n", sqlite3_libversion(), filesystem.f_fstypename, vfs, psow, sector);
#else
  printf("{\"sqlite\":\"%s\",\"filesystem_magic\":\"%lx\",\"vfs\":\"%s\",\"powersafe_overwrite\":%d,\"sector_bytes\":%d}\n", sqlite3_libversion(), (unsigned long)filesystem.f_type, vfs, psow, sector);
#endif
  sqlite3_stmt *statement = NULL;
  check(sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?1,?2)", -1, &statement, NULL), db, "prepare insert");
  unsigned char payload[4096];
  for (int i = 0; i < 4096; i++) payload[i] = (unsigned char)(i * 31);
  reset(); double start = now();
  for (int i = 1; i <= commits; i++) insert(db, statement, i, 128, payload);
  report("autocommit", path, start);
  reset(); start = now();
  sql(db, "BEGIN IMMEDIATE");
  for (int i = 1; i <= batch; i++) insert(db, statement, commits + i, 4096, payload);
  sql(db, "COMMIT");
  report("batch", path, start);
  check(sqlite3_finalize(statement), db, "finalize insert");
  reset(); start = now();
  int log_frames = 0, checkpointed = 0;
  check(sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, &log_frames, &checkpointed), db, "checkpoint");
  report("checkpoint", path, start);
  check(sqlite3_prepare_v2(db, "SELECT count(*) FROM t", -1, &statement, NULL), db, "prepare count");
  if (sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_int(statement, 0) != commits + batch) return 1;
  check(sqlite3_finalize(statement), db, "finalize count");
  check(sqlite3_close(db), NULL, "close database");
  if (stop) check(stop(), NULL, "stop maintenance");
  check(native->xSetSystemCall(native, NULL, NULL), NULL, "restore syscall hooks");
  return sqlite3_shutdown();
}
