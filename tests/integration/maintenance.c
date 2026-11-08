#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "maintenance: %s\n", message); exit(1); }
}
static void ok(int rc, sqlite3 *db, const char *operation) {
  if (rc != SQLITE_OK) {
    fprintf(stderr, "%s: %d %s\n", operation, rc, db ? sqlite3_errmsg(db) : ""); exit(1);
  }
}
static void sql(sqlite3 *db, const char *text) {
  char *error = NULL;
  int rc = sqlite3_exec(db, text, NULL, NULL, &error);
  if (rc != SQLITE_OK) { fprintf(stderr, "%s: %d %s\n", text, rc, error ? error : sqlite3_errmsg(db)); exit(1); }
  sqlite3_free(error);
}
static void preserved_value(sqlite3 *db) {
  sqlite3_stmt *stmt = NULL;
  ok(sqlite3_prepare_v2(db, "SELECT value FROM repair WHERE id=1", -1, &stmt, NULL), db, "read prepare");
  require(sqlite3_step(stmt) == SQLITE_ROW, "corrected row missing");
  const unsigned char *value = sqlite3_column_text(stmt, 0);
  require(value && strcmp((const char *)value, "preserved") == 0, "corrected value differs");
  ok(sqlite3_finalize(stmt), db, "read finalize");
}
static void status(sqlite3 *db, char *output, size_t capacity) {
  sqlite3_stmt *stmt = NULL;
  ok(sqlite3_prepare_v2(db, "PRAGMA monstervfs_maintenance", -1, &stmt, NULL), db, "status prepare");
  require(sqlite3_step(stmt) == SQLITE_ROW, "status has no row");
  snprintf(output, capacity, "%s", sqlite3_column_text(stmt, 0));
  ok(sqlite3_finalize(stmt), db, "status finalize");
}
static void pause_briefly(void) { struct timespec delay = {0, 10000000}; nanosleep(&delay, NULL); }
static void read_bytes(int fd, off_t offset, void *bytes, size_t count) {
  require(pread(fd, bytes, count, offset) == (ssize_t)count, "physical read failed");
}
static void damage_byte(int fd, off_t offset) {
  unsigned char byte; read_bytes(fd, offset, &byte, 1); byte ^= 0x01;
  require(pwrite(fd, &byte, 1, offset) == 1, "fault injection write failed");
  require(fsync(fd) == 0, "fault injection fsync failed");
}
static sqlite3 *open_database(const char *path) {
  sqlite3 *db = NULL;
  ok(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, MVFS_NAME), db, "open");
  ok(sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL), db, "disable close checkpoint");
  sql(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
  return db;
}
static void wait_status(sqlite3 *db,const char *expected) {
  char state[4096];
  for(int attempt=0;attempt<500;attempt++) {
    status(db,state,sizeof(state));
    if(strstr(state,expected)) return;
    pause_briefly();
  }
  fprintf(stderr,"maintenance: wanted %s, got %s\n",expected,state);exit(1);
}
static void value_is(sqlite3 *db,const char *table,const char *expected) {
  char query[256];snprintf(query,sizeof(query),"SELECT value FROM %s WHERE id=1",table);
  sqlite3_stmt *stmt=NULL;
  ok(sqlite3_prepare_v2(db,query,-1,&stmt,NULL),db,"current value prepare");
  require(sqlite3_step(stmt)==SQLITE_ROW,"current value missing");
  const char *value=(const char *)sqlite3_column_text(stmt,0);
  require(value && strcmp(value,expected)==0,"repair restored stale contents");
  require(sqlite3_step(stmt)==SQLITE_DONE,"unexpected current rows");
  ok(sqlite3_finalize(stmt),db,"current value finalize");
}
/* A broken page must not permanently suppress its repairable batch neighbor. */
static void mixed_damage(const char *base) {
  char path[4096];require(snprintf(path,sizeof(path),"%s-mixed",base)<sizeof(path),"mixed path too long");
  sqlite3 *db=open_database(path);
  sql(db,"CREATE TABLE filler(id INTEGER PRIMARY KEY,value TEXT);"
         "CREATE TABLE bad(id INTEGER PRIMARY KEY,value TEXT);"
         "CREATE TABLE good(id INTEGER PRIMARY KEY,value TEXT);"
         "INSERT INTO bad VALUES(1,'bad'); INSERT INTO good VALUES(1,'current');"
         "PRAGMA wal_checkpoint(TRUNCATE);");
  int fd=open(path,O_RDWR);require(fd>=0,"mixed main open");
  const off_t bad=8192+2*8192,good=8192+3*8192;
  sql(db,"BEGIN IMMEDIATE;");
  damage_byte(fd,bad+4096+4094);damage_byte(fd,good+4096+4094);
  sql(db,"PRAGMA shrink_memory; SELECT data FROM sqlite_dbpage WHERE pgno IN (3,4);");
  wait_status(db,"\"blocked\":true");
  char state[4096];status(db,state,sizeof(state));
  require(strstr(state,"\"pending_pages\":2")!=NULL,"mixed pages not queued");
  /* Exceed parity capacity and damage both page copies, leaving page4 repairable. */
  for(int region=0;region<2;region++) {
    unsigned char bytes[64];off_t at=bad+region*4096+64;
    read_bytes(fd,at,bytes,sizeof(bytes));
    for(int i=0;i<64;i++) bytes[i]^=(unsigned char)(i+1);
    require(pwrite(fd,bytes,sizeof(bytes),at)==sizeof(bytes),"mixed damage write");
  }
  require(fsync(fd)==0,"mixed damage sync");sql(db,"ROLLBACK;");
  wait_status(db,"\"committed_replacements\":1");
  status(db,state,sizeof(state));
  require(strstr(state,"\"pending_pages\":0") && strstr(state,"\"main_damage\":2") &&
          strstr(state,"\"code\":8202"),"bad page falsely reported repaired");
  value_is(db,"good","current");
  sql(db,"PRAGMA shrink_memory;");
  int rc=sqlite3_exec(db,"SELECT data FROM sqlite_dbpage WHERE pgno=3",NULL,NULL,NULL);
  require(rc==SQLITE_IOERR && sqlite3_extended_errcode(db)==SQLITE_IOERR_DATA,"bad page no longer reports damage");
  value_is(db,"good","current");
  sql(db,"PRAGMA wal_checkpoint(TRUNCATE);");
  status(db,state,sizeof(state));
  require(strstr(state,"\"main_damage\":1")!=NULL,"checkpoint did not retire only the repaired page");
  close(fd);ok(sqlite3_close(db),NULL,"mixed close");
  db=open_database(path);value_is(db,"good","current");ok(sqlite3_close(db),NULL,"mixed reopen close");
  puts("unrecoverable page leaves its batch neighbor repairable without closing the application");
}
static pthread_t application_thread;
static _Atomic int reject_commit,rejected_commits;
static int commit_hook(void *unused) {
  (void)unused;
  if(!atomic_load(&reject_commit)) return 0;
  atomic_fetch_add(&rejected_commits,1);return 1;
}
static int worker_extension(sqlite3 *db,char **error,const sqlite3_api_routines *api) {
  (void)error;(void)api;
  if(!pthread_equal(application_thread,pthread_self())) sqlite3_commit_hook(db,commit_hook,NULL);
  return SQLITE_OK;
}
/* Repair queued before a writer's commit must use the current page, even when
 * that page now belongs to another table. A rejected repair commit must retry. */
static void current_pages(const char *base) {
  application_thread=pthread_self();
  ok(sqlite3_auto_extension((void(*)(void))worker_extension),NULL,"install worker commit hook");
  for(int mode=0;mode<3;mode++) {
    char path[4096];require(snprintf(path,sizeof(path),"%s-current-%d",base,mode)<sizeof(path),"current path too long");
    sqlite3 *db=open_database(path);
    sql(db,"CREATE TABLE repair(id INTEGER PRIMARY KEY,value TEXT); INSERT INTO repair VALUES(1,'original');"
           "PRAGMA wal_checkpoint(TRUNCATE); BEGIN IMMEDIATE;");
    int fd=open(path,O_RDWR);require(fd>=0,"current main open");
    damage_byte(fd,8192+8192+4096+4094);
    sql(db,"PRAGMA shrink_memory; SELECT data FROM sqlite_dbpage WHERE pgno=2;");
    value_is(db,"repair","original");wait_status(db,"\"blocked\":true");
    const char *table="repair";
    if(mode==1) {
      sql(db,"DROP TABLE repair; CREATE TABLE replacement(id INTEGER PRIMARY KEY,value TEXT);"
             "INSERT INTO replacement VALUES(1,'current');");
      table="replacement";
    } else sql(db,"UPDATE repair SET value='current';");
    if(mode==2) atomic_store(&reject_commit,1);
    sql(db,"COMMIT;");
    if(mode==2) {
      wait_status(db,"\"code\":531");value_is(db,table,"current");
      char state[4096];status(db,state,sizeof(state));
      require(atomic_load(&rejected_commits)>0 && strstr(state,"\"committed_replacements\":0"),
              "rejected repair counted as committed");
      atomic_store(&reject_commit,0);
    }
    wait_status(db,"\"committed_replacements\":1");value_is(db,table,"current");
    sql(db,"PRAGMA wal_checkpoint(TRUNCATE);");wait_status(db,"\"main_damage\":0");
    close(fd);ok(sqlite3_close(db),NULL,"current close");
    db=open_database(path);value_is(db,table,"current");ok(sqlite3_close(db),NULL,"current reopen close");
  }
  sqlite3_cancel_auto_extension((void(*)(void))worker_extension);
  puts("newer WAL pages, reused pages, and rejected repair commits preserve current contents");
}
int main(int argc, char **argv) {
  if (argc != 3) { fprintf(stderr, "usage: maintenance EXTENSION DATABASE\n"); return 2; }
  sqlite3 *loader = NULL;
  ok(sqlite3_open(":memory:", &loader), loader, "loader open");
  ok(sqlite3_db_config(loader, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1, NULL), loader, "enable extension");
  char *error = NULL;
  int rc = sqlite3_load_extension(loader, argv[1], MVFS_ENTRY, &error);
  if (rc != SQLITE_OK) { fprintf(stderr, "load: %s\n", error); return 1; }
  ok(sqlite3_close(loader), NULL, "loader close");
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  require(library != NULL, "dlopen failed");
  int (*shutdown)(void) = (int (*)(void))dlsym(library, "monstervfs_shutdown");
  require(shutdown != NULL, "shutdown symbol missing");

  sqlite3 *db = open_database(argv[2]);
  sql(db, "CREATE TABLE repair(id INTEGER PRIMARY KEY, value TEXT); INSERT INTO repair VALUES(1,'preserved'); PRAGMA wal_checkpoint(TRUNCATE);");
  int fd = open(argv[2], O_RDWR);
  require(fd >= 0, "physical main open failed");
  // Default format: 8KiB prefix, 8KiB slots with the 4KiB payload at the end.
  const off_t slot = 8192 + 8192;
  unsigned char original[8192], changed[8192];
  read_bytes(fd, slot, original, sizeof(original));
  damage_byte(fd, slot + 4096 + 4095);
  sql(db, "PRAGMA shrink_memory; SELECT data FROM sqlite_dbpage WHERE pgno=2;");
  preserved_value(db);

  char state[4096];
  int completed = 0;
  for (int attempt=0; attempt<500; attempt++) {
    status(db, state, sizeof(state));
    if (strstr(state, "\"committed_replacements\":1")) { completed=1; break; }
    pause_briefly();
  }
  if (!completed) { fprintf(stderr, "repair status: %s\n", state); return 1; }
  require(strstr(state, "\"main_damage\":1") != NULL, "replacement incorrectly retired physical damage");
  read_bytes(fd, slot, changed, sizeof(changed));
  require(memcmp(original, changed, sizeof(original)) != 0, "worker implicitly checkpointed main");
  char wal_path[4096]; snprintf(wal_path, sizeof(wal_path), "%s-wal", argv[2]);
  struct stat wal;
  require(stat(wal_path, &wal) == 0 && wal.st_size > 8192, "repair did not commit a WAL replacement");
  sql(db, "PRAGMA wal_checkpoint(TRUNCATE);");
  read_bytes(fd, slot, changed, sizeof(changed));
  require(memcmp(original, changed, sizeof(original)) == 0, "application checkpoint did not retire main damage");
  status(db, state, sizeof(state));
  require(strstr(state, "\"main_damage\":0") != NULL, "physical retirement not reflected in status");

  unsigned char prefix[256], repaired[256];
  read_bytes(fd, 0, prefix, sizeof(prefix));
  damage_byte(fd, 24);
  sqlite3 *probe = open_database(argv[2]);
  int prefix_done = 0;
  for (int attempt=0; attempt<500; attempt++) {
    read_bytes(fd, 0, repaired, sizeof(repaired));
    if (memcmp(prefix, repaired, sizeof(prefix)) == 0) { prefix_done=1; break; }
    pause_briefly();
  }
  require(prefix_done, "discovered prefix damage was not repaired");
  ok(sqlite3_close(probe), NULL, "probe close");

  sql(db, "PRAGMA locking_mode=EXCLUSIVE; BEGIN EXCLUSIVE; COMMIT;");
  damage_byte(fd, slot + 4096 + 4094);
  sql(db, "PRAGMA shrink_memory; SELECT data FROM sqlite_dbpage WHERE pgno=2;");
  preserved_value(db);
  int blocked = 0;
  for (int attempt=0; attempt<500; attempt++) {
    status(db, state, sizeof(state));
    if (strstr(state, "\"blocked\":true")) { blocked=1; break; }
    pause_briefly();
  }
  if (!blocked) { fprintf(stderr, "exclusive repair status: %s\n", state); return 1; }
  require(strstr(state, "\"committed_replacements\":0") != NULL, "blocked repair claimed success");
  sql(db, "PRAGMA locking_mode=NORMAL; BEGIN; SELECT * FROM repair; COMMIT;");
  completed = 0;
  for (int attempt=0; attempt<500; attempt++) {
    status(db, state, sizeof(state));
    if (strstr(state, "\"committed_replacements\":1")) { completed=1; break; }
    pause_briefly();
  }
  if (!completed) { fprintf(stderr, "resumed repair status: %s\n", state); return 1; }
  read_bytes(fd, slot, changed, sizeof(changed));
  require(memcmp(original, changed, sizeof(original)) != 0, "resumed worker implicitly checkpointed");
  sql(db, "PRAGMA wal_checkpoint(TRUNCATE);");
  read_bytes(fd, slot, changed, sizeof(changed));
  require(memcmp(original, changed, sizeof(original)) == 0, "resumed repair was not checkpointed");
  close(fd);
  ok(sqlite3_close(db), NULL, "application close");
  current_pages(argv[2]);
  mixed_damage(argv[2]);
  ok(shutdown(), NULL, "maintenance shutdown");
  ok(sqlite3_shutdown(), NULL, "SQLite shutdown");
  puts("maintenance SQL replacement, explicit checkpoint retirement, and prefix repair passed");
  return 0;
}
