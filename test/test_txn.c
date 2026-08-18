#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_util.h"
#include "txn.h"
#include "row.h"

static int build_cols(column_def_t *cols)
{
    column_def_set(&cols[0], "id",   COL_INT64, 0);
    column_def_set(&cols[1], "name", COL_TEXT, 16);
    column_def_set(&cols[2], "n",    COL_INT64, 0);
    return 3;
}

static void make_row(uint8_t *buf, const table_def_t *def, int64_t key)
{
    row_t r;

    row_bind(&r, def, buf);
    row_clear(&r);
    row_set_int(&r, 0, key);
    row_set_text(&r, 1, "x");
    row_set_int(&r, 2, key * 3);
}

static void test_open_close(void)
{
    char   path[PATH_MAX];
    khb_db db;

    khb_temp_path(path, sizeof path, "txopen");

    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);
    CHECK_EQ(txn_current(&db), TXN_NONE);
    CHECK_STATUS(db_close(&db), KHB_OK);

    CHECK_STATUS(db_open(&db, path, 0), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    khb_temp_remove(path);
}

static void test_state_transitions(void)
{
    char   path[PATH_MAX];
    khb_db db;

    khb_temp_path(path, sizeof path, "txstate");
    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);

    CHECK_STATUS(txn_commit(&db), KHB_ERR_STATE);
    CHECK_STATUS(txn_rollback(&db), KHB_ERR_STATE);

    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    CHECK_EQ(txn_current(&db), TXN_WRITE);
    CHECK_STATUS(txn_begin(&db, 0), KHB_ERR_STATE);
    CHECK_STATUS(txn_begin(&db, 1), KHB_ERR_STATE);
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_EQ(txn_current(&db), TXN_NONE);

    CHECK_STATUS(txn_begin(&db, 1), KHB_OK);
    CHECK_EQ(txn_current(&db), TXN_READ);
    CHECK_STATUS(txn_rollback(&db), KHB_OK);
    CHECK_EQ(txn_current(&db), TXN_NONE);

    CHECK_STATUS(db_close(&db), KHB_OK);
    khb_temp_remove(path);
}

static void test_readonly_rejects_writes(void)
{
    char         path[PATH_MAX];
    khb_db       db;
    column_def_t cols[4];
    int          n = build_cols(cols);

    khb_temp_path(path, sizeof path, "txro");
    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);

    CHECK_STATUS(txn_begin(&db, 1), KHB_OK);
    CHECK_STATUS(txn_create_table(&db, "people", cols, n, NULL),
                 KHB_ERR_STATE);
    CHECK_STATUS(txn_commit(&db), KHB_OK);

    CHECK_STATUS(db_close(&db), KHB_OK);
    khb_temp_remove(path);
}

static void test_create_table_persists(void)
{
    char         path[PATH_MAX];
    khb_db       db;
    column_def_t cols[4];
    table_def_t *t = NULL;
    int          n = build_cols(cols);

    khb_temp_path(path, sizeof path, "txcreate");

    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    CHECK_STATUS(txn_create_table(&db, "people", cols, n, &t), KHB_OK);
    CHECK(t->root_page != 0);
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    CHECK_STATUS(db_open(&db, path, 0), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 1), KHB_OK);
    t = NULL;
    CHECK_STATUS(txn_find_table(&db, "people", &t), KHB_OK);
    CHECK(t != NULL);
    CHECK_EQ(t->column_count, 3);
    CHECK(t->root_page != 0);
    CHECK_STATUS(txn_find_table(&db, "nope", NULL), KHB_ERR_NOTFOUND);
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    khb_temp_remove(path);
}

static void test_rollback_undoes_table(void)
{
    char         path[PATH_MAX];
    khb_db       db;
    column_def_t cols[4];
    int          n = build_cols(cols);

    khb_temp_path(path, sizeof path, "txrbtable");

    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    CHECK_STATUS(txn_create_table(&db, "people", cols, n, NULL), KHB_OK);
    CHECK_STATUS(txn_rollback(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    CHECK_STATUS(db_open(&db, path, 0), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 1), KHB_OK);
    CHECK_STATUS(txn_find_table(&db, "people", NULL), KHB_ERR_NOTFOUND);
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    khb_temp_remove(path);
}

static void test_root_persists_across_splits(void)
{
    char         path[PATH_MAX];
    khb_db       db;
    column_def_t cols[4];
    table_def_t *t = NULL;
    btree_t      bt;
    uint8_t      row[KHB_MAX_ROW_SIZE], out[KHB_MAX_ROW_SIZE];
    uint32_t     first_root, n_rows;
    int          n = build_cols(cols);
    int          i;
    const int    N = 5000;

    khb_temp_path(path, sizeof path, "txroot");

    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    CHECK_STATUS(txn_create_table(&db, "big", cols, n, &t), KHB_OK);
    first_root = t->root_page;

    CHECK_STATUS(txn_open_tree(&db, t, &bt), KHB_OK);
    for (i = 0; i < N; i++) {
        make_row(row, t, i);
        CHECK_STATUS(btree_insert(&bt, row), KHB_OK);
    }
    CHECK(btree_root(&bt) != first_root);
    CHECK_STATUS(txn_close_tree(&db, t, &bt), KHB_OK);
    CHECK_EQ(t->root_page, btree_root(&bt));
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    CHECK_STATUS(db_open(&db, path, 0), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 1), KHB_OK);
    t = NULL;
    CHECK_STATUS(txn_find_table(&db, "big", &t), KHB_OK);
    CHECK(t->root_page != first_root);
    CHECK_STATUS(txn_open_tree(&db, t, &bt), KHB_OK);
    CHECK_STATUS(btree_check(&bt), KHB_OK);
    CHECK_STATUS(btree_count(&bt, &n_rows), KHB_OK);
    CHECK_EQ(n_rows, (uint32_t)N);
    for (i = 0; i < N; i++)
        CHECK_STATUS(btree_lookup(&bt, i, out), KHB_OK);
    CHECK_STATUS(txn_close_tree(&db, t, &bt), KHB_OK);
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    khb_temp_remove(path);
}

static void test_rollback_undoes_rows(void)
{
    char         path[PATH_MAX];
    khb_db       db;
    column_def_t cols[4];
    table_def_t *t = NULL;
    btree_t      bt;
    uint8_t      row[KHB_MAX_ROW_SIZE];
    uint32_t     n_rows;
    int          n = build_cols(cols);
    int          i;

    khb_temp_path(path, sizeof path, "txrbrows");

    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    CHECK_STATUS(txn_create_table(&db, "t", cols, n, &t), KHB_OK);
    CHECK_STATUS(txn_open_tree(&db, t, &bt), KHB_OK);
    for (i = 0; i < 50; i++) {
        make_row(row, t, i);
        CHECK_STATUS(btree_insert(&bt, row), KHB_OK);
    }
    CHECK_STATUS(txn_close_tree(&db, t, &bt), KHB_OK);
    CHECK_STATUS(txn_commit(&db), KHB_OK);

    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    t = NULL;
    CHECK_STATUS(txn_find_table(&db, "t", &t), KHB_OK);
    CHECK_STATUS(txn_open_tree(&db, t, &bt), KHB_OK);
    for (i = 1000; i < 5000; i++) {
        make_row(row, t, i);
        CHECK_STATUS(btree_insert(&bt, row), KHB_OK);
    }
    CHECK_STATUS(txn_close_tree(&db, t, &bt), KHB_OK);
    CHECK_STATUS(txn_rollback(&db), KHB_OK);

    CHECK_STATUS(txn_begin(&db, 1), KHB_OK);
    t = NULL;
    CHECK_STATUS(txn_find_table(&db, "t", &t), KHB_OK);
    CHECK_STATUS(txn_open_tree(&db, t, &bt), KHB_OK);
    CHECK_STATUS(btree_check(&bt), KHB_OK);
    CHECK_STATUS(btree_count(&bt, &n_rows), KHB_OK);
    CHECK_EQ(n_rows, 50);
    CHECK_STATUS(txn_commit(&db), KHB_OK);

    CHECK_STATUS(db_close(&db), KHB_OK);
    khb_temp_remove(path);
}

static void test_close_rolls_back_open_txn(void)
{
    char         path[PATH_MAX];
    khb_db       db;
    column_def_t cols[4];
    int          n = build_cols(cols);

    khb_temp_path(path, sizeof path, "txabandon");

    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    CHECK_STATUS(txn_create_table(&db, "gone", cols, n, NULL), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    CHECK_STATUS(db_open(&db, path, 0), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 1), KHB_OK);
    CHECK_STATUS(txn_find_table(&db, "gone", NULL), KHB_ERR_NOTFOUND);
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    khb_temp_remove(path);
}

static void test_recovery_on_open(void)
{
    char         path[PATH_MAX], jpath[PATH_MAX];
    khb_db       db;
    pager_t      p;
    journal_t    j;
    column_def_t cols[4];
    table_def_t *t = NULL;
    uint8_t      page[KHB_PAGE_SIZE];
    struct stat  st;
    int          n = build_cols(cols);

    khb_temp_path(path, sizeof path, "txrecover");
    journal_path_for(path, jpath, sizeof jpath);

    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    CHECK_STATUS(txn_create_table(&db, "keep", cols, n, &t), KHB_OK);
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(journal_note_page(&j, &p, 1), KHB_OK);
    page_init(page, 1, PAGE_TYPE_HEAP);
    CHECK_STATUS(pager_write(&p, 1, page), KHB_OK);
    CHECK_STATUS(pager_sync(&p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    CHECK_EQ(stat(jpath, &st), 0);

    CHECK_STATUS(db_open(&db, path, 0), KHB_OK);
    CHECK(stat(jpath, &st) != 0);
    CHECK_STATUS(txn_begin(&db, 1), KHB_OK);
    CHECK_STATUS(txn_find_table(&db, "keep", NULL), KHB_OK);
    CHECK_STATUS(txn_commit(&db), KHB_OK);
    CHECK_STATUS(db_close(&db), KHB_OK);

    khb_temp_remove(path);
}

static void test_two_readers_coexist(void)
{
    char   path[PATH_MAX];
    khb_db a, b;

    khb_temp_path(path, sizeof path, "txreaders");

    CHECK_STATUS(db_open(&a, path, 1), KHB_OK);
    CHECK_STATUS(db_close(&a), KHB_OK);

    CHECK_STATUS(db_open(&a, path, 0), KHB_OK);
    CHECK_STATUS(db_open(&b, path, 0), KHB_OK);

    CHECK_STATUS(txn_begin(&a, 1), KHB_OK);
    CHECK_STATUS(txn_begin(&b, 1), KHB_OK);

    CHECK_STATUS(txn_commit(&a), KHB_OK);
    CHECK_STATUS(txn_commit(&b), KHB_OK);

    CHECK_STATUS(db_close(&a), KHB_OK);
    CHECK_STATUS(db_close(&b), KHB_OK);
    khb_temp_remove(path);
}

static void test_writer_excludes_others(void)
{
    char   path[PATH_MAX];
    khb_db a, b;

    khb_temp_path(path, sizeof path, "txexclude");

    CHECK_STATUS(db_open(&a, path, 1), KHB_OK);
    CHECK_STATUS(db_close(&a), KHB_OK);

    CHECK_STATUS(db_open(&a, path, 0), KHB_OK);
    CHECK_STATUS(db_open(&b, path, 0), KHB_OK);

    CHECK_STATUS(txn_begin(&a, 0), KHB_OK);
    CHECK_STATUS(txn_begin(&b, 1), KHB_ERR_LOCKED);
    CHECK_STATUS(txn_begin(&b, 0), KHB_ERR_LOCKED);
    CHECK_EQ(txn_current(&b), TXN_NONE);
    CHECK_STATUS(txn_commit(&a), KHB_OK);

    CHECK_STATUS(txn_begin(&b, 0), KHB_OK);
    CHECK_STATUS(txn_begin(&a, 1), KHB_ERR_LOCKED);
    CHECK_STATUS(txn_commit(&b), KHB_OK);

    CHECK_STATUS(db_close(&a), KHB_OK);
    CHECK_STATUS(db_close(&b), KHB_OK);
    khb_temp_remove(path);
}

static void test_catalog_refreshed_per_txn(void)
{
    char         path[PATH_MAX];
    khb_db       a, b;
    column_def_t cols[4];
    int          n = build_cols(cols);

    khb_temp_path(path, sizeof path, "txrefresh");

    CHECK_STATUS(db_open(&a, path, 1), KHB_OK);
    CHECK_STATUS(db_close(&a), KHB_OK);

    CHECK_STATUS(db_open(&a, path, 0), KHB_OK);
    CHECK_STATUS(db_open(&b, path, 0), KHB_OK);

    CHECK_STATUS(txn_begin(&a, 0), KHB_OK);
    CHECK_STATUS(txn_create_table(&a, "late", cols, n, NULL), KHB_OK);
    CHECK_STATUS(txn_commit(&a), KHB_OK);

    CHECK_STATUS(txn_begin(&b, 1), KHB_OK);
    CHECK_STATUS(txn_find_table(&b, "late", NULL), KHB_OK);
    CHECK_STATUS(txn_commit(&b), KHB_OK);

    CHECK_STATUS(db_close(&a), KHB_OK);
    CHECK_STATUS(db_close(&b), KHB_OK);
    khb_temp_remove(path);
}

static void test_tree_access_requires_txn(void)
{
    char         path[PATH_MAX];
    khb_db       db;
    column_def_t cols[4];
    table_def_t *t = NULL;
    table_def_t  saved;
    btree_t      bt;
    int          n = build_cols(cols);

    khb_temp_path(path, sizeof path, "txnotxn");

    CHECK_STATUS(db_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(txn_begin(&db, 0), KHB_OK);
    CHECK_STATUS(txn_create_table(&db, "t", cols, n, &t), KHB_OK);
    saved = *t;
    CHECK_STATUS(txn_commit(&db), KHB_OK);

    CHECK_STATUS(txn_open_tree(&db, &saved, &bt), KHB_ERR_STATE);
    CHECK_STATUS(txn_find_table(&db, "t", NULL), KHB_ERR_STATE);
    CHECK_STATUS(txn_create_table(&db, "u", cols, n, NULL), KHB_ERR_STATE);

    CHECK_STATUS(db_close(&db), KHB_OK);
    khb_temp_remove(path);
}

int main(void)
{
    RUN_TEST(test_open_close);
    RUN_TEST(test_state_transitions);
    RUN_TEST(test_readonly_rejects_writes);
    RUN_TEST(test_create_table_persists);
    RUN_TEST(test_rollback_undoes_table);
    RUN_TEST(test_root_persists_across_splits);
    RUN_TEST(test_rollback_undoes_rows);
    RUN_TEST(test_close_rolls_back_open_txn);
    RUN_TEST(test_recovery_on_open);
    RUN_TEST(test_two_readers_coexist);
    RUN_TEST(test_writer_excludes_others);
    RUN_TEST(test_catalog_refreshed_per_txn);
    RUN_TEST(test_tree_access_requires_txn);

    return TEST_SUMMARY();
}
