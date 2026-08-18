#include <stdlib.h>
#include <string.h>

#include "test_util.h"
#include "btree.h"
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

static void make_row(uint8_t *buf, int64_t key)
{
    row_t r;
    char  nm[16];

    row_bind(&r, g_def, buf);
    row_clear(&r);
    snprintf(nm, sizeof nm, "k%lld", (long long)key);
    row_set_int(&r, 0, key);
    row_set_text(&r, 1, nm);
    row_set_int(&r, 2, key * 3);
}

static int64_t row_n(const uint8_t *buf)
{
    row_t   r;
    int64_t v = 0;

    row_bind(&r, g_def, (uint8_t *)buf);
    row_get_int(&r, 2, &v);
    return v;
}

static void shuffle(int64_t *a, int n, unsigned seed)
{
    int i;

    srand(seed);
    for (i = n - 1; i > 0; i--) {
        int     j = rand() % (i + 1);
        int64_t t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static void test_empty_tree(void)
{
    char           path[PATH_MAX];
    pager_t        p;
    journal_t      j;
    btree_t        t;
    btree_cursor_t c;
    uint8_t        out[KHB_MAX_ROW_SIZE];
    uint32_t       root, n;

    khb_temp_path(path, sizeof path, "btempty");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);

    CHECK_STATUS(btree_check(&t), KHB_OK);
    CHECK_STATUS(btree_count(&t, &n), KHB_OK);
    CHECK_EQ(n, 0);
    CHECK_STATUS(btree_lookup(&t, 1, out), KHB_ERR_NOTFOUND);
    CHECK_STATUS(btree_delete(&t, 1), KHB_ERR_NOTFOUND);
    CHECK_STATUS(btree_cursor_first(&t, &c), KHB_OK);
    CHECK_STATUS(btree_cursor_next(&t, &c, out), KHB_ERR_NOTFOUND);

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
    btree_t   t;
    uint8_t   row[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;

    khb_temp_path(path, sizeof path, "btdup");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);

    make_row(row, 5);
    CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    CHECK_STATUS(btree_insert(&t, row), KHB_ERR_EXISTS);
    CHECK_STATUS(btree_count(&t, &n), KHB_OK);
    CHECK_EQ(n, 1);

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_sequential_inserts(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    btree_t   t;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int       i;
    const int N = 5000;

    khb_temp_path(path, sizeof path, "btseq");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);

    for (i = 0; i < N; i++) {
        make_row(row, i);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    }

    CHECK_STATUS(btree_check(&t), KHB_OK);
    CHECK_STATUS(btree_count(&t, &n), KHB_OK);
    CHECK_EQ(n, (uint32_t)N);

    for (i = 0; i < N; i++) {
        CHECK_STATUS(btree_lookup(&t, i, out), KHB_OK);
        CHECK_EQ(row_n(out), (int64_t)i * 3);
    }
    CHECK_STATUS(btree_lookup(&t, N, out), KHB_ERR_NOTFOUND);
    CHECK_STATUS(btree_lookup(&t, -1, out), KHB_ERR_NOTFOUND);
    CHECK(btree_root(&t) != root);

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_random_inserts(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    btree_t   t;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int64_t  *keys;
    int       i;
    const int N = 10000;

    keys = malloc(sizeof(int64_t) * N);
    CHECK(keys != NULL);
    for (i = 0; i < N; i++)
        keys[i] = i;
    shuffle(keys, N, 12345);

    khb_temp_path(path, sizeof path, "btrand");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);

    for (i = 0; i < N; i++) {
        make_row(row, keys[i]);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
        if (i % 1000 == 0)
            CHECK_STATUS(btree_check(&t), KHB_OK);
    }

    CHECK_STATUS(btree_check(&t), KHB_OK);
    CHECK_STATUS(btree_count(&t, &n), KHB_OK);
    CHECK_EQ(n, (uint32_t)N);

    for (i = 0; i < N; i++) {
        CHECK_STATUS(btree_lookup(&t, keys[i], out), KHB_OK);
        CHECK_EQ(row_n(out), keys[i] * 3);
    }

    free(keys);
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_scan_is_sorted(void)
{
    char           path[PATH_MAX];
    pager_t        p;
    journal_t      j;
    btree_t        t;
    btree_cursor_t c;
    uint8_t        row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t       root;
    int64_t       *keys;
    int64_t        prev = 0;
    int            i, seen = 0;
    const int      N = 3000;

    keys = malloc(sizeof(int64_t) * N);
    CHECK(keys != NULL);
    for (i = 0; i < N; i++)
        keys[i] = i * 7;
    shuffle(keys, N, 999);

    khb_temp_path(path, sizeof path, "btscan");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);

    for (i = 0; i < N; i++) {
        make_row(row, keys[i]);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    }

    CHECK_STATUS(btree_cursor_first(&t, &c), KHB_OK);
    while (btree_cursor_next(&t, &c, out) == KHB_OK) {
        int64_t k = row_n(out) / 3;

        if (seen > 0)
            CHECK(k > prev);
        prev = k;
        seen++;
    }
    CHECK_EQ(seen, N);

    free(keys);
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_seek(void)
{
    char           path[PATH_MAX];
    pager_t        p;
    journal_t      j;
    btree_t        t;
    btree_cursor_t c;
    uint8_t        row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t       root;
    int            i;
    const int      N = 2000;

    khb_temp_path(path, sizeof path, "btseek");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);

    for (i = 0; i < N; i++) {
        make_row(row, i * 10);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    }

    CHECK_STATUS(btree_cursor_seek(&t, 5000, &c), KHB_OK);
    CHECK_STATUS(btree_cursor_next(&t, &c, out), KHB_OK);
    CHECK_EQ(row_n(out) / 3, 5000);

    CHECK_STATUS(btree_cursor_seek(&t, 5001, &c), KHB_OK);
    CHECK_STATUS(btree_cursor_next(&t, &c, out), KHB_OK);
    CHECK_EQ(row_n(out) / 3, 5010);

    CHECK_STATUS(btree_cursor_seek(&t, -100, &c), KHB_OK);
    CHECK_STATUS(btree_cursor_next(&t, &c, out), KHB_OK);
    CHECK_EQ(row_n(out) / 3, 0);

    CHECK_STATUS(btree_cursor_seek(&t, 999999, &c), KHB_OK);
    CHECK_STATUS(btree_cursor_next(&t, &c, out), KHB_ERR_NOTFOUND);

    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_delete_half(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    btree_t   t;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int64_t  *keys;
    int       i;
    const int N = 4000;

    keys = malloc(sizeof(int64_t) * N);
    CHECK(keys != NULL);
    for (i = 0; i < N; i++)
        keys[i] = i;
    shuffle(keys, N, 4242);

    khb_temp_path(path, sizeof path, "btdel");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);

    for (i = 0; i < N; i++) {
        make_row(row, keys[i]);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    }

    for (i = 0; i < N; i += 2)
        CHECK_STATUS(btree_delete(&t, i), KHB_OK);

    CHECK_STATUS(btree_check(&t), KHB_OK);
    CHECK_STATUS(btree_count(&t, &n), KHB_OK);
    CHECK_EQ(n, (uint32_t)(N / 2));

    for (i = 0; i < N; i++) {
        khb_status want = (i % 2 == 0) ? KHB_ERR_NOTFOUND : KHB_OK;
        CHECK_STATUS(btree_lookup(&t, i, out), want);
    }

    for (i = 0; i < N; i += 2) {
        make_row(row, i);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    }
    CHECK_STATUS(btree_check(&t), KHB_OK);
    CHECK_STATUS(btree_count(&t, &n), KHB_OK);
    CHECK_EQ(n, (uint32_t)N);

    free(keys);
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_differential_vs_heap(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    btree_t   t;
    heap_t    h;
    uint8_t   row[KHB_MAX_ROW_SIZE];
    uint8_t   bout[KHB_MAX_ROW_SIZE], hout[KHB_MAX_ROW_SIZE];
    uint32_t  broot, hroot, bn, hn;
    int64_t  *keys;
    int       i;
    const int N = 1500;

    keys = malloc(sizeof(int64_t) * N);
    CHECK(keys != NULL);
    for (i = 0; i < N; i++)
        keys[i] = (i * 37) % 9001;
    shuffle(keys, N, 777);

    khb_temp_path(path, sizeof path, "btdiff");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &broot), KHB_OK);
    CHECK_STATUS(heap_create(&p, &j, g_def, &hroot), KHB_OK);
    btree_open(&t, &p, &j, g_def, broot);
    heap_open(&h, &p, &j, g_def, hroot);

    for (i = 0; i < N; i++) {
        khb_status brc, hrc;

        make_row(row, keys[i]);
        brc = btree_insert(&t, row);
        hrc = heap_insert(&h, row);
        CHECK_EQ(brc, hrc);
    }

    for (i = 0; i < N; i += 3) {
        khb_status brc = btree_delete(&t, keys[i]);
        khb_status hrc = heap_delete(&h, keys[i]);
        CHECK_EQ(brc, hrc);
    }

    CHECK_STATUS(btree_check(&t), KHB_OK);
    CHECK_STATUS(btree_count(&t, &bn), KHB_OK);
    CHECK_STATUS(heap_count(&h, &hn), KHB_OK);
    CHECK_EQ(bn, hn);

    for (i = 0; i < 9001; i++) {
        khb_status brc = btree_lookup(&t, i, bout);
        khb_status hrc = heap_get(&h, i, hout);

        CHECK_EQ(brc, hrc);
        if (brc == KHB_OK)
            CHECK_EQ(memcmp(bout, hout, g_def->row_size), 0);
    }

    free(keys);
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
    btree_t   t;
    uint8_t   row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int       i;
    const int N = 3000;

    khb_temp_path(path, sizeof path, "btpersist");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);

    for (i = 0; i < N; i++) {
        make_row(row, i);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    }
    root = btree_root(&t);
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    btree_open(&t, &p, NULL, g_def, root);

    CHECK_STATUS(btree_check(&t), KHB_OK);
    CHECK_STATUS(btree_count(&t, &n), KHB_OK);
    CHECK_EQ(n, (uint32_t)N);
    for (i = 0; i < N; i++)
        CHECK_STATUS(btree_lookup(&t, i, out), KHB_OK);

    make_row(row, 99999);
    CHECK_STATUS(btree_insert(&t, row), KHB_ERR_STATE);
    CHECK_STATUS(btree_delete(&t, 0), KHB_ERR_STATE);

    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_rollback_undoes_splits(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    btree_t   t;
    uint8_t   row[KHB_MAX_ROW_SIZE];
    uint32_t  root, n;
    int       i;

    khb_temp_path(path, sizeof path, "btrollback");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    journal_init(&j);

    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(btree_create(&p, &j, g_def, &root), KHB_OK);
    btree_open(&t, &p, &j, g_def, root);
    for (i = 0; i < 100; i++) {
        make_row(row, i);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    }
    root = btree_root(&t);
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);

    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    for (i = 1000; i < 6000; i++) {
        make_row(row, i);
        CHECK_STATUS(btree_insert(&t, row), KHB_OK);
    }
    CHECK_STATUS(journal_rollback(&j, &p), KHB_OK);

    btree_open(&t, &p, &j, g_def, root);
    CHECK_STATUS(btree_check(&t), KHB_OK);
    CHECK_STATUS(btree_count(&t, &n), KHB_OK);
    CHECK_EQ(n, 100);

    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

int main(void)
{
    make_def();

    RUN_TEST(test_empty_tree);
    RUN_TEST(test_duplicate_rejected);
    RUN_TEST(test_sequential_inserts);
    RUN_TEST(test_random_inserts);
    RUN_TEST(test_scan_is_sorted);
    RUN_TEST(test_seek);
    RUN_TEST(test_delete_half);
    RUN_TEST(test_differential_vs_heap);
    RUN_TEST(test_persist_across_reopen);
    RUN_TEST(test_rollback_undoes_splits);

    return TEST_SUMMARY();
}
