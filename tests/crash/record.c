/* Records every durable effect of a real SQLite workload, with the point each
 * commit was acknowledged, so tools/test-crash can rebuild power-loss states.
 * Writes reach this host through the native VFS whichever VFS is selected. */
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "sqlite3.h"
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXTRA_CYCLES 3
#define UPDATED_ROWS 60

enum { STANDARD, HARD, VACUUM, RESUME_EXCLUSIVE, RESUME_SHARED };

static FILE *events, *payloads;
static long payload_at;
static char prefix[PATH_MAX];
static int (*real_open)(const char *, int, int);
static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*real_pwrite64)(int, const void *, size_t, off_t);
static int (*real_ftruncate)(int, off_t);
static int (*real_unlink)(const char *);
static int (*real_fcntl)(int, int, ...);

static int recorded(const char *path) {
  size_t length = strlen(path);
  if (strncmp(path, prefix, strlen(prefix)) != 0) return 0;
  return !(length > 4 && strcmp(path + length - 4, "-shm") == 0);
}
static int file_path(int fd, char *path) {
#ifdef __APPLE__
  return fcntl(fd, F_GETPATH, path) == 0 && recorded(path);
#else
  char link[64];
  snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  ssize_t length = readlink(link, path, PATH_MAX - 1);
  if (length <= 0) return 0;
  path[length] = 0;
  return recorded(path);
#endif
}
static void record_write(int fd, const void *bytes, size_t count, off_t offset) {
  char path[PATH_MAX];
  if (!events || !file_path(fd, path)) return;
  if (fwrite(bytes, 1, count, payloads) != count) exit(1);
  fprintf(events, "W %s %lld %zu %ld\n", path, (long long)offset, count, payload_at);
  payload_at += (long)count;
}
static void record_sync(int fd) {
  char path[PATH_MAX];
  if (events && file_path(fd, path)) {
    fprintf(events, "S %s\n", path);
    fflush(events);
  }
}
static ssize_t hook_write(int fd, const void *bytes, size_t count) {
  off_t offset = lseek(fd, 0, SEEK_CUR);
  ssize_t done = real_write(fd, bytes, count);
  if (done > 0) record_write(fd, bytes, (size_t)done, offset);
  return done;
}
static ssize_t hook_pwrite(int fd, const void *bytes, size_t count, off_t offset) {
  ssize_t done = real_pwrite(fd, bytes, count, offset);
  if (done > 0) record_write(fd, bytes, (size_t)done, offset);
  return done;
}
static ssize_t hook_pwrite64(int fd, const void *bytes, size_t count, off_t offset) {
  ssize_t done = real_pwrite64(fd, bytes, count, offset);
  if (done > 0) record_write(fd, bytes, (size_t)done, offset);
  return done;
}
static int hook_ftruncate(int fd, off_t length) {
  char path[PATH_MAX];
  int result = real_ftruncate(fd, length);
  if (result == 0 && events && file_path(fd, path)) {
    fprintf(events, "T %s %lld\n", path, (long long)length);
  }
  return result;
}
static int hook_unlink(const char *path) {
  int result = real_unlink(path);
  if (result == 0 && events && recorded(path)) fprintf(events, "U %s\n", path);
  return result;
}
static int hook_open(const char *path, int flags, int mode) {
  int result = real_open(path, flags, mode);
  if (result >= 0 && events && recorded(path) && (flags & O_CREAT)) fprintf(events, "O %s\n", path);
  return result;
}
/* A full sync bypasses fsync entirely, and the source rename cannot see it. */
static int hook_fcntl(int fd, int command, ...) {
  va_list arguments;
  va_start(arguments, command);
  void *argument = va_arg(arguments, void *);
  va_end(arguments);
  int result = real_fcntl(fd, command, argument);
#ifdef F_FULLFSYNC
  if (result == 0 && command == F_FULLFSYNC) record_sync(fd);
#endif
  return result;
}
int mvfs_crash_fsync(int fd) {
  int result = fsync(fd);
  if (result == 0) record_sync(fd);
  return result;
}
int mvfs_crash_fdatasync(int fd) { return mvfs_crash_fsync(fd); }

static void fail(sqlite3 *db, const char *where, int code) {
  fprintf(stderr, "%s: %d %s\n", where, code, db ? sqlite3_errmsg(db) : "");
  exit(1);
}
static void sql(sqlite3 *db, const char *text) {
  char *error = NULL;
  int code = sqlite3_exec(db, text, NULL, NULL, &error);
  if (code != SQLITE_OK) fail(db, error ? error : text, code);
}
static void run(sqlite3 *db, sqlite3_stmt *statement, const char *where) {
  int code = sqlite3_step(statement);
  if (code != SQLITE_DONE) fail(db, where, code);
  sqlite3_reset(statement);
}
/* One more session on a crash image, so that another crash can land inside it. An
 * exclusive session checkpoints before it commits, and closes. A shared one just commits. */
static void resume(const char *vfs, const char *database, int commits, int exclusive) {
  sqlite3 *db = NULL;
  sqlite3_stmt *highest = NULL, *insert = NULL;
  int code = sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE, vfs);
  if (code != SQLITE_OK) fail(db, "open", code);
  /* First, because setting synchronous reads the schema, which would open a shared index. */
  if (exclusive) sql(db, "PRAGMA locking_mode=EXCLUSIVE");
  sql(db, "PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
  code = sqlite3_prepare_v2(db, "SELECT coalesce(max(id),0) FROM t", -1, &highest, NULL);
  if (code != SQLITE_OK) fail(db, "resume", code);
  if ((code = sqlite3_step(highest)) != SQLITE_ROW) fail(db, "resume", code);
  int next = sqlite3_column_int(highest, 0) + 1;
  sqlite3_finalize(highest);
  /* Whatever recovery kept was durable when this session began. */
  fprintf(events, "A %d\n", next - 1);
  fflush(events);
  if (exclusive) sql(db, "PRAGMA wal_checkpoint");
  sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?1,printf('%01200d',?1))", -1, &insert, NULL);
  for (int commit = 0; commit < commits; commit++, next++) {
    sqlite3_bind_int(insert, 1, next);
    run(db, insert, "insert");
    fprintf(events, "A %d\n", next);
    fflush(events);
  }
  sqlite3_finalize(insert);
  if (exclusive && (code = sqlite3_close(db)) != SQLITE_OK) fail(NULL, "close", code);
  fflush(events);
  fflush(payloads);
  _exit(0);
}
#define HOOK(field, name, hook)                                                    \
  do {                                                                             \
    field = (void *)native->xGetSystemCall(native, name);                          \
    if (field) native->xSetSystemCall(native, name, (sqlite3_syscall_ptr)(hook));  \
  } while (0)

int main(int argc, char **argv) {
  if (argc != 9) {
    fprintf(stderr, "usage: record EXTENSION VFS DATABASE EVENTS PAYLOADS COMMITS AUTOCHECKPOINT MODE\n");
    return 2;
  }
  const char *vfs = argv[2], *database = argv[3];
  int commits = atoi(argv[6]), autocheckpoint = atoi(argv[7]), mode = atoi(argv[8]);
  char directory[PATH_MAX], resolved[PATH_MAX], name[PATH_MAX];
  snprintf(directory, sizeof(directory), "%s", database);
  snprintf(name, sizeof(name), "%s", database);
  if (!realpath(dirname(directory), resolved)) { perror("realpath"); return 1; }
  snprintf(prefix, sizeof(prefix), "%s/%s", resolved, basename(name));

  sqlite3 *loader = NULL;
  char *error = NULL;
  sqlite3_open(":memory:", &loader);
  sqlite3_enable_load_extension(loader, 1);
  if (sqlite3_load_extension(loader, argv[1], MVFS_ENTRY, &error)) {
    fprintf(stderr, "load: %s\n", error);
    return 1;
  }
  sqlite3_close(loader);

  sqlite3_vfs *native = sqlite3_vfs_find("unix");
  HOOK(real_open, "open", hook_open);
  HOOK(real_write, "write", hook_write);
  HOOK(real_pwrite, "pwrite", hook_pwrite);
  HOOK(real_pwrite64, "pwrite64", hook_pwrite64);
  HOOK(real_ftruncate, "ftruncate", hook_ftruncate);
  HOOK(real_unlink, "unlink", hook_unlink);
  HOOK(real_fcntl, "fcntl", hook_fcntl);
  events = fopen(argv[4], "w");
  payloads = fopen(argv[5], "wb");
  if (!events || !payloads) { perror("output"); return 1; }
  if (mode >= RESUME_EXCLUSIVE) resume(vfs, database, commits, mode == RESUME_EXCLUSIVE);

  int next = 1;
  for (int cycle = 0; cycle < EXTRA_CYCLES; cycle++) {
    sqlite3 *db = NULL;
    int code = sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs);
    if (code != SQLITE_OK) fail(db, "open", code);
    char setup[256];
    snprintf(setup, sizeof(setup), "PRAGMA page_size=4096; PRAGMA journal_mode=WAL;"
             "PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=%d;", autocheckpoint);
    sql(db, setup);
    /* Spill forces frame rewrites inside a transaction; the limit forces WAL trims. */
    if (mode != STANDARD) sql(db, "PRAGMA cache_size=6; PRAGMA journal_size_limit=30000;");
    if (cycle == 0) {
      sql(db, "BEGIN; CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);"
              "CREATE TABLE u(k INTEGER PRIMARY KEY, n INT, pad BLOB);"
              "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<60)"
              "INSERT INTO u SELECT x,0,randomblob(900) FROM c; COMMIT;");
      fprintf(events, "A 0\n");
      fflush(events);
    }
    sqlite3_stmt *insert = NULL, *update = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?1,printf('%01200d',?1))", -1, &insert, NULL);
    sqlite3_prepare_v2(db, "UPDATE u SET n=?1,pad=randomblob(900) WHERE k=?2", -1, &update, NULL);
    for (int commit = 0; commit < commits; commit++, next++) {
      sql(db, "BEGIN IMMEDIATE");
      for (int pass = 0; pass < (mode == HARD ? 3 : 1); pass++) {
        for (int row = 0; row < (mode == HARD ? 10 : 1); row++) {
          sqlite3_bind_int(update, 1, next);
          sqlite3_bind_int(update, 2, 1 + ((next + row) * 37) % UPDATED_ROWS);
          run(db, update, "update");
        }
      }
      sqlite3_bind_int(insert, 1, next);
      run(db, insert, "insert");
      sql(db, "COMMIT");
      fprintf(events, "A %d\n", next);
      fflush(events);
    }
    sqlite3_finalize(insert);
    sqlite3_finalize(update);
    /* Every page moves in one transaction, and the checkpoint after it shrinks main. */
    if (mode == VACUUM) {
      sql(db, "CREATE TABLE IF NOT EXISTS w(pad BLOB);"
              "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<40)"
              "INSERT INTO w SELECT randomblob(3000) FROM c;"
              "DELETE FROM w; VACUUM;");
    }
    int closed = sqlite3_close(db);
    if (closed != SQLITE_OK) fail(NULL, "close", closed);
  }
  fflush(events);
  fflush(payloads);
  _exit(0); /* Any later VFS teardown is not part of the workload. */
}
