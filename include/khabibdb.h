/*
 * KhabibDB — lightweight single-file relational store.
 *
 * Public surface. Internal layers (pager, journal, btree, ...) live in src/
 * and are deliberately not exposed here.
 *
 * On-disk format is little-endian only.
 */
#ifndef KHABIBDB_H
#define KHABIBDB_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    KHB_OK           =  0,
    KHB_ERR_IO       = -1,  /* read/write/fsync failed; see errno           */
    KHB_ERR_CORRUPT  = -2,  /* bad magic, bad checksum, malformed structure */
    KHB_ERR_LOCKED   = -3,  /* lock held by another process (fail-fast)     */
    KHB_ERR_NOTFOUND = -4,
    KHB_ERR_EXISTS   = -5,
    KHB_ERR_NOMEM    = -6,
    KHB_ERR_INVALID  = -7,  /* caller passed something nonsensical          */
    KHB_ERR_FULL     = -8,  /* out of pages / row too large / catalog full  */
    KHB_ERR_STATE    = -9   /* wrong transaction state for this call        */
} khb_status;

const char *khb_strerror(khb_status s);

typedef struct khb_db khb_db;

typedef enum {
    KHB_INT64  = 0,
    KHB_UINT64 = 1,
    KHB_DOUBLE = 2,
    KHB_BOOL   = 3,
    KHB_TEXT   = 4,
    KHB_BLOB   = 5
} khb_type;

#define KHB_NAME_MAX      32
#define KHB_COLUMNS_MAX   12
#define KHB_ROW_MAX     1000

/*
 * A row carries its own copy of the table schema, so it stays valid across
 * transaction boundaries and needs no allocation. Both members are private.
 */
#define KHB_ROW_SCHEMA_BYTES 512

typedef struct {
    unsigned char _schema[KHB_ROW_SCHEMA_BYTES];
    unsigned char _data[KHB_ROW_MAX];
} khb_row;

typedef struct {
    const char *name;
    khb_type    type;
    unsigned    size;   /* declared width; TEXT and BLOB only */
} khb_column;

khb_status khb_open(khb_db **out, const char *path, int create);
khb_status khb_close(khb_db *db);

khb_status khb_begin(khb_db *db, int read_only);
khb_status khb_commit(khb_db *db);
khb_status khb_rollback(khb_db *db);

khb_status khb_create_table(khb_db *db, const char *table,
                            const khb_column *cols, int ncols);
khb_status khb_table_exists(khb_db *db, const char *table);

khb_status khb_row_init(khb_row *row, khb_db *db, const char *table);
int        khb_row_columns(const khb_row *row);
khb_type   khb_row_type(const khb_row *row, int col);

khb_status khb_row_set_int(khb_row *row, int col, int64_t v);
khb_status khb_row_get_int(const khb_row *row, int col, int64_t *out);
khb_status khb_row_set_uint(khb_row *row, int col, uint64_t v);
khb_status khb_row_get_uint(const khb_row *row, int col, uint64_t *out);
khb_status khb_row_set_double(khb_row *row, int col, double v);
khb_status khb_row_get_double(const khb_row *row, int col, double *out);
khb_status khb_row_set_bool(khb_row *row, int col, int v);
khb_status khb_row_get_bool(const khb_row *row, int col, int *out);
khb_status khb_row_set_text(khb_row *row, int col, const char *s);
khb_status khb_row_get_text(const khb_row *row, int col, char *out, size_t cap);
khb_status khb_row_set_blob(khb_row *row, int col, const void *data, size_t len);
khb_status khb_row_get_blob(const khb_row *row, int col, void *out, size_t cap);
khb_status khb_row_key(const khb_row *row, int64_t *out);

khb_status khb_insert(khb_db *db, const char *table, const khb_row *row);
khb_status khb_get(khb_db *db, const char *table, int64_t key, khb_row *out);
khb_status khb_delete(khb_db *db, const char *table, int64_t key);
khb_status khb_count(khb_db *db, const char *table, uint32_t *out);

/* Return nonzero to accept the row. NULL means accept everything. */
typedef int (*khb_predicate)(const khb_row *row, void *ctx);

/* Return KHB_OK to continue; anything else stops the scan and is returned. */
typedef khb_status (*khb_visitor)(const khb_row *row, void *ctx);

khb_status khb_scan(khb_db *db, const char *table, khb_predicate pred,
                    khb_visitor visit, void *ctx);
khb_status khb_scan_range(khb_db *db, const char *table,
                          int64_t lo, int64_t hi, khb_predicate pred,
                          khb_visitor visit, void *ctx);

#endif /* KHABIBDB_H */
