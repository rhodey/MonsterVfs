/* One step of tools/test-soak. "prepare" records acknowledged writes on stdout
 * and leaves a WAL with frames on both sides of the backfill watermark. "check"
 * emits raw observations for the independent Python oracle, without recreating
 * expected values or trusting the damaged database's ledger. */
#include "sqlite3.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROWS 300
#define HELD 20
#define BACKFILLED 30
#define COMMITS 60
#define ROW_BYTES 1500

static const char *vfs;

static void die(sqlite3 *db, const char *what) {
  fprintf(stderr, "soak: %s: %s\n", what, db ? sqlite3_errmsg(db) : "");
  exit(2);
}
static void sql(sqlite3 *db, const char *text) {
  if (sqlite3_exec(db, text, NULL, NULL, NULL) != SQLITE_OK) die(db, text);
}
static void flush_output(void) {
  if (fflush(stdout) != 0 || ferror(stdout)) die(NULL, "write observations");
}
static void hex_bytes(const unsigned char *bytes, int count) {
  static const char hex[] = "0123456789abcdef";
  putchar('"');
  for (int i = 0; i < count; i++) {
    putchar(hex[bytes[i] >> 4]);
    putchar(hex[bytes[i] & 15]);
  }
  putchar('"');
}
static void expected(char *out, size_t size, int id, int version) {
  int used = snprintf(out, size, "%d:%d:", id, version);
  memset(out + used, 'a' + (id + version) % 26, (size_t)ROW_BYTES - used);
  out[ROW_BYTES] = 0;
}
/* Commit `commit` rewrites three far-apart rows and adds one ledger row, atomically. */
static int row_of(int commit, int part) { return (commit * 37 + part * 101) % ROWS + 1; }
static void history_row(int id, const char *value, int first) {
  printf("%s[%d,", first ? "" : ",", id);
  hex_bytes((const unsigned char *)value, ROW_BYTES);
  putchar(']');
}
static void acknowledge(void) {
  printf("],\"ack\":true}\n");
  flush_output();
}
/* An older image of main, from which a lost write can be simulated later. */
static void keep_early_copy(const char *path) {
  char early[4096], block[65536];
  if (snprintf(early, sizeof(early), "%s.early", path) >= (int)sizeof(early))
    die(NULL, "early copy path");
  FILE *from = fopen(path, "rb"), *to = fopen(early, "wb");
  if (!from || !to) die(NULL, "early copy");
  for (size_t n; (n = fread(block, 1, sizeof(block), from)) > 0;)
    if (fwrite(block, 1, n, to) != n) die(NULL, "write early copy");
  if (ferror(from)) die(NULL, "read early copy");
  if (fclose(from) != 0 || fclose(to) != 0) die(NULL, "close early copy");
}
static void prepare(const char *path) {
  sqlite3 *db = NULL, *reader = NULL;
  /* The native VFS ignores these, and MonsterVfs keeps them in the file. */
  const char *parity = getenv("SOAK_PARITY");
  char uri[4200];
  snprintf(uri, sizeof(uri), "file:%s%s%s", path, parity ? "?mv_parity=" : "", parity ? parity : "");
  if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, vfs)) die(db, "open");
  sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
          "PRAGMA wal_autocheckpoint=0;");
  sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); CREATE TABLE ledger(i INTEGER PRIMARY KEY); BEGIN;");
  sqlite3_stmt *insert = NULL, *update = NULL;
  char value[ROW_BYTES + 1];
  if (sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?1,?2)", -1, &insert, NULL)) die(db, "insert");
  if (sqlite3_prepare_v2(db, "UPDATE t SET v=?2 WHERE id=?1", -1, &update, NULL)) die(db, "update");
  printf("{\"format\":1,\"rows\":%d,\"commits\":%d,\"row_bytes\":%d}\n", ROWS, COMMITS, ROW_BYTES);
  printf("{\"commit\":0,\"rows\":[");
  for (int id = 1; id <= ROWS; id++) {
    expected(value, sizeof(value), id, 0);
    if (sqlite3_bind_int(insert, 1, id) != SQLITE_OK ||
        sqlite3_bind_text(insert, 2, value, ROW_BYTES, SQLITE_STATIC) != SQLITE_OK) die(db, "bind insert");
    if (sqlite3_step(insert) != SQLITE_DONE) die(db, "row");
    history_row(id, value, id == 1);
    if (sqlite3_reset(insert) != SQLITE_OK) die(db, "reset insert");
  }
  sql(db, "COMMIT");
  acknowledge();
  sql(db, "PRAGMA wal_checkpoint(TRUNCATE)");
  keep_early_copy(path);
  for (int commit = 1; commit <= COMMITS; commit++) {
    char text[80];
    sql(db, "BEGIN IMMEDIATE");
    printf("{\"commit\":%d,\"rows\":[", commit);
    for (int part = 0; part < 3; part++) {
      int id = row_of(commit, part);
      expected(value, sizeof(value), id, commit);
      if (sqlite3_bind_int(update, 1, id) != SQLITE_OK ||
          sqlite3_bind_text(update, 2, value, ROW_BYTES, SQLITE_STATIC) != SQLITE_OK) die(db, "bind update");
      if (sqlite3_step(update) != SQLITE_DONE) die(db, "update");
      history_row(id, value, part == 0);
      if (sqlite3_reset(update) != SQLITE_OK) die(db, "reset update");
    }
    snprintf(text, sizeof(text), "INSERT INTO ledger VALUES(%d); COMMIT;", commit);
    sql(db, text);
    acknowledge();
    /* A reader held here stops the checkpoint partway, and stops the WAL restarting. */
    if (commit == HELD) {
      if (sqlite3_open_v2(path, &reader, SQLITE_OPEN_READWRITE, vfs)) die(reader, "reader");
      sql(reader, "BEGIN; SELECT count(*) FROM t;");
    }
    if (commit == BACKFILLED) sql(db, "PRAGMA wal_checkpoint(PASSIVE)");
  }
  _exit(0); /* A live WAL, as a crash leaves it. Acknowledgements are flushed. */
}
static int result_code(sqlite3 *db, int code) {
  if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE || !db) return code;
  int extended = sqlite3_extended_errcode(db);
  return extended == SQLITE_OK ? code : extended;
}
/* Preserve SQL types before conversions, full byte lengths, and one extra row. */
static int observe_rows(sqlite3 *db) {
  sqlite3_stmt *query = NULL;
  int code = sqlite3_prepare_v2(db, "SELECT id,v FROM t ORDER BY id", -1, &query, NULL);
  int count = 0;
  putchar('[');
  if (code == SQLITE_OK) {
    while ((code = sqlite3_step(query)) == SQLITE_ROW) {
      int id_type = sqlite3_column_type(query, 0);
      int value_type = sqlite3_column_type(query, 1);
      sqlite3_int64 id = sqlite3_column_int64(query, 0);
      const unsigned char *bytes = value_type == SQLITE_BLOB
        ? sqlite3_column_blob(query, 1) : sqlite3_column_text(query, 1);
      int length = sqlite3_column_bytes(query, 1);
      if (!bytes && value_type != SQLITE_NULL && (value_type != SQLITE_BLOB || length > 0)) {
        code = SQLITE_NOMEM;
        break;
      }
      printf("%s[%d,%lld,%d,%d,", count ? "," : "", id_type, (long long)id, value_type, length);
      hex_bytes(bytes, length > ROW_BYTES + 1 ? ROW_BYTES + 1 : length);
      putchar(']');
      if (++count == ROWS + 1) break;
    }
  }
  code = result_code(db, code);
  sqlite3_finalize(query);
  putchar(']');
  return code;
}
static int observe_ledger(sqlite3 *db, int commits) {
  sqlite3_stmt *query = NULL;
  int code = sqlite3_prepare_v2(db, "SELECT i FROM ledger ORDER BY i", -1, &query, NULL);
  int count = 0;
  putchar('[');
  if (code == SQLITE_OK) {
    while ((code = sqlite3_step(query)) == SQLITE_ROW) {
      int type = sqlite3_column_type(query, 0);
      sqlite3_int64 id = sqlite3_column_int64(query, 0);
      printf("%s[%d,%lld]", count ? "," : "", type, (long long)id);
      if (++count == commits + 1) break;
    }
  }
  code = result_code(db, code);
  sqlite3_finalize(query);
  putchar(']');
  return code;
}
static long long status_field(sqlite3 *db, const char *name) {
  sqlite3_stmt *query = NULL;
  long long value = -1;
  char key[64];
  if (strcmp(vfs, MVFS_NAME)) return 0;
  if (sqlite3_prepare_v2(db, "PRAGMA monstervfs_maintenance", -1, &query, NULL)) return -1;
  if (sqlite3_step(query) == SQLITE_ROW) {
    const char *text = (const char *)sqlite3_column_text(query, 0);
    snprintf(key, sizeof(key), "\"%s\":", name);
    const char *found = text ? strstr(text, key) : NULL;
    if (found) {
      char *end = NULL;
      const char *start = found + strlen(key);
      errno = 0;
      long long parsed = strtoll(start, &end, 10);
      if (errno == 0 && end != start && (*end == ',' || *end == '}') && parsed >= 0) value = parsed;
    }
    if (sqlite3_step(query) != SQLITE_DONE) value = -1;
  }
  if (sqlite3_finalize(query) != SQLITE_OK) value = -1;
  return value;
}
/* Each pass is one read transaction; a failed read cannot silently start another. */
static void observe(sqlite3 *db, int wait_for_repairs, int commits) {
  int begin = result_code(db, sqlite3_exec(db, "BEGIN", NULL, NULL, NULL));
  printf("{\"open\":0,\"begin\":%d", begin);
  if (begin != SQLITE_OK) { putchar('}'); return; }
  printf(",\"rows\":");
  int rows_code = observe_rows(db);
  printf(",\"rows_code\":%d,\"ledger\":", rows_code);
  int ledger_code;
  if (sqlite3_get_autocommit(db)) {
    printf("[]");
    ledger_code = SQLITE_ABORT;
  } else {
    ledger_code = observe_ledger(db, commits);
  }
  int end = sqlite3_get_autocommit(db) ? SQLITE_OK
    : result_code(db, sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL));
  long long zero = status_field(db, "zero_slot_reads");
  long long cut = status_field(db, "wal_cutoffs");
  if (wait_for_repairs) {
    long long pending = status_field(db, "pending_pages");
    for (int i = 0; i < 300 && pending > 0; i++) {
      usleep(10000);
      pending = status_field(db, "pending_pages");
    }
    if (pending < 0) zero = cut = -1;
  }
  printf(",\"ledger_code\":%d,\"end\":%d,\"zero\":%lld,\"cut\":%lld}",
         ledger_code, end, zero, cut);
}
static int check(const char *path, int commits, int preserve_wal) {
  printf("{\"passes\":[");
  for (int pass = 0; pass < 2; pass++) {
    sqlite3 *db = NULL;
    int code = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, vfs);
    if (pass) putchar(',');
    if (code != SQLITE_OK) {
      printf("{\"open\":%d}", result_code(db, code));
      if (sqlite3_close(db) != SQLITE_OK) die(db, "close failed open");
      break;
    }
    sqlite3_extended_result_codes(db, 1);
    if (sqlite3_busy_timeout(db, 3000) != SQLITE_OK) die(db, "busy timeout");
    if (preserve_wal) {
      if (sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL) != SQLITE_OK)
        die(db, "disable close checkpoint");
      if (sqlite3_wal_autocheckpoint(db, 0) != SQLITE_OK) die(db, "disable autocheckpoint");
    }
    observe(db, pass == 0, commits);
    if (sqlite3_close(db) != SQLITE_OK) die(db, "close");
    /* Reopen with an empty cache to observe what maintenance left behind. */
  }
  printf("]}\n");
  flush_output();
  return 0;
}
/* A finished checkpoint: main becomes the only copy of everything. */
static void finish(const char *path) {
  sqlite3 *db = NULL;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, vfs)) die(db, "open");
  sql(db, "PRAGMA wal_checkpoint(TRUNCATE)");
  if (sqlite3_close(db) != SQLITE_OK) die(db, "close");
}
/* Keep follow-up work in WAL until an explicit checkpoint command is observed. */
static int configure_followup(sqlite3 *db) {
  int code = sqlite3_extended_result_codes(db, 1);
  if (code == SQLITE_OK) code = sqlite3_busy_timeout(db, 3000);
  if (code == SQLITE_OK)
    code = sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL);
  if (code == SQLITE_OK) code = sqlite3_wal_autocheckpoint(db, 0);
  if (code == SQLITE_OK) code = sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, NULL, NULL);
  if (code != SQLITE_OK) return result_code(db, code);
  sqlite3_stmt *query = NULL;
  code = sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &query, NULL);
  if (code == SQLITE_OK) {
    code = sqlite3_step(query);
    if (code == SQLITE_ROW) {
      const unsigned char *mode = sqlite3_column_text(query, 0);
      if (mode && strcmp((const char *)mode, "wal") == 0) {
        code = sqlite3_step(query);
        code = code == SQLITE_DONE ? SQLITE_OK : result_code(db, code);
      } else {
        code = SQLITE_MISMATCH;
      }
    } else {
      code = code == SQLITE_DONE ? SQLITE_MISMATCH : result_code(db, code);
    }
  } else {
    code = result_code(db, code);
  }
  int finalized = sqlite3_finalize(query);
  return code == SQLITE_OK ? result_code(db, finalized) : code;
}
static void operation_result(const char *stage, int code) {
  printf("{\"stage\":\"%s\",\"code\":%d}\n", stage, code);
  flush_output();
  /* No graceful close: neither rollback nor a close-time checkpoint can hide what
   * the explicitly reported COMMIT or checkpoint did before this owner exited. */
  _exit(0);
}
static sqlite3 *open_followup(const char *path) {
  sqlite3 *db = NULL;
  int code = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, vfs);
  if (code != SQLITE_OK) operation_result("open", result_code(db, code));
  code = configure_followup(db);
  if (code != SQLITE_OK) operation_result("configure", code);
  return db;
}
/* The parent records the exact intended bytes outside the damaged database. */
static void write_followup(const char *path, int commit, const char *payload_path) {
  char payloads[3][ROW_BYTES];
  FILE *input = fopen(payload_path, "rb");
  if (!input) die(NULL, "open follow-up payloads");
  if (fread(payloads, 1, sizeof(payloads), input) != sizeof(payloads) ||
      fgetc(input) != EOF || ferror(input)) die(NULL, "follow-up payload length/read");
  if (fclose(input) != 0) die(NULL, "close follow-up payloads");
  sqlite3 *db = open_followup(path);
  int code = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
  if (code != SQLITE_OK) operation_result("begin", result_code(db, code));
  sqlite3_stmt *statement = NULL;
  code = sqlite3_prepare_v2(db, "UPDATE t SET v=?2 WHERE id=?1", -1, &statement, NULL);
  if (code != SQLITE_OK) operation_result("update", result_code(db, code));
  const int ids[] = {1, 150, 300};
  for (int part = 0; part < 3; part++) {
    code = sqlite3_bind_int(statement, 1, ids[part]);
    if (code == SQLITE_OK)
      code = sqlite3_bind_text(statement, 2, payloads[part], ROW_BYTES, SQLITE_STATIC);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    if (code != SQLITE_DONE) operation_result("update", result_code(db, code));
    if (sqlite3_changes(db) != 1) operation_result("update", SQLITE_CORRUPT);
    code = sqlite3_reset(statement);
    if (code != SQLITE_OK) operation_result("update", result_code(db, code));
  }
  code = sqlite3_finalize(statement);
  if (code != SQLITE_OK) operation_result("update", result_code(db, code));
  statement = NULL;
  code = sqlite3_prepare_v2(db, "INSERT INTO ledger VALUES(?1)", -1, &statement, NULL);
  if (code == SQLITE_OK) code = sqlite3_bind_int(statement, 1, commit);
  if (code == SQLITE_OK) code = sqlite3_step(statement);
  if (code != SQLITE_DONE) operation_result("ledger", result_code(db, code));
  code = sqlite3_finalize(statement);
  if (code != SQLITE_OK) operation_result("ledger", result_code(db, code));
  code = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  operation_result("commit", result_code(db, code));
}
static void checkpoint_followup(const char *path) {
  sqlite3 *db = open_followup(path);
  int frames = -1, checkpointed = -1;
  int code = sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_TRUNCATE,
                                     &frames, &checkpointed);
  code = result_code(db, code);
  printf("{\"stage\":\"checkpoint\",\"code\":%d,\"log\":%d,\"checkpointed\":%d}\n",
         code, frames, checkpointed);
  flush_output();
  _exit(0);
}
static int commit_argument(const char *value, int minimum) {
  char *end = NULL;
  errno = 0;
  long parsed = strtol(value, &end, 10);
  if (errno || end == value || *end || parsed < minimum || parsed > COMMITS + 1)
    die(NULL, "follow-up commit argument");
  return (int)parsed;
}
int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: soak EXTENSION VFS MODE DATABASE [COMMITS [PAYLOAD_FILE]]\n");
    return 2;
  }
  vfs = argv[2];
  sqlite3 *loader = NULL;
  char *error = NULL;
  if (sqlite3_open(":memory:", &loader) != SQLITE_OK) die(loader, "loader");
  if (sqlite3_enable_load_extension(loader, 1) != SQLITE_OK) die(loader, "enable extension");
  if (sqlite3_load_extension(loader, argv[1], MVFS_ENTRY, &error)) {
    fprintf(stderr, "load: %s\n", error);
    sqlite3_free(error);
    sqlite3_close(loader);
    return 2;
  }
  if (sqlite3_close(loader) != SQLITE_OK) die(loader, "close loader");
  if (argc == 5 && strcmp(argv[3], "prepare") == 0) prepare(argv[4]);
  if (argc == 5 && strcmp(argv[3], "finish") == 0) { finish(argv[4]); return 0; }
  if (argc == 5 && strcmp(argv[3], "check") == 0) return check(argv[4], COMMITS, 0);
  if (argc == 6 && strcmp(argv[3], "inspect") == 0)
    return check(argv[4], commit_argument(argv[5], 0), 1);
  if (argc == 7 && strcmp(argv[3], "write") == 0)
    write_followup(argv[4], commit_argument(argv[5], 1), argv[6]);
  if (argc == 5 && strcmp(argv[3], "checkpoint") == 0) checkpoint_followup(argv[4]);
  fprintf(stderr, "soak: unknown mode or arguments for %s\n", argv[3]);
  return 2;
}
