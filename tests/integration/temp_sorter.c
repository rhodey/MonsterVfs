/* Real SQLite disk spills: exact results, statement failures, and ownership. */
#define _POSIX_C_SOURCE 200809L
#include "temp_support.h"

#define ROWS 1300
#define PAYLOAD 4096

static const char *sort_query = "SELECT id,k,p FROM source ORDER BY k,id";
static const char *transient_query =
    "WITH materialized AS MATERIALIZED(SELECT id,k,p FROM source) "
    "SELECT id,k,p FROM materialized";
static sqlite3_int64 fail_sql_after_bytes;

static void payload(unsigned char *bytes, int id) {
  for (int j = 0; j < PAYLOAD; j++) bytes[j] = (unsigned char)(id * 17 + j * 31);
  memcpy(bytes, &id, sizeof(id));
}

static void check_row(sqlite3_stmt *query, int expected_key) {
  int id = sqlite3_column_int(query, 0);
  int key = sqlite3_column_int(query, 1);
  require(id >= 1 && id <= ROWS, "valid result id");
  require(key == (id * 733) % 1301, "key matches id");
  if (expected_key > 0) require(key == expected_key, "exact sorted key");
  require(sqlite3_column_bytes(query, 2) == PAYLOAD, "payload length");
  unsigned char expected[PAYLOAD];
  payload(expected, id);
  require(memcmp(expected, sqlite3_column_blob(query, 2), PAYLOAD) == 0,
          "exact result payload");
}

static void check_durable(sqlite3 *db) {
  sqlite3_stmt *query = NULL;
  ok(sqlite3_prepare_v2(db, "SELECT id,v FROM durable", -1, &query, NULL), db,
     "durable prepare");
  require(sqlite3_step(query) == SQLITE_ROW, "durable row exists");
  require(sqlite3_column_int(query, 0) == 1, "durable id unchanged");
  require(sqlite3_column_type(query, 1) == SQLITE_TEXT &&
              sqlite3_column_bytes(query, 1) == 9 &&
              memcmp(sqlite3_column_text(query, 1), "committed", 9) == 0,
          "durable value unchanged");
  require(sqlite3_step(query) == SQLITE_DONE, "exact durable row count");
  ok(sqlite3_finalize(query), db, "durable finalize");
  ok(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &query, NULL), db,
     "integrity prepare");
  require(sqlite3_step(query) == SQLITE_ROW &&
              strcmp((const char *)sqlite3_column_text(query, 0), "ok") == 0,
          "integrity check returns ok");
  require(sqlite3_step(query) == SQLITE_DONE, "one integrity result");
  ok(sqlite3_finalize(query), db, "integrity finalize");
}

static void populate(sqlite3 *db) {
  sql(db,
      "PRAGMA page_size=4096; PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"
      "PRAGMA temp_store=FILE; PRAGMA cache_size=8;"
      "PRAGMA temp.cache_size=2; PRAGMA threads=0;"
      "CREATE TABLE durable(id INTEGER PRIMARY KEY,v);"
      "INSERT INTO durable VALUES(1,'committed');"
      "CREATE TEMP TABLE source(id INTEGER PRIMARY KEY,k INTEGER,p BLOB);"
      "BEGIN;");
  sqlite3_stmt *insert = NULL;
  ok(sqlite3_prepare_v2(db, "INSERT INTO source VALUES(?,?,?)", -1, &insert,
                        NULL), db, "insert prepare");
  unsigned char bytes[PAYLOAD];
  for (int id = 1; id <= ROWS; id++) {
    payload(bytes, id);
    ok(sqlite3_bind_int(insert, 1, id), db, "bind id");
    ok(sqlite3_bind_int(insert, 2, (id * 733) % 1301), db, "bind key");
    ok(sqlite3_bind_blob(insert, 3, bytes, PAYLOAD, SQLITE_STATIC), db,
       "bind payload");
    require(sqlite3_step(insert) == SQLITE_DONE, "insert temporary row");
    ok(sqlite3_reset(insert), db, "insert reset");
  }
  ok(sqlite3_finalize(insert), db, "insert finalize");
  sql(db, "COMMIT;");
  require(temp_stats[0].opens > 0 && temp_stats[0].writes > 0,
          "TEMP_DB table actually reaches disk");
  temp_probe_dump("populated");
}

static void exact_spill(sqlite3 *db, int kind, const char *label) {
  TempStats before = temp_stats[kind];
  long physical_before = atomic_load(&native_temp_writes);
  const char *sql_text = kind == 1 ? sort_query : transient_query;
  sqlite3_stmt *query = NULL;
  ok(sqlite3_prepare_v2(db, sql_text, -1, &query, NULL), db, "spill prepare");
  unsigned char seen[ROWS + 1] = {0};
  int count = 0, rc;
  while ((rc = sqlite3_step(query)) == SQLITE_ROW) {
    check_row(query, kind == 1 ? ++count : 0);
    int id = sqlite3_column_int(query, 0);
    require(!seen[id], "no duplicate result ids");
    seen[id] = 1;
    if (kind != 1) count++;
  }
  require(rc == SQLITE_DONE && count == ROWS, "complete spill result");
  for (int id = 1; id <= ROWS; id++) require(seen[id], "all ids returned");
  ok(sqlite3_finalize(query), db, "spill finalize");
  require(temp_stats[kind].opens > before.opens &&
              temp_stats[kind].writes > before.writes &&
              temp_stats[kind].reads > before.reads,
          "statement actually reads and writes its spill file");
  require(temp_stats[kind].opens == temp_stats[kind].closes,
          "completed statement closes spill file");
  require(atomic_load(&native_temp_writes) > physical_before,
          "native writes reached unlinked temporary files");
  printf("%s exact_rows=%d physical_temp_writes=%ld\n", label, count,
         atomic_load(&native_temp_writes) - physical_before);
}

static void early_finalize(sqlite3 *db) {
  sqlite3_stmt *query = NULL;
  ok(sqlite3_prepare_v2(db, sort_query, -1, &query, NULL), db, "early prepare");
  require(sqlite3_step(query) == SQLITE_ROW, "early first row");
  check_row(query, 1);
  require(temp_stats[1].opens > temp_stats[1].closes,
          "sorter remains live before statement finalization");
  ok(sqlite3_finalize(query), db, "early finalize");
  require(temp_stats[1].opens == temp_stats[1].closes,
          "early finalization closes sorter");
}

static void sql_failure(sqlite3_context *context, int count, sqlite3_value **args) {
  (void)count;
  if (temp_stats[1].write_bytes > fail_sql_after_bytes) {
    sqlite3_result_error(context, "intentional sorter source failure", -1);
  } else {
    sqlite3_result_int(context, sqlite3_value_int(args[0]));
  }
}

static void sql_error_after_spill(sqlite3 *db) {
  long writes = temp_stats[1].writes;
  fail_sql_after_bytes = temp_stats[1].write_bytes;
  ok(sqlite3_create_function(db, "fail_after_spill", 1, SQLITE_UTF8, NULL,
                            sql_failure, NULL, NULL), db, "install function");
  sqlite3_stmt *query = NULL;
  ok(sqlite3_prepare_v2(db,
      "SELECT id,fail_after_spill(k),p FROM source ORDER BY 2,id", -1,
      &query, NULL), db, "SQL exception prepare");
  require(sqlite3_step(query) == SQLITE_ERROR, "SQL exception returned");
  require(strstr(sqlite3_errmsg(db), "intentional sorter source failure") != NULL,
          "original SQL error preserved");
  require(sqlite3_finalize(query) == SQLITE_ERROR, "SQL exception finalize");
  require(temp_stats[1].writes > writes, "SQL error happened after disk spill");
  require(temp_stats[1].opens == temp_stats[1].closes,
          "SQL exception closes sorter");
}

static void write_error_after_spill(sqlite3 *db, int kind) {
  int previous_failures = fail_count;
  long writes = temp_stats[kind].writes;
  fail_kind = kind;
  fail_after = 20;
  sqlite3_stmt *query = NULL;
  ok(sqlite3_prepare_v2(db, kind == 1 ? sort_query : transient_query, -1,
                       &query, NULL), db, "I/O exception prepare");
  int rc = sqlite3_step(query);
  require((rc & 255) == SQLITE_IOERR, "spill write failure reaches SQL caller");
  require(fail_count == previous_failures + 1 && fail_kind == -1,
          "spill write error injected exactly once");
  require(temp_stats[kind].writes >= writes + 21,
          "earlier spill writes succeeded before error");
  require((sqlite3_finalize(query) & 255) == SQLITE_IOERR,
          "I/O exception finalize");
  require(temp_stats[kind].opens == temp_stats[kind].closes,
          "I/O exception closes spill file");
  check_durable(db);
}

int main(int argc, char **argv) {
  require(argc == 5, "usage: sorter LIBRARY DATABASE VFS TMPDIR");
  setvbuf(stdout, NULL, _IOLBF, 0);
  require(setenv("SQLITE_TMPDIR", argv[4], 1) == 0, "temporary directory");
  load(argv[1]);
  ok(temp_probe_register(argv[3], "temp-probe"), NULL, "register probe");
  temp_physical_start();
  sqlite3 *db = NULL;
  ok(sqlite3_open_v2(argv[2], &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                     "temp-probe"), db, "database open");
  populate(db);
  exact_spill(db, 1, "healthy-sorter");
  early_finalize(db);
  sql_error_after_spill(db);
  write_error_after_spill(db, 1);
  exact_spill(db, 1, "sorter-after-io-failure");
  exact_spill(db, 2, "healthy-transient");
  write_error_after_spill(db, 2);
  exact_spill(db, 2, "transient-after-io-failure");
  check_durable(db);
  require(temp_stats[0].reads > 0, "TEMP_DB table read back from disk");
  ok(sqlite3_close(db), NULL, "database close");
  for (int i = 0; i < 4; i++) {
    require(temp_stats[i].opens == temp_stats[i].closes,
            "connection close releases all temporary handles");
  }
  empty_temp_dir(argv[4]);
  ok(sqlite3_open_v2(argv[2], &db, SQLITE_OPEN_READWRITE, "temp-probe"), db,
     "database reopen");
  check_durable(db);
  sqlite3_stmt *query = NULL;
  ok(sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_temp_master", -1,
                       &query, NULL), db, "temporary schema prepare");
  require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 0,
          "temporary tables do not survive reopen");
  require(sqlite3_step(query) == SQLITE_DONE, "temporary schema done");
  ok(sqlite3_finalize(query), db, "temporary schema finalize");
  ok(sqlite3_close(db), NULL, "reopened close");
  empty_temp_dir(argv[4]);
  temp_probe_dump("final");
  printf("physical_temp_writes=%ld physical_temp_bytes=%lld\n",
         atomic_load(&native_temp_writes), atomic_load(&native_temp_bytes));
  temp_support_finish();
  puts("PASS exact SQL temporary-table, sorter, transient, failure, and cleanup");
  return 0;
}
