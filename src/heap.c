#include "heap.h"

#include <string.h>


static int64_t key_of(const uint8_t *row)
{
    int64_t k;

    memcpy(&k, row, sizeof k);
    return k;
}

static uint8_t *slot_at(uint8_t *page, uint16_t index, uint16_t row_size)
{
    return page + sizeof(heap_page_header_t) + (size_t)index * row_size;
}

static khb_status find_key(heap_t *h, int64_t key, uint32_t *out_page,
                           uint16_t *out_index, uint8_t *out_page_buf)
{
    uint32_t   page_id = h->root;
    khb_status rc;

    while (page_id != 0) {
        const heap_page_header_t *hh;
        uint16_t                  i;

        rc = pager_read(h->pager, page_id, out_page_buf);
        if (rc != KHB_OK)
            return rc;

        hh = (const heap_page_header_t *)out_page_buf;
        if (page_type_of(out_page_buf) != PAGE_TYPE_HEAP)
            return KHB_ERR_CORRUPT;

        for (i = 0; i < hh->count; i++) {
            const uint8_t *row = slot_at(out_page_buf, i, h->def->row_size);

            if (key_of(row) == key) {
                *out_page  = page_id;
                *out_index = i;
                return KHB_OK;
            }
        }

        page_id = hh->next_page;
    }

    return KHB_ERR_NOTFOUND;
}


uint16_t heap_capacity_for(uint16_t row_size)
{
    if (row_size == 0)
        return 0;
    return (uint16_t)((KHB_PAGE_SIZE - sizeof(heap_page_header_t)) / row_size);
}

khb_status heap_create(pager_t *p, journal_t *j, const table_def_t *def,
                       uint32_t *out_root)
{
    uint8_t             page[KHB_PAGE_SIZE];
    heap_page_header_t *hh;
    uint32_t            id;
    khb_status          rc;

    if (p == NULL || j == NULL || def == NULL || out_root == NULL)
        return KHB_ERR_INVALID;
    if (!journal_is_active(j))
        return KHB_ERR_STATE;
    if (heap_capacity_for(def->row_size) == 0)
        return KHB_ERR_FULL;

    rc = pager_alloc(p, &id);
    if (rc != KHB_OK)
        return rc;

    page_init(page, id, PAGE_TYPE_HEAP);
    hh            = (heap_page_header_t *)page;
    hh->count     = 0;
    hh->capacity  = heap_capacity_for(def->row_size);
    hh->next_page = 0;

    rc = pager_write(p, id, page);
    if (rc != KHB_OK)
        return rc;

    *out_root = id;
    return KHB_OK;
}

void heap_open(heap_t *h, pager_t *p, journal_t *j, const table_def_t *def,
               uint32_t root)
{
    h->pager   = p;
    h->journal = j;
    h->def     = def;
    h->root    = root;
}

khb_status heap_insert(heap_t *h, const uint8_t *row)
{
    uint8_t             page[KHB_PAGE_SIZE];
    uint8_t             probe[KHB_PAGE_SIZE];
    heap_page_header_t *hh;
    uint32_t            page_id, last_id, dummy_page;
    uint16_t            dummy_index;
    khb_status          rc;

    if (h == NULL || row == NULL)
        return KHB_ERR_INVALID;
    if (h->journal == NULL || !journal_is_active(h->journal))
        return KHB_ERR_STATE;

    rc = find_key(h, key_of(row), &dummy_page, &dummy_index, probe);
    if (rc == KHB_OK)
        return KHB_ERR_EXISTS;
    if (rc != KHB_ERR_NOTFOUND)
        return rc;

    page_id = h->root;
    last_id = h->root;

    while (page_id != 0) {
        rc = pager_read(h->pager, page_id, page);
        if (rc != KHB_OK)
            return rc;

        hh = (heap_page_header_t *)page;

        if (hh->count < hh->capacity) {
            rc = journal_note_page(h->journal, h->pager, page_id);
            if (rc != KHB_OK)
                return rc;

            memcpy(slot_at(page, hh->count, h->def->row_size), row,
                   h->def->row_size);
            hh->count++;

            return pager_write(h->pager, page_id, page);
        }

        last_id = page_id;
        page_id = hh->next_page;
    }

    {
        uint32_t new_id;
        uint8_t  newpage[KHB_PAGE_SIZE];

        rc = pager_alloc(h->pager, &new_id);
        if (rc != KHB_OK)
            return rc;

        page_init(newpage, new_id, PAGE_TYPE_HEAP);
        hh            = (heap_page_header_t *)newpage;
        hh->count     = 1;
        hh->capacity  = heap_capacity_for(h->def->row_size);
        hh->next_page = 0;
        memcpy(slot_at(newpage, 0, h->def->row_size), row, h->def->row_size);

        rc = pager_write(h->pager, new_id, newpage);
        if (rc != KHB_OK)
            return rc;

        rc = journal_note_page(h->journal, h->pager, last_id);
        if (rc != KHB_OK)
            return rc;

        rc = pager_read(h->pager, last_id, page);
        if (rc != KHB_OK)
            return rc;

        hh            = (heap_page_header_t *)page;
        hh->next_page = new_id;

        return pager_write(h->pager, last_id, page);
    }
}

khb_status heap_get(heap_t *h, int64_t key, uint8_t *out_row)
{
    uint8_t    page[KHB_PAGE_SIZE];
    uint32_t   page_id;
    uint16_t   index;
    khb_status rc;

    if (h == NULL || out_row == NULL)
        return KHB_ERR_INVALID;

    rc = find_key(h, key, &page_id, &index, page);
    if (rc != KHB_OK)
        return rc;

    memcpy(out_row, slot_at(page, index, h->def->row_size),
           h->def->row_size);
    return KHB_OK;
}

khb_status heap_delete(heap_t *h, int64_t key)
{
    uint8_t             page[KHB_PAGE_SIZE];
    heap_page_header_t *hh;
    uint32_t            page_id;
    uint16_t            index;
    khb_status          rc;

    if (h == NULL)
        return KHB_ERR_INVALID;
    if (h->journal == NULL || !journal_is_active(h->journal))
        return KHB_ERR_STATE;

    rc = find_key(h, key, &page_id, &index, page);
    if (rc != KHB_OK)
        return rc;

    rc = journal_note_page(h->journal, h->pager, page_id);
    if (rc != KHB_OK)
        return rc;

    rc = pager_read(h->pager, page_id, page);
    if (rc != KHB_OK)
        return rc;

    hh = (heap_page_header_t *)page;

    if (index != (uint16_t)(hh->count - 1))
        memcpy(slot_at(page, index, h->def->row_size),
               slot_at(page, (uint16_t)(hh->count - 1), h->def->row_size),
               h->def->row_size);

    hh->count--;

    memset(slot_at(page, hh->count, h->def->row_size), 0, h->def->row_size);

    return pager_write(h->pager, page_id, page);
}

khb_status heap_scan(heap_t *h, heap_visit_fn fn, void *ctx)
{
    uint8_t    page[KHB_PAGE_SIZE];
    uint32_t   page_id;
    khb_status rc;

    if (h == NULL || fn == NULL)
        return KHB_ERR_INVALID;

    page_id = h->root;

    while (page_id != 0) {
        const heap_page_header_t *hh;
        uint16_t                  i;

        rc = pager_read(h->pager, page_id, page);
        if (rc != KHB_OK)
            return rc;
        if (page_type_of(page) != PAGE_TYPE_HEAP)
            return KHB_ERR_CORRUPT;

        hh = (const heap_page_header_t *)page;

        for (i = 0; i < hh->count; i++) {
            rc = fn(slot_at(page, i, h->def->row_size), ctx);
            if (rc != KHB_OK)
                return rc;      /* visitor asked to stop */
        }

        page_id = hh->next_page;
    }

    return KHB_OK;
}

khb_status heap_count(heap_t *h, uint32_t *out)
{
    uint8_t    page[KHB_PAGE_SIZE];
    uint32_t   page_id, total = 0;
    khb_status rc;

    if (h == NULL || out == NULL)
        return KHB_ERR_INVALID;

    page_id = h->root;

    while (page_id != 0) {
        const heap_page_header_t *hh;

        rc = pager_read(h->pager, page_id, page);
        if (rc != KHB_OK)
            return rc;

        hh     = (const heap_page_header_t *)page;
        total += hh->count;
        page_id = hh->next_page;
    }

    *out = total;
    return KHB_OK;
}