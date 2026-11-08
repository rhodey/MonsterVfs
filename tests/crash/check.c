/* Reopens one crash image in a fresh process and reports what an application
 * would see. Prints a single line beginning OK or naming the failure. REOPEN is
 * shared, exclusive, or readonly, the three ways a connection reaches recovery. */
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void failed(const char *stage, int code, sqlite3 *db) {
  printf("FAIL %s rc=%d %s\n", stage, code, db ? sqlite3_errmsg(db) : "");
  exit(0);
}
/* A read-only connection cannot finish a creation or roll back its first transaction. */
static int needs_writer(int code, int readonly, int acknowledged) {
  int primary = code & 255;
  return readonly && acknowledged < 0 && (primary == SQLITE_NOTADB || primary == SQLITE_READONLY);
}
int main(int argc, char **argv) {
  if (argc != 7) {
    fprintf(stderr, "usage: check EXTENSION VFS DATABASE ACKNOWLEDGED BUSY_MS REOPEN\n");
    return 2;
  }
  const char *vfs = argv[2], *database = argv[3];
  int acknowledged = atoi(argv[4]), busy_ms = atoi(argv[5]);
  int readonly = strcmp(argv[6], "readonly") == 0, exclusive = strcmp(argv[6], "exclusive") == 0;
  sqlite3 *loader = NULL;
  char *error = NULL;
  sqlite3_open(":memory:", &loader);
  sqlite3_enable_load_extension(loader, 1);
  if (sqlite3_load_extension(loader, argv[1], MVFS_ENTRY, &error)) {
    printf("FAIL load %s\n", error);
    return 0;
  }
  sqlite3_close(loader);

  sqlite3 *db = NULL;
  int code = sqlite3_open_v2(database, &db, readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, vfs);
  if (code != SQLITE_OK) failed("open", code, db);
  sqlite3_busy_timeout(db, busy_ms);
  if (exclusive) {
    code = sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE", NULL, NULL, NULL);
    if (code != SQLITE_OK) failed("exclusive", code, db);
  }
  sqlite3_stmt *query = NULL;
  code = sqlite3_prepare_v2(db, "SELECT count(*),coalesce(max(id),0),"
                            "coalesce(sum(v=printf('%01200d',id)),0) FROM t", -1, &query, NULL);
  if (needs_writer(code, readonly, acknowledged)) { puts("OK setup needs a writer"); return 0; }
  if (code != SQLITE_OK) {
    if (!strstr(sqlite3_errmsg(db), "no such table")) failed("prepare", code, db);
    if (acknowledged < 0) { puts("OK no schema yet"); return 0; }
    puts("LOST schema");
    return 0;
  }
  code = sqlite3_step(query);
  if (needs_writer(code, readonly, acknowledged)) { puts("OK setup needs a writer"); return 0; }
  if (code != SQLITE_ROW) failed("select", code, db);
  int rows = sqlite3_column_int(query, 0), highest = sqlite3_column_int(query, 1);
  int matching = sqlite3_column_int(query, 2);
  sqlite3_finalize(query);
  if (rows != highest || matching != rows) {
    printf("WRONGDATA rows=%d highest=%d matching=%d\n", rows, highest, matching);
    return 0;
  }
  if (highest < acknowledged) {
    printf("LOST highest=%d acknowledged=%d\n", highest, acknowledged);
    return 0;
  }
  code = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &query, NULL);
  if (code != SQLITE_OK) failed("integrity prepare", code, db);
  code = sqlite3_step(query);
  if (code != SQLITE_ROW) failed("integrity", code, db);
  if (strcmp((const char *)sqlite3_column_text(query, 0), "ok") != 0) {
    printf("CORRUPT %s\n", sqlite3_column_text(query, 0));
    return 0;
  }
  sqlite3_finalize(query);
  if (!readonly) {
    code = sqlite3_exec(db, "INSERT INTO t VALUES(1000000,'x'); DELETE FROM t WHERE id=1000000;"
                        "PRAGMA wal_checkpoint(PASSIVE);", NULL, NULL, NULL);
    if (code != SQLITE_OK) failed("write after recovery", code, db);
  }
  /* An ordinary crash must never report a finished commit as lost. */
  if (strcmp(vfs, MVFS_NAME) == 0) {
    code = sqlite3_prepare_v2(db, "PRAGMA monstervfs_maintenance", -1, &query, NULL);
    if (code != SQLITE_OK || sqlite3_step(query) != SQLITE_ROW) failed("status", code, db);
    const char *field = strstr((const char *)sqlite3_column_text(query, 0), "\"wal_cutoff_commits\":");
    if (field == NULL || atoll(field + 21) != 0) {
      printf("FALSEALARM %s\n", field ? field : "no status");
      return 0;
    }
    sqlite3_finalize(query);
  }
  printf("OK highest=%d\n", highest);
  return 0;
}
