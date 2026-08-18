#include "row.h"

#include <string.h>

static khb_status resolve(const row_t *r, int col, uint8_t expect,
                          const column_def_t **out)
{
    const column_def_t *c;

    if (r == NULL || r->def == NULL || r->buf == NULL)
        return KHB_ERR_INVALID;
    if (col < 0 || col >= (int)r->def->column_count)
        return KHB_ERR_INVALID;

    c = &r->def->columns[col];
    if (c->type != expect)
        return KHB_ERR_INVALID;

    *out = c;
    return KHB_OK;
}

void row_bind(row_t *r, const table_def_t *def, uint8_t *buf)
{
    r->def = def;
    r->buf = buf;
}

void row_clear(row_t *r)
{
    if (r != NULL && r->def != NULL && r->buf != NULL)
        memset(r->buf, 0, r->def->row_size);
}

khb_status row_set_int(row_t *r, int col, int64_t v)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_INT64, &c);

    if (rc != KHB_OK)
        return rc;

    memcpy(r->buf + c->offset, &v, sizeof v);
    return KHB_OK;
}

khb_status row_get_int(const row_t *r, int col, int64_t *out)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_INT64, &c);

    if (rc != KHB_OK)
        return rc;
    if (out == NULL)
        return KHB_ERR_INVALID;

    memcpy(out, r->buf + c->offset, sizeof *out);
    return KHB_OK;
}

khb_status row_set_uint(row_t *r, int col, uint64_t v)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_UINT64, &c);

    if (rc != KHB_OK)
        return rc;

    memcpy(r->buf + c->offset, &v, sizeof v);
    return KHB_OK;
}

khb_status row_get_uint(const row_t *r, int col, uint64_t *out)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_UINT64, &c);

    if (rc != KHB_OK)
        return rc;
    if (out == NULL)
        return KHB_ERR_INVALID;

    memcpy(out, r->buf + c->offset, sizeof *out);
    return KHB_OK;
}

khb_status row_set_double(row_t *r, int col, double v)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_DOUBLE, &c);

    if (rc != KHB_OK)
        return rc;

    memcpy(r->buf + c->offset, &v, sizeof v);
    return KHB_OK;
}

khb_status row_get_double(const row_t *r, int col, double *out)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_DOUBLE, &c);

    if (rc != KHB_OK)
        return rc;
    if (out == NULL)
        return KHB_ERR_INVALID;

    memcpy(out, r->buf + c->offset, sizeof *out);
    return KHB_OK;
}

khb_status row_set_bool(row_t *r, int col, int v)
{
    const column_def_t *c;
    uint8_t             b;
    khb_status          rc = resolve(r, col, COL_BOOL, &c);

    if (rc != KHB_OK)
        return rc;

    b = v ? 1u : 0u;
    r->buf[c->offset] = b;
    return KHB_OK;
}

khb_status row_get_bool(const row_t *r, int col, int *out)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_BOOL, &c);

    if (rc != KHB_OK)
        return rc;
    if (out == NULL)
        return KHB_ERR_INVALID;

    *out = (r->buf[c->offset] != 0);
    return KHB_OK;
}

khb_status row_set_text(row_t *r, int col, const char *s)
{
    const column_def_t *c;
    size_t              n;
    khb_status          rc = resolve(r, col, COL_TEXT, &c);

    if (rc != KHB_OK)
        return rc;
    if (s == NULL)
        return KHB_ERR_INVALID;

    n = strlen(s);
    if (n > c->size)
        return KHB_ERR_FULL;

    memset(r->buf + c->offset, 0, c->size);
    memcpy(r->buf + c->offset, s, n);
    return KHB_OK;
}

khb_status row_get_text(const row_t *r, int col, char *out, size_t cap)
{
    const column_def_t *c;
    size_t              n;
    khb_status          rc = resolve(r, col, COL_TEXT, &c);

    if (rc != KHB_OK)
        return rc;
    if (out == NULL || cap == 0)
        return KHB_ERR_INVALID;

    n = 0;
    while (n < c->size && r->buf[c->offset + n] != '\0')
        n++;

    if (n + 1 > cap)
        return KHB_ERR_FULL;

    memcpy(out, r->buf + c->offset, n);
    out[n] = '\0';
    return KHB_OK;
}

khb_status row_set_blob(row_t *r, int col, const void *data, size_t len)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_BLOB, &c);

    if (rc != KHB_OK)
        return rc;
    if (data == NULL && len > 0)
        return KHB_ERR_INVALID;
    if (len > c->size)
        return KHB_ERR_FULL;

    memset(r->buf + c->offset, 0, c->size);
    if (len > 0)
        memcpy(r->buf + c->offset, data, len);
    return KHB_OK;
}

khb_status row_get_blob(const row_t *r, int col, void *out, size_t cap)
{
    const column_def_t *c;
    khb_status          rc = resolve(r, col, COL_BLOB, &c);

    if (rc != KHB_OK)
        return rc;
    if (out == NULL)
        return KHB_ERR_INVALID;
    if (cap < c->size)
        return KHB_ERR_FULL;

    memcpy(out, r->buf + c->offset, c->size);
    return KHB_OK;
}

khb_status row_key(const row_t *r, int64_t *out)
{
    return row_get_int(r, 0, out);
}

int row_column_count(const row_t *r)
{
    if (r == NULL || r->def == NULL)
        return 0;
    return (int)r->def->column_count;
}

uint8_t row_column_type(const row_t *r, int col)
{
    if (r == NULL || r->def == NULL ||
        col < 0 || col >= (int)r->def->column_count)
        return 0xFF;
    return r->def->columns[col].type;
}