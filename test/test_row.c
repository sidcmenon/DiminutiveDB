#include <string.h>

#include "test_util.h"
#include "row.h"

static table_def_t g_def;

static void setup_def(void)
{
    catalog_t    c;
    column_def_t cols[8];
    table_def_t *t = NULL;

    column_def_set(&cols[0], "id",    COL_INT64,  0);
    column_def_set(&cols[1], "name",  COL_TEXT,   8);
    column_def_set(&cols[2], "count", COL_UINT64, 0);
    column_def_set(&cols[3], "score", COL_DOUBLE, 0);
    column_def_set(&cols[4], "flag",  COL_BOOL,   0);
    column_def_set(&cols[5], "data",  COL_BLOB,   4);

    catalog_init(&c);
    catalog_create_table(&c, "t", cols, 6, &t);
    g_def = *t;
}

static void test_all_types_roundtrip(void)
{
    uint8_t  buf[KHB_MAX_ROW_SIZE];
    row_t    r;
    int64_t  i64;
    uint64_t u64;
    double   d;
    int      b;
    char     text[32];
    uint8_t  blob[4];

    row_bind(&r, &g_def, buf);
    row_clear(&r);

    CHECK_STATUS(row_set_int(&r, 0, -12345), KHB_OK);
    CHECK_STATUS(row_set_text(&r, 1, "abc"), KHB_OK);
    CHECK_STATUS(row_set_uint(&r, 2, 18446744073709551615ULL), KHB_OK);
    CHECK_STATUS(row_set_double(&r, 3, 3.5), KHB_OK);
    CHECK_STATUS(row_set_bool(&r, 4, 7), KHB_OK);
    CHECK_STATUS(row_set_blob(&r, 5, "\x01\x02", 2), KHB_OK);

    CHECK_STATUS(row_get_int(&r, 0, &i64), KHB_OK);
    CHECK_EQ(i64, -12345);
    CHECK_STATUS(row_get_text(&r, 1, text, sizeof text), KHB_OK);
    CHECK_EQ(strcmp(text, "abc"), 0);
    CHECK_STATUS(row_get_uint(&r, 2, &u64), KHB_OK);
    CHECK(u64 == 18446744073709551615ULL);
    CHECK_STATUS(row_get_double(&r, 3, &d), KHB_OK);
    CHECK(d == 3.5);
    CHECK_STATUS(row_get_bool(&r, 4, &b), KHB_OK);
    CHECK_EQ(b, 1);
    CHECK_STATUS(row_get_blob(&r, 5, blob, sizeof blob), KHB_OK);
    CHECK_EQ(blob[0], 1);
    CHECK_EQ(blob[1], 2);
    CHECK_EQ(blob[2], 0);
}

static void test_row_key(void)
{
    uint8_t buf[KHB_MAX_ROW_SIZE];
    row_t   r;
    int64_t k;

    row_bind(&r, &g_def, buf);
    row_clear(&r);

    CHECK_STATUS(row_set_int(&r, 0, 99), KHB_OK);
    CHECK_STATUS(row_key(&r, &k), KHB_OK);
    CHECK_EQ(k, 99);
}

static void test_text_full_width(void)
{
    uint8_t buf[KHB_MAX_ROW_SIZE];
    row_t   r;
    char    text[32];

    row_bind(&r, &g_def, buf);
    row_clear(&r);

    CHECK_STATUS(row_set_int(&r, 2 - 2, 1), KHB_OK);
    CHECK_STATUS(row_set_uint(&r, 2, 0xAAAAAAAAAAAAAAAAULL), KHB_OK);

    CHECK_STATUS(row_set_text(&r, 1, "abcdefgh"), KHB_OK);
    CHECK_STATUS(row_get_text(&r, 1, text, sizeof text), KHB_OK);
    CHECK_EQ(strcmp(text, "abcdefgh"), 0);
    CHECK_EQ(strlen(text), 8);

    CHECK_STATUS(row_set_text(&r, 1, "toolongxx"), KHB_ERR_FULL);
    CHECK_STATUS(row_get_text(&r, 1, text, 8), KHB_ERR_FULL);
}

static void test_text_is_padded(void)
{
    uint8_t buf_a[KHB_MAX_ROW_SIZE];
    uint8_t buf_b[KHB_MAX_ROW_SIZE];
    row_t   a, b;

    memset(buf_a, 0xFF, sizeof buf_a);
    memset(buf_b, 0x11, sizeof buf_b);

    row_bind(&a, &g_def, buf_a);
    row_bind(&b, &g_def, buf_b);
    row_clear(&a);
    row_clear(&b);

    CHECK_STATUS(row_set_text(&a, 1, "hi"), KHB_OK);
    CHECK_STATUS(row_set_text(&b, 1, "hi"), KHB_OK);
    CHECK_EQ(memcmp(buf_a, buf_b, g_def.row_size), 0);

    CHECK_STATUS(row_set_text(&a, 1, "longer"), KHB_OK);
    CHECK_STATUS(row_set_text(&a, 1, "hi"), KHB_OK);
    CHECK_EQ(memcmp(buf_a, buf_b, g_def.row_size), 0);
}

static void test_type_mismatch_rejected(void)
{
    uint8_t  buf[KHB_MAX_ROW_SIZE];
    row_t    r;
    int64_t  i64;
    char     text[32];

    row_bind(&r, &g_def, buf);
    row_clear(&r);

    CHECK_STATUS(row_set_text(&r, 0, "nope"), KHB_ERR_INVALID);
    CHECK_STATUS(row_set_int(&r, 1, 5), KHB_ERR_INVALID);
    CHECK_STATUS(row_get_int(&r, 3, &i64), KHB_ERR_INVALID);
    CHECK_STATUS(row_get_text(&r, 5, text, sizeof text), KHB_ERR_INVALID);
    CHECK_STATUS(row_set_bool(&r, 2, 1), KHB_ERR_INVALID);
}

static void test_column_out_of_range(void)
{
    uint8_t buf[KHB_MAX_ROW_SIZE];
    row_t   r;
    int64_t i64;

    row_bind(&r, &g_def, buf);
    row_clear(&r);

    CHECK_STATUS(row_set_int(&r, -1, 1), KHB_ERR_INVALID);
    CHECK_STATUS(row_set_int(&r, 6, 1), KHB_ERR_INVALID);
    CHECK_STATUS(row_get_int(&r, 99, &i64), KHB_ERR_INVALID);
    CHECK_EQ(row_column_type(&r, 99), 0xFF);
}

static void test_columns_are_independent(void)
{
    uint8_t  buf[KHB_MAX_ROW_SIZE];
    row_t    r;
    int64_t  i64;
    uint64_t u64;
    double   d;
    int      b;

    row_bind(&r, &g_def, buf);
    row_clear(&r);

    CHECK_STATUS(row_set_int(&r, 0, 0x7FFFFFFFFFFFFFFFLL), KHB_OK);
    CHECK_STATUS(row_set_uint(&r, 2, 0xFFFFFFFFFFFFFFFFULL), KHB_OK);
    CHECK_STATUS(row_set_double(&r, 3, -1.25), KHB_OK);
    CHECK_STATUS(row_set_bool(&r, 4, 1), KHB_OK);
    CHECK_STATUS(row_set_text(&r, 1, "12345678"), KHB_OK);
    CHECK_STATUS(row_set_blob(&r, 5, "\xFF\xFF\xFF\xFF", 4), KHB_OK);

    CHECK_STATUS(row_get_int(&r, 0, &i64), KHB_OK);
    CHECK(i64 == 0x7FFFFFFFFFFFFFFFLL);
    CHECK_STATUS(row_get_uint(&r, 2, &u64), KHB_OK);
    CHECK(u64 == 0xFFFFFFFFFFFFFFFFULL);
    CHECK_STATUS(row_get_double(&r, 3, &d), KHB_OK);
    CHECK(d == -1.25);
    CHECK_STATUS(row_get_bool(&r, 4, &b), KHB_OK);
    CHECK_EQ(b, 1);
}

static void test_blob_bounds(void)
{
    uint8_t buf[KHB_MAX_ROW_SIZE];
    uint8_t out[4];
    row_t   r;

    row_bind(&r, &g_def, buf);
    row_clear(&r);

    CHECK_STATUS(row_set_blob(&r, 5, "\x01\x02\x03\x04\x05", 5), KHB_ERR_FULL);
    CHECK_STATUS(row_get_blob(&r, 5, out, 3), KHB_ERR_FULL);
    CHECK_STATUS(row_set_blob(&r, 5, NULL, 1), KHB_ERR_INVALID);
    CHECK_STATUS(row_set_blob(&r, 5, NULL, 0), KHB_OK);
}

static void test_metadata_accessors(void)
{
    uint8_t buf[KHB_MAX_ROW_SIZE];
    row_t   r;

    row_bind(&r, &g_def, buf);
    CHECK_EQ(row_column_count(&r), 6);
    CHECK_EQ(row_column_type(&r, 0), COL_INT64);
    CHECK_EQ(row_column_type(&r, 1), COL_TEXT);
    CHECK_EQ(row_column_type(&r, 5), COL_BLOB);
    CHECK_EQ(strcmp(col_type_name(COL_DOUBLE), "DOUBLE"), 0);
}

int main(void)
{
    setup_def();

    RUN_TEST(test_all_types_roundtrip);
    RUN_TEST(test_row_key);
    RUN_TEST(test_text_full_width);
    RUN_TEST(test_text_is_padded);
    RUN_TEST(test_type_mismatch_rejected);
    RUN_TEST(test_column_out_of_range);
    RUN_TEST(test_columns_are_independent);
    RUN_TEST(test_blob_bounds);
    RUN_TEST(test_metadata_accessors);

    return TEST_SUMMARY();
}