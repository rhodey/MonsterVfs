#define SQLITE_CORE 1
#include "sqlite3ext.h"
#include "sqlite_api.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int version = 3054000, mutexes = 1, dbpage = 1, source_matches = 1;
static int version_number(void) { return version; }
static int threadsafe(void) { return mutexes; }
static int compileoption(const char *option) {
  assert(strcmp(option, "ENABLE_DBPAGE_VTAB") == 0);
  return dbpage;
}
static const char *sourceid(void) {
  return source_matches ? SQLITE_SOURCE_ID : "different source";
}
static void *allocate(sqlite3_uint64 bytes) { return malloc((size_t)bytes); }
static void rejected(sqlite3_api_routines *table, const char *reason) {
  MvfsApi *api = NULL;
  char *message = NULL;
  assert(mvfs_api_capture(table, &api, &message) == SQLITE_ERROR);
  assert(!api);
  if (reason) { assert(message && strstr(message, reason)); }
  free(message);
}
int main(void) {
  sqlite3_api_routines table = {0};
  table.libversion_number = version_number;
  table.sourceid = sourceid;
  table.xthreadsafe = threadsafe;
  table.compileoption_used = compileoption;
  table.malloc64 = allocate;
  MvfsApi *api = NULL;
  assert(mvfs_api_capture(NULL, NULL, NULL) == SQLITE_MISUSE);
  rejected(NULL, NULL);
  version = 3053000; rejected(&table, NULL); version = 3054000;
  source_matches = 0; rejected(&table, "audited"); source_matches = 1;
  mutexes = 0; rejected(&table, "thread-safe"); mutexes = 1;
  dbpage = 0; rejected(&table, "DBPAGE"); dbpage = 1;
  assert(mvfs_api_capture(&table, &api, NULL) == SQLITE_OK && api);
  table.libversion_number = NULL;
  assert(mvfs_libversion_number(api) == 3054000);
  mvfs_api_free(api);
  puts("Host API compatibility and immutable capture passed");
  return 0;
}
