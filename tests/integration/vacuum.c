#include "sqlite3.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROWS 160
#define REQUIRE(expr) do { if (!(expr)) { \
  fprintf(stderr,"vacuum line %d: %s\n",__LINE__,#expr); exit(1); \
} } while (0)

static const char *vfs;
static int native;

static void check(int rc,sqlite3 *db,const char *where) {
  if(rc!=SQLITE_OK) {
    fprintf(stderr,"%s: %d/%d %s\n",where,rc,
            db?sqlite3_extended_errcode(db):0,db?sqlite3_errmsg(db):"");
    exit(1);
  }
}
static void sql(sqlite3 *db,const char *query) {
  check(sqlite3_exec(db,query,NULL,NULL,NULL),db,query);
}
static sqlite3_int64 scalar(sqlite3 *db,const char *query) {
  sqlite3_stmt *statement=NULL;
  check(sqlite3_prepare_v2(db,query,-1,&statement,NULL),db,query);
  REQUIRE(sqlite3_step(statement)==SQLITE_ROW);
  sqlite3_int64 value=sqlite3_column_int64(statement,0);
  check(sqlite3_finalize(statement),db,"scalar finalize");
  return value;
}
static sqlite3 *open_database(const char *path,int create) {
  sqlite3 *db=NULL;
  check(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|
      (create?SQLITE_OPEN_CREATE:0),vfs),db,"open");
  check(sqlite3_busy_timeout(db,5000),db,"busy timeout");
  return db;
}
static void configure(sqlite3 *db) {
  sql(db,"PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
         "PRAGMA wal_autocheckpoint=0; PRAGMA cache_size=8; PRAGMA temp_store=FILE;");
}
static int payload_size(int id) { return 5000+id%5*777; }
static void payload(int id,unsigned char *bytes) {
  for(int i=0;i<payload_size(id);i++) bytes[i]=(unsigned char)((id*31+i*17+i/103)%251);
}
static void fill_rows(sqlite3 *db) {
  sqlite3_stmt *statement=NULL;
  sql(db,"BEGIN;");
  check(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO t VALUES(?1,?2,?3)",
      -1,&statement,NULL),db,"insert prepare");
  for(int id=1;id<=ROWS;id++) {
    char label[32]; unsigned char bytes[8200];
    snprintf(label,sizeof(label),"row-%04d",id); payload(id,bytes);
    check(sqlite3_bind_int(statement,1,id),db,"id");
    check(sqlite3_bind_text(statement,2,label,-1,SQLITE_TRANSIENT),db,"label");
    check(sqlite3_bind_blob(statement,3,bytes,payload_size(id),SQLITE_TRANSIENT),db,"blob");
    REQUIRE(sqlite3_step(statement)==SQLITE_DONE);
    check(sqlite3_reset(statement),db,"insert reset");
  }
  check(sqlite3_finalize(statement),db,"insert finalize");
  sql(db,"COMMIT;");
}
/* Expected contents live in this program, independently of the database. */
static void rows(sqlite3 *db,int deleted) {
  sqlite3_stmt *statement=NULL;
  check(sqlite3_prepare_v2(db,"SELECT id,label,data FROM t ORDER BY id",
      -1,&statement,NULL),db,"verify prepare");
  int count=0;
  for(int id=1;id<=ROWS;id++) {
    if(deleted&&id%3) continue;
    char label[32]; unsigned char expected[8200];
    REQUIRE(sqlite3_step(statement)==SQLITE_ROW);
    REQUIRE(sqlite3_column_int(statement,0)==id);
    snprintf(label,sizeof(label),"row-%04d",id); payload(id,expected);
    REQUIRE(strcmp((const char*)sqlite3_column_text(statement,1),label)==0);
    REQUIRE(sqlite3_column_bytes(statement,2)==payload_size(id));
    REQUIRE(memcmp(sqlite3_column_blob(statement,2),expected,payload_size(id))==0);
    count++;
  }
  REQUIRE(sqlite3_step(statement)==SQLITE_DONE);
  check(sqlite3_finalize(statement),db,"verify finalize");
  REQUIRE(count==(deleted?ROWS/3:ROWS));
  REQUIRE(scalar(db,"SELECT count(*) FROM t INDEXED BY label_index WHERE label>='row-'")==count);
}
static void integrity(sqlite3 *db) {
  sqlite3_stmt *statement=NULL;
  check(sqlite3_prepare_v2(db,"PRAGMA integrity_check",-1,&statement,NULL),db,"integrity");
  REQUIRE(sqlite3_step(statement)==SQLITE_ROW);
  REQUIRE(strcmp((const char*)sqlite3_column_text(statement,0),"ok")==0);
  REQUIRE(sqlite3_step(statement)==SQLITE_DONE);
  check(sqlite3_finalize(statement),db,"integrity finalize");
}
static void checkpoint(sqlite3 *db,const char *path) {
  int log=-1,done=-1;
  check(sqlite3_wal_checkpoint_v2(db,"main",SQLITE_CHECKPOINT_TRUNCATE,&log,&done),db,"checkpoint");
  REQUIRE(log==0&&done==0);
  sqlite3_int64 pages=scalar(db,"PRAGMA page_count");
  struct stat info; REQUIRE(stat(path,&info)==0);
  REQUIRE(info.st_size==(native?4096*pages:8192+8192*pages));
  sqlite3_file *file=NULL;
  check(sqlite3_file_control(db,"main",SQLITE_FCNTL_FILE_POINTER,&file),db,"file pointer");
  unsigned char header[100];
  check(file->pMethods->xRead(file,header,sizeof(header),0),db,"header read");
  REQUIRE(header[20]==(native?0:48));
  REQUIRE(header[16]==16&&header[17]==0);
}
static void setup_state(sqlite3 *db,int mode) {
  REQUIRE(scalar(db,"PRAGMA auto_vacuum")==mode);
  REQUIRE(scalar(db,"SELECT count(*) FROM setup")==1);
  REQUIRE(scalar(db,"SELECT value FROM setup")==42);
  int reserved=-1;
  check(sqlite3_file_control(db,"main",SQLITE_FCNTL_RESERVE_BYTES,&reserved),db,"reserve query");
  REQUIRE(reserved==(native?0:48));
}
/* WAL selection creates the first page. Enabling pointer maps afterward needs
 * VACUUM, even if the database has no tables; native SQLite behaves the same. */
static void creation_case(const char *path,int mode,int order) {
  sqlite3 *db=open_database(path,1);
  sql(db,"PRAGMA page_size=4096; PRAGMA synchronous=FULL;");
  if(order==1||order==3) configure(db);
  if(order==2) REQUIRE(scalar(db,"SELECT count(*) FROM sqlite_schema")==0);
  if(order==3) sql(db,"CREATE TABLE setup(value INTEGER); INSERT INTO setup VALUES(42);");
  char pragma[80]; snprintf(pragma,sizeof(pragma),"PRAGMA auto_vacuum=%d",mode);
  sql(db,pragma);
  if(order==3) sql(db,"VACUUM;");
  configure(db);
  if(order!=3) sql(db,"CREATE TABLE setup(value INTEGER); INSERT INTO setup VALUES(42);");
  int expected=order==1?0:mode;
  setup_state(db,expected); checkpoint(db,path);
  check(sqlite3_close(db),NULL,"creation close");
  db=open_database(path,0); configure(db); setup_state(db,expected); integrity(db);
  check(sqlite3_close(db),NULL,"creation reopen close");
  db=NULL;
  check(sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,vfs),db,"creation readonly open");
  setup_state(db,expected); integrity(db);
  check(sqlite3_close(db),NULL,"creation readonly close");
  printf("auto_vacuum=%d creation order %d: persisted mode %d and reserve bytes verified\n",mode,order,expected);
}
static void vacuum_case(const char *path,int mode) {
  sqlite3 *db=open_database(path,1);
  char pragma[80]; snprintf(pragma,sizeof(pragma),"PRAGMA auto_vacuum=%d",mode);
  sql(db,pragma); configure(db);
  REQUIRE(scalar(db,"PRAGMA auto_vacuum")==mode);
  sql(db,"CREATE TABLE t(id INTEGER PRIMARY KEY,label TEXT UNIQUE,data BLOB);"
         "CREATE INDEX label_index ON t(label);");
  fill_rows(db); rows(db,0); checkpoint(db,path);
  sqlite3_int64 original=scalar(db,"PRAGMA page_count");

  sql(db,"BEGIN; DELETE FROM t WHERE id%3<>0;");
  if(mode==2) sql(db,"PRAGMA incremental_vacuum(7);");
  sql(db,"ROLLBACK;"); rows(db,0);
  REQUIRE(scalar(db,"PRAGMA page_count")==original);
  REQUIRE(scalar(db,"PRAGMA freelist_count")==0);

  sqlite3 *reader=open_database(path,0);
  configure(reader); sql(reader,"BEGIN;"); rows(reader,0);
  sql(db,"DELETE FROM t WHERE id%3<>0;"); rows(db,1); rows(reader,0);
  sqlite3_int64 after_delete=scalar(db,"PRAGMA page_count");
  sqlite3_int64 free_pages=scalar(db,"PRAGMA freelist_count");
  if(mode==1) { REQUIRE(after_delete<original); REQUIRE(free_pages==0); }
  else { REQUIRE(after_delete==original); REQUIRE(free_pages>0); }

  if(mode==2) {
    sql(db,"BEGIN; PRAGMA incremental_vacuum(7);");
    REQUIRE(scalar(db,"PRAGMA page_count")<after_delete);
    sql(db,"ROLLBACK;");
    REQUIRE(scalar(db,"PRAGMA page_count")==after_delete);
    REQUIRE(scalar(db,"PRAGMA freelist_count")==free_pages); rows(db,1);
    sql(db,"PRAGMA incremental_vacuum(7);");
    REQUIRE(scalar(db,"PRAGMA freelist_count")<free_pages);
    sql(db,"PRAGMA incremental_vacuum;");
    REQUIRE(scalar(db,"PRAGMA freelist_count")==0);
  }
  sql(db,"VACUUM;"); rows(db,1); rows(reader,0);
  REQUIRE(scalar(db,"PRAGMA page_count")<original);
  REQUIRE(scalar(db,"PRAGMA freelist_count")==0);
  REQUIRE(scalar(db,"PRAGMA auto_vacuum")==mode);
  sql(reader,"COMMIT;"); rows(reader,1);
  check(sqlite3_close(reader),NULL,"reader close");
  checkpoint(db,path); integrity(db); check(sqlite3_close(db),NULL,"close");

  db=open_database(path,0); configure(db); rows(db,1);
  REQUIRE(scalar(db,"PRAGMA auto_vacuum")==mode);
  /* Enabling/disabling pointer maps on an existing database takes VACUUM. */
  sql(db,"PRAGMA auto_vacuum=INCREMENTAL; VACUUM;");
  REQUIRE(scalar(db,"PRAGMA auto_vacuum")==2); rows(db,1);
  sql(db,"PRAGMA auto_vacuum=FULL;");
  REQUIRE(scalar(db,"PRAGMA auto_vacuum")==1);
  sql(db,"PRAGMA auto_vacuum=NONE; VACUUM;");
  REQUIRE(scalar(db,"PRAGMA auto_vacuum")==0); rows(db,1);
  checkpoint(db,path); integrity(db);
  check(sqlite3_close(db),NULL,"mode transition close");

  db=open_database(path,0); configure(db); rows(db,1);
  REQUIRE(scalar(db,"PRAGMA auto_vacuum")==0);
  /* Grow again after physical truncation; discarded bytes must not reappear. */
  fill_rows(db); rows(db,0); checkpoint(db,path); integrity(db);
  check(sqlite3_close(db),NULL,"regrowth close");
  db=open_database(path,0); configure(db); rows(db,0); integrity(db);
  check(sqlite3_close(db),NULL,"final close");
  printf("auto_vacuum=%d: contents, snapshots, rollback, shrink, regrowth and reopen passed\n",mode);
}
int main(int argc,char **argv) {
  if(argc!=4) { fprintf(stderr,"usage: vacuum EXTENSION DATABASE VFS\n"); return 2; }
  vfs=argv[3]; native=strcmp(vfs,"unix")==0;
  REQUIRE(native||strcmp(vfs,MVFS_NAME)==0);
  sqlite3 *loader=NULL;
  check(sqlite3_open(":memory:",&loader),loader,"loader");
  check(sqlite3_enable_load_extension(loader,1),loader,"enable loading");
  char *error=NULL;
  int rc=sqlite3_load_extension(loader,argv[1],MVFS_ENTRY,&error);
  if(rc!=SQLITE_OK) { fprintf(stderr,"load: %s\n",error); return 1; }
  check(sqlite3_close(loader),NULL,"loader close");
  for(int mode=1;mode<=2;mode++) {
    for(int order=0;order<4;order++) {
      char path[4096];
      int length=snprintf(path,sizeof(path),"%s-init-%d-%d",argv[2],mode,order);
      REQUIRE(length>0&&(size_t)length<sizeof(path));
      creation_case(path,mode,order);
    }
  }
  for(int mode=0;mode<3;mode++) {
    char path[4096];
    int length=snprintf(path,sizeof(path),"%s-%d",argv[2],mode);
    REQUIRE(length>0&&(size_t)length<sizeof(path));
    vacuum_case(path,mode);
  }
  void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); REQUIRE(library);
  int(*stop)(void)=(int(*)(void))dlsym(library,"monstervfs_shutdown"); REQUIRE(stop);
  check(stop(),NULL,"maintenance shutdown");
  check(sqlite3_shutdown(),NULL,"SQLite shutdown");
}
