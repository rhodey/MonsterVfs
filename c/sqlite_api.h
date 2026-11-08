#ifndef MONSTERVFS_SQLITE_API_H
#define MONSTERVFS_SQLITE_API_H
#include "sqlite3.h"
typedef struct MvfsApi MvfsApi;
typedef struct sqlite3_api_routines sqlite3_api_routines;
int mvfs_api_capture(const sqlite3_api_routines *, MvfsApi **, char **);
void mvfs_api_free(MvfsApi *);
int mvfs_libversion_number(const MvfsApi *api);
const char * mvfs_sourceid(const MvfsApi *api);
int mvfs_threadsafe(const MvfsApi *api);
sqlite3_vfs * mvfs_vfs_find(const MvfsApi *api, const char *name);
int mvfs_vfs_register(const MvfsApi *api, sqlite3_vfs *vfs, int make_default);
int mvfs_vfs_unregister(const MvfsApi *api, sqlite3_vfs *vfs);
const char * mvfs_uri_parameter(const MvfsApi *api, sqlite3_filename file, const char *name);
int mvfs_uri_boolean(const MvfsApi *api, sqlite3_filename file, const char *name, int fallback);
sqlite3_int64 mvfs_uri_int64(const MvfsApi *api, sqlite3_filename file, const char *name, sqlite3_int64 fallback);
sqlite3_file * mvfs_database_file_object(const MvfsApi *api, sqlite3_filename file);
int mvfs_open_v2(const MvfsApi *api, const char *name, sqlite3 **db, int flags, const char *vfs);
int mvfs_close(const MvfsApi *api, sqlite3 *db);
int mvfs_busy_timeout(const MvfsApi *api, sqlite3 *db, int ms);
int mvfs_wal_autocheckpoint(const MvfsApi *api, sqlite3 *db, int pages);
int mvfs_extended_errcode(const MvfsApi *api, sqlite3 *db);
const char * mvfs_errmsg(const MvfsApi *api, sqlite3 *db);
int mvfs_changes(const MvfsApi *api, sqlite3 *db);
int mvfs_get_autocommit(const MvfsApi *api, sqlite3 *db);
int mvfs_prepare_v2(const MvfsApi *api, sqlite3 *db, const char *sql, int bytes, sqlite3_stmt **stmt, const char **tail);
int mvfs_bind_int64(const MvfsApi *api, sqlite3_stmt *stmt, int index, sqlite3_int64 value);
int mvfs_step(const MvfsApi *api, sqlite3_stmt *stmt);
int mvfs_reset(const MvfsApi *api, sqlite3_stmt *stmt);
int mvfs_finalize(const MvfsApi *api, sqlite3_stmt *stmt);
int mvfs_column_int(const MvfsApi *api, sqlite3_stmt *stmt, int column);
sqlite3_int64 mvfs_column_int64(const MvfsApi *api, sqlite3_stmt *stmt, int column);
const void * mvfs_column_blob(const MvfsApi *api, sqlite3_stmt *stmt, int column);
const unsigned char * mvfs_column_text(const MvfsApi *api, sqlite3_stmt *stmt, int column);
int mvfs_column_bytes(const MvfsApi *api, sqlite3_stmt *stmt, int column);
int mvfs_column_type(const MvfsApi *api, sqlite3_stmt *stmt, int column);
int mvfs_auto_extension(const MvfsApi *api, void (*entry)(void));
int mvfs_file_control(const MvfsApi *api, sqlite3 *db, const char *schema, int op, void *arg);
void * mvfs_malloc64(const MvfsApi *api, sqlite3_uint64 bytes);
void mvfs_free(const MvfsApi *api, void *p);
void mvfs_interrupt(const MvfsApi *api, sqlite3 *db);
int mvfs_exec(const MvfsApi *, sqlite3 *, const char *, char **);
int mvfs_db_config_int(const MvfsApi *, sqlite3 *, int, int, int *);
int mvfs_bind_blob(const MvfsApi *, sqlite3_stmt *, int, const void *, int);
char *mvfs_strdup(const MvfsApi *, const char *);
void mvfs_log(const MvfsApi *, int, const char *);
int mvfs_db_readonly(const MvfsApi *, sqlite3 *, const char *);
int mvfs_connection_hook(const MvfsApi *, sqlite3 *, const char *, void *, void (*)(void *));
#endif
