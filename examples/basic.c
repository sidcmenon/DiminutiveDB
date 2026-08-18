/*
 * Worked example. Compiled against include/ only — it must never need a
 * header from src/.
 */
#include <stdio.h>
#include <string.h>

#include "khabibdb.h"

struct report {
    int    matched;
    double total;
};

static int older_than_30(const khb_row *row, void *ctx)
{
    int64_t age = 0;

    (void)ctx;
    khb_row_get_int(row, 2, &age);
    return age > 30;
}

static khb_status print_row(const khb_row *row, void *ctx)
{
    struct report *rep = ctx;
    int64_t        id = 0, age = 0;
    double         score = 0.0;
    char           name[KHB_NAME_MAX];

    khb_row_get_int(row, 0, &id);
    khb_row_get_text(row, 1, name, sizeof name);
    khb_row_get_int(row, 2, &age);
    khb_row_get_double(row, 3, &score);

    printf("  id=%-3lld name=%-8s age=%-3lld score=%.1f\n",
           (long long)id, name, (long long)age, score);

    rep->matched++;
    rep->total += score;
    return KHB_OK;
}

static int fail(const char *what, khb_status rc)
{
    fprintf(stderr, "%s: %s\n", what, khb_strerror(rc));
    return 1;
}

int main(void)
{
    static const khb_column schema[] = {
        { "id",    KHB_INT64,  0 },
        { "name",  KHB_TEXT,  16 },
        { "age",   KHB_INT64,  0 },
        { "score", KHB_DOUBLE, 0 }
    };
    static const struct {
        int64_t     id;
        const char *name;
        int64_t     age;
        double      score;
    } people[] = {
        { 1, "amara",  34, 88.5 },
        { 2, "bo",     27, 91.0 },
        { 3, "chen",   45, 76.25 },
        { 4, "dara",   31, 64.0 },
        { 5, "esther", 22, 95.5 }
    };

    khb_db       *db = NULL;
    khb_row       row;
    khb_status    rc;
    struct report rep = { 0, 0.0 };
    uint32_t      n = 0;
    size_t        i;

    remove("example.db");
    remove("example.db-journal");

    rc = khb_open(&db, "example.db", 1);
    if (rc != KHB_OK)
        return fail("open", rc);

    rc = khb_create_table(db, "people", schema, 4);
    if (rc != KHB_OK)
        return fail("create_table", rc);

    /* One explicit transaction around all the inserts. Without it each
     * insert would auto-wrap and pay its own fsync. */
    rc = khb_begin(db, 0);
    if (rc != KHB_OK)
        return fail("begin", rc);

    for (i = 0; i < sizeof people / sizeof people[0]; i++) {
        rc = khb_row_init(&row, db, "people");
        if (rc != KHB_OK)
            return fail("row_init", rc);

        khb_row_set_int(&row, 0, people[i].id);
        khb_row_set_text(&row, 1, people[i].name);
        khb_row_set_int(&row, 2, people[i].age);
        khb_row_set_double(&row, 3, people[i].score);

        rc = khb_insert(db, "people", &row);
        if (rc != KHB_OK)
            return fail("insert", rc);
    }

    rc = khb_commit(db);
    if (rc != KHB_OK)
        return fail("commit", rc);

    rc = khb_count(db, "people", &n);
    if (rc != KHB_OK)
        return fail("count", rc);
    printf("inserted %u rows\n", n);

    printf("get(3):\n");
    rc = khb_get(db, "people", 3, &row);
    if (rc != KHB_OK)
        return fail("get", rc);
    print_row(&row, &rep);

    printf("scan where age > 30:\n");
    rep.matched = 0;
    rep.total   = 0.0;
    rc = khb_scan(db, "people", older_than_30, print_row, &rep);
    if (rc != KHB_OK)
        return fail("scan", rc);
    printf("  %d matched, mean score %.2f\n",
           rep.matched, rep.total / rep.matched);

    printf("scan_range keys 2..4:\n");
    rep.matched = 0;
    rep.total   = 0.0;
    rc = khb_scan_range(db, "people", 2, 4, NULL, print_row, &rep);
    if (rc != KHB_OK)
        return fail("scan_range", rc);

    rc = khb_delete(db, "people", 3);
    if (rc != KHB_OK)
        return fail("delete", rc);
    rc = khb_get(db, "people", 3, &row);
    printf("after delete, get(3) -> %s\n", khb_strerror(rc));

    rc = khb_close(db);
    if (rc != KHB_OK)
        return fail("close", rc);

    /* Reopen to prove it all persisted. */
    rc = khb_open(&db, "example.db", 0);
    if (rc != KHB_OK)
        return fail("reopen", rc);
    rc = khb_count(db, "people", &n);
    if (rc != KHB_OK)
        return fail("recount", rc);
    printf("after reopen: %u rows\n", n);
    khb_close(db);

    remove("example.db");
    return 0;
}
