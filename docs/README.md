# DiminutiveDB

This document uses ASD-STE100 Simplified Technical English.

## 1. Description

DiminutiveDB is a small database. It is a library for programs in the C
language. You put the library in your program. The database does not need a
server. The database does not need other software.

The database keeps all data in one file. The file contains tables. Each table
has columns. Each column has a name and a type. You give the columns when you
make the table. You cannot change the columns later.

The database has no query language. Your program calls C functions to read data
and to write data.

## 2. Functions

- The database keeps all data in one file.
- The database writes a journal file before it changes a page. The journal
  keeps the data safe if the program stops.
- Each table has one B-tree index. The key of the index is the first column.
- Only one program can write at one time. Many programs can read at one time.
- The library uses only the C standard library and the operating system.
- The database examines a checksum each time it reads a page.

## 3. Limits

- The first column is the key. The type of the first column must be INT64.
- A row must have 1000 bytes or less.
- A table must have 12 columns or less.
- The database must have 8 tables or less.
- A name must have 31 characters or less.
- A table has no second index.
- The database does not make the file smaller after you delete rows.
- The format of the data is little-endian. You cannot use the file on a
  big-endian machine.

## 4. Build the library

Do this command in the top directory:

```bash
make
```

The command makes the library `build/libkhabibdb.a`.

## 5. Do the tests

Do this command to build the tests and to run them:

```bash
make test
```

There are 100 tests. All tests must pass.

Do this command to test the recovery of the database:

```bash
make crash
```

The command stops the program at 32 different points. After each stop, the
command opens the database again. The data must be correct after each stop.
The database is correct if all the changes are present, or if no changes are
present. A mixture of the two is an error.

## 6. Use the database

This example makes a table. Then it writes one row. Then it reads the row.

```c
#include "khabibdb.h"

static const khb_column schema[] = {
    { "id",   KHB_INT64, 0 },
    { "name", KHB_TEXT, 16 }
};

khb_db  *db;
khb_row  row;

khb_open(&db, "example.db", 1);
khb_create_table(db, "people", schema, 2);

khb_row_init(&row, db, "people");
khb_row_set_int(&row, 0, 1);
khb_row_set_text(&row, 1, "amara");
khb_insert(db, "people", &row);

khb_get(db, "people", 1, &row);

khb_close(db);
```

Each function gives a status. The status is `KHB_OK` if the function is
correct. Examine the status after each call. The function `khb_strerror` gives
the name of a status.

The file `examples/basic.c` shows more functions. It shows how to read many
rows. It also shows how to use a filter.

## 7. Transactions

A transaction is a group of changes. The database writes all the changes, or it
writes none of them.

If you do not start a transaction, each function makes its own transaction. This
is safe, but it is slow. Each transaction writes to the disk.

To be quick, start one transaction for many changes:

```c
khb_begin(db, 0);
/* many calls to khb_insert */
khb_commit(db);
```

A speed test measured 50 rows each second with one transaction for each row. The
same test measured 123571 rows each second with one transaction for 10000 rows.

## 8. Examine a database file

Do this command to build the test tool:

```bash
make tools
```

Then do this command:

```bash
build/khbcheck <file>
```

The tool examines the checksum of each page. It examines the list of free
pages. It examines the index of each table. The tool gives a report. The tool
gives a status of 1 if it finds a problem.

## 9. Directories

| Directory | Contents |
|---|---|
| `include/` | The public header file. |
| `src/` | The source of the library. |
| `test/` | The tests. |
| `test/crash/` | The test of the recovery. |
| `tools/` | The tool that examines a file. |
| `examples/` | An example program. |
| `bench/` | The speed tests. |
| `docs/` | This document and the design documents. |
| `build/` | The output of the build. |

## 10. More information

The file `bench/RESULTS.md` gives the speed tests and their results.
