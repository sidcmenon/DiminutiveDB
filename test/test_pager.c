#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_util.h"
#include "pager.h"

/* Build a fully-formed page with a recognisable payload. */
static void fill_page(uint8_t *buf, uint32_t id, uint8_t type, uint8_t pattern)
{
    page_init(buf, id, type);
    memset(buf + sizeof(page_header_t), pattern,
           KHB_PAGE_SIZE - sizeof(page_header_t));
}

static void test_create_and_reopen(void)
{
    char    path[PATH_MAX];
    pager_t p;

    khb_temp_path(path, sizeof path, "create");

    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    CHECK_EQ(pager_page_count(&p), 1);
    CHECK_EQ(pager_freelist_head(&p), 0);
    CHECK_EQ(pager_catalog_root(&p), 0);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_EQ(pager_page_count(&p), 1);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    /* close is idempotent */
    CHECK_STATUS(pager_close(&p), KHB_OK);

    khb_temp_remove(path);
}

static void test_open_missing(void)
{
    char    path[PATH_MAX];
    pager_t p;

    khb_temp_path(path, sizeof path, "missing");
    CHECK_STATUS(pager_open(&p, path, 0), KHB_ERR_IO);
}

static void test_write_read_roundtrip(void)
{
    char     path[PATH_MAX];
    pager_t  p;
    uint8_t  buf[KHB_PAGE_SIZE];
    uint8_t  expect[KHB_PAGE_SIZE];
    uint32_t ids[8];
    int      i;

    khb_temp_path(path, sizeof path, "rt");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);

    for (i = 0; i < 8; i++) {
        CHECK_STATUS(pager_alloc(&p, &ids[i]), KHB_OK);
        CHECK_EQ(ids[i], (uint32_t)(i + 1));
        fill_page(buf, ids[i], PAGE_TYPE_HEAP, (uint8_t)(0xA0 + i));
        CHECK_STATUS(pager_write(&p, ids[i], buf), KHB_OK);
    }
    CHECK_EQ(pager_page_count(&p), 9);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_EQ(pager_page_count(&p), 9);

    for (i = 0; i < 8; i++) {
        CHECK_STATUS(pager_read(&p, ids[i], buf), KHB_OK);
        fill_page(expect, ids[i], PAGE_TYPE_HEAP, (uint8_t)(0xA0 + i));
        page_finalize(expect);
        CHECK_EQ(memcmp(buf, expect, KHB_PAGE_SIZE), 0);
    }
    CHECK_STATUS(pager_close(&p), KHB_OK);

    khb_temp_remove(path);
}

static void test_read_out_of_range(void)
{
    char    path[PATH_MAX];
    pager_t p;
    uint8_t buf[KHB_PAGE_SIZE];

    khb_temp_path(path, sizeof path, "oor");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);

    CHECK_STATUS(pager_read(&p, 1, buf), KHB_ERR_INVALID);
    CHECK_STATUS(pager_read(&p, 99999, buf), KHB_ERR_INVALID);

    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_write_id_mismatch(void)
{
    char     path[PATH_MAX];
    pager_t  p;
    uint8_t  buf[KHB_PAGE_SIZE];
    uint32_t a, b;

    khb_temp_path(path, sizeof path, "mismatch");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    CHECK_STATUS(pager_alloc(&p, &a), KHB_OK);
    CHECK_STATUS(pager_alloc(&p, &b), KHB_OK);

    /* page says it is `a`, caller claims it is `b` */
    fill_page(buf, a, PAGE_TYPE_HEAP, 0x11);
    CHECK_STATUS(pager_write(&p, b, buf), KHB_ERR_INVALID);

    /* same page, correct id, succeeds */
    CHECK_STATUS(pager_write(&p, a, buf), KHB_OK);

    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_freelist_reuse(void)
{
    char     path[PATH_MAX];
    pager_t  p;
    uint32_t a, b, c, d, e, f;

    khb_temp_path(path, sizeof path, "freelist");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);

    CHECK_STATUS(pager_alloc(&p, &a), KHB_OK);   /* 1 */
    CHECK_STATUS(pager_alloc(&p, &b), KHB_OK);   /* 2 */
    CHECK_STATUS(pager_alloc(&p, &c), KHB_OK);   /* 3 */
    CHECK_EQ(pager_page_count(&p), 4);

    CHECK_STATUS(pager_free(&p, b), KHB_OK);
    CHECK_STATUS(pager_free(&p, c), KHB_OK);
    CHECK_EQ(pager_freelist_head(&p), c);

    /* LIFO: most recently freed comes back first */
    CHECK_STATUS(pager_alloc(&p, &d), KHB_OK);
    CHECK_EQ(d, c);
    CHECK_STATUS(pager_alloc(&p, &e), KHB_OK);
    CHECK_EQ(e, b);
    CHECK_EQ(pager_freelist_head(&p), 0);

    /* list exhausted — now it extends */
    CHECK_STATUS(pager_alloc(&p, &f), KHB_OK);
    CHECK_EQ(f, 4);
    CHECK_EQ(pager_page_count(&p), 5);

    /* free list survives a reopen */
    CHECK_STATUS(pager_free(&p, f), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_EQ(pager_freelist_head(&p), f);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    khb_temp_remove(path);
}

static void test_free_rejects_page_zero(void)
{
    char    path[PATH_MAX];
    pager_t p;

    khb_temp_path(path, sizeof path, "freezero");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);

    CHECK_STATUS(pager_free(&p, 0), KHB_ERR_INVALID);
    CHECK_STATUS(pager_free(&p, 7), KHB_ERR_INVALID);   /* out of range */

    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_truncate(void)
{
    char        path[PATH_MAX];
    pager_t     p;
    uint8_t     buf[KHB_PAGE_SIZE];
    uint32_t    id;
    struct stat st;
    int         i;

    khb_temp_path(path, sizeof path, "trunc");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);

    for (i = 0; i < 4; i++)
        CHECK_STATUS(pager_alloc(&p, &id), KHB_OK);
    CHECK_EQ(pager_page_count(&p), 5);

    CHECK_STATUS(pager_truncate(&p, 6), KHB_ERR_INVALID);  /* grow refused */
    CHECK_STATUS(pager_truncate(&p, 0), KHB_ERR_INVALID);  /* page 0 stays */

    CHECK_STATUS(pager_truncate(&p, 3), KHB_OK);
    CHECK_EQ(pager_page_count(&p), 3);
    CHECK_STATUS(pager_read(&p, 3, buf), KHB_ERR_INVALID);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_EQ(stat(path, &st), 0);
    CHECK_EQ(st.st_size, (off_t)3 * KHB_PAGE_SIZE);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_EQ(pager_page_count(&p), 3);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    khb_temp_remove(path);
}

/* Damage a byte on disk behind the pager's back. */
static void test_corrupt_page_detected(void)
{
    char     path[PATH_MAX];
    pager_t  p;
    uint8_t  buf[KHB_PAGE_SIZE];
    uint32_t id;
    int      fd;
    uint8_t  byte;
    off_t    off;

    khb_temp_path(path, sizeof path, "corrupt");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    CHECK_STATUS(pager_alloc(&p, &id), KHB_OK);
    fill_page(buf, id, PAGE_TYPE_HEAP, 0x77);
    CHECK_STATUS(pager_write(&p, id, buf), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    off = (off_t)id * KHB_PAGE_SIZE + 100;
    fd  = open(path, O_RDWR);
    CHECK(fd >= 0);
    CHECK_EQ(pread(fd, &byte, 1, off), 1);
    byte ^= 0xFF;
    CHECK_EQ(pwrite(fd, &byte, 1, off), 1);
    CHECK_EQ(close(fd), 0);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(pager_read(&p, id, buf), KHB_ERR_CORRUPT);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    khb_temp_remove(path);
}

/*
 * Corrupt the magic AND recompute the checksum, so page_verify passes and
 * only file_header_validate can catch it. Without the recompute this would
 * exercise the checksum path twice and the validation path never.
 */
static void test_bad_magic_rejected(void)
{
    char           path[PATH_MAX];
    pager_t        p;
    uint8_t        buf[KHB_PAGE_SIZE];
    file_header_t *fh;
    int            fd;

    khb_temp_path(path, sizeof path, "magic");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    CHECK_EQ(pread(fd, buf, KHB_PAGE_SIZE, 0), KHB_PAGE_SIZE);

    fh = (file_header_t *)buf;
    fh->magic[0] = 'X';
    page_finalize(buf);                 /* checksum now valid again */

    CHECK_EQ(pwrite(fd, buf, KHB_PAGE_SIZE, 0), KHB_PAGE_SIZE);
    CHECK_EQ(close(fd), 0);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_ERR_CORRUPT);

    khb_temp_remove(path);
}

static void test_header_fields_persist(void)
{
    char     path[PATH_MAX];
    pager_t  p;
    uint32_t id;

    khb_temp_path(path, sizeof path, "hdrfields");
    CHECK_STATUS(pager_open(&p, path, 1), KHB_OK);
    CHECK_STATUS(pager_alloc(&p, &id), KHB_OK);

    pager_set_catalog_root(&p, id);
    CHECK_EQ(pager_catalog_root(&p), id);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_EQ(pager_catalog_root(&p), id);
    CHECK_EQ(pager_page_count(&p), 2);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    khb_temp_remove(path);
}

int main(void)
{
    RUN_TEST(test_create_and_reopen);
    RUN_TEST(test_open_missing);
    RUN_TEST(test_write_read_roundtrip);
    RUN_TEST(test_read_out_of_range);
    RUN_TEST(test_write_id_mismatch);
    RUN_TEST(test_freelist_reuse);
    RUN_TEST(test_free_rejects_page_zero);
    RUN_TEST(test_truncate);
    RUN_TEST(test_corrupt_page_detected);
    RUN_TEST(test_bad_magic_rejected);
    RUN_TEST(test_header_fields_persist);

    return TEST_SUMMARY();
}