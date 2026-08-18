#include <string.h>

#include "test_util.h"
#include "heap.h"
#include "row.h"

static catalog_t    g_cat;
static table_def_t *g_def;

static void make_def(void)
{
    column_def_t cols[4];

    column_def_set(&cols[0], "id",   COL_INT64, 0);
    column_def_set(&cols[1], "name", COL_TEXT, 16);
    column_def_set(&cols[2], "n",    COL_INT64, 0);

    catalog_init(&g_cat);
    catalog_create_table(&g_cat, "t", cols, 3, &g_def);
}

static void make_row(uint8_t *buf, int64_t key, const char *name)
{
    row_t r;

    row_bind(&r, g_def, buf);
    row_clear(&r);
    row_set_int(&r, 0, key);
    row_set_text(&r, 1, name);
    row_set_int(&r, 2, key * 2);
}

struct collect {
    int64_t keys[4096];
    int     n;
};

static khb_status collect_visit(const uint8_t *row, void *ctx)
{
    struct collect *c = (struct collect *)ctx;
    int64_t         k;

    memcpy(&k, row, sizeof k);
    if (c->n < (int)(sizeof c->keys / sizeof c->keys[0]))
        c->keys[c->n++] = k;
    return KHB_OK;
}

static void test_create_and_get(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    heap_t    h;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    row_t     r;
    char      name[32];

    khb_temp_path(path, sizeof path, "heapget");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);

    CHECK_STATUS(heap_create(&p, &j, g_def, &root), KHB_OK);
    heap_open(&h, &p, &j, g_def, root);

    make_row(row, 7, "seven");
    CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    CHECK_STATUS(heap_count(&h, &n), KHB_OK);
    CHECK_EQ(n, 1);

    CHECK_STATUS(heap_get(&h, 7, out), KHB_OK);
    row_bind(&r, g_def, out);
    CHECK_STATUS(row_get_text(&r, 1, name, sizeof name), KHB_OK);
    CHECK_EQ(strcmp(name, "seven"), 0);

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_duplicate_rejected(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    heap_t    h;
    uint8_t   row[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;

    khb_temp_path(path, sizeof path, "heapdup");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(heap_create(&p, &j, g_def, &root), KHB_OK);
    heap_open(&h, &p, &j, g_def, root);

    make_row(row, 1, "a");
    CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    make_row(row, 1, "b");
    CHECK_STATUS(heap_insert(&h, row), KHB_ERR_EXISTS);
    CHECK_STATUS(heap_count(&h, &n), KHB_OK);
    CHECK_EQ(n, 1);

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_get_missing(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    heap_t    h;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root;

    khb_temp_path(path, sizeof path, "heapmiss");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(heap_create(&p, &j, g_def, &root), KHB_OK);
    heap_open(&h, &p, &j, g_def, root);

    CHECK_STATUS(heap_get(&h, 42, out), KHB_ERR_NOTFOUND);
    make_row(row, 1, "a");
    CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    CHECK_STATUS(heap_get(&h, 42, out), KHB_ERR_NOTFOUND);
    CHECK_STATUS(heap_delete(&h, 42), KHB_ERR_NOTFOUND);

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_spans_multiple_pages(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    heap_t    h;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int       total, i;

    total = heap_capacity_for(g_def->row_size) * 2 + 5;

    khb_temp_path(path, sizeof path, "heapspan");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(heap_create(&p, &j, g_def, &root), KHB_OK);
    heap_open(&h, &p, &j, g_def, root);

    for (i = 0; i < total; i++) {
        make_row(row, i, "x");
        CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    }

    CHECK_STATUS(heap_count(&h, &n), KHB_OK);
    CHECK_EQ(n, (uint32_t)total);

    for (i = 0; i < total; i++)
        CHECK_STATUS(heap_get(&h, i, out), KHB_OK);

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_delete(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    heap_t    h;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int       i;

    khb_temp_path(path, sizeof path, "heapdel");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(heap_create(&p, &j, g_def, &root), KHB_OK);
    heap_open(&h, &p, &j, g_def, root);

    for (i = 0; i < 20; i++) {
        make_row(row, i, "x");
        CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    }

    for (i = 0; i < 20; i += 2)
        CHECK_STATUS(heap_delete(&h, i), KHB_OK);

    CHECK_STATUS(heap_count(&h, &n), KHB_OK);
    CHECK_EQ(n, 10);

    for (i = 0; i < 20; i++) {
        khb_status want = (i % 2 == 0) ? KHB_ERR_NOTFOUND : KHB_OK;
        CHECK_STATUS(heap_get(&h, i, out), want);
    }

    /* A deleted key can be reinserted. */
    make_row(row, 4, "again");
    CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    CHECK_STATUS(heap_get(&h, 4, out), KHB_OK);

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_scan_visits_all(void)
{
    char           path[PATH_MAX];
    pager_t        p;
    journal_t      j;
    heap_t         h;
    uint8_t        row[KHB_MAX_ROW_SIZE];
    uint32_t       root;
    struct collect c;
    int            i, k, found;
    int            total = heap_capacity_for(g_def->row_size) + 3;

    khb_temp_path(path, sizeof path, "heapscan");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(heap_create(&p, &j, g_def, &root), KHB_OK);
    heap_open(&h, &p, &j, g_def, root);

    for (i = 0; i < total; i++) {
        make_row(row, i * 3, "x");
        CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    }

    memset(&c, 0, sizeof c);
    CHECK_STATUS(heap_scan(&h, collect_visit, &c), KHB_OK);
    CHECK_EQ(c.n, total);

    for (i = 0; i < total; i++) {
        found = 0;
        for (k = 0; k < c.n; k++)
            if (c.keys[k] == (int64_t)(i * 3))
                found = 1;
        CHECK(found);
    }

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_persist_across_reopen(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    heap_t    h;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int       i;
    int       total = heap_capacity_for(g_def->row_size) + 10;

    khb_temp_path(path, sizeof path, "heappersist");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(heap_create(&p, &j, g_def, &root), KHB_OK);
    heap_open(&h, &p, &j, g_def, root);

    for (i = 0; i < total; i++) {
        make_row(row, i, "x");
        CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    }
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    heap_open(&h, &p, NULL, g_def, root);

    CHECK_STATUS(heap_count(&h, &n), KHB_OK);
    CHECK_EQ(n, (uint32_t)total);
    for (i = 0; i < total; i++)
        CHECK_STATUS(heap_get(&h, i, out), KHB_OK);

    /* Read-only handle refuses mutations. */
    make_row(row, 9999, "x");
    CHECK_STATUS(heap_insert(&h, row), KHB_ERR_STATE);
    CHECK_STATUS(heap_delete(&h, 0), KHB_ERR_STATE);

    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_rollback_undoes_inserts(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    heap_t    h;
    uint8_t   row[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int       i;

    khb_temp_path(path, sizeof path, "heaprollback");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);

    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(heap_create(&p, &j, g_def, &root), KHB_OK);
    heap_open(&h, &p, &j, g_def, root);
    for (i = 0; i < 5; i++) {
        make_row(row, i, "x");
        CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    }
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);

    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    for (i = 100; i < 120; i++) {
        make_row(row, i, "x");
        CHECK_STATUS(heap_insert(&h, row), KHB_OK);
    }
    CHECK_STATUS(heap_count(&h, &n), KHB_OK);
    CHECK_EQ(n, 25);
    CHECK_STATUS(journal_rollback(&j, &p), KHB_OK);

    CHECK_STATUS(heap_count(&h, &n), KHB_OK);
    CHECK_EQ(n, 5);

    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

int main(void)
{
    make_def();

    RUN_TEST(test_create_and_get);
    RUN_TEST(test_duplicate_rejected);
    RUN_TEST(test_get_missing);
    RUN_TEST(test_spans_multiple_pages);
    RUN_TEST(test_delete);
    RUN_TEST(test_scan_visits_all);
    RUN_TEST(test_persist_across_reopen);
    RUN_TEST(test_rollback_undoes_inserts);

    return TEST_SUMMARY();
}