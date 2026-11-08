/* One step of a tools/test-faults scenario, reporting what an application would see. */
#define _GNU_SOURCE
#include "sqlite3.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *vfs;
static void load(const char *ext) { if (strcmp(vfs, MVFS_NAME)) return; sqlite3 *l = NULL; char *e = NULL; sqlite3_open(":memory:", &l); sqlite3_enable_load_extension(l, 1); if (sqlite3_load_extension(l, ext, MVFS_ENTRY, &e)) { fprintf(stderr, "%s\n", e); _exit(9); } sqlite3_close(l); }
static void x(sqlite3 *d, const char *t) { char *e = 0; if (sqlite3_exec(d, t, 0, 0, &e)) { fprintf(stderr, "%s: %s\n", t, e); _exit(8); } }
static char *one(sqlite3 *d, const char *t) { static char ring[12][120]; static int at; char *o = ring[at++ % 12]; sqlite3_stmt *s = NULL; int c = sqlite3_prepare_v2(d, t, -1, &s, NULL); if (c) { snprintf(o, 120, "E%d", sqlite3_extended_errcode(d)); return o; } c = sqlite3_step(s); if (c == SQLITE_ROW) snprintf(o, 120, "%s", sqlite3_column_text(s, 0) ? (char *)sqlite3_column_text(s, 0) : "null"); else if (c == SQLITE_DONE) snprintf(o, 120, "done"); else snprintf(o, 120, "E%d", sqlite3_extended_errcode(d)); while (c == SQLITE_ROW) c = sqlite3_step(s); sqlite3_finalize(s); return o; }
static sqlite3 *base(const char *db) { sqlite3 *d = NULL; if (sqlite3_open_v2(db, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs)) _exit(7); x(d, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"); return d; }
static void rows(sqlite3 *d, int a, int b) { for (int i = a; i <= b; i++) { char t[100]; snprintf(t, sizeof(t), "INSERT INTO t VALUES(%d, randomblob(3000))", i); x(d, t); } }
static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "%s\n", message); _exit(10); }
}
static void fixed_rows(sqlite3 *d, int first, int last) {
  for (int i = first; i <= last; i++) {
    char sql[120];
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d,printf('%%03000d',%d))", i, i);
    x(d, sql);
  }
}
static void exact_rows(sqlite3 *d, int count) {
  char sql[200];
  snprintf(sql, sizeof(sql), "SELECT count(*)=%d AND min(a)=1 AND max(a)=%d "
           "AND sum(b=printf('%%03000d',a))=%d FROM t", count, count, count);
  require(!strcmp(one(d, sql), "1"), "committed rows differ");
}
static sqlite3 *control_open(const char *path, int exclusive) {
  sqlite3 *d = NULL;
  require(sqlite3_open_v2(path, &d, SQLITE_OPEN_READWRITE, vfs) == SQLITE_OK,
          "control scenario open failed");
  sqlite3_extended_result_codes(d, 1);
  sqlite3_busy_timeout(d, 3000);
  /* Set this before any read can open WAL, to exercise the private wal-index. */
  if (exclusive) x(d, "PRAGMA locking_mode=EXCLUSIVE");
  x(d, "PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
  return d;
}
static void make_control_wal(const char *path) {
  sqlite3 *writer = base(path);
  x(writer, "CREATE TABLE t(a INTEGER PRIMARY KEY, b BLOB)");
  fixed_rows(writer, 1, 20);
  x(writer, "PRAGMA wal_checkpoint(TRUNCATE)");
  fixed_rows(writer, 21, 30);
  sqlite3 *reader = control_open(path, 0);
  x(reader, "BEGIN");
  exact_rows(reader, 30);
  fixed_rows(writer, 31, 50);
  int frames = -1, copied = -1;
  require(sqlite3_wal_checkpoint_v2(writer, NULL, SQLITE_CHECKPOINT_PASSIVE,
                                   &frames, &copied) == SQLITE_OK,
          "partial checkpoint failed");
  require(copied > 0 && copied < frames, "checkpoint was not partial");
  exact_rows(writer, 50);
  printf("rows=50 frames=%d checkpointed=%d\n", frames, copied);
  /* Retain both same-generation controls and the reader-limited WAL. */
  _exit(0);
}
static void fail_control_checkpoint(const char *path, int exclusive) {
  sqlite3 *d = control_open(path, exclusive);
  exact_rows(d, 50);
  int code = sqlite3_wal_checkpoint_v2(d, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
  int closed = sqlite3_close(d);
  printf("rows=50 ckpt=E%d close=%d\n", code, closed);
  require((code & 255) == SQLITE_IOERR, "checkpoint did not report an I/O failure");
  require(closed == SQLITE_OK, "failed checkpoint connection did not close");
}
static void reopen_control(const char *path) {
  sqlite3 *d = control_open(path, 0);
  exact_rows(d, 50);
  fixed_rows(d, 51, 51);
  exact_rows(d, 51);
  require(sqlite3_wal_checkpoint_v2(d, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL)
          == SQLITE_OK, "checkpoint after restoring the device failed");
  require(sqlite3_close(d) == SQLITE_OK, "restored connection did not close");
  d = control_open(path, 0);
  exact_rows(d, 51);
  require(!strcmp(one(d, "PRAGMA integrity_check"), "ok"), "restored integrity check failed");
  require(sqlite3_close(d) == SQLITE_OK, "fresh connection did not close");
  puts("rows=50 write=ok ckpt=ok reopened=51 close=0");
}
int main(int c, char **v) {
  const char *mode = v[1]; vfs = v[3]; load(v[2]); const char *db = v[4]; setvbuf(stdout, NULL, _IOLBF, 0);
  if (!strcmp(mode, "make-control-wal")) { make_control_wal(db); return 0; }
  if (!strcmp(mode, "fail-control-checkpoint")) {
    require(c == 6 && (!strcmp(v[5], "normal") || !strcmp(v[5], "exclusive")), "invalid locking mode");
    fail_control_checkpoint(db, !strcmp(v[5], "exclusive")); return 0;
  }
  if (!strcmp(mode, "reopen-control")) { reopen_control(db); return 0; }
  if (!strcmp(mode, "make-clean") || !strcmp(mode, "make-wal")) {
    sqlite3 *d = base(db); x(d, "CREATE TABLE IF NOT EXISTS t(a INTEGER PRIMARY KEY, b BLOB)"); rows(d, 1, 20); x(d, "PRAGMA wal_checkpoint(TRUNCATE)");
    if (!strcmp(mode, "make-wal")) { rows(d, 21, 50); _exit(0); } sqlite3_close(d); return 0;
  }
  if (!strcmp(mode, "fill")) { /* Commit until the disk refuses, then show that the handle is still honest. */
    sqlite3 *d = base(db); x(d, "CREATE TABLE IF NOT EXISTS t(a INTEGER PRIMARY KEY, b BLOB)"); int ack = 0; int f = open(v[5], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (int i = 1; i < 2000000; i++) { char t[100]; snprintf(t, sizeof(t), "INSERT INTO t VALUES(%d, randomblob(3000))", i); char *e = 0; if (sqlite3_exec(d, t, 0, 0, &e)) { printf("stopped after %d: E%d; read %s\n", ack, sqlite3_extended_errcode(d), one(d, "SELECT count(*) FROM t")); break; } ack = i; if (pwrite(f, &ack, sizeof(ack), 0) != sizeof(ack)) _exit(6); }
    _exit(0);
  }
  if (!strcmp(mode, "try")) {
    sqlite3 *d = NULL; int ro = !strcmp(v[5], "ro"); int code = sqlite3_open_v2(db, &d, (ro ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) | SQLITE_OPEN_URI, vfs);
    if (code) { printf("open=E%d", sqlite3_extended_errcode(d)); } else { sqlite3_busy_timeout(d, 3000); printf("count=%s", one(d, "SELECT count(*) FROM t")); printf(" bytes=%s", one(d, "SELECT sum(length(b)) FROM t")); printf(" integrity=%.40s", one(d, "PRAGMA integrity_check"));
      if (!ro) { printf(" write=%s", one(d, "INSERT INTO t VALUES(900000, randomblob(100))")); printf(" ckpt=%s", one(d, "PRAGMA wal_checkpoint(TRUNCATE)")); } }
    printf(" close=%d\n", sqlite3_close_v2(d)); return 0;
  }
  if (!strcmp(mode, "cycles")) { /* What repeated use looks like when a page's repair cannot land. */
    for (int round = 1; round <= atoi(v[5]); round++) {
      sqlite3 *d = NULL; int code = sqlite3_open_v2(db, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, vfs);
      if (code) { printf("%d open=E%d\n", round, sqlite3_extended_errcode(d)); sqlite3_close_v2(d); continue; }
      sqlite3_busy_timeout(d, 3000); printf("%d count=%s", round, one(d, "SELECT count(*) FROM t"));
      if (!strcmp(vfs, MVFS_NAME)) {
        usleep(400000); /* Repair runs on a worker. */
        const char *status = strstr(one(d, "PRAGMA monstervfs_maintenance"), "\"pending_pages\":");
        printf(" pending=%d", status ? atoi(status + 16) : -1);
      }
      char t[100]; snprintf(t, sizeof(t), "INSERT INTO t VALUES(%d, randomblob(100))", 1000 + round);
      printf(" write=%s", one(d, t));
      printf(" ckpt=%s", one(d, "PRAGMA wal_checkpoint(TRUNCATE)"));
      printf(" again=%s", one(d, "SELECT count(*) FROM t"));
      snprintf(t, sizeof(t), "INSERT INTO t VALUES(%d, randomblob(100))", 2000 + round);
      printf(" write2=%s", one(d, t));
      printf(" close=%d\n", sqlite3_close_v2(d));
    }
    return 0;
  }
  if (!strcmp(mode, "acked")) { int ack = 0; int f = open(v[5], O_RDONLY); read(f, &ack, sizeof(ack)); sqlite3 *d = NULL; sqlite3_open_v2(db, &d, SQLITE_OPEN_READWRITE, vfs); sqlite3_busy_timeout(d, 3000); printf("acked=%d count=%s integrity=%.40s", ack, one(d, "SELECT count(*) FROM t"), one(d, "PRAGMA integrity_check")); printf(" write=%s ckpt=%s\n", one(d, "INSERT INTO t VALUES(900000, randomblob(100))"), one(d, "PRAGMA wal_checkpoint(TRUNCATE)")); sqlite3_close(d); return 0; }
  return 2;
}
