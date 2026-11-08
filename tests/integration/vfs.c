#include "sqlite3.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int (*stop_vfs)(void);
static void check(int rc, sqlite3 *db, const char *where) {
  if(rc!=SQLITE_OK){fprintf(stderr,"%s: %d %s\n",where,rc,db?sqlite3_errmsg(db):"");exit(1);}
}
static void sql(sqlite3 *db,const char *s){char *e=0;int rc=sqlite3_exec(db,s,0,0,&e);if(rc){fprintf(stderr,"SQL %s: %d %s\n",s,rc,e?e:"");exit(1);}sqlite3_free(e);}
static sqlite3_int64 scalar(sqlite3 *db,const char *s){sqlite3_stmt*q=0;check(sqlite3_prepare_v2(db,s,-1,&q,0),db,s);if(sqlite3_step(q)!=SQLITE_ROW){fprintf(stderr,"no row: %s\n",s);exit(1);}sqlite3_int64 n=sqlite3_column_int64(q,0);check(sqlite3_finalize(q),db,"finalize");return n;}
static char *text(sqlite3 *db, const char *query) {
  sqlite3_stmt *q = NULL;
  check(sqlite3_prepare_v2(db, query, -1, &q, NULL), db, query);
  if (sqlite3_step(q) != SQLITE_ROW) { fprintf(stderr, "no row: %s\n", query); exit(1); }
  char *value = sqlite3_mprintf("%s", sqlite3_column_text(q, 0));
  if (!value) exit(1);
  check(sqlite3_finalize(q), db, "finalize");
  return value;
}
static uint64_t file_hash(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) { perror("open file for hash"); exit(1); }
  uint64_t hash = UINT64_C(14695981039346656037);
  unsigned char bytes[8192];
  size_t length;
  while ((length = fread(bytes, 1, sizeof(bytes), file)) != 0)
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
  if (ferror(file) || fclose(file)) { fprintf(stderr, "file hash failed\n"); exit(1); }
  return hash;
}
static sqlite3 *open_db(const char *path,int create){sqlite3*db=0;check(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_URI|(create?SQLITE_OPEN_CREATE:0),MVFS_NAME),db,"open");sqlite3_busy_timeout(db,5000);sql(db,"PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");return db;}
static void load(const char *path){sqlite3 *db=0;check(sqlite3_open(":memory:",&db),db,"loader");sqlite3_enable_load_extension(db,1);char *error=0;int rc=sqlite3_load_extension(db,path,MVFS_ENTRY,&error);if(rc){fprintf(stderr,"load: %s\n",error);exit(1);}check(sqlite3_close(db),0,"loader close");void*h=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!h){fprintf(stderr,"dlopen failed\n");exit(1);}stop_vfs=(int(*)(void))dlsym(h,"monstervfs_shutdown");if(!stop_vfs)exit(1);}
static void expect_fail(sqlite3 *db,const char *s){int rc=sqlite3_exec(db,s,0,0,0);if(rc==0){fprintf(stderr,"unexpected accepted SQL: %s\n",s);exit(1);}}
int main(int argc,char**argv){
  if(argc!=4){fprintf(stderr,"usage: integration EXTENSION DATABASE MODE\n");return 2;}load(argv[1]);
  const char *mode=argv[3];sqlite3*db=open_db(argv[2],1);
  if(strcmp(mode,"configured")==0){
    sql(db,"CREATE TABLE configured(id INTEGER PRIMARY KEY, value BLOB); INSERT INTO configured VALUES(1,randomblob(9000)); PRAGMA wal_checkpoint(TRUNCATE);");
    if(scalar(db,"SELECT length(value) FROM configured")!=9000)return 1;
    char *expected = text(db, "SELECT hex(value) FROM configured");
    const char *filename = sqlite3_db_filename(db, "main");
    const char *parity = sqlite3_uri_parameter(filename, "mv_parity");
    if (!parity) return 1;
    int different = atoi(parity) == 32 ? 1024 : 32;
    char *path = sqlite3_mprintf("%s", filename);
    if (!path) return 1;
    check(sqlite3_close(db), NULL, "configured close");
    uint64_t before = file_hash(path);
    char *uri = sqlite3_mprintf("file:%s?mv_parity=%d", path, different);
    if (!uri) return 1;
    db = NULL;
    int code = sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, MVFS_NAME);
    if (code != SQLITE_CANTOPEN) { fprintf(stderr, "mismatched parity accepted: %d\n", code); return 1; }
    check(sqlite3_close(db), NULL, "mismatched close");
    sqlite3_free(uri);
    if (file_hash(path) != before) { fprintf(stderr, "rejected settings changed file\n"); return 1; }
    /* No option on reopen: use the file's parity, including a nondefault value. */
    db = open_db(path, 0);
    char *actual = text(db, "SELECT hex(value) FROM configured");
    if (strcmp(actual, expected)) { fprintf(stderr, "configured data changed on reopen\n"); return 1; }
    sqlite3_free(actual); sqlite3_free(expected); sqlite3_free(path);
    sql(db,"PRAGMA integrity_check;");
  }else if(strcmp(mode,"sql")==0){
    sql(db,"CREATE TABLE t(id INTEGER PRIMARY KEY, value BLOB); INSERT INTO t VALUES(1,zeroblob(7000));");
    sqlite3*r=open_db(argv[2],0);sql(r,"BEGIN;");if(scalar(r,"SELECT count(*) FROM t")!=1)return 1;
    sql(db,"INSERT INTO t VALUES(2,randomblob(5000));");
    if(scalar(r,"SELECT count(*) FROM t")!=1){fprintf(stderr,"snapshot changed\n");return 1;}
    sql(r,"COMMIT;");if(scalar(r,"SELECT count(*) FROM t")!=2)return 1;
    sql(db,"BEGIN; INSERT INTO t VALUES(3,zeroblob(9000)); SAVEPOINT s; INSERT INTO t VALUES(4,zeroblob(16000)); ROLLBACK TO s; RELEASE s; COMMIT;");
    if(scalar(db,"SELECT count(*) FROM t")!=3)return 1;
    sql(db,"PRAGMA temp_store=FILE; PRAGMA temp.cache_size=2; CREATE TEMP TABLE tempdata(x BLOB); WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1024) INSERT INTO tempdata SELECT randomblob(4096) FROM n;");
    if(scalar(db,"SELECT count(*) FROM (SELECT x FROM tempdata ORDER BY x)")!=1024)return 1;
    sql(db,"CREATE INDEX ix ON t(value); DELETE FROM t WHERE id>1;");
    check(sqlite3_close(r),0,"reader close");sql(db,"VACUUM; PRAGMA wal_checkpoint(TRUNCATE);");
    if(scalar(db,"SELECT count(*) FROM t")!=1)return 1;
    char *backup_path=sqlite3_mprintf("%s-backup",argv[2]);sqlite3 *copy=open_db(backup_path,1);
    sqlite3_backup *backup=sqlite3_backup_init(copy,"main",db,"main");
    if(!backup){fprintf(stderr,"backup init: %s\n",sqlite3_errmsg(copy));return 1;}
    int backup_rc=sqlite3_backup_step(backup,-1);if(backup_rc!=SQLITE_DONE){fprintf(stderr,"backup step: %d\n",backup_rc);return 1;}
    check(sqlite3_backup_finish(backup),copy,"backup finish");
    if(scalar(copy,"SELECT count(*) FROM t")!=1)return 1;
    check(sqlite3_close(copy),0,"backup close");sqlite3_free(backup_path);
    sqlite3 *readonly=0;check(sqlite3_open_v2(argv[2],&readonly,SQLITE_OPEN_READONLY,MVFS_NAME),readonly,"read-only open");
    if(scalar(readonly,"SELECT count(*) FROM t")!=scalar(db,"SELECT count(*) FROM t"))return 1;
    expect_fail(readonly,"DELETE FROM t");
    check(sqlite3_close(readonly),0,"readonly close");
    expect_fail(db,"PRAGMA journal_mode=DELETE");expect_fail(db,"PRAGMA synchronous=OFF");expect_fail(db,"PRAGMA page_size=8192");
    sql(db,"PRAGMA integrity_check;");
  }else if(strcmp(mode,"crash_prepare")==0){
    sql(db,"CREATE TABLE recovery(id INTEGER PRIMARY KEY); INSERT INTO recovery VALUES(1); PRAGMA wal_checkpoint(TRUNCATE);");
    for(int i=2;i<=50;i++){char s[80];snprintf(s,sizeof s,"INSERT INTO recovery VALUES(%d)",i);sql(db,s);}
    _exit(0);
  }else if(strcmp(mode,"recover")==0){
    sqlite3_int64 count=scalar(db,"SELECT count(*) FROM recovery");
    if(count<1||count>50){fprintf(stderr,"bad prefix count %lld\n",count);return 1;}
    sql(db,"INSERT INTO recovery VALUES(1000); PRAGMA integrity_check;");
    printf("recovered %lld rows\n",count);
  }else if(strcmp(mode,"writer")==0){
    sql(db,"CREATE TABLE IF NOT EXISTS concurrent(id INTEGER PRIMARY KEY);");
    for(int i=0;i<60;i++)sql(db,"INSERT INTO concurrent DEFAULT VALUES;");
  }else if(strcmp(mode,"reader")==0){
    for(int i=0;i<200;i++){sql(db,"BEGIN; SELECT count(*) FROM concurrent; COMMIT;");}
  }else if(strcmp(mode,"concurrent_prepare")==0){sql(db,"CREATE TABLE concurrent(id INTEGER PRIMARY KEY);");}
  else if(strcmp(mode,"concurrent_check")==0){if(scalar(db,"SELECT count(*) FROM concurrent")!=120)return 1;}
  else {return 2;}
  check(sqlite3_close(db),0,"close");check(stop_vfs(),0,"maintenance shutdown");check(sqlite3_shutdown(),0,"SQLite shutdown");return 0;
}
