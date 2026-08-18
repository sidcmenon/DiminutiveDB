#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "txn.h"
#include "btree.h"

static int problems;

static void problem(const char *msg)
{
    printf("  PROBLEM: %s\n", msg);
    problems++;
}

static void sweep_pages(khb_db *db)
{
    uint8_t  page[KHB_PAGE_SIZE];
    uint32_t n = pager_page_count(&db->pager);
    uint32_t i, ok = 0;
    char     buf[128];

    printf("pages: %u\n", n);

    for (i = 0; i < n; i++) {
        if (pager_read(&db->pager, i, page) != KHB_OK) {
            snprintf(buf, sizeof buf, "page %u failed verification", i);
            problem(buf);
        } else {
            ok++;
        }
    }
    printf("  %u/%u verified\n", ok, n);
}

static void walk_freelist(khb_db *db)
{
    uint8_t  page[KHB_PAGE_SIZE];
    uint32_t id    = pager_freelist_head(&db->pager);
    uint32_t count = 0;
    uint32_t limit = pager_page_count(&db->pager) + 1;
    char     buf[128];

    while (id != 0) {
        if (pager_read(&db->pager, id, page) != KHB_OK) {
            snprintf(buf, sizeof buf, "free page %u unreadable", id);
            problem(buf);
            return;
        }
        if (page_type_of(page) != PAGE_TYPE_FREE) {
            snprintf(buf, sizeof buf, "page %u on free list is not free", id);
            problem(buf);
            return;
        }

        id = ((const free_page_t *)page)->next_free;

        if (++count > limit) {
            problem("free list has a cycle");
            return;
        }
    }
    printf("free list: %u pages, no cycle\n", count);
}

static void check_tables(khb_db *db)
{
    char buf[160];
    int  i, n;

    n = catalog_table_count(&db->catalog);
    printf("tables: %d\n", n);

    for (i = 0; i < n; i++) {
        table_def_t *t = catalog_table_at(&db->catalog, i);
        btree_t      bt;
        uint32_t     rows = 0;
        khb_status   rc;

        if (t == NULL)
            continue;

        rc = txn_open_tree(db, t, &bt);
        if (rc != KHB_OK) {
            snprintf(buf, sizeof buf, "table %s: cannot open tree (%s)",
                     t->name, khb_strerror(rc));
            problem(buf);
            continue;
        }

        rc = btree_check(&bt);
        if (rc != KHB_OK) {
            snprintf(buf, sizeof buf, "table %s: tree invariants violated (%s)",
                     t->name, khb_strerror(rc));
            problem(buf);
            continue;
        }

        if (btree_count(&bt, &rows) != KHB_OK) {
            snprintf(buf, sizeof buf, "table %s: cannot count rows", t->name);
            problem(buf);
            continue;
        }

        printf("  %-16s cols=%-3u row=%-5u root=%-6u rows=%u  OK\n",
               t->name, t->column_count, t->row_size, t->root_page, rows);
    }
}

int main(int argc, char **argv)
{
    khb_db     db;
    khb_status rc;
    char       buf[160];

    if (argc != 2) {
        fprintf(stderr, "usage: khbcheck <database>\n");
        return 2;
    }

    rc = db_open(&db, argv[1], 0);
    if (rc != KHB_OK) {
        fprintf(stderr, "open %s: %s\n", argv[1], khb_strerror(rc));
        return 1;
    }

    printf("=== %s ===\n", argv[1]);

    /*
     * Structural checks first, and without a transaction. They need only the
     * pager, so a damaged catalog cannot prevent them from running — which is
     * exactly the case where this tool is most useful.
     */
    sweep_pages(&db);
    walk_freelist(&db);

    rc = txn_begin(&db, 1);
    if (rc != KHB_OK) {
        snprintf(buf, sizeof buf, "cannot read catalog (%s)",
                 khb_strerror(rc));
        problem(buf);
    } else {
        check_tables(&db);
        txn_commit(&db);
    }

    db_close(&db);

    if (problems > 0) {
        printf("\n%d problem(s) found\n", problems);
        return 1;
    }
    printf("\nno problems found\n");
    return 0;
}
