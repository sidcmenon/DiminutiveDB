#ifndef KHB_ROW_H
#define KHB_ROW_H

#include <stdint.h>
#include <stddef.h>

#include "khabibdb.h"
#include "catalog.h"

typedef struct {
    const table_def_t *def;
    uint8_t           *buf;
} row_t;

void row_bind(row_t *r, const table_def_t *def, uint8_t *buf);
void row_clear(row_t *r);

khb_status row_set_int(row_t *r, int col, int64_t v);
khb_status row_get_int(const row_t *r, int col, int64_t *out);

khb_status row_set_uint(row_t *r, int col, uint64_t v);
khb_status row_get_uint(const row_t *r, int col, uint64_t *out);

khb_status row_set_double(row_t *r, int col, double v);
khb_status row_get_double(const row_t *r, int col, double *out);

khb_status row_set_bool(row_t *r, int col, int v);
khb_status row_get_bool(const row_t *r, int col, int *out);

khb_status row_set_text(row_t *r, int col, const char *s);
khb_status row_get_text(const row_t *r, int col, char *out, size_t cap);

khb_status row_set_blob(row_t *r, int col, const void *data, size_t len);
khb_status row_get_blob(const row_t *r, int col, void *out, size_t cap);

khb_status row_key(const row_t *r, int64_t *out);

int     row_column_count(const row_t *r);
uint8_t row_column_type(const row_t *r, int col);

#endif /* KHB_ROW_H */