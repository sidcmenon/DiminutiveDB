#ifndef KHB_JOURNAL_H
#define KHB_JOURNAL_H

#include <stdint.h>
#include <stddef.h>
#include "khabibdb.h"
#include "page.h"
#include "pager.h"

#define KHB_JOURNAL_MAGIC "KHBJRNL1"
#define KHB_JOURNAL_MAGIC_LEN 8

#define KHB_JOURNAL_RECORD_SIZE (4 + KHB_PAGE_SIZE)
#pragma pack(push, 1)

typedef struct{
    char magic[KHB_JOURNAL_MAGIC_LEN];
    uint32_t page_size;
    uint32_t orig_page_count;
    uint32_t reserved;
    uint32_t checksum;
} journal_header_t;

#pragma pack(pop)

_Static_assert(sizeof(journal_header_t)==24, "journal_header_t must be 24 bytes");

typedef struct {
    int fd;
    char path[PATH_MAX];
    int active;
    uint32_t orig_page_count;
    uint32_t *noted;
    size_t noted_len;
    size_t noted_cap;
} journal_t;

void journal_init(journal_t *j);
void journal_free(journal_t *j);

khb_status journal_begin(journal_t *j, pager_t *p);

khb_status journal_note_page(journal_t *j, pager_t *p, uint32_t page_id);

khb_status journal_commit(journal_t *j, pager_t *p);
khb_status journal_rollback(journal_t *j, pager_t *p);

khb_status journal_recover(pager_t *p);

int journal_is_active(const journal_t *j);

void journal_path_for(const char *db_path, char *out, size_t cap);

#endif


