#include "catalog.h"
#include <string.h>

static uint16_t fixed_size_for(uint8_t type, uint16_t declared){
    switch(type){
        case COL_INT64:
        case COL_UINT64:
        case COL_DOUBLE: return 8;
        case COL_BOOL: return 1;
        case COL_TEXT:
        case COL_BLOB: return declared;
        default: return 0;
    }
}

static int name_is_valid(const char *name){
    size_t i, n;

    if (name == NULL)
        return 0;

    n = strlen(name);
    if (n == 0 || n >= KHB_MAX_NAME)
        return 0;

    for (i = 0; i < n; i++) {
        char ch = name[i];
        int  ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
               || (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok)
            return 0;
    }
    return 1;
}

static khb_status validate_schema(const column_def_t *cols, int ncols){
    int      i, k;
    uint32_t total = 0;

    if (cols == NULL || ncols <= 0 || ncols > KHB_MAX_COLUMNS)
        return KHB_ERR_INVALID;

    if (cols[0].type != COL_INT64)
        return KHB_ERR_INVALID;

    for (i = 0; i < ncols; i++) {
        uint16_t sz;

        if (!name_is_valid(cols[i].name))
            return KHB_ERR_INVALID;
        if (cols[i].type > COL_BLOB)
            return KHB_ERR_INVALID;

        for (k = 0; k < i; k++)
            if (strcmp(cols[i].name, cols[k].name) == 0)
                return KHB_ERR_EXISTS;

        sz = fixed_size_for(cols[i].type, cols[i].size);
        if (sz == 0 || sz > KHB_MAX_ROW_SIZE)
            return KHB_ERR_INVALID;

        total += sz;
    }

    if (total > KHB_MAX_ROW_SIZE)
        return KHB_ERR_FULL;

    return KHB_OK;
}

static void compute_layout(table_def_t *t){
    uint16_t off = 0;
    int      i;

    for (i = 0; i < t->column_count; i++) {
        t->columns[i].size   = fixed_size_for(t->columns[i].type,
                                              t->columns[i].size);
        t->columns[i].offset = off;
        off = (uint16_t)(off + t->columns[i].size);
    }

    t->row_size = off;
}

static khb_status catalog_parse_page(catalog_t *c, const uint8_t *page){
    const catalog_page_header_t *ch = (const catalog_page_header_t *)page;
    int                          i;

    if (page_type_of(page) != PAGE_TYPE_CATALOG)
        return KHB_ERR_CORRUPT;
    if (ch->table_count > KHB_MAX_TABLES)
        return KHB_ERR_CORRUPT;

    c->table_count = (int)ch->table_count;

    for (i = 0; i < c->table_count; i++) {
        size_t off = sizeof(catalog_page_header_t)
                   + (size_t)i * sizeof(table_def_t);

        memcpy(&c->tables[i], page + off, sizeof(table_def_t));

        if (c->tables[i].column_count == 0 ||
            c->tables[i].column_count > KHB_MAX_COLUMNS ||
            c->tables[i].row_size == 0 ||
            c->tables[i].row_size > KHB_MAX_ROW_SIZE)
            return KHB_ERR_CORRUPT;
    }

    return KHB_OK;
}

static void catalog_build_page(const catalog_t *c, uint8_t *page,
                               uint32_t page_id){
    catalog_page_header_t *ch;
    int i;

    page_init(page, page_id, PAGE_TYPE_CATALOG);

    ch = (catalog_page_header_t *)page;
    ch->table_count       = (uint16_t)c->table_count;
    ch->next_catalog_page = 0;

    for (i = 0; i < c->table_count; i++) {
        size_t off = sizeof(catalog_page_header_t)
                   + (size_t)i * sizeof(table_def_t);
        memcpy(page + off, &c->tables[i], sizeof(table_def_t));
    }
}
void catalog_init(catalog_t *c)
{
    memset(c, 0, sizeof *c);
}

khb_status catalog_load(catalog_t *c, pager_t *p)
{
    uint8_t    page[KHB_PAGE_SIZE];
    uint32_t   root;
    khb_status rc;

    if (c == NULL || p == NULL)
        return KHB_ERR_INVALID;

    catalog_init(c);
    root = pager_catalog_root(p);
    if (root == 0)
        return KHB_OK;

    rc = pager_read(p, root, page);
    if (rc != KHB_OK)
        return rc;

    rc = catalog_parse_page(c, page);
    if (rc != KHB_OK)
        return rc;

    c->page_id = root;
    c->dirty   = 0;
    return KHB_OK;
}

khb_status catalog_flush(catalog_t *c, pager_t *p, journal_t *j)
{
    uint8_t    page[KHB_PAGE_SIZE];
    khb_status rc;

    if (c == NULL || p == NULL || j == NULL)
        return KHB_ERR_INVALID;
    if (!c->dirty)
        return KHB_OK;

    if (!journal_is_active(j))
        return KHB_ERR_STATE;

    if (c->page_id == 0) {

        rc = pager_alloc(p, &c->page_id);
        if (rc != KHB_OK)
            return rc;

        pager_set_catalog_root(p, c->page_id);
    }

    rc = journal_note_page(j, p, c->page_id);
    if (rc != KHB_OK)
        return rc;

    catalog_build_page(c, page, c->page_id);

    rc = pager_write(p, c->page_id, page);
    if (rc != KHB_OK)
        return rc;

    c->dirty = 0;
    return KHB_OK;
}

khb_status catalog_create_table(catalog_t *c, const char *name,
                                const column_def_t *cols, int ncols,
                                table_def_t **out){
    table_def_t *t;
    khb_status   rc;

    if (c == NULL || name == NULL)
        return KHB_ERR_INVALID;
    if (!name_is_valid(name))
        return KHB_ERR_INVALID;
    if (c->table_count >= KHB_MAX_TABLES)
        return KHB_ERR_FULL;

    if (catalog_find_table(c, name, NULL) == KHB_OK)
        return KHB_ERR_EXISTS;

    rc = validate_schema(cols, ncols);
    if (rc != KHB_OK)
        return rc;

    t = &c->tables[c->table_count];
    memset(t, 0, sizeof *t);

    strncpy(t->name, name, KHB_MAX_NAME - 1);
    t->column_count = (uint8_t)ncols;
    t->root_page    = 0;           

    memcpy(t->columns, cols, (size_t)ncols * sizeof(column_def_t));
    compute_layout(t);

    c->table_count++;
    c->dirty = 1;

    if (out != NULL)
        *out = t;
    return KHB_OK;
}

khb_status catalog_find_table(catalog_t *c, const char *name,
                              table_def_t **out){
    int i;

    if (c == NULL || name == NULL)
        return KHB_ERR_INVALID;
    for (i = 0; i < c->table_count; i++) {
        if (strcmp(c->tables[i].name, name) == 0) {
            if (out != NULL)
                *out = &c->tables[i];
            return KHB_OK;
        }
    }

    return KHB_ERR_NOTFOUND;
}

table_def_t *catalog_table_at(catalog_t *c, int index)
{
    if (c == NULL || index < 0 || index >= c->table_count)
        return NULL;
    return &c->tables[index];
}

int catalog_table_count(const catalog_t *c)
{
    return (c == NULL) ? 0 : c->table_count;
}

void catalog_set_root_page(catalog_t *c, table_def_t *t, uint32_t root_page)
{
    if (c == NULL || t == NULL)
        return;

    t->root_page = root_page;
    c->dirty     = 1;
}

void column_def_set(column_def_t *c, const char *name, uint8_t type,
                    uint16_t size)
{
    memset(c, 0, sizeof *c);
    strncpy(c->name, name, KHB_MAX_NAME - 1);
    c->type   = type;
    c->size   = size;
    c->offset = 0;
}

const char *col_type_name(uint8_t type)
{
    switch (type) {
    case COL_INT64:  return "INT64";
    case COL_UINT64: return "UINT64";
    case COL_DOUBLE: return "DOUBLE";
    case COL_BOOL:   return "BOOL";
    case COL_TEXT:   return "TEXT";
    case COL_BLOB:   return "BLOB";
    }
    return "UNKNOWN";
}