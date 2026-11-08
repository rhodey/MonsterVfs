/* A database that crosses SQLite's lock page, which it never uses and so leaves as a
 * hole. Run on both VFSes by tools/test-large, which compares what they report. Quick
 * runs move the lock page down with SQLite's own test control. */
#include "sqlite3.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096
#define SLOT 8192
#define MAIN_START 8192

static const char *vfs;

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "large: %s\n", message); exit(1); }
}
static void sql(sqlite3 *db, const char *text) {
  char *error = NULL;
  int code = sqlite3_exec(db, text, NULL, NULL, &error);
  if (code != SQLITE_OK) {
    fprintf(stderr, "large: %.80s: %d %s\n", text, sqlite3_extended_errcode(db), error ? error : "");
    exit(1);
  }
}
static sqlite3_int64 scalar(sqlite3 *db, const char *text) {
  sqlite3_stmt *statement = NULL;
  sqlite3_int64 value = -1;
  require(sqlite3_prepare_v2(db, text, -1, &statement, NULL) == SQLITE_OK, "prepare failed");
  if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
  int code = sqlite3_finalize(statement);
  if (code != SQLITE_OK) {
    fprintf(stderr, "large: %.80s: %d %s\n", text, sqlite3_extended_errcode(db), sqlite3_errmsg(db));
    exit(1);
  }
  return value;
}
static sqlite3 *open_database(const char *path, int flags) {
  sqlite3 *db = NULL;
  require(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | flags, vfs) == SQLITE_OK, "open failed");
  sqlite3_busy_timeout(db, 5000);
  return db;
}
static sqlite3_int64 status_field(sqlite3 *db, const char *name) {
  sqlite3_stmt *statement = NULL;
  char key[128];
  sqlite3_int64 value = 0;
  require(sqlite3_prepare_v2(db, "PRAGMA monstervfs_maintenance", -1, &statement, NULL) == SQLITE_OK, "status");
  if (sqlite3_step(statement) == SQLITE_ROW) {
    const char *text = (const char *)sqlite3_column_text(statement, 0);
    snprintf(key, sizeof(key), "\"%s\":", name);
    const char *found = text ? strstr(text, key) : NULL;
    if (found) value = atoll(found + strlen(key));
  }
  sqlite3_finalize(statement);
  return value;
}
static void rows_intact(sqlite3 *db, sqlite3_int64 expected) {
  require(scalar(db, "SELECT count(*) FROM t") == expected, "rows are missing");
  require(scalar(db, "SELECT sum(v=printf('%04000d',id)) FROM t") == expected, "rows changed");
  require(scalar(db, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check='ok'") == 1,
          "integrity check failed");
}
/* The result of reading each page around the hole the way a scrub does. */
static void scrub(sqlite3 *db, sqlite3_int64 pages, sqlite3_int64 lock_page) {
  sqlite3_stmt *statement = NULL;
  require(sqlite3_prepare_v2(db, "SELECT length(data) FROM sqlite_dbpage WHERE schema='main' AND pgno=?1",
                             -1, &statement, NULL) == SQLITE_OK, "scrub prepare");
  sqlite3_int64 failed = 0, at_lock = 0;
  for (sqlite3_int64 page = 1; page <= pages; page++) {
    sqlite3_bind_int64(statement, 1, page);
    int step = sqlite3_step(statement);
    int length = step == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    int code = sqlite3_reset(statement);
    if (page == lock_page) { at_lock = code; printf("lock page: step=%d length=%d code=%d\n", step, length, code); }
    else if (code != SQLITE_OK || length != PAGE) failed++;
  }
  sqlite3_finalize(statement);
  printf("scrub failures away from the lock page: %lld, at it: %s\n", failed, at_lock ? "error" : "none");
  require(failed == 0, "scrub failed away from the lock page");
}
static void flip_page_sector(const char *path, sqlite3_int64 page) {
  off_t at = (off_t)MAIN_START + (page - 1) * SLOT + SLOT - PAGE + 200;
  unsigned char bytes[80];
  int fd = open(path, O_RDWR);
  require(fd >= 0 && pread(fd, bytes, sizeof(bytes), at) == (ssize_t)sizeof(bytes), "physical read");
  /* Eight two-byte symbols is what 32 parity bytes can repair. */
  for (int i = 0; i < 8; i++) bytes[i * 4] ^= 0x5a;
  require(pwrite(fd, bytes, sizeof(bytes), at) == (ssize_t)sizeof(bytes), "physical write");
  fsync(fd);
  close(fd);
}
int main(int argc, char **argv) {
  if (argc != 6) { fprintf(stderr, "usage: large EXTENSION VFS DATABASE LOCK_BYTE PAGES\n"); return 2; }
  vfs = argv[2];
  const char *path = argv[3];
  long long lock_byte = atoll(argv[4]), target = atoll(argv[5]);
  if (lock_byte != 0x40000000) sqlite3_test_control(SQLITE_TESTCTRL_PENDING_BYTE, (unsigned)lock_byte);
  sqlite3 *loader = NULL;
  char *error = NULL;
  require(sqlite3_open(":memory:", &loader) == SQLITE_OK, "loader");
  sqlite3_enable_load_extension(loader, 1);
  if (sqlite3_load_extension(loader, argv[1], MVFS_ENTRY, &error)) {
    fprintf(stderr, "load: %s\n", error);
    return 1;
  }
  sqlite3_close(loader);
  sqlite3_int64 lock_page = lock_byte / PAGE + 1;

  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"
          "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);");
  sqlite3_int64 rows = 0;
  while (scalar(db, "PRAGMA page_count") < target) {
    char text[300];
    snprintf(text, sizeof(text), "WITH RECURSIVE c(x) AS(VALUES(%lld) UNION ALL SELECT x+1 FROM c WHERE x<%lld)"
             "INSERT INTO t SELECT x,printf('%%04000d',x) FROM c; PRAGMA wal_checkpoint(TRUNCATE);",
             rows + 1, rows + 8192);
    sql(db, text);
    rows += 8192;
  }
  sqlite3_int64 pages = scalar(db, "PRAGMA page_count");
  require(pages > lock_page + 1, "the database did not cross the lock page");
  struct stat info;
  require(stat(path, &info) == 0, "stat failed");
  printf("rows=%lld pages=%lld lock_page=%lld physical=%lld\n", rows, pages, lock_page, (long long)info.st_size);
  rows_intact(db, rows);
  scrub(db, pages, lock_page);

  /* A fresh connection reads every page through the VFS, not from SQLite's cache. */
  sqlite3 *second = open_database(path, 0);
  rows_intact(second, rows);
  sqlite3_close(second);

  if (strcmp(vfs, MVFS_NAME) == 0) {
    sqlite3_int64 holes = status_field(db, "zero_slot_reads");
    printf("zero_slot_reads=%lld\n", holes);
    /* Damage above the hole: the slot arithmetic must land on the same page again. */
    sqlite3_int64 victim = pages - 3;
    flip_page_sector(path, victim);
    sql(db, "PRAGMA shrink_memory");
    second = open_database(path, 0);
    rows_intact(second, rows);
    sqlite3_close(second);
    for (int i = 0; i < 1000 && status_field(db, "committed_replacements") < 1; i++) usleep(10000);
    require(status_field(db, "committed_replacements") >= 1, "damage above the hole was not repaired");
    sql(db, "PRAGMA wal_checkpoint(TRUNCATE)");
    require(status_field(db, "main_damage") == 0, "checkpoint did not retire the damage");
  }

  char text[200];
  snprintf(text, sizeof(text), "DELETE FROM t WHERE id%%2=0 AND id<%lld; PRAGMA wal_checkpoint(TRUNCATE);", rows / 2);
  sql(db, text);
  sqlite3_int64 kept = scalar(db, "SELECT count(*) FROM t");
  sql(db, "VACUUM; PRAGMA wal_checkpoint(TRUNCATE);");
  rows_intact(db, kept);
  sqlite3_int64 after = scalar(db, "PRAGMA page_count");
  printf("after vacuum: rows=%lld pages=%lld\n", kept, after);
  require(after < pages, "VACUUM did not shrink the database");
  if (after > lock_page) scrub(db, after, lock_page);
  sqlite3_close(db);
  puts("large database passed");
  return 0;
}
