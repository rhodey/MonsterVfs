/* Exercise ordinary SQLite loading and connection lifetime without explicitly
 * shutting down the VFS. The host's callbacks pause a real maintenance commit. */
#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *vfs_name = MVFS_NAME;
static pthread_t application_thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int pause_commit, commit_entered, release_commit, close_started, close_finished;
static _Atomic int worker_connections;

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "lifecycle: %s\n", message); exit(1); }
}
static void ok(int rc, sqlite3 *db, const char *operation) {
  if (rc != SQLITE_OK) {
    fprintf(stderr, "lifecycle: %s: %d %s\n", operation, rc, db ? sqlite3_errmsg(db) : "");
    exit(1);
  }
}
static void sql(sqlite3 *db, const char *text) {
  char *error = NULL;
  int rc = sqlite3_exec(db, text, NULL, NULL, &error);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "lifecycle: %s: %d %s\n", text, rc, error ? error : ""); exit(1);
  }
  sqlite3_free(error);
}
static struct timespec deadline(int milliseconds) {
  struct timespec until;
  require(clock_gettime(CLOCK_REALTIME, &until) == 0, "clock failed");
  until.tv_sec += milliseconds / 1000;
  until.tv_nsec += (long)(milliseconds % 1000) * 1000000;
  if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
  return until;
}
/* Caller holds mutex. */
static void wait_for(int *flag, const char *message) {
  struct timespec until = deadline(10000);
  while (!*flag) require(pthread_cond_timedwait(&condition, &mutex, &until) == 0, message);
}
static void worker_closed(void *unused) {
  (void)unused;
  atomic_fetch_sub(&worker_connections, 1);
}
static void noop(sqlite3_context *context, int argc, sqlite3_value **argv) {
  (void)context; (void)argc; (void)argv;
}
static int commit_hook(void *unused) {
  (void)unused;
  pthread_mutex_lock(&mutex);
  if (pause_commit) {
    commit_entered = 1;
    pthread_cond_broadcast(&condition);
    while (!release_commit) pthread_cond_wait(&condition, &mutex);
  }
  pthread_mutex_unlock(&mutex);
  return 0;
}
static int observe_worker(sqlite3 *db, char **error, const sqlite3_api_routines *api) {
  (void)error; (void)api;
  if (!pthread_equal(pthread_self(), application_thread)) {
    atomic_fetch_add(&worker_connections, 1);
    int rc = sqlite3_create_function_v2(db, "test_worker_lifetime", 0, SQLITE_UTF8,
                                      NULL, noop, NULL, NULL, worker_closed);
    if (rc != SQLITE_OK) return rc;
    sqlite3_commit_hook(db, commit_hook, NULL);
  }
  return SQLITE_OK;
}
static sqlite3 *open_database(const char *path, int shared) {
  sqlite3 *db = NULL;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (shared) flags |= SQLITE_OPEN_SHAREDCACHE;
  ok(sqlite3_open_v2(path, &db, flags, vfs_name), db, "open");
  ok(sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL), db, "disable close checkpoint");
  sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
          "PRAGMA wal_autocheckpoint=0;");
  return db;
}
static void prepare_database(sqlite3 *db) {
  sql(db, "CREATE TABLE IF NOT EXISTS repair(value TEXT); DELETE FROM repair;"
          "INSERT INTO repair VALUES('preserved'); PRAGMA wal_checkpoint(TRUNCATE);");
}
static void queue_repair(sqlite3 *db, const char *path) {
  int fd = open(path, O_RDWR);
  require(fd >= 0, "physical open failed");
  /* Both default codecs have an 8 KiB prefix and 8 KiB slots. Damage the
   * second page's primary payload; its correction does not touch SQL values. */
  off_t offset = 8192 + 8192 + 4096 + 4095;
  unsigned char byte;
  require(pread(fd, &byte, 1, offset) == 1, "physical read failed");
  byte ^= 1;
  require(pwrite(fd, &byte, 1, offset) == 1 && fsync(fd) == 0, "physical damage failed");
  require(close(fd) == 0, "physical close failed");
  sql(db, "PRAGMA shrink_memory; SELECT data FROM sqlite_dbpage WHERE pgno=2;");
}
static void value_is_preserved(sqlite3 *db) {
  sqlite3_stmt *query = NULL;
  ok(sqlite3_prepare_v2(db, "SELECT value FROM repair", -1, &query, NULL), db, "value prepare");
  require(sqlite3_step(query) == SQLITE_ROW &&
          strcmp((const char *)sqlite3_column_text(query, 0), "preserved") == 0,
          "stored value changed");
  require(sqlite3_step(query) == SQLITE_DONE, "unexpected rows");
  ok(sqlite3_finalize(query), db, "value finalize");
}
static void await_repair(sqlite3 *db) {
  for (int i = 0; i < 1000; i++) {
    sqlite3_stmt *query = NULL;
    ok(sqlite3_prepare_v2(db, "PRAGMA monstervfs_maintenance", -1, &query, NULL), db, "status prepare");
    require(sqlite3_step(query) == SQLITE_ROW, "status row missing");
    const char *state = (const char *)sqlite3_column_text(query, 0);
    int repaired = strstr(state, "\"committed_replacements\":1") != NULL;
    if (i == 999) fprintf(stderr, "lifecycle: last status: %s\n", state);
    ok(sqlite3_finalize(query), db, "status finalize");
    if (repaired) return;
    struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL);
  }
  require(0, "later open did not resume maintenance");
}
/* Last close must quiesce, rather than permanently disabling future repairs. */
static void reopen_repair(const char *path) {
  for (int pass = 0; pass < 2; pass++) {
    sqlite3 *db = open_database(path, 0);
    prepare_database(db);
    queue_repair(db, path);
    await_repair(db);
    value_is_preserved(db);
    sql(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    ok(sqlite3_close(db), NULL, "repair close");
    require(atomic_load(&worker_connections) == 0, "close left a worker connection open");
  }
}
struct closing {
  sqlite3 *db;
  sqlite3_stmt *statement;
  int result;
};
static void *finish_close(void *argument) {
  struct closing *closing = argument;
  pthread_mutex_lock(&mutex);
  close_started = 1;
  pthread_cond_broadcast(&condition);
  pthread_mutex_unlock(&mutex);
  closing->result = closing->statement ? sqlite3_finalize(closing->statement) : sqlite3_close(closing->db);
  pthread_mutex_lock(&mutex);
  close_finished = 1;
  pthread_cond_broadcast(&condition);
  pthread_mutex_unlock(&mutex);
  return NULL;
}
/* Mode 0 closes directly; 1 finalizes a close_v2 zombie; 2 closes the last
 * shared-cache connection; 3 opens again while last close waits. A paused
 * worker owns an actual SQLite connection. */
static void inflight_close(const char *path, int mode) {
  sqlite3 *db = open_database(path, mode == 2);
  prepare_database(db);
  sqlite3 *other = mode == 2 ? open_database(path, 1) : NULL;
  struct closing closing = {db, NULL, SQLITE_ERROR};
  if (mode == 1) {
    ok(sqlite3_prepare_v2(db, "SELECT 1", -1, &closing.statement, NULL), db, "deferred prepare");
    require(sqlite3_close(db) == SQLITE_BUSY, "close with a live statement did not refuse");
  }
  pthread_mutex_lock(&mutex);
  pause_commit = 1; commit_entered = release_commit = close_started = close_finished = 0;
  pthread_mutex_unlock(&mutex);
  queue_repair(db, path);
  pthread_mutex_lock(&mutex);
  wait_for(&commit_entered, "worker did not reach its commit hook");
  pthread_mutex_unlock(&mutex);
  require(atomic_load(&worker_connections) > 0, "no in-flight worker connection");
  if (mode == 1) {
    ok(sqlite3_close_v2(db), NULL, "deferred close");
    require(sqlite3_step(closing.statement) == SQLITE_ROW, "zombie statement stopped working");
    require(sqlite3_step(closing.statement) == SQLITE_DONE, "zombie statement did not finish");
  } else if (mode == 2) {
    ok(sqlite3_close(db), NULL, "nonfinal shared-cache close");
    closing.db = other;
  }
  pthread_t thread;
  require(pthread_create(&thread, NULL, finish_close, &closing) == 0, "close thread failed");
  pthread_mutex_lock(&mutex);
  wait_for(&close_started, "close did not start");
  struct timespec until = deadline(200);
  int rc = 0;
  while (!close_finished && rc != ETIMEDOUT) {
    rc = pthread_cond_timedwait(&condition, &mutex, &until);
    require(rc == 0 || rc == ETIMEDOUT, "close condition wait failed");
  }
  require(!close_finished, "last close returned before its worker connection closed");
  if (mode == 3) {
    pthread_mutex_unlock(&mutex);
    other = open_database(path, 0);
    pthread_mutex_lock(&mutex);
    require(!close_finished, "concurrent open released an unfinished close barrier");
  }
  release_commit = 1;
  pthread_cond_broadcast(&condition);
  wait_for(&close_finished, "last close deadlocked with maintenance");
  pause_commit = 0;
  pthread_mutex_unlock(&mutex);
  require(pthread_join(thread, NULL) == 0, "close join failed");
  ok(closing.result, NULL, "final close");
  require(atomic_load(&worker_connections) == 0, "worker outlived final close");
  db = mode == 3 ? other : open_database(path, 0);
  value_is_preserved(db);
  ok(sqlite3_close(db), NULL, "in-flight reopen close");
}
static void load_library(const char *library) {
  sqlite3 *loader = NULL;
  ok(sqlite3_open(":memory:", &loader), loader, "loader open");
  ok(sqlite3_db_config(loader, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, NULL), loader, "enable load");
  char *error = NULL;
  int rc = sqlite3_load_extension(loader, library, NULL, &error);
  if (rc != SQLITE_OK) { fprintf(stderr, "lifecycle: load: %s\n", error); exit(1); }
  sqlite3_free(error);
  ok(sqlite3_close(loader), NULL, "loader close");
}
static void run_cases(const char *directory) {
  char path[4096];
  require(snprintf(path, sizeof(path), "%s/%s-reopen", directory, vfs_name) < (int)sizeof(path), "path too long");
  reopen_repair(path);
  for (int mode = 0; mode < 4; mode++) {
    require(snprintf(path, sizeof(path), "%s/%s-inflight-%d", directory, vfs_name, mode) < (int)sizeof(path), "path too long");
    inflight_close(path, mode);
  }
  printf("%s automatic close, in-flight repair, deferred/shared close, and concurrent reopen passed.\n", vfs_name);
}
int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) {
    fprintf(stderr, "usage: lifecycle EXTENSION DIRECTORY [OTHER-CODEC-EXTENSION]\n"); return 2;
  }
  application_thread = pthread_self();
  const char *names[] = {MVFS_NAME, MVFS_COPY ? "monstervfs_parity" : "monstervfs_copy"};
  int count = argc == 4 ? 2 : 1;
  load_library(argv[1]);
  if (count == 2) load_library(argv[3]);
  ok(sqlite3_auto_extension((void (*)(void))observe_worker), NULL, "worker observer install");
  for (int i = 0; i < count; i++) {
    vfs_name = names[i];
    run_cases(argv[2]);
  }
  sqlite3_cancel_auto_extension((void (*)(void))observe_worker);
  ok(sqlite3_shutdown(), NULL, "ordinary SQLite shutdown");
  /* SQLite shutdown clears autoextensions; loading again must restore ordinary
   * operation and repair without permanently stopping the registered service. */
  load_library(argv[1]);
  if (count == 2) load_library(argv[3]);
  ok(sqlite3_auto_extension((void (*)(void))observe_worker), NULL, "worker observer reinstall");
  for (int i = 0; i < count; i++) {
    vfs_name = names[i];
    char path[4096];
    require(snprintf(path, sizeof(path), "%s/%s-reopen", argv[2], vfs_name) < (int)sizeof(path), "path too long");
    sqlite3 *db = open_database(path, 0);
    value_is_preserved(db);
    ok(sqlite3_close(db), NULL, "reinitialized close");
    reopen_repair(path);
  }
  sqlite3_cancel_auto_extension((void (*)(void))observe_worker);
  ok(sqlite3_shutdown(), NULL, "reinitialized SQLite shutdown");
  puts("SQLite reinitialization and resumed maintenance passed without explicit VFS shutdown.");
  return 0;
}
