/* Both builds in one process. Each works, and each refuses the other's files, by name
 * rather than as damage, and without changing them. */
#include "sqlite3.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *parity = "monstervfs_parity", *copy = "monstervfs_copy";
static int named;

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "builds: %s\n", message); exit(1); }
}
static void on_log(void *unused, int code, const char *message) {
  (void)unused; (void)code;
  named += strstr(message, "the other MonsterVfs build") != NULL;
}
static void load(const char *library) {
  sqlite3 *loader = NULL;
  char *error = NULL;
  require(sqlite3_open(":memory:", &loader) == SQLITE_OK, "loader open failed");
  sqlite3_enable_load_extension(loader, 1);
  if (sqlite3_load_extension(loader, library, NULL, &error)) { fprintf(stderr, "load: %s\n", error); exit(1); }
  sqlite3_close(loader);
}
static void make(const char *path, const char *vfs) {
  sqlite3 *db = NULL;
  require(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs) == SQLITE_OK, "create failed");
  require(sqlite3_exec(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
                           "CREATE TABLE t(v); INSERT INTO t VALUES(1); INSERT INTO t VALUES(2);",
                       NULL, NULL, NULL) == SQLITE_OK, "setup failed");
  require(sqlite3_close(db) == SQLITE_OK, "close failed");
}
static long long fingerprint(const char *path) {
  unsigned char buffer[65536];
  long long sum = 0;
  int fd = open(path, O_RDONLY);
  require(fd >= 0, "fingerprint open failed");
  for (ssize_t n; (n = read(fd, buffer, sizeof(buffer))) > 0;) {
    for (ssize_t i = 0; i < n; i++) sum = sum * 31 + buffer[i];
  }
  close(fd);
  return sum;
}
static void refuses(const char *path, const char *vfs) {
  long long before = fingerprint(path);
  int logged = named;
  sqlite3 *db = NULL;
  int code = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, vfs);
  if (code == SQLITE_OK) code = sqlite3_exec(db, "SELECT count(*) FROM t", NULL, NULL, NULL);
  sqlite3_close(db);
  require(code != SQLITE_OK, "a build opened the other build's file");
  require(named > logged, "a refusal did not say why");
  require(fingerprint(path) == before, "a refusal changed the other build's file");
}
static long long rows(const char *path, const char *vfs) {
  sqlite3 *db = NULL;
  sqlite3_stmt *statement = NULL;
  require(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, vfs) == SQLITE_OK, "reopen failed");
  require(sqlite3_prepare_v2(db, "SELECT count(*) FROM t", -1, &statement, NULL) == SQLITE_OK, "prepare failed");
  require(sqlite3_step(statement) == SQLITE_ROW, "read failed");
  long long count = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(db);
  return count;
}
/* The size a new file has before anything is in it: the one a crash could also leave. */
static void empty_file(const char *path, const char *vfs) {
  sqlite3 *db = NULL;
  require(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs) == SQLITE_OK, "empty open failed");
  require(sqlite3_close(db) == SQLITE_OK, "empty close failed");
  struct stat info;
  require(stat(path, &info) == 0 && info.st_size == 8192, "an empty file is not just its prefix");
}

int main(int argc, char **argv) {
  if (argc != 4) { fprintf(stderr, "usage: builds PARITY-LIBRARY COPY-LIBRARY DIRECTORY\n"); return 2; }
  require(sqlite3_config(SQLITE_CONFIG_LOG, on_log, NULL) == SQLITE_OK, "log hook failed");
  load(argv[1]);
  load(argv[2]);
  require(sqlite3_vfs_find("monstervfs") == NULL, "the old name is still registered");
  char first[4096], second[4096], third[4096], fourth[4096];
  snprintf(first, sizeof(first), "%s/parity", argv[3]);
  snprintf(second, sizeof(second), "%s/copy", argv[3]);
  snprintf(third, sizeof(third), "%s/parity-empty", argv[3]);
  snprintf(fourth, sizeof(fourth), "%s/copy-empty", argv[3]);
  make(first, parity);
  make(second, copy);
  refuses(first, copy);
  refuses(second, parity);
  require(rows(first, parity) == 2 && rows(second, copy) == 2, "a refusal cost a build its own file");
  empty_file(third, parity);
  empty_file(fourth, copy);
  refuses(third, copy);
  refuses(fourth, parity);
  require(sqlite3_shutdown() == SQLITE_OK, "host shutdown failed");
  puts("Each build works beside the other and refuses its files.");
  return 0;
}
