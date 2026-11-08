#ifndef TEMP_SUPPORT_H
#define TEMP_SUPPORT_H

#include "sqlite3.h"
#include <dirent.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void ok(int rc, sqlite3 *db, const char *message) {
  if (rc != SQLITE_OK) {
    fprintf(stderr, "FAIL: %s: %d %s\n", message, rc,
            db ? sqlite3_errmsg(db) : "");
    exit(1);
  }
}

static void sql(sqlite3 *db, const char *statement) {
  char *error = NULL;
  int rc = sqlite3_exec(db, statement, NULL, NULL, &error);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "FAIL SQL: %s: %d %s\n", statement, rc,
            error ? error : "");
    exit(1);
  }
  sqlite3_free(error);
}

static int (*stop_vfs)(void);

static void load(const char *library) {
  if (strcmp(library, "-") == 0) return;
  sqlite3 *db = NULL;
  ok(sqlite3_open(":memory:", &db), db, "loader open");
  ok(sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, NULL),
     db, "enable extension load");
  char *error = NULL;
  int rc = sqlite3_load_extension(db, library, MVFS_ENTRY, &error);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "FAIL load: %d %s\n", rc, error ? error : "");
    exit(1);
  }
  sqlite3_free(error);
  ok(sqlite3_close(db), NULL, "loader close");
  void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
  require(handle != NULL, "extension dlopen");
  stop_vfs = (int (*)(void))dlsym(handle, "monstervfs_shutdown");
  require(stop_vfs != NULL, "shutdown export");
}

/* Observe only temporary handles. Persistent files go directly to the parent
 * so SQLite's main-file object, WAL ownership, and file controls stay intact. */
typedef struct {
  long opens, closes, reads, writes;
  sqlite3_int64 read_bytes, write_bytes;
} TempStats;

typedef struct {
  sqlite3_file base;
  sqlite3_file *real;
  int kind;
} TempProbeFile;

static TempStats temp_stats[4];
static const int temp_flags[4] = {
  SQLITE_OPEN_TEMP_DB, SQLITE_OPEN_TEMP_JOURNAL,
  SQLITE_OPEN_TRANSIENT_DB, SQLITE_OPEN_SUBJOURNAL
};
static const char *temp_names[4] = {
  "TEMP_DB", "TEMP_JOURNAL", "TRANSIENT_DB", "SUBJOURNAL"
};
static sqlite3_vfs temp_vfs;
static sqlite3_vfs *temp_parent;
/* Fail one logical temporary write after fail_after successful attempts. */
static int fail_kind = -1, fail_after, fail_count;

static int temp_close(sqlite3_file *file) {
  TempProbeFile *temp = (void *)file;
  int rc = temp->real->pMethods->xClose(temp->real);
  temp_stats[temp->kind].closes++;
  sqlite3_free(temp->real);
  temp->base.pMethods = NULL;
  return rc;
}

static int temp_read(sqlite3_file *file, void *bytes, int size, sqlite3_int64 offset) {
  TempProbeFile *temp = (void *)file;
  temp_stats[temp->kind].reads++;
  temp_stats[temp->kind].read_bytes += size;
  return temp->real->pMethods->xRead(temp->real, bytes, size, offset);
}

static int temp_write(sqlite3_file *file, const void *bytes, int size, sqlite3_int64 offset) {
  TempProbeFile *temp = (void *)file;
  temp_stats[temp->kind].writes++;
  temp_stats[temp->kind].write_bytes += size;
  if (fail_kind == temp->kind && fail_after-- == 0) {
    fail_kind = -1;
    fail_count++;
    return SQLITE_IOERR_WRITE;
  }
  return temp->real->pMethods->xWrite(temp->real, bytes, size, offset);
}

static int temp_truncate(sqlite3_file *file, sqlite3_int64 size) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xTruncate(temp->real, size);
}
static int temp_sync(sqlite3_file *file, int flags) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xSync(temp->real, flags);
}
static int temp_size(sqlite3_file *file, sqlite3_int64 *size) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xFileSize(temp->real, size);
}
static int temp_lock(sqlite3_file *file, int level) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xLock(temp->real, level);
}
static int temp_unlock(sqlite3_file *file, int level) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xUnlock(temp->real, level);
}
static int temp_reserved(sqlite3_file *file, int *held) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xCheckReservedLock(temp->real, held);
}
static int temp_control(sqlite3_file *file, int operation, void *argument) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xFileControl(temp->real, operation, argument);
}
static int temp_sector(sqlite3_file *file) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xSectorSize(temp->real);
}
static int temp_characteristics(sqlite3_file *file) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xDeviceCharacteristics(temp->real);
}
static int temp_shm_map(sqlite3_file *file, int page, int size, int extend,
                        void volatile **out) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xShmMap(temp->real, page, size, extend, out);
}
static int temp_shm_lock(sqlite3_file *file, int offset, int count, int flags) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xShmLock(temp->real, offset, count, flags);
}
static void temp_shm_barrier(sqlite3_file *file) {
  TempProbeFile *temp = (void *)file;
  temp->real->pMethods->xShmBarrier(temp->real);
}
static int temp_shm_unmap(sqlite3_file *file, int delete_flag) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xShmUnmap(temp->real, delete_flag);
}
static int temp_fetch(sqlite3_file *file, sqlite3_int64 offset, int size, void **out) {
  TempProbeFile *temp = (void *)file;
  if (temp->real->pMethods->xFetch)
    return temp->real->pMethods->xFetch(temp->real, offset, size, out);
  *out = NULL;
  return SQLITE_OK;
}
static int temp_unfetch(sqlite3_file *file, sqlite3_int64 offset, void *bytes) {
  TempProbeFile *temp = (void *)file;
  return temp->real->pMethods->xUnfetch
    ? temp->real->pMethods->xUnfetch(temp->real, offset, bytes) : SQLITE_OK;
}

static const sqlite3_io_methods temp_methods = {
  3, temp_close, temp_read, temp_write, temp_truncate, temp_sync, temp_size,
  temp_lock, temp_unlock, temp_reserved, temp_control, temp_sector,
  temp_characteristics, temp_shm_map, temp_shm_lock, temp_shm_barrier,
  temp_shm_unmap, temp_fetch, temp_unfetch
};

static int temp_open(sqlite3_vfs *vfs, const char *path, sqlite3_file *file,
                     int flags, int *actual) {
  (void)vfs;
  int kind = -1;
  for (int i = 0; i < 4; i++) {
    if (flags & temp_flags[i]) kind = i;
  }
  if (kind < 0)
    return temp_parent->xOpen(temp_parent, path, file, flags, actual);
  TempProbeFile *temp = (void *)file;
  memset(temp, 0, sizeof(*temp));
  temp->real = sqlite3_malloc(temp_parent->szOsFile);
  if (!temp->real) return SQLITE_NOMEM;
  memset(temp->real, 0, temp_parent->szOsFile);
  int rc = temp_parent->xOpen(temp_parent, path, temp->real, flags, actual);
  if (rc != SQLITE_OK) {
    if (temp->real->pMethods) temp->real->pMethods->xClose(temp->real);
    sqlite3_free(temp->real);
    return rc;
  }
  temp->kind = kind;
  temp->base.pMethods = &temp_methods;
  temp_stats[kind].opens++;
  return SQLITE_OK;
}

static int temp_probe_register(const char *parent, const char *name) {
  temp_parent = sqlite3_vfs_find(parent);
  if (!temp_parent) return SQLITE_ERROR;
  temp_vfs = *temp_parent;
  temp_vfs.zName = name;
  temp_vfs.pNext = NULL;
  temp_vfs.xOpen = temp_open;
  if (temp_vfs.szOsFile < (int)sizeof(TempProbeFile))
    temp_vfs.szOsFile = sizeof(TempProbeFile);
  return sqlite3_vfs_register(&temp_vfs, 0);
}

static void temp_probe_dump(const char *label) {
  for (int i = 0; i < 4; i++) {
    TempStats *s = &temp_stats[i];
    printf("%s %s opens=%ld closes=%ld reads=%ld writes=%ld read_bytes=%lld write_bytes=%lld\n",
           label, temp_names[i], s->opens, s->closes, s->reads, s->writes,
           s->read_bytes, s->write_bytes);
  }
}

/* Native writes to unlinked files prove that SQLite spilled beyond its own
 * memory journal. The hooks leave the actual filesystem operation unchanged. */
static sqlite3_vfs *native_vfs;
static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static ssize_t (*real_pwrite64)(int, const void *, size_t, off_t);
static ssize_t (*real_write)(int, const void *, size_t);
static _Atomic long native_temp_writes;
static _Atomic long long native_temp_bytes;

static void count_physical_write(int fd, ssize_t size) {
  struct stat info;
  if (size > 0 && fstat(fd, &info) == 0 && info.st_nlink == 0) {
    atomic_fetch_add(&native_temp_writes, 1);
    atomic_fetch_add(&native_temp_bytes, size);
  }
}
static ssize_t counted_pwrite(int fd, const void *bytes, size_t size, off_t offset) {
  ssize_t written = real_pwrite(fd, bytes, size, offset);
  count_physical_write(fd, written);
  return written;
}
static ssize_t counted_pwrite64(int fd, const void *bytes, size_t size, off_t offset) {
  ssize_t written = real_pwrite64(fd, bytes, size, offset);
  count_physical_write(fd, written);
  return written;
}
static ssize_t counted_write(int fd, const void *bytes, size_t size) {
  ssize_t written = real_write(fd, bytes, size);
  count_physical_write(fd, written);
  return written;
}

static void temp_physical_start(void) {
  native_vfs = sqlite3_vfs_find("unix");
  require(native_vfs && native_vfs->xGetSystemCall && native_vfs->xSetSystemCall,
          "native syscall hooks");
  real_pwrite = (void *)native_vfs->xGetSystemCall(native_vfs, "pwrite");
  real_pwrite64 = (void *)native_vfs->xGetSystemCall(native_vfs, "pwrite64");
  real_write = (void *)native_vfs->xGetSystemCall(native_vfs, "write");
  if (real_pwrite)
    ok(native_vfs->xSetSystemCall(native_vfs, "pwrite", (sqlite3_syscall_ptr)counted_pwrite),
       NULL, "hook pwrite");
  if (real_pwrite64)
    ok(native_vfs->xSetSystemCall(native_vfs, "pwrite64", (sqlite3_syscall_ptr)counted_pwrite64),
       NULL, "hook pwrite64");
  if (real_write)
    ok(native_vfs->xSetSystemCall(native_vfs, "write", (sqlite3_syscall_ptr)counted_write),
       NULL, "hook write");
}

static void empty_temp_dir(const char *path) {
  DIR *directory = opendir(path);
  require(directory != NULL, "open temporary directory");
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    require(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0,
            "temporary pathname cleanup");
  }
  closedir(directory);
}

static void temp_support_finish(void) {
  if (native_vfs) {
    if (real_pwrite)
      ok(native_vfs->xSetSystemCall(native_vfs, "pwrite", (sqlite3_syscall_ptr)real_pwrite),
         NULL, "restore pwrite");
    if (real_pwrite64)
      ok(native_vfs->xSetSystemCall(native_vfs, "pwrite64", (sqlite3_syscall_ptr)real_pwrite64),
         NULL, "restore pwrite64");
    if (real_write)
      ok(native_vfs->xSetSystemCall(native_vfs, "write", (sqlite3_syscall_ptr)real_write),
         NULL, "restore write");
  }
  if (stop_vfs) ok(stop_vfs(), NULL, "maintenance shutdown");
  ok(sqlite3_vfs_unregister(&temp_vfs), NULL, "unregister temporary observer");
  ok(sqlite3_shutdown(), NULL, "SQLite shutdown");
}

#endif
