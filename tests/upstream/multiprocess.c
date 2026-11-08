/* SQLite's mptest, unchanged. Each client process loads the extension itself. */
#include <stdlib.h>
#define main mptest_main
#include "mptest.c"
#undef main
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
  }
  return mptest_main(argc, argv);
}
