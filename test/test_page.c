#include <string.h>

#include "test_util.h"
#include "page.h"
#include "crc32.h"

static uint8_t page_a[KHB_PAGE_SIZE];
static uint8_t page_b[KHB_PAGE_SIZE];

static void test_crc_vector(void)
{
    CHECK_EQ(khb_crc32("123456789", 9), 0xCBF43926u);
}

static void test_struct_sizes(void)
{
    CHECK_EQ(sizeof(page_header_t), 12);
    CHECK_EQ(sizeof(file_header_t), 40);
}

static void test_roundtrip(void)
{
    page_init(page_a, 7, PAGE_TYPE_BTREE_LEAF);
    page_a[100] = 0xAB;
    page_finalize(page_a);

    CHECK_STATUS(page_verify(page_a, 7), KHB_OK);
    CHECK_EQ(page_type_of(page_a), PAGE_TYPE_BTREE_LEAF);
    CHECK_EQ(page_a[100], 0xAB);
}

static void test_wrong_id(void)
{
    page_init(page_a, 7, PAGE_TYPE_BTREE_LEAF);
    page_finalize(page_a);

    CHECK_STATUS(page_verify(page_a, 7), KHB_OK);
    CHECK_STATUS(page_verify(page_a, 8), KHB_ERR_CORRUPT);
}

/* Catches a wrong length passed to page_finalize — e.g. subtracting
 * sizeof(page_header_t) instead of 4, which leaves 8 bytes unprotected
 * while every simpler test still passes. */
static void test_bitflip_sweep(void)
{
    size_t off;
    int    bit;

    page_init(page_a, 3, PAGE_TYPE_HEAP);
    memset(page_a + sizeof(page_header_t), 0x5A,
           KHB_PAGE_SIZE - sizeof(page_header_t));
    page_finalize(page_a);
    CHECK_STATUS(page_verify(page_a, 3), KHB_OK);

    for (off = KHB_CHECKSUM_SKIP; off < (size_t)KHB_PAGE_SIZE; off++) {
        for (bit = 0; bit < 8; bit++) {
            page_a[off] ^= (uint8_t)(1u << bit);
            if (page_verify(page_a, 3) != KHB_ERR_CORRUPT)
                FAILF("undetected flip at byte %zu, bit %d", off, bit);
            page_a[off] ^= (uint8_t)(1u << bit);
        }
    }

    CHECK_STATUS(page_verify(page_a, 3), KHB_OK);
}

/* The only test that catches a missing memset in page_init: identical
 * logical content must checksum identically regardless of prior buffer
 * contents. */
static void test_uninitialized_tail(void)
{
    memset(page_a, 0xFF, KHB_PAGE_SIZE);
    memset(page_b, 0x11, KHB_PAGE_SIZE);

    page_init(page_a, 5, PAGE_TYPE_CATALOG);
    page_init(page_b, 5, PAGE_TYPE_CATALOG);

    page_a[64] = 0x42;
    page_b[64] = 0x42;

    page_finalize(page_a);
    page_finalize(page_b);

    CHECK_EQ(((const page_header_t *)page_a)->checksum,
             ((const page_header_t *)page_b)->checksum);
}

static void test_file_header(void)
{
    file_header_t *fh;

    file_header_init(page_a);
    page_finalize(page_a);

    CHECK_STATUS(page_verify(page_a, 0), KHB_OK);
    CHECK_STATUS(file_header_validate(page_a), KHB_OK);

    fh = (file_header_t *)page_a;
    CHECK_EQ(fh->page_size, KHB_PAGE_SIZE);
    CHECK_EQ(fh->format_version, KHB_FORMAT_VERSION);
    CHECK_EQ(fh->page_count, 1);
    CHECK_EQ(fh->catalog_root, 0);
    CHECK_EQ(fh->freelist_head, 0);
    CHECK_EQ(page_type_of(page_a), PAGE_TYPE_FILE_HEADER);

    fh->magic[0] = 'X';
    CHECK_STATUS(file_header_validate(page_a), KHB_ERR_CORRUPT);

    file_header_init(page_a);
    fh = (file_header_t *)page_a;
    fh->format_version = KHB_FORMAT_VERSION + 1;
    CHECK_STATUS(file_header_validate(page_a), KHB_ERR_CORRUPT);

    file_header_init(page_a);
    ((page_header_t *)page_a)->page_type = PAGE_TYPE_HEAP;
    CHECK_STATUS(file_header_validate(page_a), KHB_ERR_CORRUPT);
}

int main(void)
{
    RUN_TEST(test_crc_vector);
    RUN_TEST(test_struct_sizes);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_wrong_id);
    RUN_TEST(test_bitflip_sweep);
    RUN_TEST(test_uninitialized_tail);
    RUN_TEST(test_file_header);

    return TEST_SUMMARY();
}