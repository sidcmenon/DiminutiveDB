#include <string.h>

#include "test_util.h"
#include "catalog.h"

static int build_people(column_def_t *cols)
{
    column_def_set(&cols[0], "id",     COL_INT64,  0);
    column_def_set(&cols[1], "name",   COL_TEXT,  32);
    column_def_set(&cols[2], "age",    COL_INT64,  0);
    column_def_set(&cols[3], "score",  COL_DOUBLE, 0);
    column_def_set(&cols[4], "active", COL_BOOL,   0);
    return 5;
}

static void test_create_and_find(void)
{
    catalog_t    c;
    column_def_t cols[8];
    table_def_t *t = NULL;
    int          n = build_people(cols);

    catalog_init(&c);
    CHECK_EQ(catalog_table_count(&c), 0);

    CHECK_STATUS(catalog_create_table(&c, "people", cols, n, &t), KHB_OK);
    CHECK(t != NULL);
    CHECK_EQ(catalog_table_count(&c), 1);
    CHECK_EQ(strcmp(t->name, "people"), 0);
    CHECK_EQ(t->column_count, 5);
    CHECK_EQ(t->root_page, 0);

    t = NULL;
    CHECK_STATUS(catalog_find_table(&c, "people", &t), KHB_OK);
    CHECK(t != NULL);
    CHECK_STATUS(catalog_find_table(&c, "nobody", NULL), KHB_ERR_NOTFOUND);
    CHECK(catalog_table_at(&c, 0) != NULL);
    CHECK(catalog_table_at(&c, 1) == NULL);
}

static void test_offsets_and_row_size(void)
{
    catalog_t    c;
    column_def_t cols[8];
    table_def_t *t = NULL;
    int          n = build_people(cols);

    catalog_init(&c);
    CHECK_STATUS(catalog_create_table(&c, "people", cols, n, &t), KHB_OK);

    CHECK_EQ(t->columns[0].offset, 0);   CHECK_EQ(t->columns[0].size, 8);
    CHECK_EQ(t->columns[1].offset, 8);   CHECK_EQ(t->columns[1].size, 32);
    CHECK_EQ(t->columns[2].offset, 40);  CHECK_EQ(t->columns[2].size, 8);
    CHECK_EQ(t->columns[3].offset, 48);  CHECK_EQ(t->columns[3].size, 8);
    CHECK_EQ(t->columns[4].offset, 56);  CHECK_EQ(t->columns[4].size, 1);
    CHECK_EQ(t->row_size, 57);
}

static void test_duplicate_table_rejected(void)
{
    catalog_t    c;
    column_def_t cols[8];
    int          n = build_people(cols);

    catalog_init(&c);
    CHECK_STATUS(catalog_create_table(&c, "people", cols, n, NULL), KHB_OK);
    CHECK_STATUS(catalog_create_table(&c, "people", cols, n, NULL),
                 KHB_ERR_EXISTS);
    CHECK_EQ(catalog_table_count(&c), 1);
}

static void test_schema_validation(void)
{
    catalog_t    c;
    column_def_t cols[KHB_MAX_COLUMNS + 2];
    int          i;

    catalog_init(&c);

    /* Column 0 must be the int64 primary key. */
    column_def_set(&cols[0], "name", COL_TEXT, 16);
    column_def_set(&cols[1], "id",   COL_INT64, 0);
    CHECK_STATUS(catalog_create_table(&c, "bad_pk", cols, 2, NULL),
                 KHB_ERR_INVALID);

    /* Duplicate column names. */
    column_def_set(&cols[0], "id", COL_INT64, 0);
    column_def_set(&cols[1], "id", COL_INT64, 0);
    CHECK_STATUS(catalog_create_table(&c, "dup_col", cols, 2, NULL),
                 KHB_ERR_EXISTS);

    /* Illegal characters in a name. */
    column_def_set(&cols[0], "id",      COL_INT64, 0);
    column_def_set(&cols[1], "bad name", COL_TEXT, 8);
    CHECK_STATUS(catalog_create_table(&c, "bad_colname", cols, 2, NULL),
                 KHB_ERR_INVALID);

    /* Zero-width TEXT. */
    column_def_set(&cols[1], "empty", COL_TEXT, 0);
    CHECK_STATUS(catalog_create_table(&c, "zero_text", cols, 2, NULL),
                 KHB_ERR_INVALID);

    /* Too many columns. */
    for (i = 0; i < KHB_MAX_COLUMNS + 1; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "c%d", i);
        column_def_set(&cols[i], nm, i == 0 ? COL_INT64 : COL_BOOL, 0);
    }
    CHECK_STATUS(catalog_create_table(&c, "wide", cols,
                                      KHB_MAX_COLUMNS + 1, NULL),
                 KHB_ERR_INVALID);

    /* Row wider than the fanout limit allows. */
    column_def_set(&cols[0], "id",  COL_INT64, 0);
    column_def_set(&cols[1], "big", COL_BLOB, KHB_MAX_ROW_SIZE);
    CHECK_STATUS(catalog_create_table(&c, "fat", cols, 2, NULL), KHB_ERR_FULL);

    CHECK_EQ(catalog_table_count(&c), 0);
    CHECK_STATUS(catalog_create_table(&c, "bad table", cols, 2, NULL),
                 KHB_ERR_INVALID);
}

static void test_table_limit(void)
{
    catalog_t    c;
    column_def_t cols[8];
    int          n = build_people(cols);
    int          i;

    catalog_init(&c);
    for (i = 0; i < KHB_MAX_TABLES; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "t%d", i);
        CHECK_STATUS(catalog_create_table(&c, nm, cols, n, NULL), KHB_OK);
    }
    CHECK_EQ(catalog_table_count(&c), KHB_MAX_TABLES);
    CHECK_STATUS(catalog_create_table(&c, "overflow", cols, n, NULL),
                 KHB_ERR_FULL);
}

static void test_flush_requires_transaction(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    catalog_t c;
    column_def_t cols[8];
    int          n = build_people(cols);

    khb_temp_path(path, sizeof path, "catnotxn");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    catalog_init(&c);
    journal_init(&j);

    CHECK_STATUS(catalog_create_table(&c, "people", cols, n, NULL), KHB_OK);
    CHECK_STATUS(catalog_flush(&c, &p, &j), KHB_ERR_STATE);

    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_persist_across_reopen(void)
{
    char         path[PATH_MAX];
    pager_t      p;
    journal_t    j;
    catalog_t    c;
    column_def_t cols[8];
    table_def_t *t = NULL;
    int          n = build_people(cols);

    khb_temp_path(path, sizeof path, "catpersist");

    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    catalog_init(&c);
    CHECK_STATUS(catalog_load(&c, &p), KHB_OK);
    CHECK_EQ(catalog_table_count(&c), 0);
    CHECK_EQ(pager_catalog_root(&p), 0);

    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(catalog_create_table(&c, "people", cols, n, &t), KHB_OK);
    catalog_set_root_page(&c, t, 42);
    CHECK_STATUS(catalog_flush(&c, &p, &j), KHB_OK);
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK(pager_catalog_root(&p) != 0);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    catalog_init(&c);
    CHECK_STATUS(catalog_load(&c, &p), KHB_OK);
    CHECK_EQ(catalog_table_count(&c), 1);

    t = NULL;
    CHECK_STATUS(catalog_find_table(&c, "people", &t), KHB_OK);
    CHECK_EQ(t->column_count, 5);
    CHECK_EQ(t->row_size, 57);
    CHECK_EQ(t->root_page, 42);
    CHECK_EQ(t->columns[1].offset, 8);
    CHECK_EQ(strcmp(t->columns[3].name, "score"), 0);

    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_rollback_undoes_schema(void)
{
    char         path[PATH_MAX];
    pager_t      p;
    journal_t    j;
    catalog_t    c;
    column_def_t cols[8];
    int          n = build_people(cols);

    khb_temp_path(path, sizeof path, "catrollback");

    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    catalog_init(&c);
    journal_init(&j);

    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(catalog_create_table(&c, "people", cols, n, NULL), KHB_OK);
    CHECK_STATUS(catalog_flush(&c, &p, &j), KHB_OK);
    CHECK_STATUS(journal_rollback(&j, &p), KHB_OK);
    journal_free(&j);

    CHECK_EQ(pager_catalog_root(&p), 0);
    catalog_init(&c);
    CHECK_STATUS(catalog_load(&c, &p), KHB_OK);
    CHECK_EQ(catalog_table_count(&c), 0);

    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_multiple_tables_persist(void)
{
    char         path[PATH_MAX];
    pager_t      p;
    journal_t    j;
    catalog_t    c;
    column_def_t cols[8];
    table_def_t *t = NULL;
    int          n = build_people(cols);
    int          i;

    khb_temp_path(path, sizeof path, "catmulti");

    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    catalog_init(&c);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);

    for (i = 0; i < 4; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "tbl%d", i);
        CHECK_STATUS(catalog_create_table(&c, nm, cols, n, &t), KHB_OK);
        catalog_set_root_page(&c, t, (uint32_t)(100 + i));
    }
    CHECK_STATUS(catalog_flush(&c, &p, &j), KHB_OK);
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    catalog_init(&c);
    CHECK_STATUS(catalog_load(&c, &p), KHB_OK);
    CHECK_EQ(catalog_table_count(&c), 4);
    for (i = 0; i < 4; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "tbl%d", i);
        t = NULL;
        CHECK_STATUS(catalog_find_table(&c, nm, &t), KHB_OK);
        CHECK_EQ(t->root_page, (uint32_t)(100 + i));
    }

    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

int main(void)
{
    RUN_TEST(test_create_and_find);
    RUN_TEST(test_offsets_and_row_size);
    RUN_TEST(test_duplicate_table_rejected);
    RUN_TEST(test_schema_validation);
    RUN_TEST(test_table_limit);
    RUN_TEST(test_flush_requires_transaction);
    RUN_TEST(test_persist_across_reopen);
    RUN_TEST(test_rollback_undoes_schema);
    RUN_TEST(test_multiple_tables_persist);

    return TEST_SUMMARY();
}