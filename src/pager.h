#ifndef KHB_PAGER_H
#define KHB_PAGER_H
#include <stdint.h>
#include <limits.h>
#include "khabibdb.h"
#include "page.h"
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#pragma pack(push,1)


typedef struct {
    page_header_t hdr;
    uint32_t next_free;
} free_page_t;
#pragma pack(pop)
_Static_assert(sizeof(free_page_t) == 16, "free_page_t must be 16 bytes");

typedef struct {
    int fd;
    char path[PATH_MAX];
    uint32_t page_count;
    uint32_t freelist_head;
    uint32_t catalog_root;
    int header_dirty;
    int use_full_fsync;
} pager_t;

khb_status pager_open(pager_t *p, const char *path, int create);
khb_status pager_close(pager_t *p);
khb_status pager_read(pager_t *p, uint32_t page_id, void *buf);
khb_status pager_write(pager_t *p, uint32_t page_id, void *buf);
khb_status pager_alloc(pager_t *p, uint32_t *out_page_id);
khb_status pager_free(pager_t *p, uint32_t page_id);
khb_status pager_flush_header(pager_t *p);
khb_status pager_sync(pager_t *p);
khb_status pager_sync_dir(pager_t *p);
khb_status pager_truncate(pager_t *p, uint32_t page_count);

uint32_t pager_page_count(const pager_t *p);
uint32_t pager_freelist_head(const pager_t *p);
uint32_t pager_catalog_root(const pager_t *p);
void pager_set_catalog_root(pager_t *p, uint32_t page_id);

khb_status pager_reload_header(pager_t *p);
khb_status pager_fsync_fd(const pager_t *p, int fd);


#endif