/* Host SQLite API dispatch. No SQLite engine is linked in loadable builds. */
#define SQLITE_CORE 1
#include "sqlite3ext.h"
#include "sqlite_api.h"
#include <stdlib.h>
#include <string.h>

struct MvfsApi {
  int (*libversion_number)(void);
  const char * (*sourceid)(void);
  int (*threadsafe)(void);
  int (*compileoption_used)(const char *option);
  sqlite3_vfs * (*vfs_find)(const char *name);
  int (*vfs_register)(sqlite3_vfs *vfs, int make_default);
  int (*vfs_unregister)(sqlite3_vfs *vfs);
  const char * (*uri_parameter)(sqlite3_filename file, const char *name);
  int (*uri_boolean)(sqlite3_filename file, const char *name, int fallback);
  sqlite3_int64 (*uri_int64)(sqlite3_filename file, const char *name, sqlite3_int64 fallback);
  sqlite3_file * (*database_file_object)(sqlite3_filename file);
  int (*open_v2)(const char *name, sqlite3 **db, int flags, const char *vfs);
  int (*close)(sqlite3 *db);
  int (*busy_timeout)(sqlite3 *db, int ms);
  int (*wal_autocheckpoint)(sqlite3 *db, int pages);
  int (*extended_errcode)(sqlite3 *db);
  const char * (*errmsg)(sqlite3 *db);
  int (*changes)(sqlite3 *db);
  int (*get_autocommit)(sqlite3 *db);
  int (*prepare_v2)(sqlite3 *db, const char *sql, int bytes, sqlite3_stmt **stmt, const char **tail);
  int (*bind_int64)(sqlite3_stmt *stmt, int index, sqlite3_int64 value);
  int (*step)(sqlite3_stmt *stmt);
  int (*reset)(sqlite3_stmt *stmt);
  int (*finalize)(sqlite3_stmt *stmt);
  int (*column_int)(sqlite3_stmt *stmt, int column);
  sqlite3_int64 (*column_int64)(sqlite3_stmt *stmt, int column);
  const void * (*column_blob)(sqlite3_stmt *stmt, int column);
  const unsigned char * (*column_text)(sqlite3_stmt *stmt, int column);
  int (*column_bytes)(sqlite3_stmt *stmt, int column);
  int (*column_type)(sqlite3_stmt *stmt, int column);
  int (*file_control)(sqlite3 *db, const char *schema, int op, void *arg);
  void * (*malloc64)(sqlite3_uint64 bytes);
  void (*free)(void *p);
  void (*interrupt)(sqlite3 *db);
  int (*exec)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
  int (*db_config)(sqlite3 *, int, ...);
  int (*bind_blob)(sqlite3_stmt *, int, const void *, int, void (*)(void *));
  void (*log)(int, const char *, ...);
  int (*auto_extension)(void (*)(void));
  int (*db_readonly)(sqlite3 *, const char *);
  int (*create_function_v2)(sqlite3 *, const char *, int, int, void *,
      void (*)(sqlite3_context *, int, sqlite3_value **),
      void (*)(sqlite3_context *, int, sqlite3_value **),
      void (*)(sqlite3_context *), void (*)(void *));
};

int mvfs_api_capture(const sqlite3_api_routines *source, MvfsApi **out, char **error) {
  if (!out) return SQLITE_MISUSE;
  *out = NULL;
  if (error) *error = NULL;
  MvfsApi api;
#ifdef MVFS_STATIC
  (void)source;
  api.libversion_number = sqlite3_libversion_number;
  api.sourceid = sqlite3_sourceid;
  api.threadsafe = sqlite3_threadsafe;
  api.compileoption_used = sqlite3_compileoption_used;
  api.vfs_find = sqlite3_vfs_find;
  api.vfs_register = sqlite3_vfs_register;
  api.vfs_unregister = sqlite3_vfs_unregister;
  api.uri_parameter = sqlite3_uri_parameter;
  api.uri_boolean = sqlite3_uri_boolean;
  api.uri_int64 = sqlite3_uri_int64;
  api.database_file_object = sqlite3_database_file_object;
  api.open_v2 = sqlite3_open_v2;
  api.close = sqlite3_close;
  api.busy_timeout = sqlite3_busy_timeout;
  api.wal_autocheckpoint = sqlite3_wal_autocheckpoint;
  api.extended_errcode = sqlite3_extended_errcode;
  api.errmsg = sqlite3_errmsg;
  api.changes = sqlite3_changes;
  api.get_autocommit = sqlite3_get_autocommit;
  api.prepare_v2 = sqlite3_prepare_v2;
  api.bind_int64 = sqlite3_bind_int64;
  api.step = sqlite3_step;
  api.reset = sqlite3_reset;
  api.finalize = sqlite3_finalize;
  api.column_int = sqlite3_column_int;
  api.column_int64 = sqlite3_column_int64;
  api.column_blob = sqlite3_column_blob;
  api.column_text = sqlite3_column_text;
  api.column_bytes = sqlite3_column_bytes;
  api.column_type = sqlite3_column_type;
  api.file_control = sqlite3_file_control;
  api.malloc64 = sqlite3_malloc64;
  api.free = sqlite3_free;
  api.interrupt = sqlite3_interrupt;
  api.exec = sqlite3_exec;
  api.db_config = sqlite3_db_config;
  api.bind_blob = sqlite3_bind_blob;
  api.log = sqlite3_log;
  api.auto_extension = sqlite3_auto_extension;
  api.db_readonly = sqlite3_db_readonly;
  api.create_function_v2 = sqlite3_create_function_v2;
#else
  if (!source || !source->libversion_number || source->libversion_number() != 3054000) {
    if (source && source->mprintf && error) *error = source->mprintf("%s", "monstervfs requires SQLite 3.54.0");
    return SQLITE_ERROR;
  }
  api.libversion_number = source->libversion_number;
  api.sourceid = source->sourceid;
  api.threadsafe = source->xthreadsafe;
  api.compileoption_used = source->compileoption_used;
  api.vfs_find = source->vfs_find;
  api.vfs_register = source->vfs_register;
  api.vfs_unregister = source->vfs_unregister;
  api.uri_parameter = source->uri_parameter;
  api.uri_boolean = source->uri_boolean;
  api.uri_int64 = source->uri_int64;
  api.database_file_object = source->database_file_object;
  api.open_v2 = source->open_v2;
  api.close = source->close;
  api.busy_timeout = source->busy_timeout;
  api.wal_autocheckpoint = source->wal_autocheckpoint;
  api.extended_errcode = source->extended_errcode;
  api.errmsg = source->errmsg;
  api.changes = source->changes;
  api.get_autocommit = source->get_autocommit;
  api.prepare_v2 = source->prepare_v2;
  api.bind_int64 = source->bind_int64;
  api.step = source->step;
  api.reset = source->reset;
  api.finalize = source->finalize;
  api.column_int = source->column_int;
  api.column_int64 = source->column_int64;
  api.column_blob = source->column_blob;
  api.column_text = source->column_text;
  api.column_bytes = source->column_bytes;
  api.column_type = source->column_type;
  api.file_control = source->file_control;
  api.malloc64 = source->malloc64;
  api.free = source->free;
  api.interrupt = source->interruptx;
  api.exec = source->exec;
  api.db_config = source->db_config;
  api.bind_blob = source->bind_blob;
  api.log = source->log;
  api.auto_extension = source->auto_extension;
  api.db_readonly = source->db_readonly;
  api.create_function_v2 = source->create_function_v2;
#endif
  const char *reason = NULL;
  if (api.libversion_number() != 3054000 || strcmp(api.sourceid(),
      "2026-10-09 15:46:58 be8d059e9a49089ab2dce5ed26dd87aaf598fdc5fbe0b107c0dd758464bcd5e3") != 0)
    reason = "monstervfs requires the audited SQLite 3.54.0 source";
  else if (!api.threadsafe()) reason = "monstervfs requires a thread-safe SQLite build";
  else if (!api.compileoption_used("ENABLE_DBPAGE_VTAB")) reason = "monstervfs requires SQLITE_ENABLE_DBPAGE_VTAB";
  if (reason) {
    if (error) *error = mvfs_strdup(&api, reason);
    return SQLITE_ERROR;
  }
  MvfsApi *copy = malloc(sizeof(*copy));
  if (!copy) return SQLITE_NOMEM;
  *copy = api;
  *out = copy;
  return SQLITE_OK;
}

void mvfs_api_free(MvfsApi *api) { free(api); }
int mvfs_libversion_number(const MvfsApi *api) { return api->libversion_number(); }
const char * mvfs_sourceid(const MvfsApi *api) { return api->sourceid(); }
int mvfs_threadsafe(const MvfsApi *api) { return api->threadsafe(); }
sqlite3_vfs * mvfs_vfs_find(const MvfsApi *api, const char *name) { return api->vfs_find(name); }
int mvfs_vfs_register(const MvfsApi *api, sqlite3_vfs *vfs, int make_default) { return api->vfs_register(vfs, make_default); }
int mvfs_vfs_unregister(const MvfsApi *api, sqlite3_vfs *vfs) { return api->vfs_unregister(vfs); }
const char * mvfs_uri_parameter(const MvfsApi *api, sqlite3_filename file, const char *name) { return api->uri_parameter(file, name); }
int mvfs_uri_boolean(const MvfsApi *api, sqlite3_filename file, const char *name, int fallback) { return api->uri_boolean(file, name, fallback); }
sqlite3_int64 mvfs_uri_int64(const MvfsApi *api, sqlite3_filename file, const char *name, sqlite3_int64 fallback) { return api->uri_int64(file, name, fallback); }
sqlite3_file * mvfs_database_file_object(const MvfsApi *api, sqlite3_filename file) { return api->database_file_object(file); }
int mvfs_open_v2(const MvfsApi *api, const char *name, sqlite3 **db, int flags, const char *vfs) { return api->open_v2(name, db, flags, vfs); }
int mvfs_close(const MvfsApi *api, sqlite3 *db) { return api->close(db); }
int mvfs_busy_timeout(const MvfsApi *api, sqlite3 *db, int ms) { return api->busy_timeout(db, ms); }
int mvfs_wal_autocheckpoint(const MvfsApi *api, sqlite3 *db, int pages) { return api->wal_autocheckpoint(db, pages); }
int mvfs_extended_errcode(const MvfsApi *api, sqlite3 *db) { return api->extended_errcode(db); }
const char * mvfs_errmsg(const MvfsApi *api, sqlite3 *db) { return api->errmsg(db); }
int mvfs_changes(const MvfsApi *api, sqlite3 *db) { return api->changes(db); }
int mvfs_get_autocommit(const MvfsApi *api, sqlite3 *db) { return api->get_autocommit(db); }
int mvfs_prepare_v2(const MvfsApi *api, sqlite3 *db, const char *sql, int bytes, sqlite3_stmt **stmt, const char **tail) { return api->prepare_v2(db, sql, bytes, stmt, tail); }
int mvfs_bind_int64(const MvfsApi *api, sqlite3_stmt *stmt, int index, sqlite3_int64 value) { return api->bind_int64(stmt, index, value); }
int mvfs_step(const MvfsApi *api, sqlite3_stmt *stmt) { return api->step(stmt); }
int mvfs_reset(const MvfsApi *api, sqlite3_stmt *stmt) { return api->reset(stmt); }
int mvfs_finalize(const MvfsApi *api, sqlite3_stmt *stmt) { return api->finalize(stmt); }
int mvfs_column_int(const MvfsApi *api, sqlite3_stmt *stmt, int column) { return api->column_int(stmt, column); }
sqlite3_int64 mvfs_column_int64(const MvfsApi *api, sqlite3_stmt *stmt, int column) { return api->column_int64(stmt, column); }
const void * mvfs_column_blob(const MvfsApi *api, sqlite3_stmt *stmt, int column) { return api->column_blob(stmt, column); }
const unsigned char * mvfs_column_text(const MvfsApi *api, sqlite3_stmt *stmt, int column) { return api->column_text(stmt, column); }
int mvfs_column_bytes(const MvfsApi *api, sqlite3_stmt *stmt, int column) { return api->column_bytes(stmt, column); }
int mvfs_column_type(const MvfsApi *api, sqlite3_stmt *stmt, int column) { return api->column_type(stmt, column); }
int mvfs_auto_extension(const MvfsApi *api, void (*entry)(void)) { return api->auto_extension(entry); }
int mvfs_file_control(const MvfsApi *api, sqlite3 *db, const char *schema, int op, void *arg) { return api->file_control(db, schema, op, arg); }
void * mvfs_malloc64(const MvfsApi *api, sqlite3_uint64 bytes) { return api->malloc64(bytes); }
void mvfs_free(const MvfsApi *api, void *p) { api->free(p); }
void mvfs_interrupt(const MvfsApi *api, sqlite3 *db) { api->interrupt(db); }
int mvfs_exec(const MvfsApi *api, sqlite3 *db, const char *sql, char **error) {
  return api->exec(db, sql, NULL, NULL, error);
}
int mvfs_db_config_int(const MvfsApi *api, sqlite3 *db, int option, int value, int *out) {
  return api->db_config(db, option, value, out);
}
int mvfs_bind_blob(const MvfsApi *api, sqlite3_stmt *stmt, int index, const void *value, int bytes) {
  return api->bind_blob(stmt, index, value, bytes, SQLITE_TRANSIENT);
}
char *mvfs_strdup(const MvfsApi *api, const char *text) {
  size_t bytes = strlen(text) + 1;
  char *copy = api->malloc64(bytes);
  if (copy) memcpy(copy, text, bytes);
  return copy;
}
void mvfs_log(const MvfsApi *api, int code, const char *text) { api->log(code, "%s", text); }

int mvfs_db_readonly(const MvfsApi *api, sqlite3 *db, const char *schema) {
  return api->db_readonly(db, schema);
}
static void connection_value(sqlite3_context *context, int count, sqlite3_value **values) {
  (void)context; (void)count; (void)values;
}
int mvfs_connection_hook(const MvfsApi *api, sqlite3 *db, const char *name,
    void *data, void (*destroy)(void *)) {
  return api->create_function_v2(db, name, 0, SQLITE_UTF8 | SQLITE_DIRECTONLY,
      data, connection_value, NULL, NULL, destroy);
}
