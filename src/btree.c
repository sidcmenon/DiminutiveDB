#include "btree.h"

#include <stdlib.h>
#include <string.h>

static uint16_t leaf_entry_size(const btree_t *t)
{
    return (uint16_t)(8 + t->def->row_size);
}

static uint8_t *leaf_entry(uint8_t *page, uint16_t i, uint16_t esize)
{
    return page + sizeof(btree_leaf_header_t) + (size_t)i * esize;
}

static int64_t leaf_key_at(const uint8_t *page, uint16_t i, uint16_t esize)
{
    int64_t k;

    memcpy(&k, page + sizeof(btree_leaf_header_t) + (size_t)i * esize,
           sizeof k);
    return k;
}

static int leaf_search(const uint8_t *page, uint16_t count, uint16_t esize,
                       int64_t key, uint16_t *out_pos)
{
    uint16_t lo = 0, hi = count;

    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        int64_t  k   = leaf_key_at(page, mid, esize);

        if (k == key) {
            *out_pos = mid;
            return 1;
        }
        if (k < key)
            lo = (uint16_t)(mid + 1);
        else
            hi = mid;
    }

    *out_pos = lo;
    return 0;
}

static int64_t int_key(const uint8_t *page, uint16_t i)
{
    int64_t k;
    memcpy(&k, page + KHB_BTREE_KEYS_OFF + (size_t)i * 8, sizeof k);
    return k;
}

static void int_set_key(uint8_t *page, uint16_t i, int64_t k)
{
    memcpy(page + KHB_BTREE_KEYS_OFF + (size_t)i * 8, &k, sizeof k);
}

static uint32_t int_child(const uint8_t *page, uint16_t i)
{
    uint32_t c;
    memcpy(&c, page + KHB_BTREE_CHILD_OFF + (size_t)i * 4, sizeof c);
    return c;
}

static void int_set_child(uint8_t *page, uint16_t i, uint32_t c)
{
    memcpy(page + KHB_BTREE_CHILD_OFF + (size_t)i * 4, &c, sizeof c);
}

static uint16_t internal_child_index(const uint8_t *page, uint16_t count,
                                     int64_t key)
{
    uint16_t lo = 0, hi = count;

    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);

        if (int_key(page, mid) <= key)
            lo = (uint16_t)(mid + 1);
        else
            hi = mid;
    }
    return lo;
}

uint16_t btree_leaf_capacity_for(uint16_t row_size)
{
    uint16_t esize;

    if (row_size == 0)
        return 0;

    esize = (uint16_t)(8 + row_size);
    return (uint16_t)((KHB_PAGE_SIZE - sizeof(btree_leaf_header_t)) / esize);
}

static khb_status new_leaf(btree_t *t, uint32_t next_leaf, uint32_t *out_id)
{
    uint8_t              page[KHB_PAGE_SIZE];
    btree_leaf_header_t *lh;
    uint32_t             id;
    khb_status           rc;

    rc = pager_alloc(t->pager, &id);
    if (rc != KHB_OK)
        return rc;

    page_init(page, id, PAGE_TYPE_BTREE_LEAF);
    lh            = (btree_leaf_header_t *)page;
    lh->count     = 0;
    lh->capacity  = btree_leaf_capacity_for(t->def->row_size);
    lh->next_leaf = next_leaf;

    rc = pager_write(t->pager, id, page);
    if (rc != KHB_OK)
        return rc;

    *out_id = id;
    return KHB_OK;
}

khb_status btree_create(pager_t *p, journal_t *j, const table_def_t *def,
                        uint32_t *out_root)
{
    btree_t t;

    if (p == NULL || j == NULL || def == NULL || out_root == NULL)
        return KHB_ERR_INVALID;
    if (!journal_is_active(j))
        return KHB_ERR_STATE;
    if (btree_leaf_capacity_for(def->row_size) < 4)
        return KHB_ERR_FULL;

    btree_open(&t, p, j, def, 0);
    return new_leaf(&t, 0, out_root);
}

void btree_open(btree_t *t, pager_t *p, journal_t *j, const table_def_t *def,
                uint32_t root)
{
    t->pager   = p;
    t->journal = j;
    t->def     = def;
    t->root    = root;
}

uint32_t btree_root(const btree_t *t)
{
    return t->root;
}

static khb_status descend_to_leaf(btree_t *t, int64_t key, uint32_t *out_leaf,
                                  uint8_t *page)
{
    uint32_t   id = t->root;
    khb_status rc;
    int        guard = 0;

    for (;;) {
        rc = pager_read(t->pager, id, page);
        if (rc != KHB_OK)
            return rc;

        if (page_type_of(page) == PAGE_TYPE_BTREE_LEAF) {
            *out_leaf = id;
            return KHB_OK;
        }
        if (page_type_of(page) != PAGE_TYPE_BTREE_INTERNAL)
            return KHB_ERR_CORRUPT;

        {
            const btree_internal_header_t *ih =
                (const btree_internal_header_t *)page;
            id = int_child(page, internal_child_index(page, ih->count, key));
        }

        if (++guard > 64)
            return KHB_ERR_CORRUPT;
    }
}

static khb_status leftmost_leaf(btree_t *t, uint32_t *out_leaf, uint8_t *page)
{
    uint32_t   id = t->root;
    khb_status rc;
    int        guard = 0;

    for (;;) {
        rc = pager_read(t->pager, id, page);
        if (rc != KHB_OK)
            return rc;

        if (page_type_of(page) == PAGE_TYPE_BTREE_LEAF) {
            *out_leaf = id;
            return KHB_OK;
        }
        if (page_type_of(page) != PAGE_TYPE_BTREE_INTERNAL)
            return KHB_ERR_CORRUPT;

        id = int_child(page, 0);
        if (++guard > 64)
            return KHB_ERR_CORRUPT;
    }
}

khb_status btree_lookup(btree_t *t, int64_t key, uint8_t *out_row)
{
    uint8_t                    page[KHB_PAGE_SIZE];
    const btree_leaf_header_t *lh;
    uint32_t                   leaf_id;
    uint16_t                   pos, esize;
    khb_status                 rc;

    if (t == NULL || out_row == NULL)
        return KHB_ERR_INVALID;

    rc = descend_to_leaf(t, key, &leaf_id, page);
    if (rc != KHB_OK)
        return rc;

    (void)leaf_id;
    lh    = (const btree_leaf_header_t *)page;
    esize = leaf_entry_size(t);

    if (!leaf_search(page, lh->count, esize, key, &pos))
        return KHB_ERR_NOTFOUND;

    memcpy(out_row, leaf_entry(page, pos, esize) + 8, t->def->row_size);
    return KHB_OK;
}

static khb_status split_leaf(btree_t *t, uint32_t page_id, uint8_t *page,
                             int64_t key, const uint8_t *row,
                             uint16_t insert_at, int64_t *sep_key,
                             uint32_t *right_id)
{
    uint8_t              newent[8 + KHB_MAX_ROW_SIZE];
    uint8_t              rpage[KHB_PAGE_SIZE];
    btree_leaf_header_t *lh = (btree_leaf_header_t *)page;
    btree_leaf_header_t *rh;
    uint16_t             esize   = leaf_entry_size(t);
    uint16_t             n       = lh->count;
    uint16_t             total   = (uint16_t)(n + 1);
    uint16_t             left_n  = (uint16_t)(total / 2);
    uint16_t             right_n = (uint16_t)(total - left_n);
    uint32_t             rid;
    uint16_t             i;
    khb_status           rc;

    memcpy(newent, &key, 8);
    memcpy(newent + 8, row, t->def->row_size);

    rc = new_leaf(t, lh->next_leaf, &rid);
    if (rc != KHB_OK)
        return rc;

    rc = pager_read(t->pager, rid, rpage);
    if (rc != KHB_OK)
        return rc;
    rh = (btree_leaf_header_t *)rpage;

    for (i = left_n; i < total; i++) {
        const uint8_t *src =
            (i < insert_at)  ? leaf_entry(page, i, esize)
          : (i == insert_at) ? newent
                             : leaf_entry(page, (uint16_t)(i - 1), esize);
        memcpy(leaf_entry(rpage, (uint16_t)(i - left_n), esize), src, esize);
    }
    rh->count = right_n;

    for (i = left_n; i > 0; i--) {
        uint16_t       d   = (uint16_t)(i - 1);
        uint8_t       *dst = leaf_entry(page, d, esize);
        const uint8_t *src =
            (d < insert_at)  ? leaf_entry(page, d, esize)
          : (d == insert_at) ? newent
                             : leaf_entry(page, (uint16_t)(d - 1), esize);
        if (src != dst)
            memmove(dst, src, esize);
    }
    lh->count     = left_n;
    lh->next_leaf = rid;

    rc = pager_write(t->pager, rid, rpage);
    if (rc != KHB_OK)
        return rc;
    rc = pager_write(t->pager, page_id, page);
    if (rc != KHB_OK)
        return rc;

    *sep_key  = leaf_key_at(rpage, 0, esize);
    *right_id = rid;
    return KHB_OK;
}

static khb_status split_internal(btree_t *t, uint32_t page_id, uint8_t *page,
                                 int64_t key, uint32_t child,
                                 uint16_t insert_at, int64_t *sep_key,
                                 uint32_t *right_id)
{
    int64_t                  keys[KHB_BTREE_INTERNAL_CAP + 1];
    uint32_t                 kids[KHB_BTREE_INTERNAL_CAP + 2];
    uint8_t                  rpage[KHB_PAGE_SIZE];
    btree_internal_header_t *ih = (btree_internal_header_t *)page;
    btree_internal_header_t *rh;
    uint16_t                 n     = ih->count;
    uint16_t                 total = (uint16_t)(n + 1);
    uint16_t                 mid, left_n, right_n, i;
    uint32_t                 rid;
    khb_status               rc;

    for (i = 0; i < insert_at; i++) {
        keys[i] = int_key(page, i);
        kids[i] = int_child(page, i);
    }
    keys[insert_at]     = key;
    kids[insert_at]     = int_child(page, insert_at);
    kids[insert_at + 1] = child;
    for (i = insert_at; i < n; i++) {
        keys[i + 1] = int_key(page, i);
        kids[i + 2] = int_child(page, (uint16_t)(i + 1));
    }

    mid     = (uint16_t)(total / 2);
    left_n  = mid;
    right_n = (uint16_t)(total - mid - 1);

    rc = pager_alloc(t->pager, &rid);
    if (rc != KHB_OK)
        return rc;

    page_init(rpage, rid, PAGE_TYPE_BTREE_INTERNAL);
    rh           = (btree_internal_header_t *)rpage;
    rh->count    = right_n;
    rh->capacity = (uint16_t)KHB_BTREE_INTERNAL_CAP;
    rh->reserved = 0;

    for (i = 0; i < right_n; i++)
        int_set_key(rpage, i, keys[mid + 1 + i]);
    for (i = 0; i <= right_n; i++)
        int_set_child(rpage, i, kids[mid + 1 + i]);

    ih->count = left_n;
    for (i = 0; i < left_n; i++)
        int_set_key(page, i, keys[i]);
    for (i = 0; i <= left_n; i++)
        int_set_child(page, i, kids[i]);

    rc = pager_write(t->pager, rid, rpage);
    if (rc != KHB_OK)
        return rc;
    rc = pager_write(t->pager, page_id, page);
    if (rc != KHB_OK)
        return rc;

    *sep_key  = keys[mid];
    *right_id = rid;
    return KHB_OK;
}

static khb_status insert_into(btree_t *t, uint32_t page_id, int64_t key,
                              const uint8_t *row, int *did_split,
                              int64_t *sep_key, uint32_t *right_id)
{
    uint8_t    page[KHB_PAGE_SIZE];
    khb_status rc;

    *did_split = 0;

    rc = pager_read(t->pager, page_id, page);
    if (rc != KHB_OK)
        return rc;

    if (page_type_of(page) == PAGE_TYPE_BTREE_LEAF) {
        btree_leaf_header_t *lh    = (btree_leaf_header_t *)page;
        uint16_t             esize = leaf_entry_size(t);
        uint16_t             pos;

        if (leaf_search(page, lh->count, esize, key, &pos))
            return KHB_ERR_EXISTS;

        rc = journal_note_page(t->journal, t->pager, page_id);
        if (rc != KHB_OK)
            return rc;

        if (lh->count < lh->capacity) {
            if (pos < lh->count)
                memmove(leaf_entry(page, (uint16_t)(pos + 1), esize),
                        leaf_entry(page, pos, esize),
                        (size_t)(lh->count - pos) * esize);

            memcpy(leaf_entry(page, pos, esize), &key, 8);
            memcpy(leaf_entry(page, pos, esize) + 8, row, t->def->row_size);
            lh->count++;

            return pager_write(t->pager, page_id, page);
        }

        *did_split = 1;
        return split_leaf(t, page_id, page, key, row, pos, sep_key, right_id);
    }

    if (page_type_of(page) != PAGE_TYPE_BTREE_INTERNAL)
        return KHB_ERR_CORRUPT;

    {
        btree_internal_header_t *ih = (btree_internal_header_t *)page;
        uint16_t                 idx =
            internal_child_index(page, ih->count, key);
        int      child_split = 0;
        int64_t  ckey        = 0;
        uint32_t cright      = 0;

        rc = insert_into(t, int_child(page, idx), key, row,
                         &child_split, &ckey, &cright);
        if (rc != KHB_OK)
            return rc;
        if (!child_split)
            return KHB_OK;

        rc = journal_note_page(t->journal, t->pager, page_id);
        if (rc != KHB_OK)
            return rc;

        if (ih->count < ih->capacity) {
            uint16_t i;

            for (i = ih->count; i > idx; i--)
                int_set_key(page, i, int_key(page, (uint16_t)(i - 1)));
            for (i = (uint16_t)(ih->count + 1); i > idx + 1; i--)
                int_set_child(page, i, int_child(page, (uint16_t)(i - 1)));

            int_set_key(page, idx, ckey);
            int_set_child(page, (uint16_t)(idx + 1), cright);
            ih->count++;

            return pager_write(t->pager, page_id, page);
        }

        *did_split = 1;
        return split_internal(t, page_id, page, ckey, cright, idx,
                              sep_key, right_id);
    }
}

khb_status btree_insert(btree_t *t, const uint8_t *row)
{
    int64_t    key;
    int        did_split = 0;
    int64_t    sep_key   = 0;
    uint32_t   right_id  = 0;
    khb_status rc;

    if (t == NULL || row == NULL)
        return KHB_ERR_INVALID;
    if (t->journal == NULL || !journal_is_active(t->journal))
        return KHB_ERR_STATE;

    memcpy(&key, row, sizeof key);

    rc = insert_into(t, t->root, key, row, &did_split, &sep_key, &right_id);
    if (rc != KHB_OK)
        return rc;
    if (!did_split)
        return KHB_OK;

    {
        uint8_t                  page[KHB_PAGE_SIZE];
        btree_internal_header_t *ih;
        uint32_t                 new_root;

        rc = pager_alloc(t->pager, &new_root);
        if (rc != KHB_OK)
            return rc;

        page_init(page, new_root, PAGE_TYPE_BTREE_INTERNAL);
        ih           = (btree_internal_header_t *)page;
        ih->count    = 1;
        ih->capacity = (uint16_t)KHB_BTREE_INTERNAL_CAP;
        ih->reserved = 0;

        int_set_key(page, 0, sep_key);
        int_set_child(page, 0, t->root);
        int_set_child(page, 1, right_id);

        rc = pager_write(t->pager, new_root, page);
        if (rc != KHB_OK)
            return rc;

        t->root = new_root;
        return KHB_OK;
    }
}

khb_status btree_delete(btree_t *t, int64_t key)
{
    uint8_t              page[KHB_PAGE_SIZE];
    btree_leaf_header_t *lh;
    uint32_t             leaf_id;
    uint16_t             pos, esize;
    khb_status           rc;

    if (t == NULL)
        return KHB_ERR_INVALID;
    if (t->journal == NULL || !journal_is_active(t->journal))
        return KHB_ERR_STATE;

    rc = descend_to_leaf(t, key, &leaf_id, page);
    if (rc != KHB_OK)
        return rc;

    lh    = (btree_leaf_header_t *)page;
    esize = leaf_entry_size(t);

    if (!leaf_search(page, lh->count, esize, key, &pos))
        return KHB_ERR_NOTFOUND;

    rc = journal_note_page(t->journal, t->pager, leaf_id);
    if (rc != KHB_OK)
        return rc;

    if (pos + 1 < lh->count)
        memmove(leaf_entry(page, pos, esize),
                leaf_entry(page, (uint16_t)(pos + 1), esize),
                (size_t)(lh->count - pos - 1) * esize);

    lh->count--;
    memset(leaf_entry(page, lh->count, esize), 0, esize);

    return pager_write(t->pager, leaf_id, page);
}

khb_status btree_cursor_first(btree_t *t, btree_cursor_t *c)
{
    khb_status rc;

    if (t == NULL || c == NULL)
        return KHB_ERR_INVALID;

    rc = leftmost_leaf(t, &c->page_id, c->buf);
    if (rc != KHB_OK)
        return rc;

    c->index  = 0;
    c->loaded = c->page_id;
    return KHB_OK;
}

khb_status btree_cursor_seek(btree_t *t, int64_t key, btree_cursor_t *c)
{
    const btree_leaf_header_t *lh;
    khb_status                 rc;

    if (t == NULL || c == NULL)
        return KHB_ERR_INVALID;

    rc = descend_to_leaf(t, key, &c->page_id, c->buf);
    if (rc != KHB_OK)
        return rc;

    lh = (const btree_leaf_header_t *)c->buf;
    (void)leaf_search(c->buf, lh->count, leaf_entry_size(t), key, &c->index);
    c->loaded = c->page_id;
    return KHB_OK;
}

khb_status btree_cursor_next(btree_t *t, btree_cursor_t *c, uint8_t *out_row)
{
    uint16_t   esize;
    khb_status rc;

    if (t == NULL || c == NULL || out_row == NULL)
        return KHB_ERR_INVALID;

    esize = leaf_entry_size(t);

    for (;;) {
        const btree_leaf_header_t *lh;

        if (c->page_id == 0)
            return KHB_ERR_NOTFOUND;

        if (c->loaded != c->page_id) {
            rc = pager_read(t->pager, c->page_id, c->buf);
            if (rc != KHB_OK)
                return rc;
            if (page_type_of(c->buf) != PAGE_TYPE_BTREE_LEAF)
                return KHB_ERR_CORRUPT;
            c->loaded = c->page_id;
        }

        lh = (const btree_leaf_header_t *)c->buf;

        if (c->index < lh->count) {
            memcpy(out_row, leaf_entry(c->buf, c->index, esize) + 8,
                   t->def->row_size);
            c->index++;
            return KHB_OK;
        }

        c->page_id = lh->next_leaf;
        c->index   = 0;
    }
}

khb_status btree_count(btree_t *t, uint32_t *out)
{
    uint8_t    page[KHB_PAGE_SIZE];
    uint32_t   id, total = 0;
    khb_status rc;

    if (t == NULL || out == NULL)
        return KHB_ERR_INVALID;

    rc = leftmost_leaf(t, &id, page);
    if (rc != KHB_OK)
        return rc;

    while (id != 0) {
        const btree_leaf_header_t *lh;

        rc = pager_read(t->pager, id, page);
        if (rc != KHB_OK)
            return rc;

        lh      = (const btree_leaf_header_t *)page;
        total  += lh->count;
        id      = lh->next_leaf;
    }

    *out = total;
    return KHB_OK;
}

static khb_status check_node(btree_t *t, uint32_t page_id, const int64_t *lo,
                             const int64_t *hi, int depth, int *leaf_depth,
                             uint8_t *seen)
{
    uint8_t    page[KHB_PAGE_SIZE];
    khb_status rc;

    if (page_id == 0)
        return KHB_ERR_CORRUPT;

    if (seen[page_id / 8] & (uint8_t)(1u << (page_id % 8)))
        return KHB_ERR_CORRUPT;
    seen[page_id / 8] |= (uint8_t)(1u << (page_id % 8));

    rc = pager_read(t->pager, page_id, page);
    if (rc != KHB_OK)
        return rc;

    if (page_type_of(page) == PAGE_TYPE_BTREE_LEAF) {
        const btree_leaf_header_t *lh    = (const btree_leaf_header_t *)page;
        uint16_t                   esize = leaf_entry_size(t);
        uint16_t                   i;

        if (*leaf_depth < 0)
            *leaf_depth = depth;
        else if (*leaf_depth != depth)
            return KHB_ERR_CORRUPT;

        if (lh->count > lh->capacity)
            return KHB_ERR_CORRUPT;

        for (i = 0; i < lh->count; i++) {
            int64_t k = leaf_key_at(page, i, esize);

            if (i > 0 && k <= leaf_key_at(page, (uint16_t)(i - 1), esize))
                return KHB_ERR_CORRUPT;
            if (lo != NULL && k < *lo)
                return KHB_ERR_CORRUPT;
            if (hi != NULL && k >= *hi)
                return KHB_ERR_CORRUPT;
        }
        return KHB_OK;
    }

    if (page_type_of(page) != PAGE_TYPE_BTREE_INTERNAL)
        return KHB_ERR_CORRUPT;

    {
        const btree_internal_header_t *ih =
            (const btree_internal_header_t *)page;
        uint16_t i;

        if (ih->count == 0 || ih->count > ih->capacity)
            return KHB_ERR_CORRUPT;

        for (i = 0; i < ih->count; i++) {
            int64_t k = int_key(page, i);

            if (i > 0 && k <= int_key(page, (uint16_t)(i - 1)))
                return KHB_ERR_CORRUPT;
            if (lo != NULL && k < *lo)
                return KHB_ERR_CORRUPT;
            if (hi != NULL && k >= *hi)
                return KHB_ERR_CORRUPT;
        }

        for (i = 0; i <= ih->count; i++) {
            int64_t        clo_v = 0, chi_v = 0;
            const int64_t *clo = lo;
            const int64_t *chi = hi;

            if (i > 0) {
                clo_v = int_key(page, (uint16_t)(i - 1));
                clo   = &clo_v;
            }
            if (i < ih->count) {
                chi_v = int_key(page, i);
                chi   = &chi_v;
            }

            rc = check_node(t, int_child(page, i), clo, chi, depth + 1,
                            leaf_depth, seen);
            if (rc != KHB_OK)
                return rc;
        }
        return KHB_OK;
    }
}

khb_status btree_check(btree_t *t)
{
    uint8_t   *seen;
    uint32_t   npages;
    int        leaf_depth = -1;
    khb_status rc;

    if (t == NULL)
        return KHB_ERR_INVALID;

    npages = pager_page_count(t->pager);
    seen   = calloc((size_t)(npages / 8 + 1), 1);
    if (seen == NULL)
        return KHB_ERR_NOMEM;

    rc = check_node(t, t->root, NULL, NULL, 0, &leaf_depth, seen);
    free(seen);

    if (rc != KHB_OK)
        return rc;

    {
        uint8_t  page[KHB_PAGE_SIZE];
        uint32_t id;
        uint16_t esize     = leaf_entry_size(t);
        int64_t  prev      = 0;
        int      have_prev = 0;
        int      guard     = 0;

        rc = leftmost_leaf(t, &id, page);
        if (rc != KHB_OK)
            return rc;

        while (id != 0) {
            const btree_leaf_header_t *lh;
            uint16_t                   i;

            rc = pager_read(t->pager, id, page);
            if (rc != KHB_OK)
                return rc;
            if (page_type_of(page) != PAGE_TYPE_BTREE_LEAF)
                return KHB_ERR_CORRUPT;

            lh = (const btree_leaf_header_t *)page;

            for (i = 0; i < lh->count; i++) {
                int64_t k = leaf_key_at(page, i, esize);

                if (have_prev && k <= prev)
                    return KHB_ERR_CORRUPT;
                prev      = k;
                have_prev = 1;
            }

            id = lh->next_leaf;
            if (++guard > 1000000)
                return KHB_ERR_CORRUPT;
        }
    }

    return KHB_OK;
}
