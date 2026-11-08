/* Metadata faults are injected by the Python driver while no SQLite owner runs.
 * Every invocation loads a fresh VFS: no child inherits worker-thread state. */
#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include "monstervfs.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void require(int yes, const char *message) {
  if (!yes) { fprintf(stderr, "metadata: %s\n", message); exit(1); }
}
static void ok(int code, sqlite3 *db, const char *message) {
  if (code != SQLITE_OK) {
    fprintf(stderr, "metadata: %s: %d/%d %s\n", message, code,
            db ? sqlite3_extended_errcode(db) : code, db ? sqlite3_errmsg(db) : "");
    exit(1);
  }
}
static void sql(sqlite3 *db, const char *text) {
  ok(sqlite3_exec(db, text, NULL, NULL, NULL), db, text);
}
static void load(const char *library) {
  sqlite3 *loader = NULL;
  ok(sqlite3_open(":memory:", &loader), loader, "loader");
  ok(sqlite3_enable_load_extension(loader, 1), loader, "enable extension");
  char *error = NULL;
  int code = sqlite3_load_extension(loader, library, MVFS_ENTRY, &error);
  if (code != SQLITE_OK) { fprintf(stderr, "metadata: load: %s\n", error); exit(1); }
  ok(sqlite3_close(loader), NULL, "loader close");
}
static void exact_rows(sqlite3 *db, int count) {
  sqlite3_stmt *statement = NULL;
  ok(sqlite3_prepare_v2(db, "SELECT v FROM t ORDER BY v", -1, &statement, NULL), db, "rows prepare");
  for (int i = 1; i <= count; i++) {
    require(sqlite3_step(statement) == SQLITE_ROW, "committed row missing");
    require(sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
            sqlite3_column_int(statement, 0) == i, "committed row differs");
  }
  require(sqlite3_step(statement) == SQLITE_DONE, "unexpected row or read failure");
  ok(sqlite3_finalize(statement), db, "rows finalize");
  ok(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &statement, NULL), db, "integrity prepare");
  require(sqlite3_step(statement) == SQLITE_ROW &&
          !strcmp((const char *)sqlite3_column_text(statement, 0), "ok"), "integrity check");
  require(sqlite3_step(statement) == SQLITE_DONE, "extra integrity result");
  ok(sqlite3_finalize(statement), db, "integrity finalize");
}
static void snapshot(const char *path, const char *destination, size_t length) {
  unsigned char bytes[16384]; require(length <= sizeof(bytes), "snapshot length");
  int input = open(path, O_RDONLY), output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
  require(input >= 0 && output >= 0, "snapshot open");
  require(pread(input, bytes, length, 0) == (ssize_t)length, "snapshot read");
  require(write(output, bytes, length) == (ssize_t)length, "snapshot write");
  require(close(input) == 0 && close(output) == 0, "snapshot close");
}
static void checkpoint(sqlite3 *db) {
  int log, done;
  ok(sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_TRUNCATE, &log, &done), db, "checkpoint");
  require(log == 0 && done == 0, "checkpoint did not truncate WAL");
}
static void repaired_prefix(const char *path, const char *reference) {
  unsigned char wanted[8192], actual[8192];
  int source = open(reference, O_RDONLY), file = open(path, O_RDONLY);
  require(source >= 0 && file >= 0, "prefix repair open");
  require(pread(source, wanted, sizeof(wanted), 0) == sizeof(wanted), "reference prefix read");
  int repaired = 0;
  for (int i = 0; i < 500; i++) {
    require(pread(file, actual, sizeof(actual), 0) == sizeof(actual), "repair prefix read");
    if (!memcmp(actual, wanted, sizeof(actual))) { repaired = 1; break; }
    struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
  }
  require(repaired, "immutable prefix disagreement was not repaired");
  require(close(source) == 0 && close(file) == 0, "prefix repair close");
}
int main(int argc, char **argv) {
  if (argc < 5) return 2;
  const char *mode = argv[2], *path = argv[3]; int count = atoi(argv[4]);
  load(argv[1]);
  int readonly = !strcmp(mode, "read") || !strcmp(mode, "refuse-ro");
  int refusal = !strncmp(mode, "refuse-", 7);
  sqlite3 *db = NULL;
  int code = sqlite3_open_v2(path, &db, readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, MVFS_NAME);
  if (!refusal || code == SQLITE_OK) ok(code, db, "database open");
  if (code == SQLITE_OK && !readonly && !refusal)
    ok(sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL), db, "disable close checkpoint");
  if (refusal) {
    if (code == SQLITE_OK) {
      if (!strcmp(mode, "refuse-exclusive")) sql(db, "PRAGMA locking_mode=EXCLUSIVE");
      code = sqlite3_exec(db, "SELECT v FROM t", NULL, NULL, NULL);
    }
    require(code != SQLITE_OK, "conflicting metadata was accepted");
    require(sqlite3_extended_errcode(db) == SQLITE_CORRUPT, "conflict must report SQLITE_CORRUPT");
    printf("refused=%d\n", sqlite3_extended_errcode(db));
    ok(sqlite3_close(db), NULL, "refused database close");
    return 0;
  }
  if (!strcmp(mode, "prepare") || !strcmp(mode, "prepare-empty")) {
    sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
    sql(db, "CREATE TABLE t(v); INSERT INTO t VALUES(1)");
    char wal[4096], before[4096];
    snprintf(wal, sizeof(wal), "%s-wal", path); snprintf(before, sizeof(before), "%s-old-control", path);
    snapshot(wal, before, 16384); checkpoint(db);
    int reuse = argc > 5 ? atoi(argv[5]) : 0;
    for (int i = 2; i <= count; i++) {
      if (i > 2 && reuse > 0) { checkpoint(db); reuse--; }
      char statement[96]; snprintf(statement, sizeof(statement), "INSERT INTO t VALUES(%d)", i); sql(db, statement);
    }
    exact_rows(db, count);
    if (!strcmp(mode, "prepare-empty")) checkpoint(db);
    /* Preserve the successfully committed WAL without a close checkpoint. */
    _exit(0);
  }
  exact_rows(db, count);
  if (!strcmp(mode, "continue")) {
    sql(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
    char statement[96]; snprintf(statement, sizeof(statement), "INSERT INTO t VALUES(%d)", count + 1); sql(db, statement);
    checkpoint(db); exact_rows(db, count + 1);
  } else if (!strcmp(mode, "repair-prefix")) {
    require(argc == 6, "repair needs reference"); repaired_prefix(path, argv[5]); exact_rows(db, count);
  } else require(!strcmp(mode, "read"), "unknown mode");
  ok(sqlite3_close(db), NULL, "database close");
  return 0;
}
