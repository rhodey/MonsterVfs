#define _POSIX_C_SOURCE 200809L
#include "temp_support.h"
#include <stdint.h>

#define ROWS 192
#define PAYLOAD_BYTES 3000
#define SUBJOURNAL_KIND 3

static void payload(unsigned char *out, int id, int phase) {
  uint32_t state = (uint32_t)id * UINT32_C(747796405)
                 + (uint32_t)phase * UINT32_C(2891336453);
  for (int i = 0; i < PAYLOAD_BYTES; i++) {
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (unsigned char)(state >> 24);
  }
}

static sqlite3 *open_db(const char *path) {
  sqlite3 *db = NULL;
  ok(sqlite3_open_v2(path, &db,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
                    "temp-probe"), db, "database open");
  sqlite3_extended_result_codes(db, 1);
  sqlite3_busy_timeout(db, 5000);
  sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL;"
          "PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"
          "PRAGMA temp_store=FILE; PRAGMA cache_size=8; PRAGMA cache_spill=ON;");
  return db;
}

static int update(sqlite3 *db, int phase, int stride) {
  sqlite3_stmt *statement = NULL;
  ok(sqlite3_prepare_v2(db,
      "UPDATE t SET phase=?2,value=?3 WHERE id=?1", -1, &statement, NULL),
      db, "update prepare");
  unsigned char bytes[PAYLOAD_BYTES];
  int rc = SQLITE_DONE;
  for (int id = 1; id <= ROWS; id += stride) {
    payload(bytes, id, phase);
    ok(sqlite3_bind_int(statement, 1, id), db, "update bind id");
    ok(sqlite3_bind_int(statement, 2, phase), db, "update bind phase");
    ok(sqlite3_bind_blob(statement, 3, bytes, sizeof(bytes), SQLITE_TRANSIENT),
       db, "update bind payload");
    rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) break;
    require(sqlite3_changes(db) == 1, "exactly one updated row");
    ok(sqlite3_reset(statement), db, "update reset");
  }
  int finalize_rc = sqlite3_finalize(statement);
  return rc == SQLITE_DONE ? finalize_rc : rc;
}

static void verify(sqlite3 *db, int phase) {
  sqlite3_stmt *statement = NULL;
  ok(sqlite3_prepare_v2(db, "SELECT id,phase,value FROM t ORDER BY id",
                       -1, &statement, NULL), db, "verify prepare");
  unsigned char expected[PAYLOAD_BYTES];
  int row = 0, rc;
  while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
    int id = ++row;
    require(sqlite3_column_int(statement, 0) == id, "exact id sequence");
    require(sqlite3_column_int(statement, 1) == phase, "exact phase");
    require(sqlite3_column_bytes(statement, 2) == PAYLOAD_BYTES,
            "exact payload size");
    payload(expected, id, phase);
    require(memcmp(sqlite3_column_blob(statement, 2), expected,
                   sizeof(expected)) == 0, "exact payload bytes");
  }
  require(rc == SQLITE_DONE, "verify completed");
  require(row == ROWS, "exact row count");
  ok(sqlite3_finalize(statement), db, "verify finalize");
}

static void seed(sqlite3 *db) {
  sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY,"
          "phase INTEGER NOT NULL,value BLOB NOT NULL); BEGIN;");
  sqlite3_stmt *statement = NULL;
  ok(sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?1,0,?2)",
                       -1, &statement, NULL), db, "insert prepare");
  unsigned char bytes[PAYLOAD_BYTES];
  for (int id = 1; id <= ROWS; id++) {
    payload(bytes, id, 0);
    ok(sqlite3_bind_int(statement, 1, id), db, "seed id");
    ok(sqlite3_bind_blob(statement, 2, bytes, sizeof(bytes), SQLITE_TRANSIENT),
       db, "seed payload");
    require(sqlite3_step(statement) == SQLITE_DONE, "seed step");
    ok(sqlite3_reset(statement), db, "seed reset");
  }
  ok(sqlite3_finalize(statement), db, "seed finalize");
  sql(db, "COMMIT; PRAGMA wal_checkpoint(TRUNCATE);");
  verify(db, 0);
}

static void nested_rollback(sqlite3 *db) {
  sql(db, "BEGIN;");
  ok(update(db, 1, 1), db, "changes before savepoints");
  sql(db, "SAVEPOINT outer;");
  ok(update(db, 2, 1), db, "outer savepoint changes");
  sql(db, "SAVEPOINT inner;");
  ok(update(db, 3, 2), db, "inner savepoint changes");
  sql(db, "ROLLBACK TO inner; RELEASE inner;");
  verify(db, 2);
  sql(db, "ROLLBACK TO outer; RELEASE outer;");
  verify(db, 1);
  sql(db, "COMMIT;");
  verify(db, 1);

  TempStats *stats = &temp_stats[SUBJOURNAL_KIND];
  require(stats->opens > 0, "actual SUBJOURNAL file opened");
  require(stats->reads > 0, "savepoint rollback read SUBJOURNAL");
  require(stats->writes > 0, "savepoints wrote SUBJOURNAL");
  require(stats->opens == stats->closes, "committing closes SUBJOURNAL");
  require(atomic_load(&native_temp_writes) > 0,
          "actual OS writes to unlinked SUBJOURNAL file");
  int spills = 0, high = 0;
  ok(sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_SPILL, &spills, &high, 0),
     db, "cache spill status");
  require(spills > 0, "actual dirty cache spills");
  printf("healthy cache_spills=%d\n", spills);
  temp_probe_dump("healthy");
}

static void failed_subjournal_write(sqlite3 *db) {
  sql(db, "BEGIN;");
  ok(update(db, 4, 1), db, "uncommitted changes before failed savepoint");
  sql(db, "SAVEPOINT failed;");
  fail_kind = SUBJOURNAL_KIND;
  fail_after = 40;
  int rc = update(db, 5, 1);
  printf("fault update_rc=%d extended=%d autocommit=%d injected=%d\n",
         rc, sqlite3_extended_errcode(db), sqlite3_get_autocommit(db), fail_count);
  require(rc == SQLITE_IOERR_WRITE,
          "subjournal write failure returns SQLITE_IOERR_WRITE");
  require(fail_count == 1, "one SUBJOURNAL write failure injected");
  require(sqlite3_get_autocommit(db), "I/O error rolls back whole transaction");
  verify(db, 1);
  require(temp_stats[SUBJOURNAL_KIND].opens == temp_stats[SUBJOURNAL_KIND].closes,
          "failed transaction closes SUBJOURNAL");
  temp_probe_dump("fault");
}

int main(int argc, char **argv) {
  require(argc == 5, "usage: subjournal LIBRARY DATABASE VFS TMPDIR");
  setvbuf(stdout, NULL, _IOLBF, 0);
  require(setenv("SQLITE_TMPDIR", argv[4], 1) == 0, "temporary directory");
  load(argv[1]);
  ok(temp_probe_register(argv[3], "temp-probe"), NULL, "probe register");
  temp_physical_start();

  sqlite3 *db = open_db(argv[2]);
  seed(db);
  nested_rollback(db);
  ok(sqlite3_close(db), NULL, "healthy close");
  db = open_db(argv[2]);
  verify(db, 1);
  failed_subjournal_write(db);
  ok(sqlite3_close(db), NULL, "failed transaction close");

  db = open_db(argv[2]);
  verify(db, 1);
  sql(db, "BEGIN; SAVEPOINT retry;");
  ok(update(db, 6, 1), db, "healthy retry after I/O error");
  sql(db, "RELEASE retry; COMMIT;");
  verify(db, 6);
  ok(sqlite3_close(db), NULL, "retry close");
  db = open_db(argv[2]);
  verify(db, 6);
  ok(sqlite3_close(db), NULL, "persisted retry close");

  for (int i = 0; i < 4; i++)
    require(temp_stats[i].opens == temp_stats[i].closes,
            "all temporary handles closed");
  empty_temp_dir(argv[4]);
  temp_probe_dump("final");
  printf("physical_temp_writes=%ld physical_temp_bytes=%lld\n",
         atomic_load(&native_temp_writes), atomic_load(&native_temp_bytes));
  temp_support_finish();
  puts("PASS exact nested rollback, temp-write error rollback, reopen, healthy retry");
  return 0;
}
