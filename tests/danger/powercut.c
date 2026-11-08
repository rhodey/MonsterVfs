/* Real power loss under a real filesystem, for tools/test-powercut and tools/test-blocklog.
 * `write` commits until the device goes away, recording each acknowledgement on another
 * filesystem. `read` holds snapshots and checkpoints beside it. `script` is a short fixed
 * life for one database. `check` is what the next process sees. */
#define _GNU_SOURCE
#include "sqlite3.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *vfs, *db, *ack_path;
static int ack_fd = -1;
static void load(const char *ext) {
  if (strcmp(vfs, MVFS_NAME)) return;
  sqlite3 *l = NULL; char *e = NULL;
  if (sqlite3_open(":memory:", &l)) _exit(9);
  sqlite3_enable_load_extension(l, 1);
  if (sqlite3_load_extension(l, ext, MVFS_ENTRY, &e)) { fprintf(stderr, "%s\n", e); _exit(9); }
  sqlite3_close(l);
}
static void gone(void) { _exit(3); } /* The device went away, which is the point. */
static void x(sqlite3 *d, const char *t) { if (sqlite3_exec(d, t, 0, 0, 0)) gone(); }
static int size_of(long long seq) { unsigned long long z = seq * 0x9E3779B97F4A7C15ULL; return 100 + (int)((z ^ (z >> 29)) % 6000); }
static void payload(long long seq, unsigned char *out, int n) { unsigned long long z = seq * 0xBF58476D1CE4E5B9ULL + 1; for (int i = 0; i < n; i++) { z ^= z << 13; z ^= z >> 7; z ^= z << 17; out[i] = (unsigned char)z; } }
static long long scalar(sqlite3 *d, const char *t, int *code) { sqlite3_stmt *s = NULL; long long v = -1; int c = sqlite3_prepare_v2(d, t, -1, &s, NULL); if (!c) { c = sqlite3_step(s); if (c == SQLITE_ROW) { v = sqlite3_column_int64(s, 0); c = 0; } else if (c == SQLITE_DONE) c = 0; } if (code) *code = c ? sqlite3_extended_errcode(d) : 0; sqlite3_finalize(s); return v; }
static void setup(sqlite3 *d) {
  x(d, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
  if (getenv("POWERCUT_NOSYNC")) x(d, "PRAGMA synchronous=OFF"); /* Must lose commits, or this test proves nothing. */
  sqlite3_busy_timeout(d, 8000);
  x(d, "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, v BLOB);"
       "CREATE TABLE IF NOT EXISTS ledger(id INTEGER PRIMARY KEY, seq INTEGER);"
       "CREATE TABLE IF NOT EXISTS other(id INTEGER PRIMARY KEY, seq INTEGER);"
       "INSERT OR IGNORE INTO ledger VALUES(1, 0); COMMIT;");
}
static void writer(unsigned seed) {
  srandom(seed); static unsigned char blob[8192];
  ack_fd = open(ack_path, O_WRONLY | O_CREAT, 0644); if (ack_fd < 0) gone();
  static const char *limits[] = {"PRAGMA wal_autocheckpoint=10", "PRAGMA wal_autocheckpoint=50", "PRAGMA wal_autocheckpoint=1000", "PRAGMA wal_autocheckpoint=0"};
  for (;;) {
    sqlite3 *d = NULL; if (sqlite3_open_v2(db, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs)) gone();
    setup(d); x(d, limits[random() % 4]);
    long long seq = scalar(d, "SELECT coalesce(max(id), 0) FROM t", NULL); if (seq < 0) gone();
    sqlite3_stmt *ins, *led, *oth;
    if (sqlite3_prepare_v2(d, "INSERT INTO t VALUES(?1, ?2)", -1, &ins, NULL) || sqlite3_prepare_v2(d, "UPDATE ledger SET seq=?1", -1, &led, NULL) || sqlite3_prepare_v2(d, "INSERT OR REPLACE INTO other VALUES(?1, ?2)", -1, &oth, NULL)) gone();
    for (int n = 1 + random() % 60; n > 0; n--) {
      int rows = random() % 15 ? 1 : 2 + random() % 40;
      x(d, "BEGIN IMMEDIATE");
      for (int r = 0; r < rows; r++) {
        seq++; int size = size_of(seq); payload(seq, blob, size);
        sqlite3_bind_int64(ins, 1, seq); sqlite3_bind_blob(ins, 2, blob, size, SQLITE_STATIC);
        if (sqlite3_step(ins) != SQLITE_DONE) gone(); sqlite3_reset(ins);
      }
      sqlite3_bind_int64(led, 1, seq); if (sqlite3_step(led) != SQLITE_DONE) gone(); sqlite3_reset(led);
      sqlite3_bind_int64(oth, 1, seq % 500); sqlite3_bind_int64(oth, 2, seq); if (sqlite3_step(oth) != SQLITE_DONE) gone(); sqlite3_reset(oth);
      x(d, "COMMIT");
      if (pwrite(ack_fd, &seq, sizeof(seq), 0) != sizeof(seq)) _exit(4);
      long choice = random() % 40;
      if (choice == 0) x(d, "PRAGMA wal_checkpoint(PASSIVE)");
      if (choice == 1) sqlite3_exec(d, "PRAGMA wal_checkpoint(TRUNCATE)", 0, 0, 0);
    }
    sqlite3_finalize(ins); sqlite3_finalize(led); sqlite3_finalize(oth);
    if (random() % 3 == 0) { if (sqlite3_close(d)) gone(); } /* A cold start, so a new WAL. */
    else { sqlite3_close_v2(d); }
  }
}
static void reader(unsigned seed) {
  srandom(seed); char uri[1200]; snprintf(uri, sizeof(uri), "file:%s?mode=ro", db);
  for (;;) {
    usleep(1000 + random() % 20000); sqlite3 *d = NULL;
    if (sqlite3_open_v2(uri, &d, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, vfs)) { sqlite3_close(d); continue; }
    sqlite3_busy_timeout(d, 200);
    if (sqlite3_exec(d, "BEGIN", 0, 0, 0) == 0) { scalar(d, "SELECT count(*) FROM t", NULL); usleep(random() % 30000); sqlite3_exec(d, "COMMIT", 0, 0, 0); }
    sqlite3_close(d);
    if (random() % 4 == 0) { d = NULL; if (sqlite3_open_v2(db, &d, SQLITE_OPEN_READWRITE, vfs) == 0) { sqlite3_busy_timeout(d, 200); sqlite3_exec(d, "PRAGMA wal_checkpoint(PASSIVE)", 0, 0, 0); } sqlite3_close(d); }
  }
}
/* A short fixed life for one database: every step of it lands in the block log. */
static void script(void) {
  static unsigned char blob[8192]; long long seq = 0; char mark[120]; ack_fd = -1;
  for (int session = 0; session < 5; session++) {
    sqlite3 *d = NULL; if (sqlite3_open_v2(db, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs)) gone();
    setup(d); x(d, session % 2 ? "PRAGMA wal_autocheckpoint=8" : "PRAGMA wal_autocheckpoint=0");
    sqlite3_stmt *ins; if (sqlite3_prepare_v2(d, "INSERT INTO t VALUES(?1, ?2)", -1, &ins, NULL)) gone();
    for (int n = 0; n < 16; n++) {
      x(d, "BEGIN IMMEDIATE");
      for (int r = 0; r < (n % 7 == 6 ? 6 : 1); r++) { seq++; int size = size_of(seq); payload(seq, blob, size); sqlite3_bind_int64(ins, 1, seq); sqlite3_bind_blob(ins, 2, blob, size, SQLITE_STATIC); if (sqlite3_step(ins) != SQLITE_DONE) gone(); sqlite3_reset(ins); }
      char t[200]; snprintf(t, sizeof(t), "UPDATE ledger SET seq=%lld; INSERT OR REPLACE INTO other VALUES(%lld, %lld); COMMIT;", seq, seq % 500, seq); x(d, t);
      snprintf(mark, sizeof(mark), "dmsetup message monstervfs-blocklog 0 mark ack-%lld", seq); if (system(mark)) _exit(5);
      if (n % 5 == 4) x(d, "PRAGMA wal_checkpoint(PASSIVE)");
    }
    sqlite3_finalize(ins);
    if (session == 2) x(d, "PRAGMA wal_checkpoint(TRUNCATE)");
    if (sqlite3_close(d)) gone();
  }
  _exit(0);
}
static void field(sqlite3 *d, const char *name, long long *out) {
  const char *t = NULL; sqlite3_stmt *s = NULL; *out = -1;
  if (sqlite3_prepare_v2(d, "PRAGMA monstervfs_maintenance", -1, &s, NULL) == 0 && sqlite3_step(s) == SQLITE_ROW) { t = (const char *)sqlite3_column_text(s, 0); char k[64]; snprintf(k, sizeof(k), "\"%s\":", name); const char *f = t ? strstr(t, k) : NULL; if (f) *out = atoll(f + strlen(k)); }
  sqlite3_finalize(s);
}
static int check(void) {
  long long ack = 0; int f = open(ack_path, O_RDONLY); if (f >= 0) { if (read(f, &ack, sizeof(ack)) != sizeof(ack)) ack = 0; close(f); }
  struct stat st; if (stat(db, &st)) { if (ack) { printf("FAIL lost the database, ack=%lld\n", ack); return 1; } printf("OK none\n"); return 0; }
  char uri[1200]; snprintf(uri, sizeof(uri), "file:%s?mode=ro", db); sqlite3 *d = NULL; int code = 0;
  /* MonsterFt's first look after a crash is read-only. */
  int c = sqlite3_open_v2(uri, &d, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, vfs);
  if (c) { if (ack) { printf("FAIL read-only open %d ack=%lld\n", sqlite3_extended_errcode(d), ack); return 1; } sqlite3_close(d); d = NULL; }
  else { sqlite3_busy_timeout(d, 8000); long long r = scalar(d, "SELECT count(*) FROM t", &code); if (code && ack) { printf("FAIL read-only read %d ack=%lld\n", code, ack); return 1; } if (!code && r < ack) { printf("FAIL read-only rows=%lld ack=%lld\n", r, ack); return 1; } sqlite3_close(d); d = NULL; }
  if (sqlite3_open_v2(db, &d, SQLITE_OPEN_READWRITE, vfs)) { printf("FAIL open %d ack=%lld size=%lld\n", sqlite3_extended_errcode(d), ack, (long long)st.st_size); return 1; }
  sqlite3_busy_timeout(d, 8000);
  long long rows = scalar(d, "SELECT count(*) FROM t", &code);
  if (code == 1 && ack == 0) { printf("OK empty\n"); sqlite3_close(d); return 0; } /* Created, no table yet. */
  if (code) { printf("FAIL count %d ack=%lld\n", code, ack); return 1; }
  long long top = scalar(d, "SELECT coalesce(max(id), 0) FROM t", &code), ledger = scalar(d, "SELECT seq FROM ledger", &code);
  if (code) { printf("FAIL ledger %d ack=%lld\n", code, ack); return 1; }
  if (rows != top) { printf("FAIL hole rows=%lld top=%lld\n", rows, top); return 1; }
  if (ledger != top && !(ledger == -1 && top == 0)) { printf("FAIL atomicity ledger=%lld top=%lld\n", ledger, top); return 1; }
  if (top < ack) { printf("FAIL lost commit top=%lld ack=%lld\n", top, ack); return 1; }
  sqlite3_stmt *s = NULL; static unsigned char want[8192];
  if (sqlite3_prepare_v2(d, "SELECT id, v FROM t", -1, &s, NULL)) { printf("FAIL prepare %d\n", sqlite3_extended_errcode(d)); return 1; }
  while ((c = sqlite3_step(s)) == SQLITE_ROW) { long long id = sqlite3_column_int64(s, 0); int n = sqlite3_column_bytes(s, 1); if (n != size_of(id)) { printf("FAIL size row %lld\n", id); return 1; } payload(id, want, n); if (memcmp(want, sqlite3_column_blob(s, 1), n)) { printf("FAIL bytes row %lld\n", id); return 1; } }
  if (c != SQLITE_DONE) { printf("FAIL scan %d\n", sqlite3_extended_errcode(d)); return 1; } sqlite3_finalize(s);
  if (sqlite3_prepare_v2(d, "PRAGMA integrity_check", -1, &s, NULL) || sqlite3_step(s) != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(s, 0), "ok")) { printf("FAIL integrity %s\n", sqlite3_errmsg(d)); return 1; } sqlite3_finalize(s);
  long long cuts = 0, lost = 0, refusals = 0; if (!strcmp(vfs, MVFS_NAME)) { field(d, "wal_cutoffs", &cuts); field(d, "wal_cutoff_commits", &lost); field(d, "wal_refusals", &refusals); if (lost != 0 || refusals != 0) { printf("FAIL false alarm lost=%lld refusals=%lld\n", lost, refusals); return 1; } }
  char t[200]; long long next = top + 1; int size = size_of(next); payload(next, want, size);
  if (sqlite3_prepare_v2(d, "INSERT INTO t VALUES(?1, ?2)", -1, &s, NULL)) { printf("FAIL insert prepare\n"); return 1; }
  sqlite3_exec(d, "BEGIN IMMEDIATE", 0, 0, 0); sqlite3_bind_int64(s, 1, next); sqlite3_bind_blob(s, 2, want, size, SQLITE_STATIC); c = sqlite3_step(s); sqlite3_finalize(s);
  snprintf(t, sizeof(t), "UPDATE ledger SET seq=%lld; COMMIT;", next);
  if (c != SQLITE_DONE || sqlite3_exec(d, t, 0, 0, 0)) { printf("FAIL write after recovery %d\n", sqlite3_extended_errcode(d)); return 1; }
  if (sqlite3_exec(d, "PRAGMA wal_checkpoint(TRUNCATE)", 0, 0, 0)) { printf("FAIL checkpoint after recovery %d\n", sqlite3_extended_errcode(d)); return 1; }
  sqlite3_close(d); f = open(ack_path, O_WRONLY | O_CREAT, 0644); if (f >= 0) { pwrite(f, &next, sizeof(next), 0); close(f); }
  printf("OK rows=%lld ack=%lld ahead=%lld cuts=%lld\n", top, ack, top - ack, cuts); return 0;
}
int main(int argc, char **argv) {
  if (argc != 7) { fprintf(stderr, "usage: powercut write|read|check EXT VFS DB ACK SEED\n"); return 2; }
  vfs = argv[3]; db = argv[4]; ack_path = argv[5]; load(argv[2]);
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (!strcmp(argv[1], "script")) script();
  if (!strcmp(argv[1], "write")) writer((unsigned)atoi(argv[6]));
  if (!strcmp(argv[1], "read")) reader((unsigned)atoi(argv[6]));
  return check();
}
