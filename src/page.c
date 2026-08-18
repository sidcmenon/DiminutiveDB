#include "page.h"
#include "crc32.h"
#include <string.h>

static uint32_t page_compute_checksum(const void *page){
    const uint8_t *p = (const uint8_t *)page;
    return khb_crc32(p + KHB_CHECKSUM_SKIP, KHB_PAGE_SIZE - KHB_CHECKSUM_SKIP);
}

void page_init(void *page, uint32_t page_id, uint8_t type){
    page_header_t *h;
    memset(page, 0, KHB_PAGE_SIZE);
    h = (page_header_t *)page;
    h->page_id = page_id;
    h->page_type = type;
}

void page_finalize(void *page){
    page_header_t *h = (page_header_t *)page;
    h->checksum = page_compute_checksum(page);
}

khb_status page_verify(const void *page, uint32_t expect_id){
    const page_header_t *h = (const page_header_t *)page;
    if (h->checksum != page_compute_checksum(page))
        return KHB_ERR_CORRUPT;
    if (h->page_id != expect_id)
        return KHB_ERR_CORRUPT;

    return KHB_OK;
}

uint8_t page_type_of(const void *page){
    return ((const page_header_t *)page) -> page_type;
}

void file_header_init(void *page){
    file_header_t *fh;
    page_init(page, 0, PAGE_TYPE_FILE_HEADER);
    fh = (file_header_t *)page;
    memcpy(fh->magic, KHB_MAGIC, KHB_MAGIC_LEN);
    fh->page_size = KHB_PAGE_SIZE;
    fh->format_version = KHB_FORMAT_VERSION;
    fh->catalog_root = 0;
    fh->freelist_head = 0;
    fh->page_count = 1;
}

khb_status file_header_validate(const void *page){
    const file_header_t *fh = (const file_header_t *)page;

    if (page_type_of(page) != PAGE_TYPE_FILE_HEADER)
        return KHB_ERR_CORRUPT;
    if (memcmp(fh->magic, KHB_MAGIC, KHB_MAGIC_LEN) != 0)
        return KHB_ERR_CORRUPT;
    if (fh->page_size != KHB_PAGE_SIZE)
        return KHB_ERR_CORRUPT;
    if (fh->format_version != KHB_FORMAT_VERSION)
        return KHB_ERR_CORRUPT;

    return KHB_OK;
}