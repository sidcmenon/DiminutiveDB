#include <string.h>

#include "test_util.h"
#include "khabibdb.h"

static const khb_column SCHEMA[] = {
    { "id",    KHB_INT64,  0 },
    { "name",  KHB_TEXT,  16 },
    { "age",   KHB_INT64,  0 },
    { "score", KHB_DOUBLE, 0 },
    { "ok",    KHB_BOOL,   0 }
};
#define NCOLS 5

struct collect {
    int     n;
    int64_t keys[8192];
};

static khb_status collect_visit(const khb_row *row, void *ctx)
{
    struct collect *c = ctx;
    int64_t         k = 0;

    khb_row_key(row, &k);
    if (c->n < (int)(sizeof c->keys / sizeof c->keys[0]))
        c->keys[c->n++] = k;
    return KHB_OK;
}

static int even_keys(const khb_row *row, void *ctx)
{
    int64_t k = 0;

    (void)ctx;
    khb_row_key(row, &k);
    return (k % 2) == 0;
}

static khb_status stop_after_three(const khb_row *row, void *ctx)
{
    struct collect *c = ctx;

    (void)row;
    if (c->n >= 3)
        return KHB_ERR_FULL;
    c->n++;
    return KHB_OK;
}

static khb_status add_row(khb_db *db, const char *table, int64_t id,
                          const char *name, int64_t age, double score)
{
    khb_row    row;
    khb_status rc;

    rc = khb_row_init(&row, db, table);
    if (rc != KHB_OK)
        return rc;

    khb_row_set_int(&row, 0, id);
    khb_row_set_text(&row, 1, name);
    khb_row_set_int(&row, 2, age);
    khb_row_set_double(&row, 3, score);
    khb_row_set_bool(&row, 4, 1);

    return khb_insert(db, table, &row);
}

static void test_open_and_create(void)
{
    char    path[PATH_MAX];
    khb_db *db = NULL;

    khb_temp_path(path, sizeof path, "dbopen");

    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);
    CHECK_STATUS(khb_table_exists(db, "people"), KHB_OK);
    CHECK_STATUS(khb_table_exists(db, "nope"), KHB_ERR_NOTFOUND);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS),
                 KHB_ERR_EXISTS);
    CHECK_STATUS(khb_close(db), KHB_OK);

    CHECK_STATUS(khb_open(&db, path, 0), KHB_OK);
    CHECK_STATUS(khb_table_exists(db, "people"), KHB_OK);
    CHECK_STATUS(khb_close(db), KHB_OK);

    khb_temp_remove(path);
}

static void test_row_types(void)
{
    char     path[PATH_MAX];
    khb_db  *db = NULL;
    khb_row  row;
    int64_t  i64 = 0;
    double   d = 0;
    int      b = 0;
    char     name[32];

    khb_temp_path(path, sizeof path, "dbrow");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_row_init(&row, db, "people"), KHB_OK);
    CHECK_EQ(khb_row_columns(&row), NCOLS);
    CHECK_EQ(khb_row_type(&row, 0), KHB_INT64);
    CHECK_EQ(khb_row_type(&row, 1), KHB_TEXT);
    CHECK_EQ(khb_row_type(&row, 3), KHB_DOUBLE);

    CHECK_STATUS(khb_row_set_int(&row, 0, 7), KHB_OK);
    CHECK_STATUS(khb_row_set_text(&row, 1, "zed"), KHB_OK);
    CHECK_STATUS(khb_row_set_double(&row, 3, 1.5), KHB_OK);
    CHECK_STATUS(khb_row_set_bool(&row, 4, 9), KHB_OK);

    CHECK_STATUS(khb_row_get_int(&row, 0, &i64), KHB_OK);
    CHECK_EQ(i64, 7);
    CHECK_STATUS(khb_row_get_text(&row, 1, name, sizeof name), KHB_OK);
    CHECK_EQ(strcmp(name, "zed"), 0);
    CHECK_STATUS(khb_row_get_double(&row, 3, &d), KHB_OK);
    CHECK(d == 1.5);
    CHECK_STATUS(khb_row_get_bool(&row, 4, &b), KHB_OK);
    CHECK_EQ(b, 1);
    CHECK_STATUS(khb_row_key(&row, &i64), KHB_OK);
    CHECK_EQ(i64, 7);

    CHECK_STATUS(khb_row_set_text(&row, 0, "wrong"), KHB_ERR_INVALID);
    CHECK_STATUS(khb_row_set_int(&row, 99, 1), KHB_ERR_INVALID);
    CHECK_STATUS(khb_row_init(&row, db, "missing"), KHB_ERR_NOTFOUND);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_insert_get_delete(void)
{
    char     path[PATH_MAX];
    khb_db  *db = NULL;
    khb_row  row;
    uint32_t n = 0;
    int64_t  age = 0;
    char     name[32];

    khb_temp_path(path, sizeof path, "dbcrud");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(add_row(db, "people", 1, "amara", 34, 88.5), KHB_OK);
    CHECK_STATUS(add_row(db, "people", 2, "bo", 27, 91.0), KHB_OK);
    CHECK_STATUS(add_row(db, "people", 1, "dup", 1, 0.0), KHB_ERR_EXISTS);

    CHECK_STATUS(khb_count(db, "people", &n), KHB_OK);
    CHECK_EQ(n, 2);

    CHECK_STATUS(khb_get(db, "people", 1, &row), KHB_OK);
    CHECK_STATUS(khb_row_get_text(&row, 1, name, sizeof name), KHB_OK);
    CHECK_EQ(strcmp(name, "amara"), 0);
    CHECK_STATUS(khb_row_get_int(&row, 2, &age), KHB_OK);
    CHECK_EQ(age, 34);

    CHECK_STATUS(khb_get(db, "people", 99, &row), KHB_ERR_NOTFOUND);
    CHECK_STATUS(khb_delete(db, "people", 99), KHB_ERR_NOTFOUND);
    CHECK_STATUS(khb_delete(db, "people", 1), KHB_OK);
    CHECK_STATUS(khb_get(db, "people", 1, &row), KHB_ERR_NOTFOUND);
    CHECK_STATUS(khb_count(db, "people", &n), KHB_OK);
    CHECK_EQ(n, 1);

    CHECK_STATUS(khb_insert(db, "missing", &row), KHB_ERR_NOTFOUND);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_explicit_transaction(void)
{
    char     path[PATH_MAX];
    khb_db  *db = NULL;
    uint32_t n = 0;
    int      i;

    khb_temp_path(path, sizeof path, "dbtxn");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_begin(db, 0), KHB_OK);
    for (i = 0; i < 200; i++)
        CHECK_STATUS(add_row(db, "people", i, "x", i, 1.0), KHB_OK);
    CHECK_STATUS(khb_commit(db), KHB_OK);

    CHECK_STATUS(khb_count(db, "people", &n), KHB_OK);
    CHECK_EQ(n, 200);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_rollback_discards(void)
{
    char     path[PATH_MAX];
    khb_db  *db = NULL;
    uint32_t n = 0;
    int      i;

    khb_temp_path(path, sizeof path, "dbrb");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);
    CHECK_STATUS(add_row(db, "people", 1, "keep", 30, 1.0), KHB_OK);

    CHECK_STATUS(khb_begin(db, 0), KHB_OK);
    for (i = 100; i < 1000; i++)
        CHECK_STATUS(add_row(db, "people", i, "gone", i, 1.0), KHB_OK);
    CHECK_STATUS(khb_rollback(db), KHB_OK);

    CHECK_STATUS(khb_count(db, "people", &n), KHB_OK);
    CHECK_EQ(n, 1);
    CHECK_STATUS(khb_close(db), KHB_OK);

    CHECK_STATUS(khb_open(&db, path, 0), KHB_OK);
    CHECK_STATUS(khb_count(db, "people", &n), KHB_OK);
    CHECK_EQ(n, 1);
    CHECK_STATUS(khb_close(db), KHB_OK);

    khb_temp_remove(path);
}

static void test_readonly_txn_rejects_write(void)
{
    char    path[PATH_MAX];
    khb_db *db = NULL;

    khb_temp_path(path, sizeof path, "dbro");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_begin(db, 1), KHB_OK);
    CHECK_STATUS(add_row(db, "people", 1, "x", 1, 1.0), KHB_ERR_STATE);
    CHECK_STATUS(khb_commit(db), KHB_OK);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_scan_all_sorted(void)
{
    char           path[PATH_MAX];
    khb_db        *db = NULL;
    struct collect c;
    int            i;
    const int      N = 500;

    khb_temp_path(path, sizeof path, "dbscan");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_begin(db, 0), KHB_OK);
    for (i = N - 1; i >= 0; i--)
        CHECK_STATUS(add_row(db, "people", i, "x", i, 1.0), KHB_OK);
    CHECK_STATUS(khb_commit(db), KHB_OK);

    memset(&c, 0, sizeof c);
    CHECK_STATUS(khb_scan(db, "people", NULL, collect_visit, &c), KHB_OK);
    CHECK_EQ(c.n, N);
    for (i = 0; i < N; i++)
        CHECK_EQ(c.keys[i], (int64_t)i);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_scan_predicate(void)
{
    char           path[PATH_MAX];
    khb_db        *db = NULL;
    struct collect c;
    int            i;

    khb_temp_path(path, sizeof path, "dbpred");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_begin(db, 0), KHB_OK);
    for (i = 0; i < 100; i++)
        CHECK_STATUS(add_row(db, "people", i, "x", i, 1.0), KHB_OK);
    CHECK_STATUS(khb_commit(db), KHB_OK);

    memset(&c, 0, sizeof c);
    CHECK_STATUS(khb_scan(db, "people", even_keys, collect_visit, &c),
                 KHB_OK);
    CHECK_EQ(c.n, 50);
    for (i = 0; i < c.n; i++)
        CHECK_EQ(c.keys[i] % 2, 0);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_scan_range(void)
{
    char           path[PATH_MAX];
    khb_db        *db = NULL;
    struct collect c;
    int            i;

    khb_temp_path(path, sizeof path, "dbrange");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_begin(db, 0), KHB_OK);
    for (i = 0; i < 200; i++)
        CHECK_STATUS(add_row(db, "people", i * 2, "x", i, 1.0), KHB_OK);
    CHECK_STATUS(khb_commit(db), KHB_OK);

    memset(&c, 0, sizeof c);
    CHECK_STATUS(khb_scan_range(db, "people", 10, 20, NULL, collect_visit, &c),
                 KHB_OK);
    CHECK_EQ(c.n, 6);
    CHECK_EQ(c.keys[0], 10);
    CHECK_EQ(c.keys[5], 20);

    memset(&c, 0, sizeof c);
    CHECK_STATUS(khb_scan_range(db, "people", 9, 11, NULL, collect_visit, &c),
                 KHB_OK);
    CHECK_EQ(c.n, 1);
    CHECK_EQ(c.keys[0], 10);

    memset(&c, 0, sizeof c);
    CHECK_STATUS(khb_scan_range(db, "people", 100000, 200000, NULL,
                                collect_visit, &c), KHB_OK);
    CHECK_EQ(c.n, 0);

    CHECK_STATUS(khb_scan_range(db, "people", 50, 10, NULL, collect_visit, &c),
                 KHB_ERR_INVALID);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_visitor_can_stop(void)
{
    char           path[PATH_MAX];
    khb_db        *db = NULL;
    struct collect c;
    int            i;

    khb_temp_path(path, sizeof path, "dbstop");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_begin(db, 0), KHB_OK);
    for (i = 0; i < 50; i++)
        CHECK_STATUS(add_row(db, "people", i, "x", i, 1.0), KHB_OK);
    CHECK_STATUS(khb_commit(db), KHB_OK);

    memset(&c, 0, sizeof c);
    CHECK_STATUS(khb_scan(db, "people", NULL, stop_after_three, &c),
                 KHB_ERR_FULL);
    CHECK_EQ(c.n, 3);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_row_outlives_transaction(void)
{
    char     path[PATH_MAX];
    khb_db  *db = NULL;
    khb_row  row;
    int64_t  v = 0;

    khb_temp_path(path, sizeof path, "dbrowlife");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_row_init(&row, db, "people"), KHB_OK);
    CHECK_STATUS(khb_row_set_int(&row, 0, 42), KHB_OK);

    CHECK_STATUS(khb_begin(db, 0), KHB_OK);
    CHECK_STATUS(khb_commit(db), KHB_OK);
    CHECK_STATUS(khb_begin(db, 1), KHB_OK);
    CHECK_STATUS(khb_commit(db), KHB_OK);

    CHECK_STATUS(khb_row_get_int(&row, 0, &v), KHB_OK);
    CHECK_EQ(v, 42);
    CHECK_EQ(khb_row_columns(&row), NCOLS);
    CHECK_STATUS(khb_insert(db, "people", &row), KHB_OK);

    CHECK_STATUS(khb_close(db), KHB_OK);
    khb_temp_remove(path);
}

static void test_persist_and_reopen(void)
{
    char     path[PATH_MAX];
    khb_db  *db = NULL;
    khb_row  row;
    uint32_t n = 0;
    char     name[32];
    int      i;
    const int N = 2000;

    khb_temp_path(path, sizeof path, "dbpersist");
    CHECK_STATUS(khb_open(&db, path, 1), KHB_OK);
    CHECK_STATUS(khb_create_table(db, "people", SCHEMA, NCOLS), KHB_OK);

    CHECK_STATUS(khb_begin(db, 0), KHB_OK);
    for (i = 0; i < N; i++)
        CHECK_STATUS(add_row(db, "people", i, "amara", i, 1.0), KHB_OK);
    CHECK_STATUS(khb_commit(db), KHB_OK);
    CHECK_STATUS(khb_close(db), KHB_OK);

    CHECK_STATUS(khb_open(&db, path, 0), KHB_OK);
    CHECK_STATUS(khb_count(db, "people", &n), KHB_OK);
    CHECK_EQ(n, (uint32_t)N);
    CHECK_STATUS(khb_get(db, "people", N - 1, &row), KHB_OK);
    CHECK_STATUS(khb_row_get_text(&row, 1, name, sizeof name), KHB_OK);
    CHECK_EQ(strcmp(name, "amara"), 0);
    CHECK_STATUS(khb_close(db), KHB_OK);

    khb_temp_remove(path);
}

int main(void)
{
    RUN_TEST(test_open_and_create);
    RUN_TEST(test_row_types);
    RUN_TEST(test_insert_get_delete);
    RUN_TEST(test_explicit_transaction);
    RUN_TEST(test_rollback_discards);
    RUN_TEST(test_readonly_txn_rejects_write);
    RUN_TEST(test_scan_all_sorted);
    RUN_TEST(test_scan_predicate);
    RUN_TEST(test_scan_range);
    RUN_TEST(test_visitor_can_stop);
    RUN_TEST(test_row_outlives_transaction);
    RUN_TEST(test_persist_and_reopen);

    return TEST_SUMMARY();
}
