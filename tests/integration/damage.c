/* Storage damage and resource scenarios through real SQLite on real files.
 * Each mode is one process; tools/test-damage composes them and asserts results.
 * Offsets assume the defaults: 8 KiB slots, with main at 8 KiB and WAL at 16 KiB. A slot's
 * second sector is the page in `parity`, and the second copy in `copy`. */
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "sqlite3.h"
#include "monstervfs.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SLOT 8192
#define MAIN_START 8192
#define WAL_START 16384
#define WAL_PAYLOAD (SLOT - 4120)

static const char *vfs;
static char logged[1024];

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "damage: %s\n", message); exit(1); }
}
static double now(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}
static void on_log(void *unused, int code, const char *message) {
  (void)unused;
  if (strncmp(message, "monstervfs ", 11) == 0) snprintf(logged, sizeof(logged), "%d %s", code, message);
}
static void load(const char *extension) {
  sqlite3 *loader = NULL;
  char *error = NULL;
  require(sqlite3_config(SQLITE_CONFIG_LOG, on_log, NULL) == SQLITE_OK, "log hook failed");
  require(sqlite3_open(":memory:", &loader) == SQLITE_OK, "loader open failed");
  sqlite3_enable_load_extension(loader, 1);
  if (sqlite3_load_extension(loader, extension, MVFS_ENTRY, &error)) {
    fprintf(stderr, "load: %s\n", error);
    exit(1);
  }
  sqlite3_close(loader);
}
static sqlite3 *open_database(const char *path, int flags) {
  sqlite3 *db = NULL;
  int code = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | flags, vfs);
  if (code != SQLITE_OK) {
    fprintf(stderr, "open %s: %d %s\n", path, code, db ? sqlite3_errmsg(db) : "");
    exit(1);
  }
  return db;
}
static void configure(sqlite3 *db) {
  require(sqlite3_exec(db, "PRAGMA page_size=4096; PRAGMA journal_mode=WAL;"
                           "PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;",
                       NULL, NULL, NULL) == SQLITE_OK, "configuration failed");
}
static void sql(sqlite3 *db, const char *text) {
  char *error = NULL;
  int code = sqlite3_exec(db, text, NULL, NULL, &error);
  if (code != SQLITE_OK) {
    fprintf(stderr, "%s: %d %s\n", text, code, error ? error : "");
    exit(1);
  }
}
static int query(sqlite3 *db, const char *text, sqlite3_int64 *value) {
  sqlite3_stmt *statement = NULL;
  int code = sqlite3_prepare_v2(db, text, -1, &statement, NULL);
  if (code == SQLITE_OK) {
    code = sqlite3_step(statement);
    if (code == SQLITE_ROW) {
      if (value) *value = sqlite3_column_int64(statement, 0);
      code = SQLITE_OK;
    } else if (code == SQLITE_DONE) {
      code = SQLITE_OK;
    }
  }
  sqlite3_finalize(statement);
  return code ? sqlite3_extended_errcode(db) : SQLITE_OK;
}
static sqlite3_int64 scalar(sqlite3 *db, const char *text) {
  sqlite3_int64 value = -1;
  int code = query(db, text, &value);
  if (code != SQLITE_OK) { fprintf(stderr, "%s: %d\n", text, code); exit(1); }
  return value;
}
static void integrity(sqlite3 *db) {
  sqlite3_stmt *statement = NULL;
  require(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &statement, NULL) == SQLITE_OK, "integrity prepare");
  require(sqlite3_step(statement) == SQLITE_ROW, "integrity has no row");
  require(strcmp((const char *)sqlite3_column_text(statement, 0), "ok") == 0, "integrity check failed");
  sqlite3_finalize(statement);
}
static void status(sqlite3 *db, char *output, size_t capacity) {
  sqlite3_stmt *statement = NULL;
  require(sqlite3_prepare_v2(db, "PRAGMA monstervfs_maintenance", -1, &statement, NULL) == SQLITE_OK, "status prepare");
  require(sqlite3_step(statement) == SQLITE_ROW, "status has no row");
  snprintf(output, capacity, "%s", sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
}
static sqlite3_int64 status_field(sqlite3 *db, const char *name) {
  char text[2048], key[128];
  status(db, text, sizeof(text));
  snprintf(key, sizeof(key), "\"%s\":", name);
  const char *found = strstr(text, key);
  require(found != NULL, "status field missing");
  return atoll(found + strlen(key));
}
static void corrupt(const char *path, off_t offset, int bytes) {
  unsigned char buffer[256];
  require(bytes <= (int)sizeof(buffer), "corruption too large");
  int fd = open(path, O_RDWR);
  require(fd >= 0, "physical open failed");
  require(pread(fd, buffer, (size_t)bytes, offset) == bytes, "physical read failed");
  for (int i = 0; i < bytes; i++) buffer[i] ^= 0x5a;
  require(pwrite(fd, buffer, (size_t)bytes, offset) == bytes, "physical write failed");
  fsync(fd);
  close(fd);
}
/* Rot that takes the page with it, given an offset in the slot's second sector. */
static void rot(const char *path, off_t offset) {
  corrupt(path, offset, 80);
  if (MVFS_COPY) corrupt(path, offset - 4096, 80);
}
static void erase(const char *path, off_t offset, int bytes) {
  static const char zeros[SLOT];
  int fd = open(path, O_RDWR);
  require(fd >= 0 && pwrite(fd, zeros, (size_t)bytes, offset) == bytes, "physical erase failed");
  fsync(fd);
  close(fd);
}
static off_t file_size(const char *path) {
  struct stat info;
  return stat(path, &info) ? -1 : info.st_size;
}
static void companion(char *output, size_t capacity, const char *path, const char *suffix) {
  snprintf(output, capacity, "%s%s", path, suffix);
}
static int scrub_page(sqlite3 *db, sqlite3_stmt *statement, int page) {
  sqlite3_bind_int(statement, 1, page);
  int code = sqlite3_step(statement);
  int result = code == SQLITE_ROW ? SQLITE_OK : sqlite3_extended_errcode(db);
  sqlite3_reset(statement);
  return result;
}
static off_t wal_payload(int frame) { return WAL_START + (off_t)frame * SLOT + WAL_PAYLOAD + 100; }

/* Forty accounts on separate pages: any consistent state sums to 40000. Later
 * transfers credit accounts that earlier frames never mention. */
static void bank(const char *path, const char *variant) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "CREATE TABLE account(id INTEGER PRIMARY KEY, balance INT NOT NULL, pad BLOB);"
          "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<40)"
          "INSERT INTO account SELECT x,1000,zeroblob(3600) FROM c;"
          "PRAGMA wal_checkpoint(TRUNCATE);");
  for (int transfer = 1; transfer <= 120; transfer++) {
    int from = (transfer * 11 % 20) + 1;
    int to = transfer <= 60 ? ((transfer * 7 + 3) % 20) + 1 : 21 + (transfer % 20);
    if (from == to) to = (to % 20) + 1;
    char text[200];
    snprintf(text, sizeof(text), "BEGIN IMMEDIATE; UPDATE account SET balance=balance-1 WHERE id=%d;"
             "UPDATE account SET balance=balance+1 WHERE id=%d; COMMIT;", from, to);
    sql(db, text);
  }
  int frame = 60;
  if (strcmp(variant, "clean") == 0) sql(db, "PRAGMA wal_checkpoint(PASSIVE)");
  else if (strcmp(variant, "rot") == 0) rot(wal, wal_payload(frame));
  else if (strcmp(variant, "rot-checkpoint") == 0) {
    rot(wal, wal_payload(frame));
    sql(db, "PRAGMA wal_checkpoint(PASSIVE)");
  } else if (strcmp(variant, "checkpoint-rot") == 0) {
    sql(db, "PRAGMA wal_checkpoint(PASSIVE)");
    rot(wal, wal_payload(frame));
  } else if (strcmp(variant, "no-control") == 0) {
    /* With no record to hold a watermark, a checkpoint must not touch main. */
    char before[SLOT], after[SLOT];
    int fd = open(path, O_RDONLY);
    require(fd >= 0 && pread(fd, before, SLOT, MAIN_START + 5 * SLOT) == SLOT, "main read failed");
    for (int region = 0; region < 2; region++) {
      corrupt(wal, MAIN_START + region * 4096, 200);
      if (MVFS_COPY) corrupt(wal, MAIN_START + region * 4096 + 2048, 200);
    }
    require(sqlite3_exec(db, "PRAGMA wal_checkpoint(PASSIVE)", NULL, NULL, NULL) != SQLITE_OK,
            "a checkpoint ran without recording its watermark");
    require(pread(fd, after, SLOT, MAIN_START + 5 * SLOT) == SLOT, "main reread failed");
    require(memcmp(before, after, SLOT) == 0, "an unrecorded checkpoint changed main");
    close(fd);
  } else if (strcmp(variant, "zero") == 0) erase(wal, WAL_START + (off_t)frame * SLOT, SLOT);
  else if (strcmp(variant, "checkpoint-zero") == 0) {
    sql(db, "PRAGMA wal_checkpoint(PASSIVE)");
    erase(wal, WAL_START + (off_t)frame * SLOT, SLOT);
  } else require(0, "unknown variant");
  fflush(stdout);
  _exit(0); /* No close: the WAL must survive for recovery. */
}
static void bank_consistent(const char *path, const char *expect) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  off_t before = file_size(wal);
  sqlite3 *db = open_database(path, 0);
  sqlite3_int64 sum = -1;
  int code = query(db, "SELECT sum(balance) FROM account", &sum);
  if (strcmp(expect, "refuse") == 0) {
    require(code == MONSTERVFS_CORRUPT_BACKFILLED, "recovery did not refuse a backfilled WAL");
    require((code & 255) == SQLITE_CORRUPT, "refusal is not an SQLITE_CORRUPT family code");
    require(file_size(wal) == before, "a refused recovery changed the WAL");
    require(strstr(logged, "refused") != NULL, "refusal was not logged");
    require(status_field(db, "wal_refusals") >= 1, "refusal missing from status");
    require(query(db, "SELECT sum(balance) FROM account", &sum) == code, "refusal is not stable");
    return;
  }
  require(code == SQLITE_OK, "consistent recovery failed");
  require(sum == 40000, "retained state is not atomic");
  integrity(db);
  if (strcmp(expect, "cut") == 0) {
    require(status_field(db, "wal_cutoffs") == 1, "cutoff missing from status");
    require(status_field(db, "wal_cutoff_commits") > 0, "finished commits missing from status");
    require(strstr(logged, "cut WAL") != NULL, "cutoff was not logged");
    require(file_size(wal) < before, "cutoff did not shorten the WAL");
  } else {
    require(status_field(db, "wal_cutoffs") == 0, "clean recovery reported a cutoff");
  }
  sql(db, "UPDATE account SET balance=balance WHERE id=1");
}

/* Every committed frame is healthy; only bytes nothing needs are damaged. */
static void dead_slot(const char *path) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);"
          "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<100)"
          "INSERT INTO t SELECT x,randomblob(3000) FROM c;"
          "PRAGMA wal_checkpoint(PASSIVE); INSERT INTO t VALUES(1000,'restart');");
  rot(wal, wal_payload(40));
  for (int i = 0; i < 80; i++) {
    char text[100];
    snprintf(text, sizeof(text), "INSERT INTO t VALUES(%d,randomblob(3000))", 2000 + i);
    sql(db, text);
  }
  require(scalar(db, "SELECT count(*) FROM t") == 181, "rows are missing");
  integrity(db);
}

/* A real short write while a checkpoint extends main, stopping mid-slot. */
static void disk_full(const char *path) {
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB); INSERT INTO t VALUES(1,zeroblob(100));"
          "PRAGMA wal_checkpoint(TRUNCATE);"
          "WITH RECURSIVE c(x) AS(VALUES(2) UNION ALL SELECT x+1 FROM c WHERE x<120)"
          "INSERT INTO t SELECT x,randomblob(3000) FROM c;");
  signal(SIGXFSZ, SIG_IGN);
  struct rlimit limit = {(rlim_t)(file_size(path) + 5 * SLOT + SLOT / 2), RLIM_INFINITY};
  require(setrlimit(RLIMIT_FSIZE, &limit) == 0, "file limit failed");
  require(sqlite3_exec(db, "PRAGMA wal_checkpoint(PASSIVE)", NULL, NULL, NULL) != SQLITE_OK,
          "checkpoint unexpectedly fit");
  require((file_size(path) - MAIN_START) % SLOT == SLOT / 2, "short write did not stop mid-slot");
  /* The failed write does not take reads down with it. */
  require(scalar(db, "SELECT count(*) FROM t") == 120, "reads failed after a full disk");
  fflush(stdout);
  _exit(0);
}
static void disk_full_reopen(const char *path) {
  sqlite3 *db = open_database(path, 0);
  require(scalar(db, "SELECT count(*) FROM t") == 120, "rows lost after a full disk");
  sql(db, "PRAGMA wal_checkpoint(TRUNCATE); INSERT INTO t VALUES(5000,'after');");
  require(scalar(db, "SELECT count(*) FROM t") == 121, "write failed after a full disk");
}

/* An all-zero slot is an accepted limit for main; it must at least be visible. */
static void erased_page(const char *path) {
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "CREATE TABLE b(id INTEGER PRIMARY KEY, v BLOB NOT NULL);"
          "INSERT INTO b VALUES(1,CAST(hex(randomblob(5000)) AS BLOB));"
          "PRAGMA wal_checkpoint(TRUNCATE);");
  sqlite3_int64 page = scalar(db, "SELECT max(pageno) FROM dbstat WHERE name='b' AND pagetype='overflow'");
  sql(db, "PRAGMA shrink_memory");
  erase(path, MAIN_START + (off_t)(page - 1) * SLOT, SLOT);
  sqlite3 *reader = open_database(path, 0);
  require(scalar(reader, "SELECT hex(substr(v,-16))=printf('%032d',0) FROM b") == 1, "erased page read back");
  require(status_field(reader, "zero_slot_reads") > 0, "zero slot read was not counted");
  sqlite3_close(reader);
}

static int physical_byte(const char *path, off_t offset) {
  unsigned char byte = 0;
  int fd = open(path, O_RDONLY);
  require(fd >= 0 && pread(fd, &byte, 1, offset) == 1, "physical read failed");
  close(fd);
  return byte;
}
static void row_count(const char *path, sqlite3_int64 expected) {
  sqlite3 *db = open_database(path, 0);
  require(scalar(db, "SELECT count(*) FROM t") == expected, "rows are missing");
  require(scalar(db, "SELECT count(*) FROM t WHERE id<1 OR id>200 OR typeof(v)!='blob'"
                     " OR v IS NOT CAST(printf('%08d:',id)||hex(zeroblob(500+(id%4)*1000)) AS BLOB)") == 0,
          "row identity or exact payload changed");
  if (expected == 100) require(scalar(db, "SELECT count(*) FROM t WHERE id%2=0") == 0,
                               "deleted rows returned");
  require(scalar(db, "SELECT count(*) FROM sqlite_schema WHERE name='ix' AND type='index'") == 1,
          "index disappeared");
  integrity(db);
  require(physical_byte(path, MAIN_START + SLOT - 4096 + 20) == 48, "reserved bytes are missing");
  require(sqlite3_close(db) == SQLITE_OK, "verification close failed");
}
/* Who gets the reserved bytes, and who keeps them. */
static void reserved_bytes(const char *path) {
  char into[4096], copy[4096], plain[4096], text[4400];
  companion(into, sizeof(into), path, "-into");
  companion(copy, sizeof(copy), path, "-copy");
  companion(plain, sizeof(plain), path, "-plain");
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  require(scalar(db, "PRAGMA page_size") == 4096, "page_size changed");
  sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB); CREATE INDEX ix ON t(v);"
          "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<200)"
          "INSERT INTO t SELECT x,CAST(printf('%08d:',x)||hex(zeroblob(500+(x%4)*1000)) AS BLOB) FROM c;"
          "PRAGMA wal_checkpoint(TRUNCATE);");
  row_count(path, 200);
  sql(db, "DELETE FROM t WHERE id%2=0; VACUUM; PRAGMA wal_checkpoint(TRUNCATE);");
  row_count(path, 100);
  snprintf(text, sizeof(text), "VACUUM INTO '%s'", into);
  sql(db, text);
  row_count(into, 100);
  sqlite3 *target = open_database(copy, SQLITE_OPEN_CREATE);
  sqlite3_backup *backup = sqlite3_backup_init(target, "main", db, "main");
  require(backup != NULL, "backup init failed");
  require(sqlite3_backup_step(backup, -1) == SQLITE_DONE, "backup failed");
  require(sqlite3_backup_finish(backup) == SQLITE_OK, "backup finish failed");
  require(sqlite3_close(target) == SQLITE_OK, "backup target close failed");
  row_count(copy, 100);
  sqlite3 *native = NULL;
  require(sqlite3_open_v2(plain, &native, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix") == SQLITE_OK,
          "plain open failed");
  sql(native, "CREATE TABLE t(a)");
  sqlite3_close(native);
  require(physical_byte(plain, 20) == 0, "a plain database was given reserved bytes");
  require(sqlite3_close(db) == SQLITE_OK, "source close failed");
  row_count(path, 100);
}
static void overwrite(const char *path, off_t offset, int bytes, int value) {
  char buffer[4096];
  require(bytes <= (int)sizeof(buffer), "overwrite too large");
  memset(buffer, value, (size_t)bytes);
  int fd = open(path, O_RDWR);
  require(fd >= 0 && pwrite(fd, buffer, (size_t)bytes, offset) == bytes, "physical overwrite failed");
  fsync(fd);
  close(fd);
}
/* A whole first sector gone from a main page, with its page sector intact. */
static void lost_metadata_page(const char *path) {
  sqlite3 *owner = open_database(path, SQLITE_OPEN_CREATE);
  sqlite3_busy_timeout(owner, 5000);
  configure(owner);
  sql(owner, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);"
             "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<50)"
             "INSERT INTO t SELECT x,randomblob(3000) FROM c;"
             "PRAGMA wal_checkpoint(TRUNCATE);");
  overwrite(path, MAIN_START + 19 * SLOT, 4096, 0xa5);
  sql(owner, "PRAGMA shrink_memory");
  sqlite3 *reader = open_database(path, 0);
  require(scalar(reader, "SELECT count(*) FROM t") == 50, "rows are missing");
  integrity(reader);
  sqlite3_close(reader);
  double deadline = now() + 10;
  while (status_field(owner, "committed_replacements") < 1) {
    require(now() < deadline, "the lost sector was not repaired");
    usleep(10000);
  }
  sql(owner, "PRAGMA wal_checkpoint(TRUNCATE)");
  require(status_field(owner, "main_damage") == 0, "checkpoint did not retire the damage");
  require(physical_byte(path, MAIN_START + 19 * SLOT) != 0xa5, "the repair left the sector damaged");
}
/* A device acknowledged a slot's write and kept only one of its sectors. They prove two
 * different writes, and nothing says which is newer, so the page must fail: answering
 * with the old one would be silent, and a repair would then make it permanent. */
static void lost_sector_write(const char *path) {
  char old[SLOT], kept[SLOT], after[SLOT];
  off_t slot = MAIN_START + 19 * SLOT;
  sqlite3 *owner = open_database(path, SQLITE_OPEN_CREATE);
  sqlite3_busy_timeout(owner, 5000);
  configure(owner);
  sql(owner, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);"
             "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<50)"
             "INSERT INTO t SELECT x,randomblob(3000) FROM c;"
             "PRAGMA wal_checkpoint(TRUNCATE);");
  int fd = open(path, O_RDWR);
  require(fd >= 0 && pread(fd, old, SLOT, slot) == SLOT, "old slot read failed");
  sql(owner, "UPDATE t SET v=randomblob(3000); PRAGMA wal_checkpoint(TRUNCATE); PRAGMA shrink_memory");
  require(pwrite(fd, old + SLOT / 2, SLOT / 2, slot + SLOT / 2) == SLOT / 2, "lost write failed");
  require(fsync(fd) == 0 && pread(fd, kept, SLOT, slot) == SLOT, "slot read failed");
  sqlite3 *reader = open_database(path, 0);
  sqlite3_stmt *statement = NULL;
  require(sqlite3_prepare_v2(reader, "SELECT data FROM sqlite_dbpage WHERE schema='main' AND pgno=20",
                             -1, &statement, NULL) == SQLITE_OK, "page read prepare failed");
  require(scrub_page(reader, statement, 20) == SQLITE_IOERR_DATA, "a slot holding two writes was read");
  sqlite3_finalize(statement);
  sqlite3_close(reader);
  usleep(200000);
  require(status_field(owner, "committed_replacements") == 0, "a repair chose one of the writes");
  require(pread(fd, after, SLOT, slot) == SLOT && memcmp(kept, after, SLOT) == 0, "the evidence changed");
  close(fd);
}
/* Each copy alone is a whole page, whichever one goes. */
static void lost_copy(const char *path) {
  sqlite3 *owner = open_database(path, SQLITE_OPEN_CREATE);
  sqlite3_busy_timeout(owner, 5000);
  configure(owner);
  sql(owner, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);"
             "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<50)"
             "INSERT INTO t SELECT x,randomblob(3000) FROM c;"
             "PRAGMA wal_checkpoint(TRUNCATE);");
  overwrite(path, MAIN_START + 19 * SLOT + 4096, 4096, 0xa5);
  overwrite(path, MAIN_START + 30 * SLOT, 4096, 0);
  sql(owner, "PRAGMA shrink_memory");
  sqlite3 *reader = open_database(path, 0);
  require(scalar(reader, "SELECT count(*) FROM t") == 50, "rows are missing");
  integrity(reader);
  sqlite3_close(reader);
  double deadline = now() + 10;
  while (status_field(owner, "committed_replacements") < 2) {
    require(now() < deadline, "the lost copies were not repaired");
    usleep(10000);
  }
  sql(owner, "PRAGMA wal_checkpoint(TRUNCATE)");
  require(status_field(owner, "main_damage") == 0, "checkpoint did not retire the damage");
  require(physical_byte(path, MAIN_START + 19 * SLOT + 4096) != 0xa5, "the repair left the copy damaged");
}
/* A size limit landing inside a frame cuts that frame whole, since no VFS can use part of one.
 * So nothing reads it, damage to it cannot stop the writer, and it stops counting as damage. */
static void trim_dead_frames(const char *path) {
  char wal[4200];
  snprintf(wal, sizeof(wal), "%s-wal", path);
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "PRAGMA journal_size_limit=38112;"     /* Inside frame 9: 32 + 9 * 4120 + 1000. */
          "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);"
          "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<50)"
          "INSERT INTO t SELECT x,randomblob(3000) FROM c;");
  require(file_size(wal) > WAL_START + 20 * SLOT, "the WAL is too short to trim");
  overwrite(wal, WAL_START + 9 * SLOT, 4096, 0xa5);   /* One sector: a read can correct this. */
  sql(db, "PRAGMA wal_checkpoint(PASSIVE)");
  require(status_field(db, "wal_damage") == 1, "the checkpoint did not notice the damage");
  overwrite(wal, WAL_START + 9 * SLOT + 4096, 4096, 0xa5);   /* Now neither sector can. */
  sql(db, "INSERT INTO t VALUES(51,randomblob(3000))");
  sql(db, "INSERT INTO t VALUES(52,randomblob(3000))");
  require(file_size(wal) == WAL_START + 9 * SLOT, "the trim did not cut the whole frame");
  require(status_field(db, "wal_damage") == 0, "a frame that is gone still counts as damage");
  require(scalar(db, "SELECT count(*) FROM t") == 52, "rows are missing");
  integrity(db);
  sqlite3_close(db);
  db = open_database(path, 0);
  require(scalar(db, "SELECT count(*) FROM t") == 52, "rows are missing after reopening");
  integrity(db);
  sqlite3_close(db);
}
/* Settings a build does not have fail at open, rather than being ignored. */
static void settings(const char *path) {
  char uri[4200];
  sqlite3 *db = NULL;
  snprintf(uri, sizeof(uri), "file:%s?mv_parity=32", path);
  int code = sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, vfs);
  sqlite3_close(db);
  require(MVFS_COPY ? code != SQLITE_OK : code == SQLITE_OK, "mv_parity was not handled by build");
  const char *options[] = {"mv_block=1024", "mv_block=4096", "mv_block=65536",
                           "mv_align=512", "mv_align=4096"};
  for (unsigned i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
    snprintf(uri, sizeof(uri), "file:%s-geometry%u?%s", path, i, options[i]);
    db = NULL;
    code = sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, vfs);
    sqlite3_close(db);
    require(code == SQLITE_CANTOPEN, "a removed geometry option was accepted");
  }
}

/* The same for a WAL frame after a crash, whose header lived in the lost sector. */
static void lost_metadata_frame_prepare(const char *path) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB); PRAGMA wal_checkpoint(TRUNCATE);");
  for (int i = 0; i < 10; i++) sql(db, "INSERT INTO t(v) VALUES(randomblob(100))");
  overwrite(wal, WAL_START + 3 * SLOT, 4096, 0xa5);
}
static void lost_metadata_frame(const char *path) {
  sqlite3 *db = open_database(path, 0);
  require(scalar(db, "SELECT count(*) FROM t") == 10, "a recoverable frame was cut");
  integrity(db);
  require(status_field(db, "wal_cutoffs") == 0, "recovery reported a cutoff");
}

/* A frame erased whole reads cleanly, so only the watermark can refuse it. */
static void refusal_prepare(const char *path) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  sqlite3 *writer = open_database(path, SQLITE_OPEN_CREATE), *reader = NULL;
  configure(writer);
  sql(writer, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB); PRAGMA wal_checkpoint(TRUNCATE);");
  for (int i = 1; i <= 30; i++) {
    char text[100];
    snprintf(text, sizeof(text), "INSERT INTO t VALUES(%d,randomblob(2000))", i);
    sql(writer, text);
    if (i == 10) { reader = open_database(path, 0); sql(reader, "BEGIN; SELECT count(*) FROM t;"); }
    if (i == 20) sql(writer, "PRAGMA wal_checkpoint(PASSIVE)");
  }
  erase(wal, WAL_START + 2 * SLOT, SLOT);
  fflush(stdout);
  _exit(0);
}
/* Closing recovers in exclusive mode, out of sight of the hook that refused. */
static void refusal_survives_close(const char *path) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  off_t size = file_size(wal);
  for (int round = 0; round < 3; round++) {
    sqlite3 *db = open_database(path, 0);
    require(query(db, "SELECT count(*) FROM t", NULL) == MONSTERVFS_CORRUPT_BACKFILLED, "the refusal did not hold");
    sqlite3_close(db);
    require(file_size(wal) == size, "closing after a refusal changed the WAL");
  }
}

static int open_descriptors(void) {
  int count = 0;
  for (int fd = 0; fd < 4096; fd++) count += fcntl(fd, F_GETFD) != -1;
  return count;
}
static void descriptors(const char *path) {
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "CREATE TABLE t(a); INSERT INTO t VALUES(1);");
  int before = open_descriptors();
  for (int i = 0; i < 300; i++) {
    sqlite3 *other = open_database(path, 0);
    require(scalar(other, "SELECT count(*) FROM t") == 1, "short connection read failed");
    sqlite3_close(other);
  }
  require(open_descriptors() <= before + 4, "descriptors grow with connections");
}

/* A read-only mapping must never be written through, not even by the last close. */
static void readonly_shm_writer(const char *path) {
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1);");
  usleep(1500000);
  sqlite3_close(db);
}
static void readonly_shm_client(const char *path) {
  char uri[4200];
  snprintf(uri, sizeof(uri), "file:%s?readonly_shm=1", path);
  usleep(500000);
  sqlite3 *db = NULL;
  int code = sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, vfs);
  require(code == SQLITE_OK, "read-only SHM open failed");
  require(query(db, "SELECT count(*) FROM t", NULL) == SQLITE_OK, "read through read-only SHM failed");
  require((query(db, "INSERT INTO t VALUES(2)", NULL) & 255) == SQLITE_READONLY, "wrote with read-only SHM");
  require(sqlite3_close(db) == SQLITE_OK, "close with read-only SHM failed");
  /* The same state without the URI: an unwritable SHM file. */
  char shm[4096];
  companion(shm, sizeof(shm), path, "-shm");
  require(chmod(shm, 0400) == 0, "SHM chmod failed");
  db = open_database(path, 0);
  require(query(db, "SELECT count(*) FROM t", NULL) == SQLITE_OK, "read with unwritable SHM failed");
  usleep(1500000);
  require(sqlite3_close(db) == SQLITE_OK, "close failed");
  chmod(shm, 0600);
}

static void cutoff_prepare(const char *path) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB); PRAGMA wal_checkpoint(TRUNCATE);");
  for (int i = 0; i < 40; i++) {
    char text[100];
    snprintf(text, sizeof(text), "INSERT INTO t VALUES(%d,randomblob(3000))", i + 1);
    sql(db, text);
  }
  rot(wal, wal_payload(20));
  fflush(stdout);
  _exit(0);
}
/* Started together: every process must finish recovery without any waiting on
 * another to close, because in WAL mode an idle connection never drops SHARED. */
static void cutoff_open(const char *path, double at, double hold) {
  sqlite3 *db = open_database(path, 0);
  sqlite3_busy_timeout(db, 3000);
  while (now() < at) usleep(100);
  sqlite3_int64 rows = -1;
  int code = query(db, "SELECT count(*) FROM t", &rows);
  if (code != SQLITE_OK) { fprintf(stderr, "cutoff read: %d\n", code); exit(1); }
  require(rows > 0 && rows < 40, "cutoff did not apply");
  usleep((useconds_t)(hold * 1e6));
  sqlite3_close(db);
}
static void cutoff_exclusive(const char *path) {
  sqlite3 *db = open_database(path, 0);
  sql(db, "PRAGMA locking_mode=EXCLUSIVE");
  sqlite3_int64 rows = scalar(db, "SELECT count(*) FROM t");
  require(rows > 0 && rows < 40, "exclusive recovery did not apply a cutoff");
  sql(db, "INSERT INTO t VALUES(9999,'after')");
}
/* These build crash images by copying files under an open connection, which is what a
 * crash leaves once everything synced is on disk. The connection stays open, because
 * closing it would checkpoint: its descriptors are never used again. */
static void copy_file(const char *from, const char *to) {
  char data[65536];
  int in = open(from, O_RDONLY);
  require(in >= 0, "copy source failed");
  int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  require(out >= 0, "copy target failed");
  for (ssize_t n; (n = read(in, data, sizeof(data))) != 0;) {
    require(n > 0 && write(out, data, (size_t)n) == n, "copy failed");
  }
  close(out);
}
static void crash_copy(const char *from, const char *to) {
  char wal[4096], target[4096];
  companion(wal, sizeof(wal), from, "-wal");
  companion(target, sizeof(target), to, "-wal");
  copy_file(from, to);
  copy_file(wal, target);
}
static sqlite3 *committed(const char *path, int rows) {
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "PRAGMA wal_autocheckpoint=0; CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)");
  for (int row = 1; row <= rows; row++) sql(db, "INSERT INTO t(v) VALUES(randomblob(200))");
  return db;
}
/* The frame a commit in flight tore, leaving nothing that can prove it. One sector would
 * do in `parity`, but either copy alone is a whole frame in `copy`. */
static void tear_frame(const char *path, int frame) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  overwrite(wal, WAL_START + (off_t)frame * SLOT + 4096, 4096, 0);
  if (MVFS_COPY) overwrite(wal, WAL_START + (off_t)frame * SLOT, 4096, 255);
}
static sqlite3_int64 last_frame(sqlite3 *db) {
  sqlite3_stmt *statement = NULL;
  require(sqlite3_prepare_v2(db, "PRAGMA wal_checkpoint(NOOP)", -1, &statement, NULL) == SQLITE_OK, "frame count failed");
  require(sqlite3_step(statement) == SQLITE_ROW, "frame count failed");
  sqlite3_int64 frames = sqlite3_column_int64(statement, 1);
  sqlite3_finalize(statement);
  return frames;
}
static void opens_clean(const char *path, sqlite3_int64 least) {
  sqlite3 *db = open_database(path, 0);
  sqlite3_int64 rows = 0;
  int code = query(db, "SELECT count(*) FROM t", &rows);
  if (code != SQLITE_OK) { fprintf(stderr, "reopen: %d %s\n", code, sqlite3_errmsg(db)); exit(1); }
  require(rows >= least, "a crash lost a commit it should have kept");
  require(status_field(db, "wal_refusals") == 0, "an ordinary crash was refused");
  integrity(db);
  sql(db, "INSERT INTO t(v) VALUES(1)");
  sqlite3_close(db);
}
static void image_name(char *output, size_t capacity, const char *path, const char *suffix) {
  snprintf(output, capacity, "%s%s", path, suffix);
}
/* A torn tail, cut by an exclusive open, then a crash after its checkpoint. */
static void watermark_cut(const char *path) {
  char first[4096], second[4096];
  image_name(first, sizeof(first), path, "-first");
  image_name(second, sizeof(second), path, "-second");
  sqlite3 *db = committed(path, 40);
  sqlite3_int64 frames = last_frame(db);
  crash_copy(path, first);
  tear_frame(first, (int)frames - 1);
  sqlite3 *exclusive = open_database(first, 0);
  sql(exclusive, "PRAGMA locking_mode=EXCLUSIVE");
  require(scalar(exclusive, "SELECT count(*) FROM t") == 39, "the torn commit was not cut");
  sql(exclusive, "PRAGMA wal_checkpoint");
  crash_copy(first, second);
  opens_clean(second, 39);
}
/* An exclusive checkpoint on a reused WAL, a crash, then a torn commit in that WAL. */
static void watermark_reused(const char *path) {
  char first[4096], second[4096], third[4096];
  image_name(first, sizeof(first), path, "-first");
  image_name(second, sizeof(second), path, "-second");
  image_name(third, sizeof(third), path, "-third");
  sqlite3 *db = committed(path, 40);
  sql(db, "PRAGMA wal_checkpoint; INSERT INTO t(v) VALUES(2)");
  crash_copy(path, first);
  sqlite3 *exclusive = open_database(first, 0);
  sql(exclusive, "PRAGMA locking_mode=EXCLUSIVE");
  require(scalar(exclusive, "SELECT count(*) FROM t") == 41, "the first crash lost a commit");
  sql(exclusive, "PRAGMA wal_checkpoint");
  crash_copy(first, second);
  sqlite3 *shared = open_database(second, 0);
  sql(shared, "PRAGMA wal_autocheckpoint=0; INSERT INTO t(v) VALUES(3)");
  sqlite3_int64 frames = last_frame(shared);
  crash_copy(second, third);
  tear_frame(third, (int)frames - 1);
  opens_clean(third, 41);
}
/* Frames of a rolled-back transaction stay in the file, but no checkpoint copies them. */
static void watermark_rollback(const char *path) {
  char first[4096], second[4096];
  image_name(first, sizeof(first), path, "-first");
  image_name(second, sizeof(second), path, "-second");
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "PRAGMA locking_mode=EXCLUSIVE; PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=4;"
          "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB); INSERT INTO t(v) VALUES(1)");
  sql(db, "BEGIN; WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<60)"
          "INSERT INTO t(v) SELECT randomblob(1500) FROM c; ROLLBACK");
  sql(db, "PRAGMA wal_checkpoint");
  crash_copy(path, first);
  sqlite3 *shared = open_database(first, 0);
  sql(shared, "PRAGMA wal_autocheckpoint=0; INSERT INTO t(v) VALUES(2)");
  sqlite3_int64 frames = last_frame(shared);
  crash_copy(first, second);
  tear_frame(second, (int)frames - 1);
  opens_clean(second, 1);
}
/* A frame only its page sector proves stays in the file once the WAL restarts. It reads as
 * unusable then, but it sits past the end of the log, so it must not move the watermark. */
static void watermark_rebuilt(const char *path) {
  char first[4096], second[4096], wal[4096];
  image_name(first, sizeof(first), path, "-first");
  image_name(second, sizeof(second), path, "-second");
  sqlite3 *db = committed(path, 40);
  sqlite3_int64 frames = last_frame(db);
  crash_copy(path, first);
  companion(wal, sizeof(wal), first, "-wal");
  overwrite(wal, WAL_START + (off_t)(frames - 1) * SLOT, 4096, 255);
  sqlite3 *exclusive = open_database(first, 0);
  sql(exclusive, "PRAGMA locking_mode=EXCLUSIVE");
  require(scalar(exclusive, "SELECT count(*) FROM t") == 40, "the page sector did not vouch for its frame");
  sql(exclusive, "PRAGMA wal_checkpoint; INSERT INTO t(v) VALUES(1); PRAGMA wal_checkpoint");
  crash_copy(first, second);
  opens_clean(second, 41);
}
/* The same, with that frame directly after the log, where a scan first reaches it. */
static void watermark_rebuilt_next(const char *path) {
  char first[4096], second[4096], wal[4096];
  image_name(first, sizeof(first), path, "-first");
  image_name(second, sizeof(second), path, "-second");
  sqlite3 *db = committed(path, 40);
  sqlite3_int64 frames = last_frame(db);
  crash_copy(path, first);
  companion(wal, sizeof(wal), first, "-wal");
  overwrite(wal, WAL_START + (off_t)(frames - 1) * SLOT, 4096, 255);
  sqlite3 *exclusive = open_database(first, 0);
  sql(exclusive, "PRAGMA locking_mode=EXCLUSIVE");
  require(scalar(exclusive, "SELECT count(*) FROM t") == 40, "the page sector did not vouch for its frame");
  sql(exclusive, "PRAGMA wal_checkpoint");
  /* The first update restarts the WAL. One that changes nothing would write no frame. */
  sql(exclusive, "UPDATE t SET v=randomblob(200) WHERE id=1");
  for (int tries = 0; tries < 1000 && last_frame(exclusive) < frames - 1; tries++) {
    sql(exclusive, "UPDATE t SET v=randomblob(200) WHERE id=1");
  }
  require(last_frame(exclusive) == frames - 1, "the new log did not end beside the old frame");
  sql(exclusive, "PRAGMA wal_checkpoint");
  crash_copy(first, second);
  opens_clean(second, 40);
}
/* Damage to a dead frame costs nothing, wherever it sits and whatever the WAL has done. */
static void watermark_dead_frame(const char *path, int restart, int gap) {
  char first[4096], second[4096], wal[4096];
  image_name(first, sizeof(first), path, "-first");
  image_name(second, sizeof(second), path, "-second");
  sqlite3 *db = committed(path, 40);
  if (!restart) sql(db, "PRAGMA wal_checkpoint; INSERT INTO t(v) VALUES(2)");
  crash_copy(path, first);
  companion(wal, sizeof(wal), first, "-wal");
  sqlite3 *exclusive = open_database(first, 0);
  sql(exclusive, "PRAGMA locking_mode=EXCLUSIVE");
  require(scalar(exclusive, "SELECT count(*) FROM t") >= 40, "the first crash lost a commit");
  if (restart) {
    sql(exclusive, "PRAGMA wal_checkpoint");
    for (int update = 0; update < 3; update++) {
      sql(exclusive, "UPDATE t SET v=randomblob(200) WHERE id=1");
    }
  }
  rot(wal, wal_payload((int)last_frame(exclusive) + gap));
  sql(exclusive, "PRAGMA wal_checkpoint");
  crash_copy(first, second);
  opens_clean(second, 40);
}
/* A write stops part way through a slot, the transaction rolls back, and work carries on. */
static void watermark_failed_write(const char *path, int restart) {
  char first[4096], second[4096], wal[4096];
  image_name(first, sizeof(first), path, "-first");
  image_name(second, sizeof(second), path, "-second");
  sqlite3 *db = committed(path, 5);
  (void)db;
  crash_copy(path, first);
  companion(wal, sizeof(wal), first, "-wal");
  sqlite3 *exclusive = open_database(first, 0);
  sql(exclusive, "PRAGMA locking_mode=EXCLUSIVE");
  require(scalar(exclusive, "SELECT count(*) FROM t") == 5, "the first crash lost a commit");
  if (restart) {
    /* After a restart the WAL overwrites its dead frames, so use them up to reach new space. */
    sqlite3_int64 slots = (file_size(wal) - WAL_START) / SLOT;
    sql(exclusive, "PRAGMA wal_checkpoint; UPDATE t SET v=randomblob(200) WHERE id=1");
    for (int tries = 0; tries < 1000 && last_frame(exclusive) < slots; tries++) {
      sql(exclusive, "UPDATE t SET v=randomblob(200) WHERE id=1");
    }
  }
  signal(SIGXFSZ, SIG_IGN);
  struct rlimit limit = {(rlim_t)(file_size(wal) + SLOT / 2), RLIM_INFINITY};
  require(setrlimit(RLIMIT_FSIZE, &limit) == 0, "file limit failed");
  sql(exclusive, "PRAGMA cache_size=2");
  int code = sqlite3_exec(exclusive, "BEGIN; UPDATE t SET v=randomblob(250); COMMIT", NULL, NULL, NULL);
  sqlite3_exec(exclusive, "ROLLBACK", NULL, NULL, NULL);
  limit.rlim_cur = RLIM_INFINITY;
  require(setrlimit(RLIMIT_FSIZE, &limit) == 0, "file limit failed");
  require(code != SQLITE_OK, "the write unexpectedly fit");
  require((file_size(wal) - WAL_START) % SLOT != 0, "the failed write left a whole slot");
  sql(exclusive, "PRAGMA wal_checkpoint");
  crash_copy(first, second);
  opens_clean(second, 5);
}
/* The watermark still has to be right: damage to a frame a checkpoint copied is refused. */
static void watermark_refuses(const char *path) {
  char first[4096];
  image_name(first, sizeof(first), path, "-first");
  sqlite3 *db = open_database(path, SQLITE_OPEN_CREATE);
  configure(db);
  sql(db, "PRAGMA locking_mode=EXCLUSIVE; PRAGMA wal_autocheckpoint=0;"
          "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)");
  for (int row = 0; row < 40; row++) sql(db, "INSERT INTO t(v) VALUES(randomblob(200))");
  sqlite3_int64 frames = last_frame(db);
  sql(db, "PRAGMA wal_checkpoint");
  crash_copy(path, first);
  char wal[4096];
  companion(wal, sizeof(wal), first, "-wal");
  rot(wal, wal_payload((int)frames - 1));
  sqlite3 *reopened = open_database(first, 0);
  int code = query(reopened, "SELECT count(*) FROM t", NULL);
  require(code == MONSTERVFS_CORRUPT_BACKFILLED, "damage below the watermark was not refused");
}
/* A reader inside a snapshot may need every frame, so it must block a cutoff.
 * The driver runs the blocked process while this one holds its snapshot. */
static void cutoff_reader(const char *path) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  sqlite3 *reader = open_database(path, SQLITE_OPEN_CREATE);
  configure(reader);
  sql(reader, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB); PRAGMA wal_checkpoint(TRUNCATE);");
  for (int i = 0; i < 40; i++) {
    char text[100];
    snprintf(text, sizeof(text), "INSERT INTO t VALUES(%d,randomblob(3000))", i + 1);
    sql(reader, text);
  }
  sql(reader, "BEGIN");
  require(scalar(reader, "SELECT count(*) FROM t") == 40, "snapshot read failed");
  rot(wal, wal_payload(20));
  /* The driver invalidates the wal-index: closing a descriptor here would drop
   * this process's POSIX read lock, which is the thing under test. */
  puts("ready");
  fflush(stdout);
  require(getchar() != EOF, "driver went away");
  sql(reader, "COMMIT");
  sqlite3_close(reader);
}
static void cutoff_blocked(const char *path) {
  char wal[4096];
  companion(wal, sizeof(wal), path, "-wal");
  off_t before = file_size(wal);
  sqlite3 *db = open_database(path, 0);
  int code = query(db, "SELECT count(*) FROM t", NULL);
  require((code & 255) == SQLITE_BUSY || (code & 255) == SQLITE_PROTOCOL, "a live snapshot did not block the cutoff");
  require(file_size(wal) == before, "a blocked recovery changed the WAL");
}
static void cutoff_released(const char *path) {
  sqlite3 *db = open_database(path, 0);
  sqlite3_int64 rows = scalar(db, "SELECT count(*) FROM t");
  require(rows > 0 && rows < 40, "cutoff did not apply after the snapshot ended");
}

/* MonsterFt scrubs by reading pages in stages through a separate connection. Any
 * damaged page such a read finds must queue a repair on the worker. */
static void scrub(const char *path) {
  sqlite3 *owner = open_database(path, SQLITE_OPEN_CREATE);
  sqlite3_busy_timeout(owner, 5000);
  configure(owner);
  sql(owner, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);"
             "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<120)"
             "INSERT INTO t SELECT x,randomblob(3000) FROM c;"
             "PRAGMA wal_checkpoint(TRUNCATE);");
  int pages = (int)scalar(owner, "PRAGMA page_count");
  int flipped[] = {30, 77, 110}, lost = 90;
  for (int i = 0; i < 3; i++) corrupt(path, MAIN_START + (off_t)(flipped[i] - 1) * SLOT + SLOT - 1, 1);
  rot(path, MAIN_START + (off_t)(lost - 1) * SLOT + SLOT - 200);
  int unreadable = 0;
  for (int first = 1; first <= pages; first += 25) {
    sqlite3 *stage = open_database(path, 0);
    sqlite3_stmt *statement = NULL;
    require(sqlite3_prepare_v2(stage, "SELECT data FROM sqlite_dbpage WHERE schema='main' AND pgno=?1",
                               -1, &statement, NULL) == SQLITE_OK, "scrub prepare failed");
    for (int page = first; page < first + 25 && page <= pages; page++) {
      int code = scrub_page(stage, statement, page);
      if (code == SQLITE_OK) continue;
      require(code == SQLITE_IOERR_DATA && page == lost, "scrub read failed unexpectedly");
      unreadable++;
    }
    sqlite3_finalize(statement);
    require(sqlite3_close(stage) == SQLITE_OK, "scrub close failed");
  }
  require(unreadable == 1, "the unrecoverable page was not reported to the scrub");
  double deadline = now() + 10;
  while (status_field(owner, "committed_replacements") < 3) {
    require(now() < deadline, "scrub-found damage was not repaired");
    usleep(10000);
  }
  require(status_field(owner, "main_damage") == 3, "repairs claimed physical retirement early");
  sql(owner, "PRAGMA wal_checkpoint(TRUNCATE)");
  require(status_field(owner, "main_damage") == 0, "checkpoint did not retire the damage");
  sqlite3 *again = open_database(path, 0);
  sqlite3_stmt *statement = NULL;
  require(sqlite3_prepare_v2(again, "SELECT data FROM sqlite_dbpage WHERE schema='main' AND pgno=?1",
                             -1, &statement, NULL) == SQLITE_OK, "rescan prepare failed");
  for (int i = 0; i < 3; i++) {
    require(scrub_page(again, statement, flipped[i]) == SQLITE_OK, "repaired page failed a rescan");
  }
  sqlite3_finalize(statement);
  sqlite3_close(again);
  require(status_field(owner, "committed_replacements") == 0, "a clean rescan queued more work");
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: damage EXTENSION MODE DATABASE [ARG...]\n"); return 2; }
  vfs = getenv("MONSTERVFS_TEST_VFS") ? getenv("MONSTERVFS_TEST_VFS") : MVFS_NAME;
  load(argv[1]);
  const char *mode = argv[2], *path = argv[3];
  if (strcmp(mode, "bank") == 0) bank(path, argv[4]);
  else if (strcmp(mode, "bank-check") == 0) bank_consistent(path, argv[4]);
  else if (strcmp(mode, "dead-slot") == 0) dead_slot(path);
  else if (strcmp(mode, "disk-full") == 0) disk_full(path);
  else if (strcmp(mode, "disk-full-reopen") == 0) disk_full_reopen(path);
  else if (strcmp(mode, "erased-page") == 0) erased_page(path);
  else if (strcmp(mode, "descriptors") == 0) descriptors(path);
  else if (strcmp(mode, "shm-writer") == 0) readonly_shm_writer(path);
  else if (strcmp(mode, "shm-client") == 0) readonly_shm_client(path);
  else if (strcmp(mode, "cutoff-prepare") == 0) cutoff_prepare(path);
  else if (strcmp(mode, "cutoff-open") == 0) cutoff_open(path, atof(argv[4]), atof(argv[5]));
  else if (strcmp(mode, "cutoff-exclusive") == 0) cutoff_exclusive(path);
  else if (strcmp(mode, "watermark-cut") == 0) watermark_cut(path);
  else if (strcmp(mode, "watermark-reused") == 0) watermark_reused(path);
  else if (strcmp(mode, "watermark-rollback") == 0) watermark_rollback(path);
  else if (strcmp(mode, "watermark-rebuilt") == 0) watermark_rebuilt(path);
  else if (strcmp(mode, "watermark-rebuilt-next") == 0) watermark_rebuilt_next(path);
  else if (strcmp(mode, "watermark-dead-next") == 0) watermark_dead_frame(path, 1, 0);
  else if (strcmp(mode, "watermark-dead-past") == 0) watermark_dead_frame(path, 1, 3);
  else if (strcmp(mode, "watermark-dead-open") == 0) watermark_dead_frame(path, 0, 0);
  else if (strcmp(mode, "watermark-write") == 0) watermark_failed_write(path, 0);
  else if (strcmp(mode, "watermark-write-restart") == 0) watermark_failed_write(path, 1);
  else if (strcmp(mode, "watermark-refuses") == 0) watermark_refuses(path);
  else if (strcmp(mode, "cutoff-reader") == 0) cutoff_reader(path);
  else if (strcmp(mode, "cutoff-blocked") == 0) cutoff_blocked(path);
  else if (strcmp(mode, "cutoff-released") == 0) cutoff_released(path);
  else if (strcmp(mode, "scrub") == 0) scrub(path);
  else if (strcmp(mode, "reserved-bytes") == 0) reserved_bytes(path);
  else if (strcmp(mode, "refusal-prepare") == 0) refusal_prepare(path);
  else if (strcmp(mode, "refusal-close") == 0) refusal_survives_close(path);
  else if (strcmp(mode, "lost-metadata-page") == 0) lost_metadata_page(path);
  else if (strcmp(mode, "lost-sector-write") == 0) lost_sector_write(path);
  else if (strcmp(mode, "lost-copy") == 0) lost_copy(path);
  else if (strcmp(mode, "trim-dead-frames") == 0) trim_dead_frames(path);
  else if (strcmp(mode, "settings") == 0) settings(path);
  else if (strcmp(mode, "lost-metadata-frame-prepare") == 0) lost_metadata_frame_prepare(path);
  else if (strcmp(mode, "lost-metadata-frame") == 0) lost_metadata_frame(path);
  else return 2;
  fflush(stdout);
  _exit(0);
}
