/* SQLite's threadtest3, unchanged, with MonsterVfs as the default VFS when asked for. */
#include <stdlib.h>
#define main threadtest3_main
#include "threadtest3.c"
#undef main
int sqlite3_multiplex_initialize(const char *name, int make_default) { (void)name; (void)make_default; return SQLITE_ERROR; }
int sqlite3_multiplex_shutdown(int unregister) { (void)unregister; return SQLITE_OK; }
int main(int argc, char **argv) {
  const char *extension = getenv("MONSTERVFS_EXTENSION");
  if (extension) {
    sqlite3 *loader = NULL;
    char *error = NULL;
    sqlite3_open(":memory:", &loader);
    sqlite3_enable_load_extension(loader, 1);
    if (sqlite3_load_extension(loader, extension, MVFS_ENTRY, &error)) {
      fprintf(stderr, "load: %s\n", error);
      return 1;
    }
    sqlite3_close(loader);
    sqlite3_vfs_register(sqlite3_vfs_find(MVFS_NAME), 1);
  }
  return threadtest3_main(argc, argv);
}
