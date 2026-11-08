#include "sqlite3.h"
#include "monstervfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int code, sqlite3 *db, const char *operation) {
  if (code != SQLITE_OK) {
    fprintf(stderr, "%s: %d %s\n", operation, code, db ? sqlite3_errmsg(db) : "");
    exit(1);
  }
}
static void sql(sqlite3 *db, const char *text) {
  char *error = NULL;
  int code = sqlite3_exec(db, text, NULL, NULL, &error);
  if (code != SQLITE_OK) {
    fprintf(stderr, "%s: %d %s\n", text, code, error ? error : sqlite3_errmsg(db));
    sqlite3_free(error);
    exit(1);
  }
}
int main(int argc, char **argv) {
#ifdef MVFS_STATIC_HOST
  if (argc != 2) { fprintf(stderr, "usage: abi-static DATABASE\n"); return 2; }
  const char *database = argv[1];
  check(monstervfs_register(), NULL, "static registration");
  check(monstervfs_register(), NULL, "repeat static registration");
#else
  if (argc != 3) { fprintf(stderr, "usage: abi-loadable EXTENSION DATABASE\n"); return 2; }
  const char *database = argv[2];
  sqlite3 *loader = NULL;
  check(sqlite3_open(":memory:", &loader), loader, "loader open");
  check(sqlite3_db_config(loader, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, NULL), loader, "enable load");
  char *error = NULL;
  int code = sqlite3_load_extension(loader, argv[1], NULL, &error);
  if (code != SQLITE_OK) { fprintf(stderr, "load: %s\n", error ? error : sqlite3_errmsg(loader)); return 1; }
  check(sqlite3_load_extension(loader, argv[1], NULL, &error), loader, "repeat load");
  check(sqlite3_close(loader), NULL, "loader close");
#endif
  if (!sqlite3_vfs_find(MVFS_NAME)) { fprintf(stderr, "missing registered VFS\n"); return 1; }
  sqlite3 *db = NULL;
  check(sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, MVFS_NAME), db, "protected open");
  sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
  sql(db, "CREATE TABLE IF NOT EXISTS smoke(id INTEGER PRIMARY KEY, value TEXT); INSERT INTO smoke(value) VALUES('same engine');");
  sqlite3_stmt *statement = NULL;
  check(sqlite3_prepare_v2(db, "SELECT count(*) FROM smoke WHERE value='same engine'", -1, &statement, NULL), db, "query prepare");
  if (sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_int(statement, 0) < 1) return 1;
  check(sqlite3_finalize(statement), db, "finalize");
  check(sqlite3_close(db), NULL, "protected close");
  check(sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE, MVFS_NAME), db, "protected reopen");
  sql(db, "PRAGMA synchronous=FULL;");
  check(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &statement, NULL), db, "integrity prepare");
  if (sqlite3_step(statement) != SQLITE_ROW ||
      strcmp((const char *)sqlite3_column_text(statement, 0), "ok")) return 1;
  if (sqlite3_step(statement) != SQLITE_DONE) return 1;
  check(sqlite3_finalize(statement), db, "integrity finalize");
  check(sqlite3_prepare_v2(db, "SELECT count(*) FROM smoke WHERE value='same engine'", -1, &statement, NULL), db, "reopen query prepare");
  if (sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_int(statement, 0) < 1) return 1;
  check(sqlite3_finalize(statement), db, "reopen finalize");
  check(sqlite3_close(db), NULL, "reopen close");
  check(sqlite3_shutdown(), NULL, "host SQLite shutdown");
  printf("ABI smoke passed with SQLite %s\n", SQLITE_VERSION);
  return 0;
}
