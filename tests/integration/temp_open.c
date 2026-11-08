/* Failed temporary opens must release only the pathname they just created. */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* tools/test-faults links a host with these sync names. */
int mvfs_test_fsync(int fd) { return fsync(fd); }
int mvfs_test_fdatasync(int fd) {
#ifdef __APPLE__
  return fsync(fd);
#else
  return fdatasync(fd);
#endif
}

static const char sentinel[] = "preserve this file";
static const int kinds[] = {
  SQLITE_OPEN_TEMP_DB, SQLITE_OPEN_TEMP_JOURNAL,
  SQLITE_OPEN_TRANSIENT_DB, SQLITE_OPEN_SUBJOURNAL
};
static const int temporary_flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                               | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE;
static const char *directory;
static sqlite3_vfs *native;
static int (*ordinary_open)(const char *, int, int);
static int (*shutdown_vfs)(void);
static int fault, injections;
static char *replaced_path, *moved_path;
static struct stat original_identity;

enum { HEALTHY, FAIL_OPEN, REPLACE_FILE, REPLACE_SYMLINK };

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "temp-open: %s\n", message);
    exit(1);
  }
}

static void ok(int rc, const char *message) {
  if (rc != SQLITE_OK) {
    fprintf(stderr, "temp-open: %s: %d\n", message, rc);
    exit(1);
  }
}

static int entries(const char *path) {
  DIR *dir = opendir(path);
  require(dir != NULL, "open directory for cleanup check");
  int count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)))
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) count++;
  require(closedir(dir) == 0, "close directory");
  return count;
}

static void write_sentinel(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  require(fd >= 0, "create sentinel");
  require(write(fd, sentinel, sizeof(sentinel)) == sizeof(sentinel), "write sentinel");
  require(close(fd) == 0, "close sentinel");
}

static void verify_sentinel(const char *path) {
  char bytes[sizeof(sentinel) + 1];
  int fd = open(path, O_RDONLY);
  require(fd >= 0, "preserved file exists");
  require(read(fd, bytes, sizeof(bytes)) == sizeof(sentinel), "preserved file length");
  require(memcmp(bytes, sentinel, sizeof(sentinel)) == 0, "preserved file contents");
  require(close(fd) == 0, "close preserved file");
}

static int failing_open(const char *path, int flags, int mode) {
  size_t length = strlen(directory);
  /* Linux O_TMPFILE opens the directory itself before trying a named file. */
  if (fault != HEALTHY && strncmp(path, directory, length) == 0
      && (path[length] == '/' || path[length] == '\0')) {
    if (injections++ == 0 && (fault == REPLACE_FILE || fault == REPLACE_SYMLINK)) {
      require(lstat(path, &original_identity) == 0, "guard created file before native open");
      require(S_ISREG(original_identity.st_mode) && original_identity.st_size == 0,
              "guard-owned file is initially empty");
      replaced_path = sqlite3_mprintf("%s", path);
      moved_path = sqlite3_mprintf("%s.moved", path);
      require(replaced_path && moved_path, "replacement path allocation");
      require(rename(path, moved_path) == 0, "move guard-owned file");
      if (fault == REPLACE_FILE) write_sentinel(path);
      else require(symlink(moved_path, path) == 0, "replace path with symlink to owned inode");
    }
    errno = EMFILE;
    return -1;
  }
  return ordinary_open(path, flags, mode);
}

static int open_close(sqlite3_vfs *vfs, const char *name, int flags, int failure) {
  int before = entries("/dev/fd"), actual = 0;
  sqlite3_file *file = calloc(1, (size_t)vfs->szOsFile);
  require(file != NULL, "allocate VFS handle");
  fault = failure;
  injections = 0;
  int rc = vfs->xOpen(vfs, name, file, flags, &actual);
  fault = HEALTHY;
  if (rc == SQLITE_OK) {
    require(failure == HEALTHY, "injected native failure reached open");
    /* The Linux native O_TMPFILE fast path leaves actual flags unset.
     * Successful byte-for-byte I/O establishes writability on both VFSes. */
    unsigned char expected[] = {0, 1, 3, 7, 15, 31, 63, 127, 255};
    unsigned char got[sizeof(expected)] = {0};
    ok(file->pMethods->xWrite(file, expected, sizeof(expected), 0), "healthy write");
    ok(file->pMethods->xRead(file, got, sizeof(got), 0), "healthy read");
    require(memcmp(expected, got, sizeof(got)) == 0, "healthy temporary contents");
  }
  if (file->pMethods) ok(file->pMethods->xClose(file), "close installed methods");
  free(file);
  require(entries("/dev/fd") == before, "open/close releases all descriptors");
  if (failure != HEALTHY) require(injections > 0, "native open fault actually injected");
  return rc;
}

static void basic_opens(sqlite3_vfs *vfs) {
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    int flags = kinds[i] | temporary_flags;
    ok(open_close(vfs, NULL, flags, HEALTHY), "initial temporary open");
    require(entries(directory) == 0, "healthy open removes temporary pathname");
    int rc = open_close(vfs, NULL, flags, FAIL_OPEN);
    require(rc == SQLITE_CANTOPEN, "failed native open preserves CANTOPEN");
    require(entries(directory) == 0, "failed open removes its newly created temporary pathname");
    ok(open_close(vfs, NULL, flags, HEALTHY), "healthy retry after failed open");
    require(entries(directory) == 0, "retry removes temporary pathname");
    printf("%s kind=%x success/failure/retry and descriptor cleanup passed\n", vfs->zName, kinds[i]);
  }
}

static void preserved_paths(sqlite3_vfs *vfs) {
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    char *path = sqlite3_mprintf("%s/preexisting", directory);
    require(path != NULL, "sentinel path allocation");
    write_sentinel(path);
    require(open_close(vfs, path, kinds[i] | temporary_flags, FAIL_OPEN) == SQLITE_CANTOPEN,
            "preexisting failed open preserves CANTOPEN");
    verify_sentinel(path);
    require(unlink(path) == 0, "remove test-owned preexisting file");
    sqlite3_free(path);

    path = sqlite3_mprintf("%s/retained", directory);
    require(path != NULL, "retained path allocation");
    require(open_close(vfs, path, kinds[i] | (temporary_flags & ~SQLITE_OPEN_DELETEONCLOSE),
                       FAIL_OPEN) == SQLITE_CANTOPEN, "ordinary failed open preserves CANTOPEN");
    struct stat info;
    require(lstat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == 0,
            "new file without DELETEONCLOSE is retained");
    require(unlink(path) == 0, "remove test-owned retained file");
    sqlite3_free(path);

    for (int replacement = REPLACE_FILE; replacement <= REPLACE_SYMLINK; replacement++) {
      require(open_close(vfs, NULL, kinds[i] | temporary_flags, replacement) == SQLITE_CANTOPEN,
              "replaced-path failed open preserves CANTOPEN");
      require(lstat(moved_path, &info) == 0 && info.st_dev == original_identity.st_dev
                  && info.st_ino == original_identity.st_ino && info.st_size == 0,
              "relocated original file is unchanged");
      if (replacement == REPLACE_FILE) verify_sentinel(replaced_path);
      else {
        require(lstat(replaced_path, &info) == 0 && S_ISLNK(info.st_mode),
                "replacement symlink is preserved, even when its target is the owned inode");
        require(stat(replaced_path, &info) == 0 && info.st_dev == original_identity.st_dev
                    && info.st_ino == original_identity.st_ino,
                "replacement symlink target is unchanged");
      }
      require(unlink(replaced_path) == 0 && unlink(moved_path) == 0, "remove replacement fixtures");
      sqlite3_free(replaced_path); sqlite3_free(moved_path);
      replaced_path = moved_path = NULL;
    }
    require(entries(directory) == 0, "preservation fixtures cleaned up");
    printf("%s kind=%x preexisting/replaced/non-delete paths preserved\n", vfs->zName, kinds[i]);
  }
}

static void invalid_allocation(sqlite3_vfs *vfs) {
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    int saved = native->szOsFile;
    native->szOsFile = -1;
    int rc = open_close(vfs, NULL, kinds[i] | temporary_flags, HEALTHY);
    native->szOsFile = saved;
    require(rc == SQLITE_MISUSE, "invalid native allocation preserves MISUSE");
    require(entries(directory) == 0, "pre-native allocation error leaves no temporary pathname");
  }
  printf("%s pre-native allocation failures leave no files or descriptors\n", vfs->zName);
}

int main(int argc, char **argv) {
  require(argc == 4, "usage: temp-open LIBRARY DIRECTORY VFS");
  setvbuf(stdout, NULL, _IOLBF, 0);
  directory = argv[2];
  require(setenv("SQLITE_TMPDIR", directory, 1) == 0, "set isolated temporary directory");
  require(entries(directory) == 0, "temporary directory starts empty");
  sqlite3 *loader = NULL;
  ok(sqlite3_open(":memory:", &loader), "loader open");
  ok(sqlite3_db_config(loader, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, NULL), "enable extension");
  char *error = NULL;
  int rc = sqlite3_load_extension(loader, argv[1], MVFS_ENTRY, &error);
  if (rc != SQLITE_OK) { fprintf(stderr, "temp-open: load: %d %s\n", rc, error ? error : ""); return 1; }
  sqlite3_free(error);
  ok(sqlite3_close(loader), "loader close");
  void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  require(handle != NULL, "extension handle");
  shutdown_vfs = (int (*)(void))dlsym(handle, "monstervfs_shutdown");
  require(shutdown_vfs != NULL, "shutdown export");
  native = sqlite3_vfs_find("unix");
  sqlite3_vfs *vfs = sqlite3_vfs_find(argv[3]);
  require(native && vfs && native->xGetSystemCall && native->xSetSystemCall, "native open hooks");
  ordinary_open = (int (*)(const char *, int, int))native->xGetSystemCall(native, "open");
  require(ordinary_open != NULL, "native open syscall");
  ok(native->xSetSystemCall(native, "open", (sqlite3_syscall_ptr)failing_open), "install native open fault");
  basic_opens(vfs);
  if (vfs != native) {
    preserved_paths(vfs);
    invalid_allocation(vfs);
  }
  ok(native->xSetSystemCall(native, "open", (sqlite3_syscall_ptr)ordinary_open), "restore native open");
  ok(shutdown_vfs(), "maintenance shutdown");
  ok(sqlite3_shutdown(), "SQLite shutdown");
  puts("Temporary open ownership and failure cleanup passed.");
  return 0;
}
