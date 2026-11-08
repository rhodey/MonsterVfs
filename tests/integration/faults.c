#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "sqlite3.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static _Atomic int fail_write, fail_truncate, fail_sync, refuse_reads, refused;
static ssize_t (*real_read)(int,void*,size_t);
static ssize_t (*real_pread)(int,void*,size_t,off_t);
static ssize_t (*real_pread64)(int,void*,size_t,off_t);
static ssize_t (*real_pwrite)(int,const void*,size_t,off_t);
static ssize_t (*real_pwrite64)(int,const void*,size_t,off_t);
static ssize_t (*real_write)(int,const void*,size_t);
static int (*real_truncate)(int,off_t);
static int (*real_fcntl)(int,int,...);
static int consume(_Atomic int *flag) {
  if (atomic_exchange(flag, 0)) { errno=EIO; return 1; }
  return 0;
}
/* Target the VFS's control update, after SQLite's own pre-checkpoint WAL sync. */
static struct stat checkpoint_main, checkpoint_wal;
static _Atomic int checkpoint_fault, checkpoint_controls, checkpoint_syncs;
static _Atomic int checkpoint_main_writes, checkpoint_triggered;
static int same_file(int fd,const struct stat *expected) {
  struct stat actual;
  return fstat(fd,&actual)==0 && actual.st_dev==expected->st_dev && actual.st_ino==expected->st_ino;
}
static int checkpoint_write_fault(int fd,off_t offset) {
  int fault=atomic_load(&checkpoint_fault);
  if(!fault) return 0;
  if(same_file(fd,&checkpoint_main)) atomic_fetch_add(&checkpoint_main_writes,1);
  if(offset>=8192 && offset<16384 && same_file(fd,&checkpoint_wal)) {
    atomic_fetch_add(&checkpoint_controls,1);
    if(fault==1 && !atomic_exchange(&checkpoint_triggered,1)) { errno=EIO; return 1; }
  }
  return 0;
}
static int checkpoint_sync_fault(int fd) {
  int fault=atomic_load(&checkpoint_fault);
  if(!fault || !same_file(fd,&checkpoint_wal)) return 0;
  atomic_fetch_add(&checkpoint_syncs,1);
  if(fault==2 && atomic_load(&checkpoint_controls)>0 && !atomic_exchange(&checkpoint_triggered,1)) {
    errno=EIO; return 1;
  }
  return 0;
}
static ssize_t injected_pwrite(int fd,const void *data,size_t size,off_t offset) {
  return checkpoint_write_fault(fd,offset) || consume(&fail_write) ? -1 : real_pwrite(fd,data,size,offset);
}
static ssize_t injected_pwrite64(int fd,const void *data,size_t size,off_t offset) {
  return checkpoint_write_fault(fd,offset) || consume(&fail_write) ? -1 : real_pwrite64(fd,data,size,offset);
}
static ssize_t injected_write(int fd,const void *data,size_t size) {
  return checkpoint_write_fault(fd,lseek(fd,0,SEEK_CUR)) || consume(&fail_write) ? -1 : real_write(fd,data,size);
}
/* Unlike the one-shot hooks, these keep failing: this is a read the page cache refuses. */
static int refuse(void) {
  if (!atomic_load(&refuse_reads)) return 0;
  atomic_fetch_add(&refused,1); errno=EIO; return 1;
}
static ssize_t refused_read(int fd,void *data,size_t size) { return refuse() ? -1 : real_read(fd,data,size); }
static ssize_t refused_pread(int fd,void *data,size_t size,off_t offset) { return refuse() ? -1 : real_pread(fd,data,size,offset); }
static ssize_t refused_pread64(int fd,void *data,size_t size,off_t offset) { return refuse() ? -1 : real_pread64(fd,data,size,offset); }
static int injected_truncate(int fd,off_t size) { return consume(&fail_truncate) ? -1 : real_truncate(fd,size); }
/* SQLite's unix syscall table omits sync. The independent test-host object
 * renames only its own fsync/fdatasync references to these wrappers. */
static void reader_during_sync(void);
static _Atomic unsigned long long fail_sync_of;
static int fails_this_file(int fd) {
  struct stat info;
  if(!atomic_load(&fail_sync_of)||fstat(fd,&info)!=0) return 0;
  if((unsigned long long)info.st_ino!=atomic_load(&fail_sync_of)) return 0;
  atomic_store(&fail_sync_of,0); errno=EIO; return 1;
}
int mvfs_test_fsync(int fd) {
  reader_during_sync();
  if(checkpoint_sync_fault(fd) || fails_this_file(fd)) return -1;
  return consume(&fail_sync) ? -1 : fsync(fd);
}
int mvfs_test_fdatasync(int fd) {
#ifdef __APPLE__
  return mvfs_test_fsync(fd);
#else
  reader_during_sync();
  if(checkpoint_sync_fault(fd) || fails_this_file(fd)) return -1;
  return consume(&fail_sync) ? -1 : fdatasync(fd);
#endif
}
/* A full sync never reaches fsync, so send it there where faults can be injected. */
static int injected_fcntl(int fd,int command,...) {
  va_list arguments; va_start(arguments,command); void *argument=va_arg(arguments,void*); va_end(arguments);
#ifdef F_FULLFSYNC
  if (command==F_FULLFSYNC) { errno=ENOTSUP; return -1; }
#endif
  return real_fcntl(fd,command,argument);
}
static void require(int condition,const char *message) {
  if (!condition) { fprintf(stderr,"faults: %s\n",message); exit(1); }
}
static void ok(int rc,const char *operation) {
  if (rc != SQLITE_OK) { fprintf(stderr,"%s: %d\n",operation,rc); exit(1); }
}
static void sql(sqlite3 *db,const char *text) {
  char *error=NULL;
  int rc=sqlite3_exec(db,text,NULL,NULL,&error);
  if (rc != SQLITE_OK) { fprintf(stderr,"%s: %d %s\n",text,rc,error?error:sqlite3_errmsg(db)); exit(1); }
  sqlite3_free(error);
}
static sqlite3_vfs *protected_vfs;
static sqlite3_file *open_file(const char *path,int kind,int create,int disposable) {
  sqlite3_file *file=calloc(1,(size_t)protected_vfs->szOsFile);
  require(file!=NULL,"file allocation failed");
  int flags=SQLITE_OPEN_READWRITE|kind;
  if (create) flags|=SQLITE_OPEN_CREATE|SQLITE_OPEN_EXCLUSIVE;
  if (disposable) flags|=SQLITE_OPEN_DELETEONCLOSE;
  int actual=0;
  ok(protected_vfs->xOpen(protected_vfs,path,file,flags,&actual),"direct xOpen");
  require(file->pMethods!=NULL,"open did not install methods");
  return file;
}
static void close_file(sqlite3_file *file) {
  ok(file->pMethods->xUnlock(file,SQLITE_LOCK_NONE),"xUnlock");
  ok(file->pMethods->xClose(file),"xClose");
  free(file);
}
static void assert_size(sqlite3_file *file,sqlite3_int64 expected) {
  sqlite3_int64 size=-1;
  ok(file->pMethods->xFileSize(file,&size),"xFileSize");
  require(size==expected,"logical file size differs");
}
static void check_failed(sqlite3_file *file,int failure) {
  char bytes[20]; memset(bytes,7,sizeof(bytes)); sqlite3_int64 size;
  require((failure&255)==SQLITE_IOERR,"injection did not return IOERR");
  require(file->pMethods->xRead(file,bytes,sizeof(bytes),0)==failure,"failed file allowed a read");
  require(file->pMethods->xWrite(file,bytes,sizeof(bytes),0)==failure,"failed file allowed a write");
  require(file->pMethods->xTruncate(file,0)==failure,"failed file allowed truncate");
  require(file->pMethods->xSync(file,SQLITE_SYNC_NORMAL)==failure,"failed file allowed sync");
  require(file->pMethods->xFileSize(file,&size)==failure,"failed file allowed size");
}
static void sparse_and_temporary(const char *directory) {
  const int kinds[]={SQLITE_OPEN_TEMP_DB,SQLITE_OPEN_TEMP_JOURNAL,SQLITE_OPEN_TRANSIENT_DB,SQLITE_OPEN_SUBJOURNAL};
  for (size_t i=0;i<sizeof(kinds)/sizeof(kinds[0]);i++) {
    sqlite3_file *file=open_file(NULL,kinds[i],1,1);
    char buffer[32]; memset(buffer,1,sizeof(buffer));
    require(file->pMethods->xRead(file,buffer,sizeof(buffer),0)==SQLITE_IOERR_SHORT_READ,"empty temp read succeeded");
    for(size_t j=0;j<sizeof(buffer);j++) require(buffer[j]==0,"short read was not zero filled");
    ok(file->pMethods->xWrite(file,"temporary",9,0),"temporary write");
    assert_size(file,9);
    close_file(file);
  }
  char path[4096]; snprintf(path,sizeof(path),"%s/sparse",directory);
  sqlite3_file *file=open_file(path,SQLITE_OPEN_TEMP_DB,1,0);
  const sqlite3_int64 far=((sqlite3_int64)1<<40)+123;
  ok(file->pMethods->xWrite(file,"ABC",3,far),"large sparse write");
  assert_size(file,far+3);
  unsigned char buffer[256]; memset(buffer,7,sizeof(buffer));
  ok(file->pMethods->xRead(file,buffer,sizeof(buffer),far-8192),"sparse hole read");
  for(size_t i=0;i<sizeof(buffer);i++) require(buffer[i]==0,"hole returned nonzero bytes");
  memset(buffer,7,sizeof(buffer));
  require(file->pMethods->xRead(file,buffer,20,far)==SQLITE_IOERR_SHORT_READ,"sparse EOF read did not report short read");
  require(memcmp(buffer,"ABC",3)==0,"sparse payload differs");
  for(size_t i=3;i<20;i++) require(buffer[i]==0,"EOF suffix was not zero filled");
  struct stat statbuf; require(stat(path,&statbuf)==0,"sparse stat failed");
  require((sqlite3_int64)statbuf.st_blocks*512<64*1024*1024,"sparse write allocated the gap");
  ok(file->pMethods->xTruncate(file,far+1),"partial truncate");
  ok(file->pMethods->xTruncate(file,far+20),"sparse extension");
  ok(file->pMethods->xRead(file,buffer,20,far),"read reextended suffix");
  require(buffer[0]=='A',"truncate lost preserved byte");
  for(size_t i=1;i<20;i++) require(buffer[i]==0,"extension resurrected removed bytes");
  ok(file->pMethods->xTruncate(file,0),"truncate zero"); assert_size(file,0);
  ok(file->pMethods->xTruncate(file,far+20),"huge empty extension");
  ok(file->pMethods->xRead(file,buffer,20,far),"extended zero read");
  for(size_t i=0;i<20;i++) require(buffer[i]==0,"extended hole is nonzero");
  require(file->pMethods->xWrite(file,"x",1,-1)==SQLITE_MISUSE,"negative write accepted");
  require(file->pMethods->xRead(file,buffer,1,-1)==SQLITE_MISUSE,"negative read accepted");
  require(file->pMethods->xTruncate(file,-1)==SQLITE_MISUSE,"negative truncate accepted");
  ok(file->pMethods->xWrite(file,"x",1,0),"argument rejection poisoned healthy file");
  close_file(file);
  require(unlink(path)==0,"sparse cleanup failed");
}
static void flip(int fd,off_t offset) {
  unsigned char value;
  require(pread(fd,&value,1,offset)==1,"fault read failed"); value^=1;
  require(pwrite(fd,&value,1,offset)==1,"fault write failed");
}
static void checksum_only_replacement(const char *directory) {
  char path[4096]; snprintf(path,sizeof(path),"%s/checksums",directory);
  sqlite3_file *file=open_file(path,SQLITE_OPEN_TEMP_DB,1,0);
  unsigned char page[4096],readback[4096]; memset(page,0x5a,sizeof(page));
  ok(file->pMethods->xWrite(file,page,sizeof(page),0),"initial full page");
  int fd=open(path,O_RDWR); require(fd>=0,"checksum file open failed");
  flip(fd,8192+4096+200);
  require(file->pMethods->xRead(file,readback,sizeof(readback),0)==SQLITE_IOERR_DATA,"checksum-only damage was not rejected");
  memset(page,0x6b,sizeof(page));
  ok(file->pMethods->xWrite(file,page,sizeof(page),0),"complete replacement read unreadable old data");
  ok(file->pMethods->xRead(file,readback,sizeof(readback),0),"replacement read");
  require(memcmp(page,readback,sizeof(page))==0,"full replacement differs");
  flip(fd,8192+4096+201);
  int rc=file->pMethods->xWrite(file,"x",1,0);
  require(rc==SQLITE_IOERR_DATA,"partial replacement ignored unreadable preserved bytes");
  /* Nothing was written, so that refusal leaves the handle usable. */
  ok(file->pMethods->xWrite(file,page,sizeof(page),0),"complete replacement after refusal");
  close(fd); close_file(file); require(unlink(path)==0,"checksum cleanup failed");
}
static void syscall_failures(sqlite3_vfs *native) {
  real_pwrite=(ssize_t(*)(int,const void*,size_t,off_t))native->xGetSystemCall(native,"pwrite");
  real_pwrite64=(ssize_t(*)(int,const void*,size_t,off_t))native->xGetSystemCall(native,"pwrite64");
  real_write=(ssize_t(*)(int,const void*,size_t))native->xGetSystemCall(native,"write");
  real_truncate=(int(*)(int,off_t))native->xGetSystemCall(native,"ftruncate");
  ok(native->xSetSystemCall(native,"fcntl",(sqlite3_syscall_ptr)injected_fcntl),"hook fcntl");
  require(real_truncate!=NULL && (real_pwrite||real_pwrite64||real_write),"native syscall hooks missing");
  if(real_pwrite) ok(native->xSetSystemCall(native,"pwrite",(sqlite3_syscall_ptr)injected_pwrite),"hook pwrite");
  if(real_pwrite64) ok(native->xSetSystemCall(native,"pwrite64",(sqlite3_syscall_ptr)injected_pwrite64),"hook pwrite64");
  if(real_write) ok(native->xSetSystemCall(native,"write",(sqlite3_syscall_ptr)injected_write),"hook write");
  ok(native->xSetSystemCall(native,"ftruncate",(sqlite3_syscall_ptr)injected_truncate),"hook truncate");
  unsigned char page[4096]; memset(page,0xa6,sizeof(page));
  for(int fault=0;fault<3;fault++) {
    sqlite3_file *file=open_file(NULL,SQLITE_OPEN_TEMP_DB,1,1);
    ok(file->pMethods->xWrite(file,page,sizeof(page),0),"pre-fault write");
    int rc;
    if(fault==0) {
      atomic_store(&fail_write,1); rc=file->pMethods->xWrite(file,page,sizeof(page),4096); require(!atomic_load(&fail_write),"write hook missed");
      /* Slots verify on every read and a retry replaces them whole: work continues. */
      unsigned char readback[4096];
      require((rc&255)==SQLITE_IOERR,"injection did not return IOERR");
      ok(file->pMethods->xRead(file,readback,sizeof(readback),0),"read after failed write");
      require(memcmp(page,readback,sizeof(page))==0,"failed write changed earlier data");
      ok(file->pMethods->xWrite(file,page,sizeof(page),4096),"retry after failed write");
      close_file(file); continue;
    }
    else if(fault==1) { atomic_store(&fail_truncate,1); rc=file->pMethods->xTruncate(file,0); require(!atomic_load(&fail_truncate),"truncate hook missed"); }
    else { atomic_store(&fail_sync,1); rc=file->pMethods->xSync(file,SQLITE_SYNC_NORMAL); require(!atomic_load(&fail_sync),"sync hook missed"); }
    check_failed(file,rc); close_file(file);
  }
  ok(native->xSetSystemCall(native,NULL,NULL),"restore native syscalls");
  ok(native->xSetSystemCall(native,"fcntl",(sqlite3_syscall_ptr)injected_fcntl),"hook fcntl");
}
/* fsync needs no slot coherence, so a commit's sync must not stop readers. This
 * runs inside the sync itself: a held guard would leave the reader waiting. */
static _Atomic int probe_armed, probe_done;
static const char *probe_path;
static void *probe_reader(void *unused) {
  (void)unused; sqlite3 *db=NULL;
  ok(sqlite3_open_v2(probe_path,&db,SQLITE_OPEN_READWRITE,MVFS_NAME),"probe open");
  sql(db,"SELECT count(*) FROM t");
  ok(sqlite3_close(db),"probe close");
  atomic_store(&probe_done,1);
  return NULL;
}
static void reader_during_sync(void) {
  if (!atomic_exchange(&probe_armed,0)) return;
  pthread_t thread;
  require(pthread_create(&thread,NULL,probe_reader,NULL)==0,"probe thread failed");
  for (int i=0;i<3000 && !atomic_load(&probe_done);i++) usleep(1000);
  require(atomic_load(&probe_done),"a reader waited for a commit sync");
  require(pthread_join(thread,NULL)==0,"probe join failed");
}
static void reader_progress_during_sync(const char *directory) {
  static char path[4096]; snprintf(path,sizeof(path),"%s/progress",directory); probe_path=path;
  sqlite3 *db=NULL;
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"progress open");
  sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE t(id INTEGER PRIMARY KEY); INSERT INTO t VALUES(1);");
  atomic_store(&probe_armed,1);
  sql(db,"INSERT INTO t VALUES(2)");
  require(atomic_load(&probe_done),"commit did not reach a sync");
  ok(sqlite3_close(db),"progress close");
}
static void transaction_sync_failure(const char *directory) {
  char path[4096]; snprintf(path,sizeof(path),"%s/commit",directory);
  sqlite3 *db=NULL;
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"SQL open");
  sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0; CREATE TABLE t(id INTEGER PRIMARY KEY); INSERT INTO t VALUES(1); PRAGMA wal_checkpoint(TRUNCATE);");
  atomic_store(&fail_sync,1);
  int rc=sqlite3_exec(db,"BEGIN; INSERT INTO t VALUES(2); COMMIT;",NULL,NULL,NULL);
  require(!atomic_load(&fail_sync),"commit sync hook missed");
  require((rc&255)==SQLITE_IOERR,"failed commit did not report IOERR");
  ok(sqlite3_close(db),"SQL failed connection close");
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,MVFS_NAME),"SQL reopen");
  sql(db,"PRAGMA synchronous=FULL; PRAGMA integrity_check;");
  sqlite3_stmt *stmt=NULL;
  ok(sqlite3_prepare_v2(db,"SELECT count(*), min(id), max(id) FROM t",-1,&stmt,NULL),"recovery query");
  require(sqlite3_step(stmt)==SQLITE_ROW,"recovery query failed");
  int count=sqlite3_column_int(stmt,0);
  require((count==1||count==2)&&sqlite3_column_int(stmt,1)==1&&sqlite3_column_int(stmt,2)==count,"acknowledged baseline lost after failed sync");
  ok(sqlite3_finalize(stmt),"query finalize"); ok(sqlite3_close(db),"SQL reopen close");
}
static long long status_number(sqlite3 *db,const char *name) {
  sqlite3_stmt *stmt=NULL; char key[64]; long long value=-1;
  ok(sqlite3_prepare_v2(db,"PRAGMA monstervfs_maintenance",-1,&stmt,NULL),"status prepare");
  require(sqlite3_step(stmt)==SQLITE_ROW,"status row");
  snprintf(key,sizeof(key),"\"%s\":",name);
  const char *found=strstr((const char *)sqlite3_column_text(stmt,0),key);
  if(found) value=atoll(found+strlen(key));
  sqlite3_finalize(stmt);
  return value;
}
static int count_rows(sqlite3 *db) {
  sqlite3_stmt *stmt=NULL; int rows=-1;
  if(sqlite3_prepare_v2(db,"SELECT count(*) FROM t",-1,&stmt,NULL)==SQLITE_OK&&sqlite3_step(stmt)==SQLITE_ROW) rows=sqlite3_column_int(stmt,0);
  sqlite3_finalize(stmt);
  return rows;
}
/* Hints must succeed without changing the length that says how much of the file exists. */
static void file_controls(const char *directory) {
  char path[4096]; snprintf(path,sizeof(path),"%s/controls",directory);
  sqlite3 *db=NULL;
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"controls open");
  sql(db,"PRAGMA journal_mode=WAL; CREATE TABLE t(id INTEGER PRIMARY KEY); INSERT INTO t VALUES(1); PRAGMA wal_checkpoint(TRUNCATE);");
  int chunk=1<<20; sqlite3_int64 hint=1<<24; int reader=-1; char *name=NULL;
  struct stat before,after; require(stat(path,&before)==0,"controls stat");
  ok(sqlite3_file_control(db,"main",SQLITE_FCNTL_CHUNK_SIZE,&chunk),"chunk size");
  ok(sqlite3_file_control(db,"main",SQLITE_FCNTL_SIZE_HINT,&hint),"size hint");
  require(stat(path,&after)==0&&after.st_size==before.st_size,"a hint changed the file");
  ok(sqlite3_file_control(db,"main",SQLITE_FCNTL_EXTERNAL_READER,&reader),"external reader");
  require(reader==0,"a reader was reported with none open");
  ok(sqlite3_file_control(db,"main",SQLITE_FCNTL_TEMPFILENAME,&name),"temporary name");
  require(name!=NULL,"no temporary name"); sqlite3_free(name);
  sql(db,"INSERT INTO t VALUES(2)");
  require(count_rows(db)==2,"rows differ after hints");
  ok(sqlite3_close(db),"controls close");
}
/* A failed main sync is a failed checkpoint, and the WAL still holds everything. */
static void checkpoint_sync_failure(const char *directory) {
  char path[4096]; snprintf(path,sizeof(path),"%s/checkpoint",directory);
  sqlite3 *db=NULL;
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"checkpoint open");
  sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"
         "CREATE TABLE t(id INTEGER PRIMARY KEY); INSERT INTO t VALUES(1),(2);");
  struct stat info; require(stat(path,&info)==0,"checkpoint stat");
  atomic_store(&fail_sync_of,(unsigned long long)info.st_ino);
  int rc=sqlite3_exec(db,"PRAGMA wal_checkpoint(TRUNCATE)",NULL,NULL,NULL);
  require(!atomic_load(&fail_sync_of),"main sync hook missed");
  require((rc&255)==SQLITE_IOERR,"failed checkpoint did not report IOERR");
  require(count_rows(db)==2,"a failed checkpoint stopped reads");
  sql(db,"INSERT INTO t VALUES(3)");
  require(status_number(db,"main_sync_failures")==1,"failed main sync was not counted");
  sql(db,"PRAGMA wal_checkpoint(TRUNCATE)");
  require(count_rows(db)==3,"rows were lost across the retried checkpoint");
  /* A commit is different: its durability is unknown, so that handle stops. */
  atomic_store(&fail_sync,1);
  rc=sqlite3_exec(db,"INSERT INTO t VALUES(4)",NULL,NULL,NULL);
  require(!atomic_load(&fail_sync)&&(rc&255)==SQLITE_IOERR,"failed commit did not report IOERR");
  require(sqlite3_exec(db,"INSERT INTO t VALUES(5)",NULL,NULL,NULL)!=SQLITE_OK,"a failed WAL sync did not stop the handle");
  ok(sqlite3_close(db),"checkpoint close");
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,MVFS_NAME),"checkpoint reopen");
  require(count_rows(db)==3,"reopened rows differ");
  ok(sqlite3_close(db),"checkpoint reopen close");
}
struct image { unsigned char *bytes; size_t size; };
static struct image read_image(const char *path) {
  struct stat info; int fd=open(path,O_RDONLY);
  require(fd>=0 && fstat(fd,&info)==0 && info.st_size>0,"checkpoint image open failed");
  struct image image={malloc((size_t)info.st_size),(size_t)info.st_size};
  require(image.bytes!=NULL,"checkpoint image allocation failed");
  size_t done=0;
  while(done<image.size) {
    ssize_t n=pread(fd,image.bytes+done,image.size-done,(off_t)done);
    require(n>0,"checkpoint image read failed"); done+=(size_t)n;
  }
  require(close(fd)==0,"checkpoint image close failed");
  return image;
}

static sqlite3 *prefix_connection(const char *path) {
  sqlite3 *db=NULL;
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"prefix open");
  ok(sqlite3_db_config(db,SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE,1,NULL),"prefix no close checkpoint");
  sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
  return db;
}
static void prefix_rows_match(sqlite3 *db) {
  sqlite3_stmt *stmt=NULL;
  ok(sqlite3_prepare_v2(db,"SELECT group_concat(id||':'||v,'|') FROM (SELECT id,v FROM t ORDER BY id)",-1,&stmt,NULL),"prefix rows prepare");
  require(sqlite3_step(stmt)==SQLITE_ROW,"prefix rows missing");
  const unsigned char *rows=sqlite3_column_text(stmt,0);
  require(rows&&strcmp((const char*)rows,"1:first|2:second|4:fourth")==0,"prefix repair changed committed rows");
  ok(sqlite3_finalize(stmt),"prefix rows finalize");
}
/* A failed connection must not prevent another writable handle repairing their shared prefix. */
static void prefix_repair_after_failed_handle(const char *directory) {
  char path[4096],wal[4200];snprintf(path,sizeof(path),"%s/prefix",directory);snprintf(wal,sizeof(wal),"%s-wal",path);
  sqlite3 *a=prefix_connection(path);
  sql(a,"CREATE TABLE t(id INTEGER PRIMARY KEY,v TEXT); INSERT INTO t VALUES(1,'first');"
        "PRAGMA wal_checkpoint(TRUNCATE); INSERT INTO t VALUES(2,'second');");
  sqlite3 *b=prefix_connection(path);
  require(count_rows(b)==2,"prefix B baseline differs");
  struct stat info;require(stat(wal,&info)==0,"prefix WAL stat");
  atomic_store(&fail_sync_of,(unsigned long long)info.st_ino);
  int rc=sqlite3_exec(a,"INSERT INTO t VALUES(3,'failed')",NULL,NULL,NULL);
  require((rc&255)==SQLITE_IOERR&&!atomic_load(&fail_sync_of),"prefix WAL sync hook missed");
  sqlite3_file *failed=NULL;sqlite3_int64 size=0;
  ok(sqlite3_file_control(a,"main",SQLITE_FCNTL_JOURNAL_POINTER,&failed),"prefix failed WAL pointer");
  require(failed&&failed->pMethods->xFileSize(failed,&size)==SQLITE_IOERR_FSYNC,"prefix A WAL not stopped");
  sql(b,"INSERT INTO t VALUES(4,'fourth')");
  prefix_rows_match(b);
  struct image before=read_image(wal);
  int fd=open(wal,O_RDWR);require(fd>=0,"prefix physical open");
  unsigned char damaged=before.bytes[24]^1;
  require(pwrite(fd,&damaged,1,24)==1&&fsync(fd)==0,"prefix damage injection");
  sqlite3 *c=prefix_connection(path);
  prefix_rows_match(c);
  unsigned char restored[256];int repaired=0;
  for(int attempt=0;attempt<500;attempt++) {
    require(pread(fd,restored,sizeof(restored),0)==sizeof(restored),"prefix repair read");
    if(memcmp(restored,before.bytes,sizeof(restored))==0 && status_number(b,"prefix_degraded")==0) { repaired=1;break; }
    sqlite3_sleep(10);
  }
  require(repaired,"stopped older handle blocked healthy prefix repair");
  require(failed->pMethods->xFileSize(failed,&size)==SQLITE_IOERR_FSYNC,"prefix repair reset failed handle");
  struct image after=read_image(wal);
  require(before.size==after.size&&memcmp(before.bytes,after.bytes,before.size)==0,"prefix repair changed other WAL bytes");
  free(before.bytes);free(after.bytes);
  require(close(fd)==0,"prefix physical close");
  prefix_rows_match(b);prefix_rows_match(c);
  ok(sqlite3_close(a),"prefix failed connection close");
  ok(sqlite3_close(c),"prefix observer close");
  ok(sqlite3_close(b),"prefix healthy connection close");
  b=prefix_connection(path);prefix_rows_match(b);ok(sqlite3_close(b),"prefix reopened close");
  puts("healthy handles repair the shared prefix while an older failed handle stays open; committed rows survive reopen");
}

static struct stat trim_wal;
static _Atomic int trim_armed, trim_hits;
static _Atomic long long trim_requested;
static int targeted_truncate(int fd,off_t size) {
  if(trim_armed && same_file(fd,&trim_wal)) {
    trim_armed=0;trim_hits++;trim_requested=size;errno=EIO;return -1;
  }
  return real_truncate(fd,size);
}
static void trim_rows_match(sqlite3 *db,int expected) {
  sqlite3_stmt *stmt=NULL;
  ok(sqlite3_prepare_v2(db,"SELECT id,v FROM t ORDER BY id",-1,&stmt,NULL),"trim rows prepare");
  unsigned char zeros[2000]={0};
  for(int i=1;i<=expected;i++) {
    require(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==i &&
            sqlite3_column_bytes(stmt,1)==sizeof(zeros) &&
            memcmp(sqlite3_column_blob(stmt,1),zeros,sizeof(zeros))==0,"trim changed committed row");
  }
  require(sqlite3_step(stmt)==SQLITE_DONE,"trim unexpected rows");
  ok(sqlite3_finalize(stmt),"trim rows finalize");
}
/* SQLite ignores journal_size_limit truncation errors after committing. Our WAL
 * latch must stop later work without losing that already acknowledged commit. */
static void wal_trim_failure(const char *directory,sqlite3_vfs *native) {
  char path[4096],wal[4200];snprintf(path,sizeof(path),"%s/trim",directory);snprintf(wal,sizeof(wal),"%s-wal",path);
  sqlite3 *db=NULL;
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"trim open");
  sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"
         "CREATE TABLE t(id INTEGER PRIMARY KEY,v BLOB);"
         "WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<50) "
         "INSERT INTO t SELECT x,zeroblob(2000) FROM n;"
         "PRAGMA wal_checkpoint(PASSIVE); PRAGMA journal_size_limit=4096;");
  require(stat(wal,&trim_wal)==0&&trim_wal.st_size>10*8192,"trim WAL too short");
  real_truncate=(int(*)(int,off_t))native->xGetSystemCall(native,"ftruncate");
  require(real_truncate!=NULL,"trim hook unavailable");
  ok(native->xSetSystemCall(native,"ftruncate",(sqlite3_syscall_ptr)targeted_truncate),"trim hook");
  trim_hits=0; trim_requested=0; trim_armed=1;
  int rc=sqlite3_exec(db,"INSERT INTO t VALUES(51,zeroblob(2000))",NULL,NULL,NULL);
  require(rc==SQLITE_OK,"benign trim error escaped COMMIT");
  require(trim_hits==1&&!trim_armed&&trim_requested>16384&&trim_requested<trim_wal.st_size,"benign trim hook missed");
  sqlite3_file *file=NULL;
  ok(sqlite3_file_control(db,"main",SQLITE_FCNTL_JOURNAL_POINTER,&file),"trim WAL pointer");
  require(file!=NULL,"trim WAL pointer missing");
  sqlite3_int64 bytes=0;rc=file->pMethods->xFileSize(file,&bytes);
  require(rc==SQLITE_IOERR_TRUNCATE,"failed trim did not latch WAL");
  rc=sqlite3_exec(db,"INSERT INTO t VALUES(52,zeroblob(2000))",NULL,NULL,NULL);
  require((rc&255)==SQLITE_IOERR,"failed trim allowed another write");
  struct image before=read_image(wal);
  ok(sqlite3_close(db),"trim failed close");
  struct image after=read_image(wal);
  require(before.size==after.size&&memcmp(before.bytes,after.bytes,before.size)==0,"close changed latched WAL");
  free(before.bytes);free(after.bytes);
  ok(native->xSetSystemCall(native,"ftruncate",(sqlite3_syscall_ptr)real_truncate),"trim restore hook");
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,MVFS_NAME),"trim reopen");
  trim_rows_match(db,51);
  sql(db,"PRAGMA synchronous=FULL; INSERT INTO t VALUES(52,zeroblob(2000)); PRAGMA wal_checkpoint(TRUNCATE);");
  trim_rows_match(db,52);ok(sqlite3_close(db),"trim final close");
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,MVFS_NAME),"trim final reopen");trim_rows_match(db,52);ok(sqlite3_close(db),"trim final reopen close");
  printf("benign trim: COMMIT succeeded; WAL latch=%d; 51 acknowledged rows preserved; reopened write/checkpoint preserved 52 rows\n",SQLITE_IOERR_TRUNCATE);
}

static void checkpoint_images_match(const char *path,const char *wal,const struct image *main_before,const struct image *wal_before) {
  struct image main_after=read_image(path), wal_after=read_image(wal);
  require(main_after.size==main_before->size && memcmp(main_after.bytes,main_before->bytes,main_before->size)==0,
          "failed control update changed main");
  require(wal_after.size==wal_before->size && wal_after.size>16384,"failed control update removed required WAL");
  /* The control region itself may contain the new, unsynced watermark. */
  require(memcmp(wal_after.bytes,wal_before->bytes,8192)==0 &&
          memcmp(wal_after.bytes+16384,wal_before->bytes+16384,wal_before->size-16384)==0,
          "failed control update changed the WAL prefix or frames");
  free(main_after.bytes); free(wal_after.bytes);
}
static void checkpoint_rows_match(sqlite3 *db) {
  sqlite3_stmt *stmt=NULL;
  ok(sqlite3_prepare_v2(db,"SELECT id,v FROM t ORDER BY id",-1,&stmt,NULL),"checkpoint exact rows prepare");
  for(int id=1;id<=12;id++) {
    char expected[3001]; snprintf(expected,sizeof(expected),"%03000d",id<=4?id+100:id);
    require(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==id &&
            sqlite3_column_bytes(stmt,1)==3000 && memcmp(sqlite3_column_blob(stmt,1),expected,3000)==0,
            "committed row changed after failed control update");
  }
  require(sqlite3_step(stmt)==SQLITE_DONE,"unexpected rows after failed control update");
  ok(sqlite3_finalize(stmt),"checkpoint exact rows finalize");
}
/* The notification's result is ignored by SQLite: our latch must stop main writes,
 * preserve the WAL during close, and allow exact recovery on a fresh connection. */
static void checkpoint_control_failures(const char *directory,sqlite3_vfs *native) {
  if(real_pwrite) ok(native->xSetSystemCall(native,"pwrite",(sqlite3_syscall_ptr)injected_pwrite),"hook checkpoint pwrite");
  if(real_pwrite64) ok(native->xSetSystemCall(native,"pwrite64",(sqlite3_syscall_ptr)injected_pwrite64),"hook checkpoint pwrite64");
  if(real_write) ok(native->xSetSystemCall(native,"write",(sqlite3_syscall_ptr)injected_write),"hook checkpoint write");
  for(int exclusive=0;exclusive<2;exclusive++) for(int fault=1;fault<=2;fault++) {
    char path[4096],wal[4096],shm[4096];
    require(snprintf(path,sizeof(path),"%s/control-%d-%d",directory,exclusive,fault)<(int)sizeof(path),"checkpoint path too long");
    require(snprintf(wal,sizeof(wal),"%s-wal",path)<(int)sizeof(wal),"checkpoint WAL path too long");
    require(snprintf(shm,sizeof(shm),"%s-shm",path)<(int)sizeof(shm),"checkpoint SHM path too long");
    sqlite3 *db=NULL;
    ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"control checkpoint open");
    if(exclusive) sql(db,"PRAGMA locking_mode=EXCLUSIVE;");
    sql(db,"PRAGMA page_size=4096; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"
           "CREATE TABLE t(id INTEGER PRIMARY KEY,v TEXT);"
           "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<8)"
           "INSERT INTO t SELECT x,printf('%03000d',x) FROM c; PRAGMA wal_checkpoint(TRUNCATE);"
           "BEGIN; UPDATE t SET v=printf('%03000d',id+100) WHERE id<=4;"
           "INSERT INTO t VALUES(9,printf('%03000d',9)),(10,printf('%03000d',10)),"
           "(11,printf('%03000d',11)),(12,printf('%03000d',12)); COMMIT;");
    checkpoint_rows_match(db);
    require((access(shm,F_OK)==0)==!exclusive,"checkpoint did not use the intended shared/private wal-index");
    require(stat(path,&checkpoint_main)==0 && stat(wal,&checkpoint_wal)==0,"checkpoint target stat failed");
    struct image main_before=read_image(path), wal_before=read_image(wal);
    atomic_store(&checkpoint_controls,0); atomic_store(&checkpoint_syncs,0);
    atomic_store(&checkpoint_main_writes,0); atomic_store(&checkpoint_triggered,0);
    atomic_store(&checkpoint_fault,fault);
    int rc=sqlite3_exec(db,"PRAGMA wal_checkpoint(TRUNCATE)",NULL,NULL,NULL);
    require(rc==(fault==1?SQLITE_IOERR_WRITE:SQLITE_IOERR_FSYNC) ||
            (rc==SQLITE_IOERR && sqlite3_extended_errcode(db)==(fault==1?SQLITE_IOERR_WRITE:SQLITE_IOERR_FSYNC)),
            "control update did not report the expected write/sync error");
    require(atomic_load(&checkpoint_triggered)==1 && atomic_load(&checkpoint_controls)==1,
            "checkpoint control hook missed or retried unexpectedly");
    require(atomic_load(&checkpoint_syncs)==fault,"fault targeted SQLite's earlier WAL sync");
    require(atomic_load(&checkpoint_main_writes)==0,"main write attempted after failed control update");
    checkpoint_images_match(path,wal,&main_before,&wal_before);
    ok(sqlite3_close(db),"failed control checkpoint close");
    require(atomic_load(&checkpoint_controls)==1 && atomic_load(&checkpoint_syncs)==fault &&
            atomic_load(&checkpoint_main_writes)==0,"close retried a poisoned checkpoint");
    checkpoint_images_match(path,wal,&main_before,&wal_before);
    atomic_store(&checkpoint_fault,0);
    free(main_before.bytes); free(wal_before.bytes);
    ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,MVFS_NAME),"control checkpoint reopen");
    if(exclusive) sql(db,"PRAGMA locking_mode=EXCLUSIVE;");
    sql(db,"PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;");
    checkpoint_rows_match(db);
    sql(db,"PRAGMA wal_checkpoint(TRUNCATE);");
    checkpoint_rows_match(db);
    ok(sqlite3_close(db),"control checkpoint recovered close");
  }
  ok(native->xSetSystemCall(native,NULL,NULL),"restore checkpoint syscalls");
  ok(native->xSetSystemCall(native,"fcntl",(sqlite3_syscall_ptr)injected_fcntl),"hook fcntl");
  puts("checkpoint control write/sync failures preserve main and WAL through close/reopen in shared and exclusive modes");
}
/* Every read SQLite makes fails, yet the bytes are fine: only the direct path can reach them. */
static void reads_past_the_page_cache(const char *directory,sqlite3_vfs *native) {
  char path[4096]; snprintf(path,sizeof(path),"%s/direct",directory);
  sqlite3 *db=NULL;
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,MVFS_NAME),"direct open");
  sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB);"
         "WITH RECURSIVE c(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM c WHERE x<50)"
         "INSERT INTO t SELECT x,randomblob(3000) FROM c; PRAGMA wal_checkpoint(TRUNCATE);");
  ok(sqlite3_close(db),"direct close");
  real_read=(void*)native->xGetSystemCall(native,"read");
  real_pread=(void*)native->xGetSystemCall(native,"pread");
  real_pread64=(void*)native->xGetSystemCall(native,"pread64");
  ok(native->xSetSystemCall(native,"read",(sqlite3_syscall_ptr)refused_read),"hook read");
  if(real_pread) ok(native->xSetSystemCall(native,"pread",(sqlite3_syscall_ptr)refused_pread),"hook pread");
  if(real_pread64) ok(native->xSetSystemCall(native,"pread64",(sqlite3_syscall_ptr)refused_pread64),"hook pread64");
  atomic_store(&refuse_reads,1);
  ok(sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE,MVFS_NAME),"direct reopen");
  sqlite3_stmt *stmt=NULL;
  ok(sqlite3_prepare_v2(db,"SELECT count(*), sum(length(v)) FROM t",-1,&stmt,NULL),"direct query");
  require(sqlite3_step(stmt)==SQLITE_ROW,"a refused read was not retried past the page cache");
  require(sqlite3_column_int(stmt,0)==50&&sqlite3_column_int(stmt,1)==150000,"direct read returned the wrong rows");
  sqlite3_finalize(stmt);
  atomic_store(&refuse_reads,0);
  require(atomic_load(&refused)>0,"read hook missed");
  ok(sqlite3_close(db),"direct final close");
  ok(native->xSetSystemCall(native,NULL,NULL),"restore native syscalls");
  ok(native->xSetSystemCall(native,"fcntl",(sqlite3_syscall_ptr)injected_fcntl),"hook fcntl");
}
int main(int argc,char **argv) {
  if(argc!=3) { fprintf(stderr,"usage: faults EXTENSION DIRECTORY\n"); return 2; }
  sqlite3 *loader=NULL; ok(sqlite3_open(":memory:",&loader),"loader open");
  ok(sqlite3_db_config(loader,SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,1,NULL),"enable load");
  char *error=NULL; int rc=sqlite3_load_extension(loader,argv[1],MVFS_ENTRY,&error);
  if(rc!=SQLITE_OK) { fprintf(stderr,"load: %s\n",error); return 1; }
  ok(sqlite3_close(loader),"loader close");
  void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); require(library!=NULL,"dlopen failed");
  int (*shutdown)(void)=(int(*)(void))dlsym(library,"monstervfs_shutdown"); require(shutdown!=NULL,"shutdown export missing");
  protected_vfs=sqlite3_vfs_find(MVFS_NAME); require(protected_vfs!=NULL,"registered VFS missing");
  sqlite3_vfs *native=sqlite3_vfs_find("unix");
  real_fcntl=(int(*)(int,int,...))native->xGetSystemCall(native,"fcntl");
  ok(native->xSetSystemCall(native,"fcntl",(sqlite3_syscall_ptr)injected_fcntl),"hook fcntl");
  sparse_and_temporary(argv[2]);
  checksum_only_replacement(argv[2]);
  syscall_failures(sqlite3_vfs_find("unix"));
  transaction_sync_failure(argv[2]);
  reader_progress_during_sync(argv[2]);
  file_controls(argv[2]);
  checkpoint_sync_failure(argv[2]);
  wal_trim_failure(argv[2],native);
  prefix_repair_after_failed_handle(argv[2]);
  checkpoint_control_failures(argv[2],native);
  reads_past_the_page_cache(argv[2],native);
  ok(native->xSetSystemCall(native,NULL,NULL),"restore native syscalls");
  ok(shutdown(),"worker shutdown"); ok(sqlite3_shutdown(),"SQLite shutdown");
  puts("native sparse, temporary, corruption, write/truncate/sync failures, direct reads, and reopen tests passed");
  return 0;
}
