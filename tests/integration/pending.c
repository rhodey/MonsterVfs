/* A failed WAL write must not survive in RAM and overwrite a later writer's commit. */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The independent host object uses these names for its sync syscalls. */
int mvfs_test_fsync(int fd) { return fsync(fd); }
int mvfs_test_fdatasync(int fd) {
#ifdef __APPLE__
  return fsync(fd);
#else
  return fdatasync(fd);
#endif
}
static void require(int condition,const char *message) {
  if(!condition) { fprintf(stderr,"pending: %s\n",message); exit(1); }
}
static void ok(int rc,const char *operation) {
  if(rc!=SQLITE_OK) { fprintf(stderr,"pending: %s: %d\n",operation,rc); exit(1); }
}
static void sql(sqlite3 *db,const char *text) {
  char *error=NULL; int rc=sqlite3_exec(db,text,NULL,NULL,&error);
  if(rc!=SQLITE_OK) { fprintf(stderr,"pending: %s: %d %s\n",text,rc,error); exit(1); }
  sqlite3_free(error);
}
static const sqlite3_io_methods *original;
static sqlite3_io_methods wrapped;
static void (*arm_guard)(int);
static int (*guard_hits)(void);
static const char *mode;
static int armed, pending, rewritten, rewrites;
static sqlite3_int64 last_frame_read=-1;
static int write_frame(sqlite3_file *file,const void *data,int n,sqlite3_int64 offset) {
  int header=n==24 && offset>=32 && (offset-32)%4120==0;
  int body=n==4096 && offset>=56 && (offset-56)%4120==0;
  if(armed && ((!strcmp(mode,"body") && pending && body) ||
               (!strcmp(mode,"release") && header))) {
    arm_guard(!strcmp(mode,"release")); armed=0;
  }
  int rc=original->xWrite(file,data,n,offset);
  if(rc==SQLITE_OK) {
    pending=header;
    rewritten=header && offset==last_frame_read;
    rewrites+=rewritten;
  }
  last_frame_read=-1;
  return rc;
}
static int read_frame(sqlite3_file *file,void *data,int n,sqlite3_int64 offset) {
  if(armed && !strcmp(mode,"flush") && rewritten) {
    arm_guard(0); armed=0;
  }
  int rc=original->xRead(file,data,n,offset);
  if(rc==SQLITE_OK) { pending=0; rewritten=0; }
  last_frame_read=rc==SQLITE_OK && n==4120 ? offset : -1;
  return rc;
}
static void rows(sqlite3 *db,int expected) {
  sqlite3_stmt *stmt=NULL;
  ok(sqlite3_prepare_v2(db,"SELECT id,v,pad FROM t ORDER BY id",-1,&stmt,NULL),"rows prepare");
  for(int id=1;id<=12;id++) {
    require(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==id &&
      sqlite3_column_int(stmt,1)==expected && sqlite3_column_bytes(stmt,2)==3000,"acknowledged rows differ");
    const unsigned char *pad=sqlite3_column_blob(stmt,2);
    for(int i=0;i<3000;i++) require(pad[i]==0,"acknowledged payload changed");
  }
  require(sqlite3_step(stmt)==SQLITE_DONE,"unexpected rows");
  ok(sqlite3_finalize(stmt),"rows finalize");
}
static void no_cutoff(sqlite3 *db) {
  sqlite3_stmt *stmt=NULL;
  ok(sqlite3_prepare_v2(db,"PRAGMA monstervfs_maintenance",-1,&stmt,NULL),"status prepare");
  require(sqlite3_step(stmt)==SQLITE_ROW,"missing status");
  const char *status=(const char *)sqlite3_column_text(stmt,0);
  require(status && strstr(status,"\"wal_cutoffs\":0") && strstr(status,"\"wal_refusals\":0"),
          "recovery discarded or refused acknowledged WAL frames");
  ok(sqlite3_finalize(stmt),"status finalize");
}
int main(int argc,char **argv) {
  require(argc==5,"usage: pending EXTENSION DB MODE write|check");
  mode=argv[3];
  sqlite3 *loader=NULL; ok(sqlite3_open(":memory:",&loader),"loader");
  ok(sqlite3_enable_load_extension(loader,1),"enable extension");
  char *error=NULL; int rc=sqlite3_load_extension(loader,argv[1],MVFS_ENTRY,&error);
  if(rc) { fprintf(stderr,"pending: load: %s\n",error); return 1; }
  ok(sqlite3_close(loader),"loader close");
  sqlite3 *a=NULL,*b=NULL;
  ok(sqlite3_open_v2(argv[2],&a,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"A open");
  if(!strcmp(argv[4],"check")) {
    rows(a,3); no_cutoff(a); ok(sqlite3_close(a),"check close");
    printf("%s: acknowledged rows survived failed writer close and process restart\n",mode); return 0;
  }
  arm_guard=dlsym(RTLD_DEFAULT,"mvfs_arm_guard"); guard_hits=dlsym(RTLD_DEFAULT,"mvfs_guard_hits");
  require(arm_guard && guard_hits,"guard interposer missing");
  sql(a,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE t(id INTEGER PRIMARY KEY,v INTEGER,pad BLOB);"
        "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<12)"
        "INSERT INTO t SELECT x,1,zeroblob(3000) FROM c; PRAGMA wal_checkpoint(TRUNCATE);");
  ok(sqlite3_open_v2(argv[2],&b,SQLITE_OPEN_READWRITE,MVFS_NAME),"B open");
  sql(b,"PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"); rows(b,1);
  sqlite3_file *wal=NULL;
  ok(sqlite3_file_control(a,"main",SQLITE_FCNTL_JOURNAL_POINTER,&wal),"WAL pointer");
  require(wal && wal->pMethods,"WAL missing"); original=wal->pMethods; wrapped=*original;
  wrapped.xWrite=write_frame; wrapped.xRead=read_frame; wal->pMethods=&wrapped;
  armed=1;
  if(!strcmp(mode,"flush") || !strcmp(mode,"rewrite-ok")) {
    /* Spill and then overwrite the same uncommitted pages. COMMIT must rewrite
     * frame checksums using header-only xWrites, followed by a read or sync. */
    sql(a,"BEGIN; UPDATE t SET v=2;"); ok(sqlite3_db_cacheflush(a),"spill");
    sql(a,"UPDATE t SET v=4;");
    rc=sqlite3_exec(a,"COMMIT",NULL,NULL,NULL);
  } else {
    rc=sqlite3_exec(a,"UPDATE t SET v=2",NULL,NULL,NULL);
  }
  if(!strcmp(mode,"rewrite-ok")) {
    require(rc==SQLITE_OK && guard_hits()==0 && rewrites>0,"header-only rewrite did not complete");
    rows(a,4);
  } else {
    require(rc==SQLITE_IOERR && sqlite3_extended_errcode(a)==SQLITE_IOERR_LOCK,
            "writer did not receive guard error");
    require(guard_hits()==1 && !armed && sqlite3_get_autocommit(a),"guard missed or transaction survived");
    if(!strcmp(mode,"flush")) require(rewrites>0,"no header-only rewrite");
  }
  printf("%s: writer result=%d, guard faults=%d, header-only rewrites=%d\n",mode,rc,guard_hits(),rewrites);
  sql(b,"UPDATE t SET v=3;"); rows(b,3);
  ok(sqlite3_close(a),"failed writer close"); rows(b,3);
  /* A separate process reopens without B checkpointing away the evidence. */
  fflush(stdout); fflush(stderr); _exit(0);
}
