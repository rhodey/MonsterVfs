/* Read-only access through real SQLite. `table` prints one outcome per scenario for any
 * VFS, and the tool compares them with native. `damage` covers what native cannot show. */
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "sqlite3.h"
#include "monstervfs.h"
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define SLOT 8192
#define MAIN_START 8192
#define WAL_START 16384
#define WAL_PAYLOAD (SLOT - 4120)
#define COUNT "SELECT count(*) FROM t"
#define MASTER "SELECT count(*) FROM sqlite_master"
static const char *vfs, *extension, *program;
static char dir[1024], db[1100], wal[1200], shm[1200];
static void die(const char *m) { fprintf(stderr, "readonly: %s\n", m); _exit(99); }
static void require(int condition, const char *m) { if (!condition) die(m); }
static void load(void) {
  if (strcmp(vfs, MVFS_NAME)) return;
  sqlite3 *l = NULL; char *e = NULL;
  require(sqlite3_open(":memory:", &l) == 0, "loader");
  sqlite3_enable_load_extension(l, 1);
  if (sqlite3_load_extension(l, extension, MVFS_ENTRY, &e)) die(e ? e : "load");
  sqlite3_close(l);
}
static void sql(sqlite3 *d, const char *t) { char *e = NULL; if (sqlite3_exec(d, t, 0, 0, &e)) die(e); }
static sqlite3 *writer(void) {
  sqlite3 *d = NULL;
  require(sqlite3_open_v2(db, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs) == 0, "open writer");
  sql(d, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
  return d;
}
static void rows(sqlite3 *d, int from, int to) {
  for (int i = from; i <= to; i++) {
    char t[128]; snprintf(t, sizeof(t), "INSERT INTO t VALUES(%d, randomblob(3000))", i); sql(d, t);
  }
}
/* 20 rows in main; with frames, that many more commits in a WAL nobody closed. */
static void make(int frames) {
  pid_t p = fork();
  if (p == 0) {
    load(); sqlite3 *d = writer();
    sql(d, "CREATE TABLE IF NOT EXISTS t(a INTEGER PRIMARY KEY, b BLOB)");
    rows(d, 1, 20); sql(d, "PRAGMA wal_checkpoint(TRUNCATE)");
    if (frames) { rows(d, 21, 20 + frames); _exit(0); }
    sqlite3_close(d); _exit(0);
  }
  int s; waitpid(p, &s, 0); require(WIFEXITED(s) && WEXITSTATUS(s) == 0, "setup");
}
static char *fmt(const char *f, ...) {
  static char ring[16][400]; static int at; char *o = ring[at++ % 16];
  va_list a; va_start(a, f); vsnprintf(o, 400, f, a); va_end(a); return o;
}
static char *run(sqlite3 *d, const char *t) {
  sqlite3_stmt *s = NULL; int c = sqlite3_prepare_v2(d, t, -1, &s, NULL);
  if (c) return fmt("E%d", sqlite3_extended_errcode(d));
  c = sqlite3_step(s); char *r;
  if (c == SQLITE_ROW) r = fmt("%s", sqlite3_column_text(s, 0) ? (char *)sqlite3_column_text(s, 0) : "null");
  else if (c == SQLITE_DONE) r = fmt("done");
  else r = fmt("E%d", sqlite3_extended_errcode(d));
  while (c == SQLITE_ROW) c = sqlite3_step(s);
  sqlite3_finalize(s); return r;
}
static int ro(sqlite3 **d, const char *query) {
  return sqlite3_open_v2(fmt("file:%s?%s", db, query), d, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, vfs);
}
static sqlite3 *rw(const char *query, int *code) {
  sqlite3 *d = NULL; *code = sqlite3_open_v2(fmt("file:%s?%s", db, query), &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, vfs); return d;
}
static void reads(const char *name, const char *query, const char *statement) {
  sqlite3 *d = NULL; int c = ro(&d, query);
  if (c) printf("%s\topen=E%d\n", name, sqlite3_extended_errcode(d)); else printf("%s\tread=%s\n", name, run(d, statement));
  sqlite3_close(d);
}
static void usable(const char *name, sqlite3 *d, int code) {
  printf("%s\topen=%d read=%s write=%s readonly=%d\n", name, code, run(d, COUNT), run(d, "INSERT INTO t VALUES(999, 1)"), sqlite3_db_readonly(d, "main"));
  sqlite3_close(d);
}
static long long hash(const char *p) {
  int f = open(p, O_RDONLY); if (f < 0) return -1;
  unsigned long long h = 14695981039346656037ULL; unsigned char b[65536]; ssize_t n;
  while ((n = read(f, b, sizeof(b))) > 0) for (ssize_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
  close(f); return (long long)h;
}
static void mode(const char *p, int m) { if (access(p, F_OK) == 0) chmod(p, m); }
/* Failed scenarios must not leave what the temporary directory cleanup cannot remove. */
static void restore(void) { chmod(dir, 0755); mode(db, 0644); mode(wal, 0644); mode(shm, 0644); }
static void pipes(int a[2], int b[2]) { require(pipe(a) == 0 && pipe(b) == 0, "pipe"); }

/* ----- Table: outcomes that must match native SQLite ----- */
static void t_clean(void) { make(0); load(); reads("clean file", "mode=ro", COUNT); }
static void t_wal(void) { make(30); load(); reads("uncheckpointed WAL", "mode=ro", COUNT); }
static void t_bytes(void) {
  make(30); load(); long long m = hash(db), w = hash(wal); sqlite3 *d = NULL; ro(&d, "mode=ro"); run(d, COUNT); sqlite3_close(d);
  printf("main and WAL unchanged\t%d\n", m == hash(db) && w == hash(wal)); printf("close leaves WAL\t%d\n", access(wal, F_OK) == 0);
}
static void t_files(void) {
  make(0); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); run(d, COUNT); int a = access(wal, F_OK) == 0, b = access(shm, F_OK) == 0;
  struct stat i; int empty = stat(wal, &i) == 0 && i.st_size == 0; sqlite3_close(d);
  printf("clean file: empty -wal, -shm, kept\twal=%d shm=%d empty=%d after=%d/%d\n", a, b, empty, access(wal, F_OK) == 0, access(shm, F_OK) == 0);
}
static void t_noshm(void) { make(30); unlink(shm); load(); reads("WAL present, -shm missing", "mode=ro", COUNT); }
static void t_writes(void) {
  make(30); load(); sqlite3 *d = NULL; ro(&d, "mode=ro");
  printf("insert\t%s\n", run(d, "INSERT INTO t VALUES(999, 1)")); printf("create table\t%s\n", run(d, "CREATE TABLE u(a)"));
  printf("VACUUM\t%s\n", run(d, "VACUUM")); printf("usable after failed writes\t%s\n", run(d, COUNT)); sqlite3_close(d);
}
static void t_checkpoint(void) {
  make(30); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); run(d, COUNT);
  printf("wal_checkpoint(PASSIVE)\t%s\n", run(d, "PRAGMA wal_checkpoint(PASSIVE)")); printf("wal_checkpoint(TRUNCATE)\t%s\n", run(d, "PRAGMA wal_checkpoint(TRUNCATE)"));
  printf("reads after failed checkpoint\t%s\n", run(d, COUNT)); sqlite3_close(d);
}
static void t_pragmas(void) {
  make(30); load(); sqlite3 *d = NULL; ro(&d, "mode=ro");
  printf("db_readonly\t%d\n", sqlite3_db_readonly(d, "main")); printf("journal_mode\t%s\n", run(d, "PRAGMA journal_mode"));
  printf("journal_mode=DELETE\t%s\n", run(d, "PRAGMA journal_mode=DELETE")); printf("synchronous\t%s\n", run(d, "PRAGMA synchronous"));
  printf("synchronous=NORMAL\t%s\n", run(d, "PRAGMA synchronous=NORMAL")); printf("page_size=1024\t%s\n", run(d, "PRAGMA page_size=1024"));
  printf("wal_autocheckpoint\t%s\n", run(d, "PRAGMA wal_autocheckpoint=10")); printf("integrity_check\t%s\n", run(d, "PRAGMA integrity_check"));
  printf("temp table\t%s\n", run(d, "CREATE TEMP TABLE x(a)")); printf("temp insert\t%s\n", run(d, "INSERT INTO x VALUES(1)")); sqlite3_close(d);
}
static void t_mmap(void) { make(30); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); printf("mmap_size\t%s\n", run(d, "PRAGMA mmap_size=1048576")); printf("reads after mmap_size\t%s\n", run(d, COUNT)); sqlite3_close(d); }
static void t_exclusive(void) { make(30); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); printf("locking_mode=EXCLUSIVE\t%s\n", run(d, "PRAGMA locking_mode=EXCLUSIVE")); printf("read in exclusive mode\t%s\n", run(d, COUNT)); sqlite3_close(d); }
static void t_snapshot(void) {
  make(0); int up[2], down[2]; pipes(up, down); pid_t p = fork();
  if (p == 0) { load(); sqlite3 *w = writer(); char c = 1; write(up[1], &c, 1); read(down[0], &c, 1); rows(w, 21, 30); write(up[1], &c, 1); read(down[0], &c, 1); sqlite3_close(w); _exit(0); }
  char c; read(up[0], &c, 1); load(); sqlite3 *d = NULL; ro(&d, "mode=ro");
  sql(d, "BEGIN"); const char *a = run(d, COUNT); write(down[1], &c, 1); read(up[0], &c, 1); const char *b = run(d, COUNT); sql(d, "COMMIT");
  printf("live writer: snapshot, then new data\t%s %s %s\n", a, b, run(d, COUNT)); write(down[1], &c, 1); sqlite3_close(d); int s; waitpid(p, &s, 0);
}
static void t_missing(void) { load(); reads("missing file", "mode=ro", MASTER); }
static void t_empty(void) { close(open(db, O_CREAT | O_WRONLY, 0644)); load(); reads("0-byte file", "mode=ro", MASTER); }
static void t_short(void) { int f = open(db, O_CREAT | O_WRONLY, 0644); char b[1000] = {0}; require(write(f, b, 1000) == 1000, "write"); close(f); load(); reads("short file", "mode=ro", MASTER); }
static void t_filled(void) {
  close(open(db, O_CREAT | O_WRONLY, 0644)); int go[2], done[2]; pipes(go, done); pid_t p = fork();
  if (p == 0) { char c; read(go[0], &c, 1); make(0); write(done[1], &c, 1); _exit(0); }
  load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); const char *a = run(d, MASTER); char c = 1; write(go[1], &c, 1); read(done[0], &c, 1);
  printf("empty, then filled while open\t%s then %s\n", a, run(d, COUNT)); sqlite3_close(d); int s; waitpid(p, &s, 0);
}
static void t_main_perm(void) { make(0); mode(db, 0444); load(); reads("main perm, dir writable", "mode=ro", COUNT); }
static void t_main_wal(void) { make(30); mode(db, 0444); load(); reads("main perm, WAL writable", "mode=ro", COUNT); sqlite3 *d = NULL; ro(&d, "mode=ro"); run(d, COUNT); printf("main perm: checkpoint\t%s\n", run(d, "PRAGMA wal_checkpoint(FULL)")); printf("main perm: read after\t%s\n", run(d, COUNT)); sqlite3_close(d); }
static void t_all_perm(void) { make(30); mode(db, 0444); mode(wal, 0444); mode(shm, 0444); load(); reads("main, WAL, -shm perm", "mode=ro", COUNT); }
static void t_media(void) { make(30); mode(db, 0444); mode(wal, 0444); mode(shm, 0444); chmod(dir, 0555); load(); reads("everything read-only with dir", "mode=ro", COUNT); }
static void t_media_noshm(void) { make(30); unlink(shm); mode(db, 0444); mode(wal, 0444); chmod(dir, 0555); load(); reads("read-only dir, no -shm", "mode=ro", COUNT); }
static void t_media_nowal(void) { make(0); mode(db, 0444); chmod(dir, 0555); load(); reads("read-only dir, no WAL", "mode=ro", COUNT); }
static void t_fallback(void) { make(0); mode(db, 0444); load(); int c; sqlite3 *d = rw("", &c); usable("read/write open of main perm", d, c); }
static void t_wal_perm(void) { make(30); mode(wal, 0444); load(); reads("WAL perm", "mode=ro", COUNT); }
static void t_wal_noshm(void) { make(30); unlink(shm); mode(wal, 0444); load(); reads("WAL perm, -shm rebuilt", "mode=ro", COUNT); }
static void t_wal_rw(void) { make(30); mode(wal, 0444); load(); int c; sqlite3 *d = rw("", &c); usable("read/write open of WAL perm", d, c); }
static void t_shm_rw(void) { make(30); mode(shm, 0444); load(); int c; sqlite3 *d = rw("", &c); usable("read/write open of -shm perm", d, c); }
static void t_rshm_rw(void) { make(30); load(); int c; sqlite3 *d = rw("readonly_shm=1", &c); usable("read/write open, readonly_shm", d, c); printf("  WAL survives its close\t%d\n", access(wal, F_OK) == 0); }
static void t_immutable(void) { make(0); load(); reads("immutable clean", "immutable=1", COUNT); }
static void t_immutable_wal(void) { make(30); load(); reads("immutable ignores WAL", "immutable=1", COUNT); }
static void t_immutable_media(void) { make(0); mode(db, 0444); chmod(dir, 0555); load(); reads("immutable read-only dir", "immutable=1", COUNT); }
static void t_immutable_empty(void) { close(open(db, O_CREAT | O_WRONLY, 0644)); load(); reads("immutable 0-byte file", "immutable=1", MASTER); }
static void t_immutable_rw(void) { make(30); load(); int c; sqlite3 *d = rw("immutable=1", &c); usable("immutable with read/write flags", d, c); }
static void t_rshm(void) { make(30); load(); reads("readonly_shm, no writer", "mode=ro&readonly_shm=1", COUNT); }
static void t_rshm_live(void) {
  make(0); int up[2], down[2]; pipes(up, down); pid_t p = fork();
  if (p == 0) { load(); sqlite3 *w = writer(); rows(w, 21, 30); char c = 1; write(up[1], &c, 1); read(down[0], &c, 1); sqlite3_close(w); _exit(0); }
  char c; read(up[0], &c, 1); load(); reads("readonly_shm, live writer", "mode=ro&readonly_shm=1", COUNT); write(down[1], &c, 1); int s; waitpid(p, &s, 0);
}
static void t_rshm_noshm(void) { make(30); unlink(shm); load(); reads("readonly_shm, -shm missing", "mode=ro&readonly_shm=1", COUNT); printf("  -shm still missing\t%d\n", access(shm, F_OK) != 0); }
static void t_rshm_empty(void) { make(30); close(open(shm, O_WRONLY | O_TRUNC)); mode(shm, 0444); load(); reads("readonly_shm, empty -shm", "mode=ro&readonly_shm=1", COUNT); }
/* The connection opens before any -shm exists, and a writer creates it afterwards. */
static void t_rshm_late(void) {
  make(30); unlink(shm); int go[2], done[2]; pipes(go, done); pid_t p = fork();
  if (p == 0) { char c; read(go[0], &c, 1); load(); sqlite3 *w = writer(); rows(w, 51, 53); sql(w, "PRAGMA wal_checkpoint"); write(done[1], &c, 1); read(go[0], &c, 1); sqlite3_close(w); _exit(0); }
  load(); sqlite3 *d = NULL; ro(&d, "mode=ro&readonly_shm=1"); char c = 1; write(go[1], &c, 1); read(done[0], &c, 1);
  printf("readonly_shm, writer arrives later\t%s %s\n", run(d, COUNT), run(d, "PRAGMA journal_mode")); write(go[1], &c, 1); sqlite3_close(d); int s; waitpid(p, &s, 0);
}
static void t_rshm_late_rw(void) {
  make(30); unlink(shm); int go[2], done[2]; pipes(go, done); pid_t p = fork();
  if (p == 0) { char c; read(go[0], &c, 1); load(); sqlite3 *w = writer(); rows(w, 51, 53); sql(w, "PRAGMA wal_checkpoint"); write(done[1], &c, 1); read(go[0], &c, 1); sqlite3_close(w); _exit(0); }
  load(); int o; sqlite3 *d = rw("readonly_shm=1", &o); char c = 1; write(go[1], &c, 1); read(done[0], &c, 1);
  printf("read/write readonly_shm, writer arrives later\t%d %s\n", o, run(d, COUNT)); write(go[1], &c, 1); sqlite3_close(d); int s; waitpid(p, &s, 0);
}
/* A file's size can land before its bytes, so a crash can leave a WAL with neither prefix. */
static void unfinished_wal(const char *name, int value) {
  make(0); char b[8192]; memset(b, value, sizeof(b)); int f = open(wal, O_CREAT | O_WRONLY, 0644); require(write(f, b, sizeof(b)) == sizeof(b), "write"); close(f); load();
  reads(fmt("%s, read-only", name), "mode=ro", COUNT); int c; sqlite3 *d = rw("", &c); usable(fmt("%s, read/write", name), d, c);
}
static void t_wal_zero(void) { unfinished_wal("WAL with size but no bytes", 0); }
static void t_wal_junk(void) { unfinished_wal("WAL with size and junk", 0x5b); }
/* Both connections stay open, so the process keeps the guard the reader started with. */
static void t_became_writable(void) {
  make(30); mode(db, 0444); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); const char *a = run(d, COUNT); mode(db, 0644);
  int c; sqlite3 *w = rw("", &c); printf("main became writable under a reader\topen=%d %s %s %s", c, a, run(w, COUNT), run(w, "INSERT INTO t VALUES(999, 1)"));
  printf(" %s %s\n", run(d, COUNT), run(w, "PRAGMA wal_checkpoint(TRUNCATE)")); sqlite3_close(w); sqlite3_close(d);
}
/* Only a fresh process may load SQLite after the parent has started its worker. */
static void contended_writer(int go, int done) {
  alarm(60); load(); int code; sqlite3 *x = rw("", &code);
  require(code == SQLITE_OK, "contender open");
  sqlite3_busy_timeout(x, 20000); sql(x, "PRAGMA wal_autocheckpoint=20"); char b = 1;
  require(write(done, &b, 1) == 1 && read(go, &b, 1) == 1, "contender coordination");
  close(done); close(go);
  for (int i = 0; i < 150; i++) sql(x, fmt("INSERT INTO t VALUES(%d, randomblob(3000))", 2000 + i));
  require(sqlite3_close(x) == SQLITE_OK, "contender close");
}
/* Then the moved guard has to exclude another process, with slots written on both sides. */
static void t_became_writable_contended(void) {
  make(30); mode(db, 0444); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); run(d, COUNT); mode(db, 0644);
  int c; sqlite3 *w = rw("", &c); sqlite3_busy_timeout(w, 20000); sql(w, "PRAGMA wal_autocheckpoint=20");
  int go[2], done[2]; pipes(go, done); char go_arg[32], done_arg[32];
  snprintf(go_arg, sizeof(go_arg), "%d", go[0]); snprintf(done_arg, sizeof(done_arg), "%d", done[1]);
  char *arguments[] = {(char *)program, "contended-writer", (char *)vfs, (char *)extension,
                       db, go_arg, done_arg, NULL};
  require(signal(SIGPIPE, SIG_IGN) != SIG_ERR, "ignore coordination SIGPIPE");
  pid_t p = fork(); require(p >= 0, "contender fork");
  if (p == 0) {
    close(go[1]); close(done[0]); execv(program, arguments); _exit(99);
  }
  close(go[0]); close(done[1]); char b = 1;
  int coordinated = read(done[0], &b, 1) == 1 && write(go[1], &b, 1) == 1;
  close(done[0]); close(go[1]);
  if (!coordinated) {
    kill(p, SIGKILL); int status;
    require(waitpid(p, &status, 0) == p, "failed contender reap");
    die("contender coordination");
  }
  for (int i = 0; i < 150; i++) { sql(w, fmt("INSERT INTO t VALUES(%d, randomblob(3000))", 1000 + i)); run(d, COUNT); }
  int status;
  require(waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0, "contender exit");
  printf("moved guard against another process\t1 %s %s %s\n", run(d, COUNT), run(w, COUNT), run(w, "PRAGMA integrity_check")); sqlite3_close(w); sqlite3_close(d);
}
/* The WAL takes the main file's mode when it is created, so it stays unwritable here. */
static void t_became_writable_wal(void) {
  make(0); mode(db, 0444); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); const char *a = run(d, COUNT); mode(db, 0644);
  int c; sqlite3 *w = rw("", &c); printf("main became writable, WAL did not\topen=%d %s %s %s\n", c, a, run(w, COUNT), run(w, "INSERT INTO t VALUES(999, 1)")); sqlite3_close(w); sqlite3_close(d);
}
static void t_nolock(void) { make(30); load(); reads("nolock", "mode=ro&nolock=1", COUNT); }
static void t_recovers(void) {
  make(30); unlink(shm); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); const char *a = run(d, COUNT); sqlite3_close(d);
  int c; sqlite3 *w = rw("", &c); const char *b = run(w, "INSERT INTO t VALUES(51, 1)"); printf("reader recovers, writer continues\t%s %s %s\n", a, b, run(w, COUNT)); sqlite3_close(w);
}
static void t_joined(void) {
  make(0); load(); sqlite3 *d = NULL; ro(&d, "mode=ro"); const char *a = run(d, COUNT); int c; sqlite3 *w = rw("", &c); const char *b = run(w, "INSERT INTO t VALUES(21, 1)"); const char *e = run(d, COUNT);
  sqlite3_close(w); printf("reader first, writer joins in process\t%s %s %s %s\n", a, b, e, run(d, COUNT)); sqlite3_close(d);
}
typedef void (*scenario)(void);
static scenario table[] = {t_clean, t_wal, t_bytes, t_files, t_noshm, t_writes, t_checkpoint, t_pragmas, t_mmap, t_exclusive, t_snapshot, t_missing, t_empty, t_short, t_filled,
  t_main_perm, t_main_wal, t_all_perm, t_media, t_media_noshm, t_media_nowal, t_fallback, t_wal_perm, t_wal_noshm, t_wal_rw, t_shm_rw, t_rshm_rw,
  t_immutable, t_immutable_wal, t_immutable_media, t_immutable_empty, t_immutable_rw, t_rshm, t_rshm_live, t_rshm_noshm, t_rshm_empty, t_rshm_late, t_rshm_late_rw, t_wal_zero, t_wal_junk, t_became_writable, t_became_writable_wal, t_became_writable_contended, t_nolock, t_recovers, t_joined};
#define LENGTH(a) (sizeof(a) / sizeof(*(a)))
static void place(const char *root, const char *name, size_t i) {
  snprintf(dir, sizeof(dir), "%s/%s-%zu", root, name, i); require(mkdir(dir, 0755) == 0, "mkdir");
  snprintf(db, sizeof(db), "%s/db", dir); snprintf(wal, sizeof(wal), "%s-wal", db); snprintf(shm, sizeof(shm), "%s-shm", db);
}
static void isolate(scenario s) {
  pid_t p = fork();
  if (p == 0) { alarm(60); s(); fflush(stdout); restore(); _exit(0); }
  int status; waitpid(p, &status, 0); restore();
  if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr, "readonly: scenario in %s failed: %d\n", dir, status); exit(1); }
}

/* ----- Damage: native has no slots, so these state the outcome directly ----- */
static void flip(const char *p, off_t at, int n) {
  unsigned char b[256]; int f = open(p, O_RDWR); require(f >= 0, "open for damage");
  require(pread(f, b, n, at) == n, "damage read"); for (int i = 0; i < n; i++) b[i] ^= 0x5a;
  require(pwrite(f, b, n, at) == n, "damage write"); close(f);
}
static off_t frame_payload(int frame) { return WAL_START + (off_t)frame * SLOT + WAL_PAYLOAD + 100; }
/* Rot that takes the page with it, given an offset in the slot's second sector. */
static void rot(const char *path, off_t at) { flip(path, at, 80); if (MVFS_COPY) flip(path, at - 4096, 80); }
static long long field(sqlite3 *d, const char *name) {
  const char *t = run(d, "PRAGMA monstervfs_maintenance"), *f = strstr(t, fmt("\"%s\":", name)); require(f != NULL, "status field");
  return atoll(f + strlen(name) + 3);
}
static int path_kind; /* 0 writable files, 1 unwritable files, 2 read-only directory too */
static void lock_down(void) { if (path_kind) { mode(db, 0444); mode(wal, 0444); mode(shm, 0444); } if (path_kind == 2) chmod(dir, 0555); }
static void checkpoint_then_rot(void) {
  pid_t p = fork();
  if (p == 0) { load(); int c; sqlite3 *d = rw("", &c); sql(d, "PRAGMA wal_checkpoint(PASSIVE)"); rot(wal, frame_payload(3)); _exit(0); }
  int s; waitpid(p, &s, 0); require(WIFEXITED(s) && WEXITSTATUS(s) == 0, "checkpoint setup");
}
static void read_unchanged(int want) {
  long long m = hash(db), w = hash(wal); sqlite3 *d = NULL; require(ro(&d, "mode=ro") == 0, "read-only open");
  const char *n = run(d, COUNT);
  if (want >= 0) { if (atoi(n) != want) { fprintf(stderr, "wanted %d rows, read %s\n", want, n); die("wrong row count"); } }
  else if (!(atoi(n) >= 20 && atoi(n) < 50 && *n != 'E')) { fprintf(stderr, "read %s\n", n); die("damage must end the view inside the WAL"); }
  require(!strcmp(run(d, "PRAGMA integrity_check"), "ok"), "integrity of the reduced view");
  if (want < 0) require(field(d, "wal_cutoffs") == 1, "a stopped read-only recovery must be counted");
  require(field(d, "committed_replacements") == 0, "no repair may reach this file yet");
  sqlite3_close(d); require(m == hash(db) && w == hash(wal), "a read-only connection changed a file");
}
static void d_correctable(void) { make(30); flip(wal, frame_payload(10), 8); lock_down(); load(); read_unchanged(50); }
static void d_cut(void) { make(30); rot(wal, frame_payload(10)); lock_down(); load(); read_unchanged(-1); }
static void d_refuse(void) {
  make(30); checkpoint_then_rot(); lock_down(); load(); long long m = hash(db), w = hash(wal); sqlite3 *d = NULL; require(ro(&d, "mode=ro") == 0, "open");
  const char *r = run(d, COUNT); if (strcmp(r, fmt("E%d", MONSTERVFS_CORRUPT_BACKFILLED))) { fprintf(stderr, "read %s\n", r); die("must refuse"); }
  sqlite3_close(d); require(m == hash(db) && w == hash(wal), "a refusal changed a file");
}
static void paths(const char *root) {
  static const char *names[] = {"writable", "unwritable", "media"};
  for (path_kind = 0; path_kind < 3; path_kind++) {
    scenario s[] = {d_correctable, d_cut, d_refuse};
    for (size_t i = 0; i < LENGTH(s); i++) { place(root, names[path_kind], i); isolate(s[i]); }
    printf("damage on %s files ok\n", names[path_kind]);
  }
}
/* A later writer replaces what a reader stopped short of, and the reader must follow it. */
static void d_writer_follows(void) {
  make(30); rot(wal, frame_payload(10)); load(); sqlite3 *d = NULL; require(ro(&d, "mode=ro") == 0, "open"); int n = atoi(run(d, COUNT)); require(n >= 20 && n < 50, "view");
  int c; sqlite3 *w = rw("", &c); require(c == 0, "writer"); sql(w, "PRAGMA wal_autocheckpoint=0");
  for (int i = 1; i <= 40; i++) sql(w, fmt("INSERT INTO t VALUES(%d, randomblob(3000))", 1000 + i));
  const char *r = run(d, COUNT); if (atoi(r) != n + 40) { fprintf(stderr, "reader sees %s of %d\n", r, n + 40); die("reader lost frames a writer appended"); }
  require(!strcmp(run(d, "PRAGMA integrity_check"), "ok"), "integrity after the writer replaced damage");
  sqlite3_close(d); require(!strcmp(run(w, COUNT), fmt("%d", n + 40)), "writer view"); fflush(stdout); _exit(0); /* no close: crash with it all in the WAL */
}
static void d_writer_recovers(void) {
  load(); int c; sqlite3 *d = rw("", &c); const char *r = run(d, COUNT); int n = atoi(r); require(n > 60, "acknowledged rows lost after the crash");
  require(!strcmp(run(d, "PRAGMA integrity_check"), "ok"), "integrity after the crash"); sql(d, "PRAGMA wal_checkpoint(TRUNCATE)"); sqlite3_close(d);
}
/* A writable connection whose shared memory is not: SQLite takes the private path. */
static void d_private_path(void) {
  make(30); rot(wal, frame_payload(10)); mode(shm, 0444); load(); long long m = hash(db), w = hash(wal); int c; sqlite3 *d = rw("", &c); require(c == 0, "open");
  const char *r = run(d, COUNT); if (!(atoi(r) >= 20 && atoi(r) < 50)) { fprintf(stderr, "read %s\n", r); die("the private path must stop at the damage"); }
  sqlite3_close(d); require(m == hash(db) && w == hash(wal), "a connection without shared memory changed a file");
}
/* A reader leaves the WAL as it found it, so the next writer makes the cut. */
static void d_writer_cuts(void) {
  make(30); rot(wal, frame_payload(10)); load(); sqlite3 *d = NULL; require(ro(&d, "mode=ro") == 0, "open"); int n = atoi(run(d, COUNT)); sqlite3_close(d); require(n >= 20 && n < 50, "view");
  int c; sqlite3 *w = rw("", &c); require(c == 0, "writer"); require(atoi(run(w, COUNT)) == n, "the writer kept a different prefix");
  require(field(w, "wal_cutoffs") == 1, "the writer did not report its cut"); sqlite3_close(w);
}
static void corrected_main(sqlite3 *d) {
  require(!strcmp(run(d, "SELECT length(data) FROM sqlite_dbpage WHERE pgno=3"), "4096"),
          "corrected page read");
  require(atoi(run(d, COUNT)) == 20, "corrected row count");
}
static void no_page_repairs(sqlite3 *d) {
  require(field(d, "pending_pages") == 0 && field(d, "committed_replacements") == 0,
          "read-only read queued or committed a repair");
}
static void wait_status(sqlite3 *d, const char *wanted) {
  for (int i = 0; i < 500; i++) {
    if (strstr(run(d, "PRAGMA monstervfs_maintenance"), wanted)) return;
    usleep(20000);
  }
  fprintf(stderr, "wanted %s: %s\n", wanted, run(d, "PRAGMA monstervfs_maintenance"));
  die("maintenance status did not arrive");
}
static void damage_main(void) { flip(db, MAIN_START + 2 * (off_t)SLOT + 4096 + 200, 8); }
static void d_main(const char *query, int kind) {
  make(0); damage_main(); if (kind) mode(db, 0444); load(); long long m = hash(db);
  sqlite3 *d = NULL; require(ro(&d, query) == 0, "open");
  corrected_main(d); no_page_repairs(d);
  long long w = hash(wal);
  sql(d, "PRAGMA shrink_memory"); corrected_main(d); no_page_repairs(d);
  require(strstr(run(d, "PRAGMA monstervfs_maintenance"), "\"last_error\":null") != NULL,
          "a read-only read attempted a failing repair");
  require(m == hash(db) && w == hash(wal), "read-only correction changed main or WAL");
  require(sqlite3_close(d) == SQLITE_OK, "reader close");
  require(m == hash(db) && w == hash(wal), "read-only close changed main or WAL");
}
static void d_main_ro(void) { d_main("mode=ro", 0); }
static void d_main_immutable(void) { d_main("immutable=1", 0); }
static void d_main_perm(void) { d_main("mode=ro", 1); }
/* The writer lock makes queued work observable before the worker can commit it. */
static void d_main_mixed(void) {
  make(0); damage_main(); load();
  sqlite3 *w = writer(), *r = NULL;
  sql(w, "BEGIN IMMEDIATE");
  require(ro(&r, "mode=ro") == SQLITE_OK, "mixed reader open");
  require(field(r, "applications") == 2, "mixed application count");
  long long m = hash(db), before = hash(wal);
  no_page_repairs(w);
  corrected_main(r); no_page_repairs(r);
  require(m == hash(db) && before == hash(wal), "mixed read-only correction wrote data");
  corrected_main(w);
  require(field(r, "pending_pages") == 1, "writable read did not queue damaged page");
  require(field(r, "committed_replacements") == 0, "repair bypassed the writer lock");
  sql(w, "ROLLBACK");
  wait_status(r, "\"committed_replacements\":1");
  require(m == hash(db), "worker checkpointed main unexpectedly");
  require(before != hash(wal), "writable discovery did not persist a WAL replacement");
  require(sqlite3_close(w) == SQLITE_OK && sqlite3_close(r) == SQLITE_OK, "mixed close");
}
/* A different process holds the write lock through the last local writer's close.
 * A live read-only connection must not keep that writer's pending repair alive. */
static void d_main_cancel(void) {
  make(0); damage_main(); int ready[2], finish[2]; pipes(ready, finish);
  pid_t p = fork(); require(p >= 0, "blocker fork");
  if (p == 0) {
    alarm(30); close(ready[0]); close(finish[1]); load(); sqlite3 *blocker = writer();
    sql(blocker, "BEGIN IMMEDIATE"); char c = 1;
    require(write(ready[1], &c, 1) == 1 && read(finish[0], &c, 1) == 1, "blocker coordination");
    sql(blocker, "ROLLBACK"); require(sqlite3_close(blocker) == SQLITE_OK, "blocker close");
    _exit(0);
  }
  close(ready[1]); close(finish[0]); char c;
  require(read(ready[0], &c, 1) == 1, "blocker ready"); close(ready[0]); load();
  int code; sqlite3 *w = rw("", &code), *r = NULL;
  require(code == SQLITE_OK && ro(&r, "mode=ro") == SQLITE_OK, "cancel connections open");
  require(sqlite3_db_config(w, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL) == SQLITE_OK,
          "disable unrelated close checkpoint");
  /* Enter SQLite's WAL reader lifecycle without reading the damaged page yet. */
  require(!strcmp(run(r, MASTER), "1"), "remaining reader schema");
  no_page_repairs(r);
  corrected_main(w);
  wait_status(r, "\"blocked\":true");
  require(field(r, "pending_pages") == 1 && field(r, "applications") == 2,
          "blocked repair or application count differs");
  long long m = hash(db), before = hash(wal);
  require(sqlite3_close(w) == SQLITE_OK, "last writer close");
  require(field(r, "applications") == 1, "read-only application must remain tracked");
  no_page_repairs(r);
  require(write(finish[1], &c, 1) == 1, "release blocker"); close(finish[1]);
  int status; require(waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                      "blocker exit");
  wait_status(r, "\"active\":false");
  sql(r, "PRAGMA shrink_memory"); corrected_main(r); no_page_repairs(r);
  require(m == hash(db) && before == hash(wal), "cancelled repair changed main or WAL");
  require(sqlite3_close(r) == SQLITE_OK, "remaining reader close");
}
/* An established main reader may retain an empty WAL handle while a writer
 * initializes that WAL. Adoption stays read-only and keeps the same handle. */
static void d_adopt_wal(void) {
  make(0);
  int f = open(wal, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  require(f >= 0, "create empty WAL"); close(f);
  load(); long long m = hash(db), before = hash(wal); sqlite3 *r = NULL;
  require(ro(&r, "mode=ro") == SQLITE_OK, "early WAL reader open");
  require(!strcmp(run(r, COUNT), "20"), "initial WAL reader contents");
  require(m == hash(db) && before == hash(wal), "reader initialized empty WAL");
  sqlite3_file *file = NULL, *after = NULL;
  require(sqlite3_file_control(r, "main", SQLITE_FCNTL_JOURNAL_POINTER, &file) == SQLITE_OK
          && file && file->pMethods, "early WAL file handle");
  unsigned char id[16];
  require(file->pMethods->xFileControl(file, MONSTERVFS_FCNTL_ID, id) == SQLITE_NOTFOUND,
          "empty WAL unexpectedly has a protected identity");
  sqlite3 *w = writer(); rows(w, 21, 23);
  m = hash(db); before = hash(wal);
  require(!strcmp(run(r, COUNT), "23"), "reader did not follow initialized WAL");
  require(sqlite3_file_control(r, "main", SQLITE_FCNTL_JOURNAL_POINTER, &after) == SQLITE_OK
          && after == file, "reader replaced its WAL handle");
  require(file->pMethods->xFileControl(file, MONSTERVFS_FCNTL_ID, id) == SQLITE_OK,
          "retained WAL handle did not adopt the protected identity");
  no_page_repairs(r);
  require(m == hash(db) && before == hash(wal), "WAL adoption wrote main or WAL");
  require(sqlite3_close(r) == SQLITE_OK, "WAL reader close");
  require(m == hash(db) && before == hash(wal), "WAL reader close changed main or WAL");
  require(sqlite3_close(w) == SQLITE_OK, "WAL writer close");
}
/* An unfinished main can be inspected, but initialization requires reopening
 * that reader. Its old handle must never attach to the new database's status. */
static void d_reopen_main(int first) {
  int f = open(db, O_CREAT | O_WRONLY, 0644); require(f >= 0, "create empty main"); close(f);
  load(); sqlite3 *r = NULL, *fresh = NULL;
  require(ro(&r, "mode=ro") == SQLITE_OK, "early reader open");
  require(!strcmp(run(r, MASTER), "0"), "early empty schema");
  sqlite3 *w = writer();
  sql(w, "CREATE TABLE t(a INTEGER PRIMARY KEY, b BLOB)"); rows(w, 1, 20);
  if (first != 2) sql(w, "PRAGMA wal_checkpoint(TRUNCATE)");
  require(field(w, "applications") == 1, "uninitialized reader must not attach");
  long long m = hash(db), before = hash(wal);
  sqlite3_file *file = NULL;
  require(sqlite3_file_control(r, "main", SQLITE_FCNTL_FILE_POINTER, &file) == SQLITE_OK
          && file && file->pMethods, "early reader file");
  unsigned char header[100]; sqlite3_int64 length = 0;
  int result;
  if (first == 2) {
    sqlite3_vfs *v = sqlite3_vfs_find(vfs);
    sqlite3_file *wf = calloc(1, v->szOsFile); require(wf != NULL, "WAL handle allocation");
    const char *name = sqlite3_filename_wal(sqlite3_db_filename(r, "main"));
    result = v->xOpen(v, name, wf, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_WAL, NULL);
    if (wf->pMethods) wf->pMethods->xClose(wf);
    free(wf);
  } else {
    result = first ? file->pMethods->xFileSize(file, &length)
                   : file->pMethods->xRead(file, header, sizeof(header), 0);
  }
  require(result == SQLITE_CANTOPEN, "initialized main requires a fresh reader");
  char *status = NULL;
  require(sqlite3_file_control(r, "main", MONSTERVFS_FCNTL_STATUS, &status) == SQLITE_NOTFOUND
          && status == NULL, "old reader attached status");
  require(field(w, "applications") == 1, "old reader counted after initialization");
  require(m == hash(db) && before == hash(wal), "old reader changed main or WAL");
  require(ro(&fresh, "mode=ro") == SQLITE_OK, "fresh reader open");
  require(field(fresh, "applications") == 2, "fresh reader missing from shared status");
  if (first != 2) damage_main();
  m = hash(db); before = hash(wal);
  corrected_main(fresh); no_page_repairs(fresh); no_page_repairs(w);
  require(m == hash(db) && before == hash(wal), "fresh read-only correction wrote data");
  require(sqlite3_close(r) == SQLITE_OK, "old reader close");
  require(field(w, "applications") == 2, "old reader close changed application count");
  require(sqlite3_close(w) == SQLITE_OK, "writer close after initialization");
  require(field(fresh, "applications") == 1, "remaining reader count differs");
  no_page_repairs(fresh);
  require(sqlite3_close(fresh) == SQLITE_OK, "fresh reader close");
}
static void d_reopen_main_read(void) { d_reopen_main(0); }
static void d_reopen_main_size(void) { d_reopen_main(1); }
static void d_reopen_main_wal(void) { d_reopen_main(2); }
/* Preflight must remain read-only and close before a writer resumes creation. */
static void d_preflight(off_t bytes) {
  int f = open(db, O_CREAT | O_WRONLY, 0644); require(f >= 0, "create unfinished main");
  require(ftruncate(f, bytes) == 0, "unfinished prefix length"); close(f);
  load(); long long before = hash(db); sqlite3 *r = NULL;
  require(ro(&r, "mode=ro") == SQLITE_OK, "preflight open");
  require(!strcmp(run(r, MASTER), bytes ? "E26" : "0"), "preflight schema result");
  require(sqlite3_close(r) == SQLITE_OK, "preflight close");
  require(before == hash(db) && access(wal, F_OK) != 0, "preflight changed unfinished storage");
  sqlite3 *w = writer();
  sql(w, "CREATE TABLE t(a INTEGER PRIMARY KEY, b BLOB)"); rows(w, 1, 3);
  require(ro(&r, "mode=ro") == SQLITE_OK, "initialized reader open");
  require(!strcmp(run(r, COUNT), "3"), "initialized reader contents");
  require(field(r, "applications") == 2, "initialized reader status");
  no_page_repairs(r);
  require(sqlite3_close(r) == SQLITE_OK && sqlite3_close(w) == SQLITE_OK, "preflight owners close");
}
static void d_preflight_empty(void) { d_preflight(0); }
static void d_preflight_partial(void) { d_preflight(4096); }
static void d_preflight_prefix(void) { d_preflight(8192); }
static void d_main_lost(void) {
  make(0); rot(db, MAIN_START + 2 * (off_t)SLOT + 4096 + 200); load(); sqlite3 *d = NULL; require(ro(&d, "mode=ro") == 0, "open");
  const char *r = run(d, "SELECT sum(length(b)) FROM t"); require(!strcmp(r, "E8202"), "lost main page must read as SQLITE_IOERR_DATA"); sqlite3_close(d);
}
static void d_prefix(void) {
  make(0); flip(db, 100, 80); mode(db, 0444); load(); long long m = hash(db); sqlite3 *d = NULL; require(ro(&d, "mode=ro") == 0, "open with one prefix copy");
  require(atoi(run(d, COUNT)) == 20, "read through the other copy"); sqlite3_close(d); require(m == hash(db), "main changed");
}
int main(int argc, char **argv) {
  program = argv[0];
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc == 7 && !strcmp(argv[1], "contended-writer")) {
    vfs = argv[2]; extension = argv[3]; snprintf(db, sizeof(db), "%s", argv[4]);
    contended_writer(atoi(argv[5]), atoi(argv[6])); return 0;
  }
  if (argc >= 4 && !strcmp(argv[1], "table")) {
    vfs = argv[2]; extension = argc > 4 ? argv[4] : "";
    for (size_t i = 0; i < LENGTH(table); i++) { place(argv[3], vfs, i); isolate(table[i]); }
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "initialization")) {
    vfs = MVFS_NAME; extension = argv[3];
    place(argv[2], "read", 0); isolate(d_reopen_main_read);
    place(argv[2], "size", 0); isolate(d_reopen_main_size);
    place(argv[2], "wal", 0); isolate(d_reopen_main_wal);
    place(argv[2], "preflight-empty", 0); isolate(d_preflight_empty);
    place(argv[2], "preflight-partial", 0); isolate(d_preflight_partial);
    place(argv[2], "preflight-prefix", 0); isolate(d_preflight_prefix);
    puts("Read-only initialization boundaries passed."); return 0;
  }
  if (argc != 4 || strcmp(argv[1], "damage")) return 2;
  vfs = MVFS_NAME; extension = argv[3]; paths(argv[2]);
  scenario once[] = {d_private_path, d_writer_cuts, d_adopt_wal, d_main_ro, d_main_immutable, d_main_perm, d_main_mixed, d_main_cancel, d_reopen_main_read, d_reopen_main_size, d_reopen_main_wal, d_preflight_empty, d_preflight_partial, d_preflight_prefix, d_main_lost, d_prefix};
  for (size_t i = 0; i < LENGTH(once); i++) { place(argv[2], "main", i); isolate(once[i]); }
  place(argv[2], "follow", 0); pid_t p = fork();
  if (p == 0) { alarm(60); d_writer_follows(); }
  int s; waitpid(p, &s, 0); require(WIFEXITED(s) && WEXITSTATUS(s) == 0, "writer scenario"); isolate(d_writer_recovers);
  puts("Read-only damage passed."); return 0;
}
