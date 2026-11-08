#include "sqlite3.h"
#include "monstervfs.h"
#include <stdlib.h>

static void stop_worker(void) {
  if (monstervfs_shutdown() != SQLITE_OK) abort();
}

/* Test-only adapter: production registration never changes SQLite's default. */
int monstervfs_test_init(const char *unused) {
  static int registered_exit = 0;
  (void)unused;
  if (!registered_exit) {
    if (atexit(stop_worker)) return SQLITE_ERROR;
    registered_exit = 1;
  }
  int code = monstervfs_register();
  if (code != SQLITE_OK) return code;
  return sqlite3_vfs_register(sqlite3_vfs_find(MVFS_NAME), 1);
}
