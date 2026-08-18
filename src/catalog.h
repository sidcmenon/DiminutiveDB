#ifndef KHB_CATALOG_H
#define KHB_CATALOG_H

#include <stdint.h>

#include "khabibdb.h"
#include "page.h"
#include "pager.h"
#include "journal.h"

#define KHB_MAX_NAME     32
#define KHB_MAX_COLUMNS  12
#define KHB_MAX_TABLES    8
#define KHB_MAX_ROW_SIZE 1000

typedef enum {
    COL_INT64  = 0,
    COL_UINT64 = 1,
    COL_DOUBLE = 2,
    COL_BOOL   = 3,
    COL_TEXT   = 4,
    COL_BLOB   = 5
} col_type;
#pragma pack(push, 1)

typedef struct {
    char     name[KHB_MAX_NAME];
    uint8_t  type;
    uint16_t size;
    uint16_t offset;
} column_def_t;

typedef struct {
    char         name[KHB_MAX_NAME];
    uint8_t      column_count;
    uint16_t     row_size;
    uint32_t     root_page;
    column_def_t columns[KHB_MAX_COLUMNS];
} table_def_t;


typedef struct {
    page_header_t hdr;
    uint16_t      table_count;
    uint32_t      next_catalog_page;
} catalog_page_header_t;

#pragma pack(pop)

_Static_assert(sizeof(column_def_t) == 37, "column_def_t must be 37 bytes");
_Static_assert(sizeof(table_def_t) == 483, "table_def_t must be 483 bytes");
_Static_assert(sizeof(catalog_page_header_t) == 18,
               "catalog_page_header_t must be 18 bytes");
_Static_assert(sizeof(catalog_page_header_t)
               + KHB_MAX_TABLES * sizeof(table_def_t) <= KHB_PAGE_SIZE,
               "catalog must fit on a single page");

typedef struct {
    uint32_t    page_id;
    int         table_count;
    table_def_t tables[KHB_MAX_TABLES];
    int         dirty;
} catalog_t;

void catalog_init(catalog_t *c);

khb_status catalog_load(catalog_t *c, pager_t *p);
khb_status catalog_flush(catalog_t *c, pager_t *p, journal_t *j);

khb_status catalog_create_table(catalog_t *c, const char *name,
                                const column_def_t *cols, int ncols,
                                table_def_t **out);

khb_status   catalog_find_table(catalog_t *c, const char *name,
                                table_def_t **out);
table_def_t *catalog_table_at(catalog_t *c, int index);
int          catalog_table_count(const catalog_t *c);
void         catalog_set_root_page(catalog_t *c, table_def_t *t,
                                   uint32_t root_page);

void        column_def_set(column_def_t *c, const char *name, uint8_t type,
                           uint16_t size);
const char *col_type_name(uint8_t type);

#endif /* KHB_CATALOG_H */
