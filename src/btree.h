#ifndef KHB_BTREE_H
#define KHB_BTREE_H

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
    uint32_t      next_leaf;
} btree_leaf_header_t;

typedef struct {
    page_header_t hdr;
    uint16_t      count;
    uint16_t      capacity;
    uint32_t      reserved;
} btree_internal_header_t;

#pragma pack(pop)

_Static_assert(sizeof(btree_leaf_header_t) == 20,
               "leaf header must be 20 bytes");
_Static_assert(sizeof(btree_internal_header_t) == 20,
               "internal header must be 20 bytes");

#define KHB_BTREE_INTERNAL_CAP \
    ((KHB_PAGE_SIZE - sizeof(btree_internal_header_t) - 4) / 12)

#define KHB_BTREE_KEYS_OFF  (sizeof(btree_internal_header_t))
#define KHB_BTREE_CHILD_OFF (KHB_BTREE_KEYS_OFF + 8 * KHB_BTREE_INTERNAL_CAP)

_Static_assert(KHB_BTREE_CHILD_OFF + 4 * (KHB_BTREE_INTERNAL_CAP + 1)
               <= KHB_PAGE_SIZE, "internal node must fit in a page");

_Static_assert(4 * (8 + KHB_MAX_ROW_SIZE)
               <= KHB_PAGE_SIZE - sizeof(btree_leaf_header_t),
               "KHB_MAX_ROW_SIZE too large for a 4-entry leaf");

typedef struct {
    pager_t           *pager;
    journal_t         *journal;
    const table_def_t *def;
    uint32_t           root;
} btree_t;

typedef struct {
    uint32_t page_id;
    uint16_t index;
    uint32_t loaded;
    uint8_t  buf[KHB_PAGE_SIZE];
} btree_cursor_t;

uint16_t btree_leaf_capacity_for(uint16_t row_size);

khb_status btree_create(pager_t *p, journal_t *j, const table_def_t *def,
                        uint32_t *out_root);
void       btree_open(btree_t *t, pager_t *p, journal_t *j,
                      const table_def_t *def, uint32_t root);
uint32_t   btree_root(const btree_t *t);

khb_status btree_insert(btree_t *t, const uint8_t *row);
khb_status btree_lookup(btree_t *t, int64_t key, uint8_t *out_row);
khb_status btree_delete(btree_t *t, int64_t key);

khb_status btree_cursor_first(btree_t *t, btree_cursor_t *c);
khb_status btree_cursor_seek(btree_t *t, int64_t key, btree_cursor_t *c);
khb_status btree_cursor_next(btree_t *t, btree_cursor_t *c, uint8_t *out_row);

khb_status btree_check(btree_t *t);
khb_status btree_count(btree_t *t, uint32_t *out);

#endif /* KHB_BTREE_H */
