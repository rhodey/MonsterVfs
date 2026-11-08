#ifndef MONSTERVFS_H
#define MONSTERVFS_H
#ifdef __cplusplus
extern "C" {
#endif
/* Available in the host-linked static build, under these names in either format. */
int monstervfs_register(void);
int monstervfs_extra_init(const char *unused);
/* sqlite3_file_control(db, "main", ..., output):
 * ID copies 16 bytes. STATUS allocates a UTF-8 JSON string; sqlite3_free it. */
#define MONSTERVFS_FCNTL_ID 0x4d560001
#define MONSTERVFS_FCNTL_STATUS 0x4d560002
/* Extended result code: WAL damage below the backfill watermark. Main may hold
 * pages newer than any retained prefix, so recovery refuses instead of cutting.
 * The primary code is SQLITE_CORRUPT. The database needs repair from a replica. */
#define MONSTERVFS_CORRUPT_BACKFILLED (11 | (0x4d << 8))
/* Optional, terminal maintenance stop; idempotent. Call outside SQLite callbacks.
 * Final connection teardown already waits for active maintenance. */
int monstervfs_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
