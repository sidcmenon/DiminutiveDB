#ifndef KHB_HEAP_H
#define KHB_HEAP_H

#include <stdint.h>

#include "khabibdb.h"
#include "page.h"
#include "pager.h"
#include "journal.h"
#include "catalog.h"

#pragma pack(push, 1)

typedef struct {
    page_header_t hdr;
    uint16_t      count;
    uint16_t      capacity;
    uint32_t      next_page; 
} heap_page_header_t;

#pragma pack(pop)

_Static_assert(sizeof(heap_page_header_t) == 20,
               "heap_page_header_t must be 20 bytes");

typedef struct {
    pager_t           *pager;
    journal_t         *journal;
    const table_def_t *def;
    uint32_t           root;
} heap_t;

typedef khb_status (*heap_visit_fn)(const uint8_t *row, void *ctx);

uint16_t heap_capacity_for(uint16_t row_size);

khb_status heap_create(pager_t *p, journal_t *j, const table_def_t *def,
                       uint32_t *out_root);
void       heap_open(heap_t *h, pager_t *p, journal_t *j,
                     const table_def_t *def, uint32_t root);

khb_status heap_insert(heap_t *h, const uint8_t *row);
khb_status heap_get(heap_t *h, int64_t key, uint8_t *out_row);
khb_status heap_delete(heap_t *h, int64_t key);
khb_status heap_scan(heap_t *h, heap_visit_fn fn, void *ctx);
khb_status heap_count(heap_t *h, uint32_t *out);

#endif /* KHB_HEAP_H */