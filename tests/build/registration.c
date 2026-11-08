/* Registration failures must not leave pointers into an unloadable extension.
 * Capture the pinned host's real API table, replacing only selected calls. */
#define _POSIX_C_SOURCE 200809L
#define SQLITE_CORE 1
#include "sqlite3ext.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <time.h>
#endif

static sqlite3_api_routines host_api, injected_api;
static int captured;
static _Atomic int fail_auto, fail_publish, auto_installed, unrelated_calls;
static void (*target_callback)(void);
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_condition = PTHREAD_COND_INITIALIZER;
static int pause_auto, auto_entered, release_auto;
static int (*target_init)(sqlite3 *, char **, const sqlite3_api_routines *);
static sqlite3 *loader;

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "registration: %s\n", message); exit(1); }
}
static void ok(int code, const char *operation) {
  if (code != SQLITE_OK) {
    fprintf(stderr, "registration: %s: %d\n", operation, code);
    exit(1);
  }
}
static int capture_api(sqlite3 *db, char **error, const sqlite3_api_routines *api) {
  (void)db; (void)error;
  memcpy(&host_api, api, sizeof(host_api));
  captured = 1;
  return SQLITE_OK;
}
static int auto_extension(void (*callback)(void)) {
  target_callback = callback;
  if (atomic_exchange(&fail_auto, 0)) return SQLITE_NOMEM;
  int code = host_api.auto_extension(callback);
  if (code == SQLITE_OK) atomic_store(&auto_installed, 1);
  pthread_mutex_lock(&gate_mutex);
  if (pause_auto && code == SQLITE_OK) {
    auto_entered = 1;
    pthread_cond_broadcast(&gate_condition);
    while (!release_auto) pthread_cond_wait(&gate_condition, &gate_mutex);
  }
  pthread_mutex_unlock(&gate_mutex);
  return code;
}
static int register_vfs(sqlite3_vfs *vfs, int make_default) {
  if (strcmp(vfs->zName, MVFS_NAME) == 0 && atomic_exchange(&fail_publish, 0)) {
    require(atomic_load(&auto_installed), "publication failure preceded automatic extension installation");
    fprintf(stderr, "injected impossible VFS publication failure\n");
    fflush(stderr);
    return SQLITE_NOMEM;
  }
  return host_api.vfs_register(vfs, make_default);
}
#ifdef __linux__
static int task_count(void) {
  DIR *directory = opendir("/proc/self/task");
  require(directory != NULL, "cannot inspect Linux worker ownership");
  int count = 0;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL)
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') count++;
  require(closedir(directory) == 0, "task directory close failed");
  return count;
}
static void require_worker_joined(int before) {
  /* pthread_join may return just before /proc drops the exited task entry. */
  for (int i = 0; i < 100; i++) {
    if (task_count() == before) return;
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
  }
  require(0, "failed initialization left a worker thread alive");
}
#endif
static int initialize(void) {
  char *error = NULL;
  int code = target_init(loader, &error, &injected_api);
  if (error) fprintf(stderr, "registration diagnostic: %s\n", error);
  sqlite3_free(error);
  return code;
}
static void sql(sqlite3 *db, const char *text) {
  char *error = NULL;
  int code = sqlite3_exec(db, text, NULL, NULL, &error);
  if (code != SQLITE_OK) {
    fprintf(stderr, "registration SQL: %s: %d %s\n", text, code, error ? error : "");
    exit(1);
  }
  sqlite3_free(error);
}
static int unrelated_extension(sqlite3 *db, char **error, const sqlite3_api_routines *api) {
  (void)db; (void)error; (void)api;
  atomic_fetch_add(&unrelated_calls, 1);
  return SQLITE_OK;
}
static void ordinary_open(void) {
  int before = atomic_load(&unrelated_calls);
  sqlite3 *db = NULL;
  ok(sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix"),
     "unrelated native open");
  require(atomic_load(&unrelated_calls) > before, "unrelated automatic extension was removed");
  sql(db, "CREATE TABLE ordinary(value); INSERT INTO ordinary VALUES(42);");
  ok(sqlite3_close(db), "unrelated native close");
}
static void protected_database(const char *path) {
  sqlite3 *db = NULL;
  ok(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, MVFS_NAME),
     "protected open");
  int reserved = -1;
  ok(sqlite3_file_control(db, "main", SQLITE_FCNTL_RESERVE_BYTES, &reserved), "reserved bytes query");
  require(reserved == 48, "successful registration omitted page reservation");
  sql(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
          "CREATE TABLE t(id INTEGER PRIMARY KEY, value TEXT);"
          "INSERT INTO t VALUES(1,'registration survived'),(2,'exact contents');");
  ok(sqlite3_close(db), "protected close");
  ok(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, MVFS_NAME), "protected reopen");
  sqlite3_stmt *query = NULL;
  ok(sqlite3_prepare_v2(db, "SELECT id,value FROM t ORDER BY id", -1, &query, NULL), "query prepare");
  require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1 &&
          strcmp((const char *)sqlite3_column_text(query, 1), "registration survived") == 0,
          "first stored row differs");
  require(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 2 &&
          strcmp((const char *)sqlite3_column_text(query, 1), "exact contents") == 0,
          "second stored row differs");
  require(sqlite3_step(query) == SQLITE_DONE, "unexpected stored rows");
  ok(sqlite3_finalize(query), "query finalize");
  ok(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &query, NULL), "integrity prepare");
  require(sqlite3_step(query) == SQLITE_ROW &&
          strcmp((const char *)sqlite3_column_text(query, 0), "ok") == 0,
          "integrity check failed");
  require(sqlite3_step(query) == SQLITE_DONE, "extra integrity errors");
  ok(sqlite3_finalize(query), "integrity finalize");
  ok(sqlite3_close(db), "protected reopen close");
}
static void failed_registration(void) {
#ifdef __linux__
  int threads_before = task_count();
#endif
  atomic_store(&fail_auto, 1);
  require(initialize() == SQLITE_NOMEM, "injected initialization failure was not returned");
  require(!atomic_load(&fail_auto), "failure hook was not reached");
#ifdef __linux__
  require_worker_joined(threads_before);
#endif
  require(sqlite3_vfs_find(MVFS_NAME) == NULL, "failed initialization left its VFS published");
  /* Cancellation returns one if it actually removed a callback. Zero proves that
   * cleanup already removed it; do not leave a dangling callback after dlclose. */
  if (target_callback)
    require(host_api.cancel_auto_extension(target_callback) == 0,
            "failed initialization left an automatic extension installed");
}
static void existing_registration(void) {
  sqlite3_vfs *ours = sqlite3_vfs_find(MVFS_NAME);
  require(ours != NULL, "successful initialization did not publish its VFS");
  /* Removing this hook makes a later auto_extension allocation realistic. */
  require(host_api.cancel_auto_extension(target_callback) == 1, "reservation hook was absent");
  atomic_store(&fail_auto, 1);
  require(initialize() == SQLITE_NOMEM && !atomic_load(&fail_auto),
          "repeat initialization did not return the injected failure");
  require(sqlite3_vfs_find(MVFS_NAME) == ours, "repeat failure removed an owned VFS");
  require(host_api.cancel_auto_extension(target_callback) == 0, "failed repeat installed a hook");
  ordinary_open();
  require(initialize() == SQLITE_OK_LOAD_PERMANENTLY, "repeat initialization did not recover");

  /* A name collision must not authorize replacing another owner's VFS. */
  ok(sqlite3_vfs_unregister(ours), "unregister owned VFS");
  sqlite3_vfs foreign = *sqlite3_vfs_find("unix");
  foreign.zName = MVFS_NAME;
  foreign.pNext = NULL;
  ok(sqlite3_vfs_register(&foreign, 0), "register foreign VFS");
  require(initialize() == SQLITE_MISUSE, "foreign VFS was treated as owned");
  require(sqlite3_vfs_find(MVFS_NAME) == &foreign, "foreign registration was replaced");
  ordinary_open();
  ok(sqlite3_vfs_unregister(&foreign), "unregister foreign VFS");
  require(initialize() == SQLITE_OK_LOAD_PERMANENTLY, "owned VFS was not restored");
  require(sqlite3_vfs_find(MVFS_NAME) == ours, "restored VFS changed identity");
}
static void *initialize_thread(void *unused) {
  (void)unused;
  int *result = malloc(sizeof(*result));
  require(result != NULL, "thread result allocation failed");
  *result = initialize();
  return result;
}
static void concurrent_registration(void) {
  pause_auto = 1;
  pthread_t thread;
  ok(pthread_create(&thread, NULL, initialize_thread, NULL), "initializer thread");
  pthread_mutex_lock(&gate_mutex);
  while (!auto_entered) pthread_cond_wait(&gate_condition, &gate_mutex);
  pthread_mutex_unlock(&gate_mutex);
  /* The callback is visible to SQLite while its registering call is paused. */
  for (int i = 0; i < 8; i++) ordinary_open();
  pthread_mutex_lock(&gate_mutex);
  release_auto = 1;
  pthread_cond_broadcast(&gate_condition);
  pthread_mutex_unlock(&gate_mutex);
  void *result = NULL;
  ok(pthread_join(thread, &result), "initializer join");
  require(*(int *)result == SQLITE_OK_LOAD_PERMANENTLY, "concurrent initialization failed");
  free(result);
}
int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: registration EXTENSION MODE DATABASE\n");
    return 2;
  }
  ok(sqlite3_auto_extension((void (*)(void))capture_api), "API capture registration");
  ok(sqlite3_open(":memory:", &loader), "API capture open");
  require(captured, "host did not provide its API table");
  require(sqlite3_cancel_auto_extension((void (*)(void))capture_api) == 1,
          "API capture cleanup failed");
  memcpy(&injected_api, &host_api, sizeof(injected_api));
  injected_api.auto_extension = auto_extension;
  injected_api.vfs_register = register_vfs;
  ok(sqlite3_auto_extension((void (*)(void))unrelated_extension), "unrelated extension registration");
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  target_init = (int (*)(sqlite3 *, char **, const sqlite3_api_routines *))dlsym(library, MVFS_ENTRY);
  require(target_init != NULL, "extension entrypoint missing");
  if (strcmp(argv[2], "publish-abort") == 0) {
    atomic_store(&fail_publish, 1);
    initialize();
    require(0, "impossible publication failure returned instead of aborting");
  } else if (strcmp(argv[2], "auto-unload") == 0) {
    failed_registration();
    require(dlclose(library) == 0, "failed extension unload failed");
    require(sqlite3_vfs_find(MVFS_NAME) == NULL, "unloaded VFS remains discoverable");
    for (int i = 0; i < 8; i++) ordinary_open();
  } else {
    if (strcmp(argv[2], "auto-retry") == 0) {
      for (int i = 0; i < 3; i++) {
        failed_registration();
        ordinary_open();
      }
      require(initialize() == SQLITE_OK_LOAD_PERMANENTLY, "initialization retry failed");
      existing_registration();
    } else if (strcmp(argv[2], "concurrent") == 0) {
      concurrent_registration();
    } else {
      fprintf(stderr, "unknown registration mode: %s\n", argv[2]);
      return 2;
    }
    require(initialize() == SQLITE_OK_LOAD_PERMANENTLY, "duplicate initialization failed");
    protected_database(argv[3]);
    ordinary_open();
    int (*stop)(void) = (int (*)(void))dlsym(library, "monstervfs_shutdown");
    require(stop != NULL, "shutdown export missing");
    ok(stop(), "worker shutdown");
    /* Successful entrypoints are permanent. Keep the library loaded until exit. */
  }
  require(sqlite3_cancel_auto_extension((void (*)(void))unrelated_extension) == 1,
          "unrelated extension cleanup failed");
  ok(sqlite3_close(loader), "loader close");
  ok(sqlite3_shutdown(), "SQLite shutdown");
  printf("registration %s passed\n", argv[2]);
  return 0;
}
