#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "journal.h"
#include "crashpoint.h"

#define BASE_PAGES 8
#define NEW_PAGES  4

static void pattern_for(uint8_t *buf, uint32_t id, uint8_t gen)
{
    page_init(buf, id, PAGE_TYPE_HEAP);
    memset(buf + sizeof(page_header_t), (uint8_t)(gen * 0x40 + id),
           KHB_PAGE_SIZE - sizeof(page_header_t));
}

static int cmd_setup(const char *path)
{
    pager_t  p;
    uint8_t  buf[KHB_PAGE_SIZE];
    uint32_t id;
    int      i;

    if (pager_open(&p, path, 1) != KHB_OK)
        return 1;

    for (i = 0; i < BASE_PAGES; i++) {
        if (pager_alloc(&p, &id) != KHB_OK)
            return 1;
        pattern_for(buf, id, 0);
        if (pager_write(&p, id, buf) != KHB_OK)
            return 1;
    }

    if (pager_sync(&p) != KHB_OK)
        return 1;
    return (pager_close(&p) == KHB_OK) ? 0 : 1;
}

static int cmd_mutate(const char *path)
{
    pager_t   p;
    journal_t j;
    uint8_t   buf[KHB_PAGE_SIZE];
    uint32_t  id;
    int       i;

    khb_crash_init();

    if (pager_open(&p, path, 0) != KHB_OK)
        return 1;
    if (journal_recover(&p) != KHB_OK)
        return 1;

    journal_init(&j);
    if (journal_begin(&j, &p) != KHB_OK)
        return 1;

    for (i = 1; i <= BASE_PAGES; i++) {
        if (journal_note_page(&j, &p, (uint32_t)i) != KHB_OK)
            return 1;
        pattern_for(buf, (uint32_t)i, 1);
        if (pager_write(&p, (uint32_t)i, buf) != KHB_OK)
            return 1;
        khb_crashpoint();
    }

    for (i = 0; i < NEW_PAGES; i++) {
        if (pager_alloc(&p, &id) != KHB_OK)
            return 1;
        pattern_for(buf, id, 1);
        if (pager_write(&p, id, buf) != KHB_OK)
            return 1;
        khb_crashpoint();
    }

    if (journal_commit(&j, &p) != KHB_OK)
        return 1;

    journal_free(&j);
    return (pager_close(&p) == KHB_OK) ? 0 : 1;
}

/*
 * Atomicity check. The database must be entirely pre-transaction or entirely
 * post-commit — any mixture is a torn transaction and a real bug.
 */
static int cmd_verify(const char *path)
{
    pager_t  p;
    uint8_t  buf[KHB_PAGE_SIZE];
    int      old_n = 0, new_n = 0, i;
    uint32_t count;

    if (pager_open(&p, path, 0) != KHB_OK) {
        fprintf(stderr, "verify: open failed\n");
        return 1;
    }
    if (journal_recover(&p) != KHB_OK) {
        fprintf(stderr, "verify: recovery failed\n");
        return 1;
    }

    count = pager_page_count(&p);

    for (i = 1; i <= BASE_PAGES; i++) {
        uint8_t want_old, want_new, got;

        if (pager_read(&p, (uint32_t)i, buf) != KHB_OK) {
            fprintf(stderr, "verify: page %d unreadable\n", i);
            return 1;
        }
        got      = buf[sizeof(page_header_t)];
        want_old = (uint8_t)i;
        want_new = (uint8_t)(0x40 + i);

        if (got == want_old)      old_n++;
        else if (got == want_new) new_n++;
        else {
            fprintf(stderr, "verify: page %d has garbage 0x%02x\n", i, got);
            return 1;
        }
    }

    pager_close(&p);

    if (old_n == BASE_PAGES && count == BASE_PAGES + 1) {
        printf("ROLLED_BACK\n");
        return 0;
    }
    if (new_n == BASE_PAGES && count == BASE_PAGES + 1 + NEW_PAGES) {
        printf("COMMITTED\n");
        return 0;
    }

    fprintf(stderr, "verify: TORN — %d old, %d new, page_count %u\n",
            old_n, new_n, count);
    return 1;
}

/* Run the workload with no crash armed and report how many points it passes. */
static int cmd_count(void)
{
    char path[512];

    snprintf(path, sizeof path, "%s/khb_crashcount_%d.db",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());

    unsetenv("KHB_CRASH_AT");

    if (cmd_setup(path) != 0)
        return 1;
    if (cmd_mutate(path) != 0)
        return 1;

    printf("%u\n", khb_crashpoint_total());

    unlink(path);
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: crash_child {count | setup <db> | mutate <db> "
                    "| verify <db>}\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "count") == 0)
        return cmd_count();
    if (argc < 3) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "setup") == 0)
        return cmd_setup(argv[2]);
    if (strcmp(argv[1], "mutate") == 0)
        return cmd_mutate(argv[2]);
    if (strcmp(argv[1], "verify") == 0)
        return cmd_verify(argv[2]);

    usage();
    return 2;
}