#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_util.h"
#include "journal.h"
#include "crc32.h"

#define BASE_PAGES 8

static void fill_pattern(uint8_t *buf, uint32_t id, uint8_t gen)
{
    page_init(buf, id, PAGE_TYPE_HEAP);
    memset(buf + sizeof(page_header_t), (uint8_t)(gen * 0x40 + id),
           KHB_PAGE_SIZE - sizeof(page_header_t));
}

static khb_status make_db(pager_t *p, const char *path)
{
    uint8_t    buf[KHB_PAGE_SIZE];
    uint32_t   id;
    int        i;
    khb_status rc;

    rc = pager_open(p, path, 1);
    if (rc != KHB_OK)
        return rc;

    for (i = 0; i < BASE_PAGES; i++) {
        rc = pager_alloc(p, &id);
        if (rc != KHB_OK)
            return rc;
        fill_pattern(buf, id, 0);
        rc = pager_write(p, id, buf);
        if (rc != KHB_OK)
            return rc;
    }
    return pager_sync(p);
}

/* CRC of the whole file — used to assert byte-for-byte restoration. */
static uint32_t file_crc(const char *path)
{
    uint8_t  buf[KHB_PAGE_SIZE];
    uint32_t crc = 0;
    ssize_t  n;
    int      fd = open(path, O_RDONLY);

    if (fd < 0)
        return 0;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        crc = khb_crc32(buf, (size_t)n) ^ (crc << 1);
    close(fd);
    return crc;
}

static void test_begin_creates_journal(void)
{
    char        path[PATH_MAX], jpath[PATH_MAX];
    pager_t     p;
    journal_t   j;
    struct stat st;

    khb_temp_path(path, sizeof path, "jbegin");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    journal_path_for(path, jpath, sizeof jpath);

    journal_init(&j);
    CHECK_EQ(journal_is_active(&j), 0);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_EQ(journal_is_active(&j), 1);

    CHECK_EQ(stat(jpath, &st), 0);
    CHECK_EQ(st.st_size, (off_t)24 + KHB_JOURNAL_RECORD_SIZE);

    CHECK_STATUS(journal_begin(&j, &p), KHB_ERR_STATE);

    CHECK_STATUS(journal_rollback(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_note_page_is_idempotent(void)
{
    char        path[PATH_MAX], jpath[PATH_MAX];
    pager_t     p;
    journal_t   j;
    struct stat st;

    khb_temp_path(path, sizeof path, "jnote");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    journal_path_for(path, jpath, sizeof jpath);

    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);

    CHECK_STATUS(journal_note_page(&j, &p, 3), KHB_OK);
    CHECK_EQ(stat(jpath, &st), 0);
    CHECK_EQ(st.st_size, (off_t)24 + 2 * KHB_JOURNAL_RECORD_SIZE);

    CHECK_STATUS(journal_note_page(&j, &p, 3), KHB_OK);
    CHECK_STATUS(journal_note_page(&j, &p, 0), KHB_OK);
    CHECK_EQ(stat(jpath, &st), 0);
    CHECK_EQ(st.st_size, (off_t)24 + 2 * KHB_JOURNAL_RECORD_SIZE);

    /* Beyond the original end: no pre-image exists, so nothing is written. */
    CHECK_STATUS(journal_note_page(&j, &p, BASE_PAGES + 5), KHB_OK);
    CHECK_EQ(stat(jpath, &st), 0);
    CHECK_EQ(st.st_size, (off_t)24 + 2 * KHB_JOURNAL_RECORD_SIZE);

    CHECK_STATUS(journal_rollback(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_rollback_restores_pages(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    uint8_t   buf[KHB_PAGE_SIZE];
    uint32_t  before, after;
    int       i;

    khb_temp_path(path, sizeof path, "jrestore");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    before = file_crc(path);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);

    for (i = 1; i <= BASE_PAGES; i++) {
        CHECK_STATUS(journal_note_page(&j, &p, (uint32_t)i), KHB_OK);
        fill_pattern(buf, (uint32_t)i, 1);
        CHECK_STATUS(pager_write(&p, (uint32_t)i, buf), KHB_OK);
    }
    CHECK_STATUS(pager_sync(&p), KHB_OK);

    CHECK_STATUS(journal_rollback(&j, &p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    after = file_crc(path);
    CHECK_EQ(after, before);
    khb_temp_remove(path);
}

static void test_rollback_restores_page_count(void)
{
    char        path[PATH_MAX];
    pager_t     p;
    journal_t   j;
    uint32_t    id;
    struct stat st;
    int         i;

    khb_temp_path(path, sizeof path, "jcount");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    CHECK_EQ(pager_page_count(&p), BASE_PAGES + 1);

    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);

    for (i = 0; i < 5; i++)
        CHECK_STATUS(pager_alloc(&p, &id), KHB_OK);
    CHECK_EQ(pager_page_count(&p), BASE_PAGES + 6);
    CHECK_STATUS(pager_sync(&p), KHB_OK);

    CHECK_STATUS(journal_rollback(&j, &p), KHB_OK);
    CHECK_EQ(pager_page_count(&p), BASE_PAGES + 1);

    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_EQ(stat(path, &st), 0);
    CHECK_EQ(st.st_size, (off_t)(BASE_PAGES + 1) * KHB_PAGE_SIZE);
    khb_temp_remove(path);
}

static void test_rollback_restores_freelist(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;

    khb_temp_path(path, sizeof path, "jfree");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    CHECK_EQ(pager_freelist_head(&p), 0);

    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);

    CHECK_STATUS(journal_note_page(&j, &p, 4), KHB_OK);
    CHECK_STATUS(pager_free(&p, 4), KHB_OK);
    CHECK_EQ(pager_freelist_head(&p), 4);
    CHECK_STATUS(pager_sync(&p), KHB_OK);

    CHECK_STATUS(journal_rollback(&j, &p), KHB_OK);
    CHECK_EQ(pager_freelist_head(&p), 0);

    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_commit_removes_journal(void)
{
    char        path[PATH_MAX], jpath[PATH_MAX];
    pager_t     p;
    journal_t   j;
    uint8_t     buf[KHB_PAGE_SIZE];
    struct stat st;

    khb_temp_path(path, sizeof path, "jcommit");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    journal_path_for(path, jpath, sizeof jpath);

    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(journal_note_page(&j, &p, 2), KHB_OK);
    fill_pattern(buf, 2, 1);
    CHECK_STATUS(pager_write(&p, 2, buf), KHB_OK);
    CHECK_STATUS(journal_commit(&j, &p), KHB_OK);

    CHECK_EQ(journal_is_active(&j), 0);
    CHECK(stat(jpath, &st) != 0);
    CHECK_STATUS(journal_commit(&j, &p), KHB_ERR_STATE);

    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    CHECK_STATUS(pager_read(&p, 2, buf), KHB_OK);
    CHECK_EQ(buf[sizeof(page_header_t)], (uint8_t)(0x40 + 2));
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

/* Leaves a journal on disk without committing, then reopens — the crash case
 * without an actual crash. */
static void test_recover_replays_on_open(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    uint8_t   buf[KHB_PAGE_SIZE];
    uint32_t  before, after;
    int       i;

    khb_temp_path(path, sizeof path, "jrecover");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    before = file_crc(path);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    for (i = 1; i <= 4; i++) {
        CHECK_STATUS(journal_note_page(&j, &p, (uint32_t)i), KHB_OK);
        fill_pattern(buf, (uint32_t)i, 1);
        CHECK_STATUS(pager_write(&p, (uint32_t)i, buf), KHB_OK);
    }
    CHECK_STATUS(pager_sync(&p), KHB_OK);

    /* Abandon the transaction: close both without commit or rollback. */
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    after = file_crc(path);
    CHECK_EQ(after, before);
    khb_temp_remove(path);
}

static void test_invalid_header_discarded(void)
{
    char        path[PATH_MAX], jpath[PATH_MAX];
    pager_t     p;
    journal_t   j;
    uint8_t     buf[KHB_PAGE_SIZE];
    struct stat st;
    int         fd;
    char        bad = 'X';

    khb_temp_path(path, sizeof path, "jbadhdr");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    journal_path_for(path, jpath, sizeof jpath);

    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    CHECK_STATUS(journal_note_page(&j, &p, 1), KHB_OK);
    fill_pattern(buf, 1, 1);
    CHECK_STATUS(pager_write(&p, 1, buf), KHB_OK);
    CHECK_STATUS(pager_sync(&p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    fd = open(jpath, O_RDWR);
    CHECK(fd >= 0);
    CHECK_EQ(pwrite(fd, &bad, 1, 0), 1);
    CHECK_EQ(close(fd), 0);

    /* The generation-1 write must SURVIVE: a bad journal header means the
     * crash predated any main-file modification, so nothing is rolled back. */
    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    CHECK(stat(jpath, &st) != 0);
    CHECK_STATUS(pager_read(&p, 1, buf), KHB_OK);
    CHECK_EQ(buf[sizeof(page_header_t)], (uint8_t)(0x40 + 1));
    CHECK_STATUS(pager_close(&p), KHB_OK);
    khb_temp_remove(path);
}

static void test_partial_record_ignored(void)
{
    char      path[PATH_MAX], jpath[PATH_MAX];
    pager_t   p;
    journal_t j;
    uint8_t   buf[KHB_PAGE_SIZE];
    uint32_t  before;
    int       fd, i;

    khb_temp_path(path, sizeof path, "jpartial");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    before = file_crc(path);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    journal_path_for(path, jpath, sizeof jpath);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    for (i = 1; i <= 3; i++) {
        CHECK_STATUS(journal_note_page(&j, &p, (uint32_t)i), KHB_OK);
        fill_pattern(buf, (uint32_t)i, 1);
        CHECK_STATUS(pager_write(&p, (uint32_t)i, buf), KHB_OK);
    }
    CHECK_STATUS(pager_sync(&p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    /* Lop 100 bytes off the last record, as a crash mid-append would. */
    fd = open(jpath, O_RDWR);
    CHECK(fd >= 0);
    CHECK_EQ(ftruncate(fd, (off_t)24 + 4 * KHB_JOURNAL_RECORD_SIZE - 100), 0);
    CHECK_EQ(close(fd), 0);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    /* Pages 1-2 restored from complete records; page 3's record was partial,
     * so its pre-image is gone — but so was its modification, in a real
     * crash. Here we forced the write, so only 1-2 revert. */
    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(pager_read(&p, 1, buf), KHB_OK);
    CHECK_EQ(buf[sizeof(page_header_t)], (uint8_t)1);
    CHECK_STATUS(pager_read(&p, 2, buf), KHB_OK);
    CHECK_EQ(buf[sizeof(page_header_t)], (uint8_t)2);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    (void)before;
    khb_temp_remove(path);
}

static void test_recover_is_idempotent(void)
{
    char      path[PATH_MAX];
    pager_t   p;
    journal_t j;
    uint8_t   buf[KHB_PAGE_SIZE];
    uint32_t  before, after;
    int       i;

    khb_temp_path(path, sizeof path, "jidem");
    CHECK_STATUS(make_db(&p, path), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);
    before = file_crc(path);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    journal_init(&j);
    CHECK_STATUS(journal_begin(&j, &p), KHB_OK);
    for (i = 1; i <= 5; i++) {
        CHECK_STATUS(journal_note_page(&j, &p, (uint32_t)i), KHB_OK);
        fill_pattern(buf, (uint32_t)i, 1);
        CHECK_STATUS(pager_write(&p, (uint32_t)i, buf), KHB_OK);
    }
    CHECK_STATUS(pager_sync(&p), KHB_OK);
    journal_free(&j);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    CHECK_STATUS(pager_open(&p, path, 0), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    CHECK_STATUS(journal_recover(&p), KHB_OK);
    CHECK_STATUS(pager_close(&p), KHB_OK);

    after = file_crc(path);
    CHECK_EQ(after, before);
    khb_temp_remove(path);
}

int main(void)
{
    RUN_TEST(test_begin_creates_journal);
    RUN_TEST(test_note_page_is_idempotent);
    RUN_TEST(test_rollback_restores_pages);
    RUN_TEST(test_rollback_restores_page_count);
    RUN_TEST(test_rollback_restores_freelist);
    RUN_TEST(test_commit_removes_journal);
    RUN_TEST(test_recover_replays_on_open);
    RUN_TEST(test_invalid_header_discarded);
    RUN_TEST(test_partial_record_ignored);
    RUN_TEST(test_recover_is_idempotent);

    return TEST_SUMMARY();
}