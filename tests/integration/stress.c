/* Multi-process stress through real SQLite on real files, with real kill -9.
 * Writers, readers and a checkpointer race while the driver kills them, flips
 * correctable bytes, and destroys WAL slots under live or dead processes. Transfers
 * span pages, so every snapshot proves atomicity. Acknowledged commits must survive
 * process death, unless a slot was destroyed. The driver never loads SQLite, so its
 * opens and closes cannot drop any process's POSIX locks.
 * Offsets assume the defaults: 8 KiB slots, with main at 8 KiB and WAL at 16 KiB. */
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "sqlite3.h"
#include "monstervfs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SLOT 8192
#define MAIN_START 8192
#define WAL_START 16384
#define ACCOUNTS 60
#define TOTAL (ACCOUNTS * 1000)
#define WRITERS 3
#define READERS 4
#define CHILDREN (WRITERS + READERS + 1)
#define EXIT_FAILED 3
#define EXIT_REFUSED 4
#define STALL_SECONDS 30.0
#define SHM_WRITE_LOCK 120
#define INVALIDATIONS 60

typedef struct {
  atomic_llong acknowledged[WRITERS];
  atomic_int relaxed;   /* A slot was destroyed: commits may be cut, reads may fail. */
  atomic_int stopping;
  atomic_int pause;     /* Hold checkpoints, so that new frames stay above the watermark. */
  atomic_llong commits, reads, checkpoints, busy, data_errors, refusals, cuts, cut_commits,
      replacements;
} Shared;
typedef struct {
  pid_t pid;
  int killed;
} Child;
typedef enum { CLEAN, REPAIR, DEAD_CUT, LIVE_CUT, KINDS } Kind;

static Shared *shared;
static const char *extension;
static char database[4096], wal[4120], shm[4120];
static const char *role = "driver";
static Child children[CHILDREN];
static int refused;
static const char *vfs_name = MVFS_NAME;
static int control;   /* STRESS_VFS names another VFS: it has no slots to damage. */

static void fail(const char *format, ...);
static void on_log(void *unused, int code, const char *message) {
  (void)unused; (void)code;
  long long commits;
  if (getenv("STRESS_TRACE") && strncmp(message, "monstervfs", 10) == 0)
    fprintf(stderr, "log %d %s\n", (int)getpid(), message);
  if (sscanf(message, "monstervfs cut WAL at byte %*d: %*d bytes, %*d frames and %lld finished",
             &commits) == 1) {
    atomic_fetch_add(&shared->cuts, 1);
    atomic_fetch_add(&shared->cut_commits, commits);
    if (commits && !atomic_load(&shared->relaxed)) fail("false alarm: %lld commits after a kill alone", commits);
  }
}
static void fail(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  fprintf(stderr, "stress %s %d: ", role, (int)getpid());
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
  _exit(EXIT_FAILED);
}
static void load(void) {
  sqlite3 *loader = NULL;
  char *error = NULL;
  if (sqlite3_config(SQLITE_CONFIG_LOG, on_log, NULL) != SQLITE_OK) fail("log hook failed");
  if (sqlite3_open(":memory:", &loader) != SQLITE_OK) fail("loader open failed");
  sqlite3_enable_load_extension(loader, 1);
  if (sqlite3_load_extension(loader, extension, MVFS_ENTRY, &error))
    fail("load: %s", error);
  sqlite3_close(loader);
}
/* Failures every role retries. Anything else is a bug, except what destroyed slots cause. */
static int admit(int code) {
  switch (code) {
    case SQLITE_OK:
      return code;
    case SQLITE_BUSY: case SQLITE_BUSY_RECOVERY: case SQLITE_BUSY_SNAPSHOT: case SQLITE_PROTOCOL:
      atomic_fetch_add(&shared->busy, 1);
      return code;
  }
  if (atomic_load(&shared->relaxed)) {
    if (code == SQLITE_IOERR_DATA) {
      atomic_fetch_add(&shared->data_errors, 1);
      return code;
    }
    if (code == MONSTERVFS_CORRUPT_BACKFILLED) {
      atomic_fetch_add(&shared->refusals, 1);
      _exit(EXIT_REFUSED);
    }
  }
  fail("unexpected SQLite code %d", code);
  return code;
}
static int run(sqlite3 *db, const char *text) {
  return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK
             ? SQLITE_OK : admit(sqlite3_extended_errcode(db));
}
static int value(sqlite3 *db, const char *text, sqlite3_int64 *output) {
  sqlite3_stmt *statement = NULL;
  int code = sqlite3_prepare_v2(db, text, -1, &statement, NULL);
  if (code == SQLITE_OK) {
    code = sqlite3_step(statement);
    if (code == SQLITE_ROW) {
      *output = sqlite3_column_int64(statement, 0);
      code = SQLITE_OK;
    }
  }
  sqlite3_finalize(statement);
  return code == SQLITE_OK ? SQLITE_OK : admit(sqlite3_extended_errcode(db));
}
static int open_flags = SQLITE_OPEN_READWRITE;
static sqlite3 *connect_database(int timeout) {
  for (;;) {
    sqlite3 *db = NULL;
    int code = sqlite3_open_v2(database, &db, open_flags, vfs_name);
    if (code == SQLITE_OK) code = sqlite3_busy_timeout(db, timeout);
    if (code == SQLITE_OK) {
      sqlite3_wal_autocheckpoint(db, 0);
      return db;
    }
    admit(db ? sqlite3_extended_errcode(db) : code);
    sqlite3_close(db);
  }
}
static sqlite3 *reconnect(sqlite3 *db, int code, int timeout) {
  sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
  if (code != SQLITE_IOERR_DATA) return db;
  sqlite3_close(db);
  return connect_database(timeout);
}
static void finish(sqlite3 *db) {
  sqlite3_stmt *statement = NULL;
  if (sqlite3_prepare_v2(db, "PRAGMA monstervfs_maintenance", -1, &statement, NULL) == SQLITE_OK
      && sqlite3_step(statement) == SQLITE_ROW) {
    const char *found = strstr((const char *)sqlite3_column_text(statement, 0),
                               "\"committed_replacements\":");
    if (found) atomic_fetch_add(&shared->replacements, atoll(strchr(found, ':') + 1));
  }
  sqlite3_finalize(statement);
  sqlite3_close(db);
  _exit(0);
}

static void writer(int index) {
  sqlite3 *db = connect_database(500);
  if (index == 0) sqlite3_wal_autocheckpoint(db, 64);
  for (;;) {
    if (atomic_load(&shared->stopping)) finish(db);
    if (atomic_load(&shared->pause)) sqlite3_wal_autocheckpoint(db, 0);
    int from = (int)(random() % ACCOUNTS) + 1, to = from % ACCOUNTS + 1;
    sqlite3_int64 seq = -1;
    char text[400];
    int code = run(db, "BEGIN IMMEDIATE");
    if (code == SQLITE_OK) {
      snprintf(text, sizeof(text), "SELECT seq FROM ledger WHERE writer=%d", index);
      code = value(db, text, &seq);
    }
    if (code == SQLITE_OK) {
      snprintf(text, sizeof(text),
               "UPDATE account SET balance=balance-1 WHERE id=%d;"
               "UPDATE account SET balance=balance+1 WHERE id=%d;"
               "UPDATE ledger SET seq=seq+1 WHERE writer=%d;"
               "INSERT INTO history VALUES(%d,%lld,randomblob(%d));",
               from, to, index, index, (long long)seq, (int)(random() % 1500));
      code = run(db, text);
    }
    if (code == SQLITE_OK) code = run(db, "COMMIT");
    if (code == SQLITE_OK) {
      atomic_store(&shared->acknowledged[index], seq + 1);
      atomic_fetch_add(&shared->commits, 1);
    } else {
      db = reconnect(db, code, 500);
    }
  }
}
/* One snapshot must balance and keep each ledger equal to its history. */
static int check_snapshot(sqlite3 *db, const long long *seen, sqlite3_int64 *sum) {
  int code = value(db, "SELECT sum(balance) FROM account", sum);
  if (code != SQLITE_OK) return code;
  if (*sum != TOTAL) fail("snapshot holds %lld, half of a transfer", (long long)*sum);
  for (int index = 0; index < WRITERS; index++) {
    char text[200];
    sqlite3_int64 seq = -1, rows = -1;
    snprintf(text, sizeof(text), "SELECT seq FROM ledger WHERE writer=%d", index);
    if ((code = value(db, text, &seq)) != SQLITE_OK) return code;
    snprintf(text, sizeof(text), "SELECT count(*) FROM history WHERE writer=%d", index);
    if ((code = value(db, text, &rows)) != SQLITE_OK) return code;
    if (seq != rows) fail("writer %d ledger %lld, history %lld", index, (long long)seq, (long long)rows);
    if (seq < seen[index] && !atomic_load(&shared->relaxed))
      fail("writer %d acknowledged %lld, snapshot has %lld", index, seen[index], (long long)seq);
  }
  return SQLITE_OK;
}
static void reader(void) {
  sqlite3 *db = connect_database(500);
  for (int pass = 0;; pass++) {
    if (atomic_load(&shared->stopping)) finish(db);
    long long seen[WRITERS];
    sqlite3_int64 first = -1, second = -1;
    for (int index = 0; index < WRITERS; index++) seen[index] = atomic_load(&shared->acknowledged[index]);
    int code = run(db, "BEGIN");
    if (code == SQLITE_OK) code = check_snapshot(db, seen, &first);
    if (code == SQLITE_OK && pass % 8 == 0) {
      /* Hold the snapshot across checkpoints, commits and any cutoff attempt. */
      usleep((useconds_t)(random() % 20000));
      code = check_snapshot(db, seen, &second);
    }
    if (code == SQLITE_OK) code = run(db, "COMMIT");
    if (code == SQLITE_OK) atomic_fetch_add(&shared->reads, 1);
    else db = reconnect(db, code, 500);
    if (pass % 16 == 15) {
      sqlite3_close(db);
      db = connect_database(500);
    }
  }
}
static void checkpointer(void) {
  static const int modes[] = {SQLITE_CHECKPOINT_PASSIVE, SQLITE_CHECKPOINT_FULL,
                              SQLITE_CHECKPOINT_RESTART, SQLITE_CHECKPOINT_TRUNCATE};
  sqlite3 *db = connect_database(200);
  for (;;) {
    if (atomic_load(&shared->stopping)) finish(db);
    int pause = atomic_load(&shared->pause);
    if (pause == 2) {
      usleep(1000);
      continue;
    }
    int code = sqlite3_wal_checkpoint_v2(db, NULL, modes[pause ? 3 : random() % 4], NULL, NULL);
    if (pause) atomic_store(&shared->pause, 2);
    code = code == SQLITE_OK ? SQLITE_OK : admit(sqlite3_extended_errcode(db));
    if (code == SQLITE_OK) atomic_fetch_add(&shared->checkpoints, 1);
    else db = reconnect(db, code, 200);
    usleep((useconds_t)(1000 + random() % 15000));
  }
}
static void child(int slot) {
  srandom((unsigned)getpid() ^ (unsigned)time(NULL));
  load();
  if (slot < WRITERS) {
    role = "writer";
    writer(slot);
  } else if (slot < WRITERS + READERS) {
    if (slot % 2) open_flags = SQLITE_OPEN_READONLY; /* They run recovery too. */
    role = "reader";
    reader();
  } else {
    role = "checkpointer";
    checkpointer();
  }
}
static void prepare(void) {
  role = "prepare";
  load();
  sqlite3 *db = NULL;
  if (sqlite3_open_v2(database, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs_name))
    fail("create failed");
  char text[600];
  snprintf(text, sizeof(text),
           "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
           "CREATE TABLE account(id INTEGER PRIMARY KEY, balance INT NOT NULL, pad BLOB);"
           "CREATE TABLE ledger(writer INTEGER PRIMARY KEY, seq INT NOT NULL);"
           "CREATE TABLE history(writer INT, seq INT, pad BLOB, PRIMARY KEY(writer,seq)) WITHOUT ROWID;"
           "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<%d)"
           "INSERT INTO account SELECT x,1000,zeroblob(3600) FROM c;"
           "WITH RECURSIVE c(x) AS(VALUES(0) UNION ALL SELECT x+1 FROM c WHERE x<%d)"
           "INSERT INTO ledger SELECT x,0 FROM c;"
           "PRAGMA wal_checkpoint(TRUNCATE);", ACCOUNTS, WRITERS - 1);
  if (sqlite3_exec(db, text, NULL, NULL, NULL)) fail("setup: %s", sqlite3_errmsg(db));
  sqlite3_close(db);
  _exit(0);
}
/* After a kill alone, nothing acknowledged may be missing and nothing may be cut. */
static void verify(void) {
  role = "verify";
  load();
  int relaxed = atomic_load(&shared->relaxed);
  long long seen[WRITERS];
  for (int index = 0; index < WRITERS; index++) seen[index] = atomic_load(&shared->acknowledged[index]);
  sqlite3 *db = connect_database(5000);
  sqlite3_int64 sum = -1;
  for (;;) {
    int code = run(db, "BEGIN");
    if (code == SQLITE_OK) code = check_snapshot(db, seen, &sum);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (code == SQLITE_OK) break;
    if (code == SQLITE_IOERR_DATA) _exit(EXIT_REFUSED);
    usleep(1000);
  }
  sqlite3_stmt *statement = NULL;
  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &statement, NULL)
      || sqlite3_step(statement) != SQLITE_ROW)
    admit(sqlite3_extended_errcode(db));
  else if (strcmp((const char *)sqlite3_column_text(statement, 0), "ok"))
    fail("integrity check: %s", sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
  sqlite3_close(db);
  _exit(0);
}

static void stop_children(void) {
  for (int slot = 0; slot < CHILDREN; slot++)
    if (children[slot].pid > 0) kill(children[slot].pid, SIGKILL);
}
static void die(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  fprintf(stderr, "stress driver: ");
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
  stop_children();
  fprintf(stderr, "stress driver: files kept at %s\n", database);
  exit(1);
}
static pid_t start(void (*work)(int), int slot) {
  pid_t pid = fork();
  if (pid < 0) die("fork: %s", strerror(errno));
  if (pid == 0) {
    work(slot);
    _exit(0);
  }
  return pid;
}
static void spawn(int slot) {
  children[slot].pid = start(child, slot);
  children[slot].killed = 0;
}
static void spawn_missing(void) {
  if (refused) return;
  for (int slot = 0; slot < CHILDREN; slot++)
    if (children[slot].pid == 0) spawn(slot);
}
static void note_exit(int slot, int status) {
  Child *entry = &children[slot];
  int killed = entry->killed;
  entry->pid = 0;
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && killed) return;
  if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_REFUSED) {
    refused = 1;
    return;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && atomic_load(&shared->stopping)) return;
  die("slot %d ended with status %#x", slot, status);
}
static void reap(void) {
  for (int slot = 0; slot < CHILDREN; slot++) {
    int status = 0;
    if (children[slot].pid > 0 && waitpid(children[slot].pid, &status, WNOHANG) > 0) note_exit(slot, status);
  }
}
static double now(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}
static void shoot(int slot) {
  if (children[slot].pid > 0) {
    children[slot].killed = 1;
    kill(children[slot].pid, SIGKILL);
  }
}
static void await_children(double limit) {
  double deadline = now() + limit;
  for (;;) {
    reap();
    int alive = 0;
    for (int slot = 0; slot < CHILDREN; slot++) alive += children[slot].pid > 0;
    if (alive == 0) return;
    if (now() > deadline) die("%d children did not stop: a lock or wait never ended", alive);
    usleep(1000);
  }
}
/* Two bytes in one slot or record: always within what protection corrects. */
static void flip(int main_only) {
  if (control) return;
  int main_file = main_only || random() % 2;
  int fd = open(main_file ? database : wal, O_RDWR);
  struct stat info;
  if (fd < 0) return;
  off_t start = main_file ? MAIN_START : WAL_START;
  if (fstat(fd, &info) == 0 && info.st_size > start + SLOT) {
    int record = !main_only && random() % 6 == 0;
    off_t offset = start + (off_t)random() % ((info.st_size - start) / SLOT) * SLOT;
    if (record) offset = random() % 2 || main_file ? random() % 256 : 8192 + random() % 2 * 4096 + random() % 256;
    for (int i = 0; i < 2; i++) {
      /* A record is 256 bytes. The lower half of a slot is padding around its record. */
      off_t at = offset + (record ? random() % 8 : SLOT / 2 + random() % (SLOT / 2));
      unsigned char byte = 0;
      if (pread(fd, &byte, 1, at) == 1) {
        byte ^= (unsigned char)(1 + random() % 255);
        if (pwrite(fd, &byte, 1, at) != 1) die("flip failed");
      }
    }
  }
  close(fd);
}
/* A whole random slot is beyond any correction. */
static int destroy(void) {
  if (control) return 0;
  int fd = open(wal, O_RDWR);
  struct stat info;
  if (fd < 0) return 0;
  int done = 0;
  if (fstat(fd, &info) == 0 && info.st_size >= WAL_START + 4 * SLOT) {
    long long frames = (info.st_size - WAL_START) / SLOT;
    long long frame = frames / 4 + random() % (frames - frames / 4);
    char bytes[SLOT];
    for (int i = 0; i < SLOT; i++) bytes[i] = (char)random();
    done = pwrite(fd, bytes, SLOT, WAL_START + (off_t)frame * SLOT) == SLOT;
  }
  close(fd);
  return done;
}
/* Connections then find no valid index and run recovery while they are alive. This
 * stands in for a writer that died while updating the header. A live writer owns that
 * header, so take its lock: scribbling under one breaks SQLite with no VFS involved. */
static void invalidate_index(void) {
  static const char zeros[96];
  int fd = open(shm, O_RDWR);
  if (fd < 0) return;
  struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = SHM_WRITE_LOCK, .l_len = 1};
  if (fcntl(fd, F_SETLKW, &lock)) die("index lock: %s", strerror(errno));
  if (pwrite(fd, zeros, sizeof(zeros), 0) != (ssize_t)sizeof(zeros)) die("index write failed");
  close(fd);
}
static void remove_epoch(const char *directory) {
  DIR *listing = opendir(directory);
  if (!listing) return;
  char path[4400];
  for (struct dirent *entry; (entry = readdir(listing));) {
    if (entry->d_name[0] == '.') continue;
    snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    unlink(path);
  }
  closedir(listing);
  rmdir(directory);
}
static int run_step(void (*step)(void)) {
  pid_t pid = fork();
  if (pid < 0) die("fork: %s", strerror(errno));
  if (pid == 0) {
    step();
    _exit(EXIT_FAILED);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) die("wait: %s", strerror(errno));
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
static void epoch_run(const char *base, int epoch, Kind kind, long long *kills, long long *flips,
                      long long *outcomes) {
  char directory[4096];
  snprintf(directory, sizeof(directory), "%s/e%d", base, epoch);
  if (mkdir(directory, 0700)) die("mkdir: %s", strerror(errno));
  snprintf(database, sizeof(database), "%s/bank", directory);
  snprintf(wal, sizeof(wal), "%s-wal", database);
  snprintf(shm, sizeof(shm), "%s-shm", database);
  for (int index = 0; index < WRITERS; index++) atomic_store(&shared->acknowledged[index], 0);
  atomic_store(&shared->relaxed, 0);
  atomic_store(&shared->stopping, 0);
  atomic_store(&shared->pause, 0);
  refused = 0;
  if (run_step(prepare)) die("prepare failed");
  spawn_missing();
  long long commits = atomic_load(&shared->commits);
  double progressed = now();
  int rounds = kind == REPAIR ? 0 : 6 + (int)(random() % 7);
  if (kind == REPAIR) {
    /* Give maintenance time for batching, write-lock retries, and repair commits. */
    for (int round = 0; round < 30; round++) {
      usleep(100000);
      flip(1);
      *flips += 1;
      reap();
    }
  }
  for (int round = 0; round < rounds; round++) {
    usleep((useconds_t)(100000 + random() % 150000));
    reap();
    int action = (int)(random() % 10);
    if (action < 5) {
      shoot((int)(random() % CHILDREN));
      *kills += 1;
    } else if (action < 7) {
      for (int slot = 0; slot < CHILDREN; slot++) shoot(slot);
      *kills += CHILDREN;
      await_children(STALL_SECONDS);
    } else {
      flip(0);
      *flips += 1;
    }
    reap();
    spawn_missing();
    long long current = atomic_load(&shared->commits);
    if (current != commits) {
      commits = current;
      progressed = now();
    } else if (now() - progressed > STALL_SECONDS) {
      die("no commit for %.0f seconds", STALL_SECONDS);
    }
  }
  if (kind >= DEAD_CUT && random() % 2) {
    atomic_store(&shared->pause, 1);
    for (double waited = now(); atomic_load(&shared->pause) == 1 && now() - waited < 1.0;) {
      usleep(1000);
      reap();
      spawn_missing();
    }
    atomic_store(&shared->pause, 2);
    usleep((useconds_t)(100000 + random() % 200000));
  }
  if (kind == DEAD_CUT) {
    for (int slot = 0; slot < CHILDREN; slot++) shoot(slot);
    *kills += CHILDREN;
    await_children(STALL_SECONDS);
    atomic_store(&shared->relaxed, 1);
    destroy();
    spawn_missing();                    /* Every role now races to recover. */
  } else if (kind == LIVE_CUT) {
    atomic_store(&shared->relaxed, !control);
    destroy();
    for (int attempt = 0; attempt < INVALIDATIONS; attempt++) {
      usleep(5000);
      invalidate_index();
      reap();
      spawn_missing();
    }
  }
  if (kind >= DEAD_CUT) {
    for (int round = 0; round < 3; round++) {
      usleep((useconds_t)(100000 + random() % 100000));
      reap();
      if (random() % 2) {
        shoot((int)(random() % CHILDREN));
        *kills += 1;
      }
      reap();
      spawn_missing();
    }
  }
  atomic_store(&shared->stopping, 1);
  await_children(STALL_SECONDS);
  int verified = run_step(verify);
  if (verified == EXIT_REFUSED && atomic_load(&shared->relaxed)) refused = 1;
  else if (verified) die("verification failed with %d", verified);
  outcomes[refused ? 1 : 0] += 1;
  remove_epoch(directory);
}
int main(int argc, char **argv) {
  if (argc != 5 && argc != 6) {
    fprintf(stderr, "usage: stress <extension> <directory> <seconds> <seed> [only-kind]\n");
    return 2;
  }
  extension = argv[1];
  if (getenv("STRESS_VFS")) {
    vfs_name = getenv("STRESS_VFS");
    control = 1;
  }
  double deadline = now() + atof(argv[3]);
  srandom((unsigned)atol(argv[4]));
  signal(SIGPIPE, SIG_IGN);
  shared = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  if (shared == MAP_FAILED) die("mmap: %s", strerror(errno));
  long long kills = 0, flips = 0, outcomes[KINDS][2] = {{0}};
  int epoch = 0;
  while (now() < deadline) {
    Kind kind = argc == 6 ? (Kind)atoi(argv[5]) : (Kind)(epoch % KINDS);
    epoch_run(argv[2], epoch, kind, &kills, &flips, outcomes[kind]);
    epoch += 1;
  }
  printf("stress: %d epochs, %lld commits, %lld reads, %lld checkpoints, %lld kills, %lld flips, "
         "%lld busy, %lld repairs, %lld data errors;\n"
         "  cuts %lld discarding %lld commits; refusals %lld;\n"
         "  clean epochs %lld, repair %lld, dead-process %lld kept/%lld refused, live-process %lld kept/%lld refused\n",
         epoch, (long long)atomic_load(&shared->commits), (long long)atomic_load(&shared->reads),
         (long long)atomic_load(&shared->checkpoints), kills, flips,
         (long long)atomic_load(&shared->busy), (long long)atomic_load(&shared->replacements),
         (long long)atomic_load(&shared->data_errors), (long long)atomic_load(&shared->cuts),
         (long long)atomic_load(&shared->cut_commits), (long long)atomic_load(&shared->refusals),
         outcomes[CLEAN][0], outcomes[REPAIR][0], outcomes[DEAD_CUT][0], outcomes[DEAD_CUT][1],
         outcomes[LIVE_CUT][0], outcomes[LIVE_CUT][1]);
  return 0;
}
