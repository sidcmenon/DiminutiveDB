#include "journal.h"

#include "crashpoint.h"
#include "crc32.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stats.h"

static khb_status jrnl_append_record_raw(journal_t *j, uint32_t page_id,
                                         const uint8_t *image);

static khb_status jrnl_append_record(journal_t *j, uint32_t page_id,
                                     const uint8_t *image)
{
    khb_status rc = jrnl_append_record_raw(j, page_id, image);

    if (rc == KHB_OK)
        khb_stat.journal_records++;
    return rc;
}

static int noted_find(const journal_t *j, uint32_t id, size_t *out_pos){
    size_t lo = 0;
    size_t hi = j->noted_len;

    while (lo<hi){
        size_t mid = lo + (hi - lo)/2;

        if (j->noted[mid]==id){
            *out_pos = mid;
            return 1;
        }

        if (j->noted[mid]<id){
            lo = mid + 1;
        }
        else{
            hi = mid;
        }
    }

    *out_pos = lo;
    return 0;
}

static khb_status noted_insert(journal_t *j, uint32_t id, size_t pos){
    if (j->noted_len == j->noted_cap){
        size_t ncap = (j->noted_cap == 0)? 64: j->noted_cap *2;
        uint32_t *n = realloc(j->noted, ncap * sizeof *n);
        if (n == NULL){
            return KHB_ERR_NOMEM;
        }
        j->noted = n;
        j->noted_cap = ncap;
    }
    if (pos < j->noted_len){
        memmove(&j->noted[pos + 1], &j->noted[pos],
                (j->noted_len - pos) * sizeof *j->noted);
        }
    j->noted[pos] = id;
    j->noted_len++;
    return KHB_OK;
}

static khb_status jrnl_pread(int fd, void *buf, size_t n, off_t off)
{
    uint8_t *p = (uint8_t *)buf;

    while (n > 0) {
        ssize_t r = pread(fd, p, n, off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return KHB_ERR_IO;
        }
        if (r == 0)
            return KHB_ERR_IO;
        p   += r;
        off += r;
        n   -= (size_t)r;
    }
    return KHB_OK;
}

static khb_status jrnl_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (n > 0) {
        ssize_t w = pwrite(fd, p, n, off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return KHB_ERR_IO;
        }
        if (w == 0)
            return KHB_ERR_IO;
        p   += w;
        off += w;
        n   -= (size_t)w;
    }
    return KHB_OK;
}

static khb_status jrnl_write_header(int fd, uint32_t orig_page_count){
    journal_header_t jh;
    memset(&jh, 0, sizeof jh);
    memcpy(jh.magic, KHB_JOURNAL_MAGIC, KHB_JOURNAL_MAGIC_LEN);
    jh.page_size = KHB_PAGE_SIZE;
    jh.orig_page_count = orig_page_count;
    jh.reserved = 0;
    jh.checksum = khb_crc32(&jh, sizeof jh - sizeof jh.checksum);

    return jrnl_pwrite(fd, &jh, sizeof jh, 0);
}

static khb_status jrnl_read_header(int fd, journal_header_t *out)
{
    journal_header_t jh;
    struct stat      st;
    uint32_t         want;

    if (fstat(fd, &st) != 0)
        return KHB_ERR_IO;
    if (st.st_size < (off_t)sizeof jh)
        return KHB_ERR_CORRUPT;

    if (jrnl_pread(fd, &jh, sizeof jh, 0) != KHB_OK)
        return KHB_ERR_IO;

    if (memcmp(jh.magic, KHB_JOURNAL_MAGIC, KHB_JOURNAL_MAGIC_LEN) != 0)
        return KHB_ERR_CORRUPT;
    if (jh.page_size != KHB_PAGE_SIZE)
        return KHB_ERR_CORRUPT;

    want = khb_crc32(&jh, sizeof jh - sizeof jh.checksum);
    if (jh.checksum != want)
        return KHB_ERR_CORRUPT;

    *out = jh;
    return KHB_OK;
}

static khb_status jrnl_append_record_raw(journal_t *j, uint32_t page_id, const uint8_t *image){
    uint8_t rec[KHB_JOURNAL_RECORD_SIZE];
    struct stat st;

    if (fstat(j->fd, &st) != 0){
        return KHB_ERR_IO;
    }

    memcpy(rec, &page_id, sizeof page_id);
    memcpy(rec +sizeof page_id, image, KHB_PAGE_SIZE);

    return jrnl_pwrite(j->fd, rec, sizeof rec, st.st_size);
}

static khb_status jrnl_discard(pager_t *p, const char *jpath)
{
    if (unlink(jpath) != 0 && errno != ENOENT){
        return KHB_ERR_IO;
    }

    return pager_sync_dir(p);
}

static khb_status jrnl_replay(int fd, pager_t *p){
    journal_header_t jh;
    struct stat st;
    uint8_t rec[KHB_JOURNAL_RECORD_SIZE];
    off_t payload;
    uint32_t count, i;
    khb_status rc;

    rc = jrnl_read_header(fd, &jh);
    if (rc!= KHB_OK){
        return rc;
    }

    if (fstat(fd, &st)!=0){
        return KHB_ERR_IO;
    }

    payload = st.st_size - (off_t)sizeof jh;
    if (payload < 0)
        payload = 0;
    count = (uint32_t)(payload / KHB_JOURNAL_RECORD_SIZE);

    for (i = count; i>0; i--){
        off_t off = (off_t)sizeof jh + (off_t)(i-1) * KHB_JOURNAL_RECORD_SIZE;
        uint32_t page_id;
        uint8_t *image;
        rc = jrnl_pread(fd, rec, sizeof rec, off);
        if (rc != KHB_OK){
            return rc;
        }
        memcpy(&page_id, rec, sizeof page_id);
        image = rec + sizeof page_id;
        if (page_verify(image, page_id)!=KHB_OK){
            if (i==count){
                continue;
            }
            return KHB_ERR_CORRUPT;
        }
        rc = pager_write(p, page_id, image);
        if (rc != KHB_OK){
            return rc;
        }
    }

    rc = pager_truncate(p, jh.orig_page_count);
    if (rc != KHB_OK){
        return rc;
    }

    rc = pager_fsync_fd(p, p->fd);
    if (rc != KHB_OK){
        return rc;
    }

    return pager_reload_header(p);
}


void journal_init(journal_t *j){
    memset(j, 0, sizeof *j);
    j->fd = -1;
}

void journal_free(journal_t *j){
    if (j==NULL){
        return;
    }

    if (j->fd >= 0){
        close(j->fd);
        j-> fd = -1;
    }

    free(j->noted);
    j->noted = NULL;
    j->noted_len = 0;
    j->noted_cap = 0;
    j->active = 0;
}

void journal_path_for(const char *db_path, char *out, size_t cap){
    snprintf(out, cap, "%s-journal", db_path);
}

int journal_is_active(const journal_t *j)
{
    return (j != NULL) && j->active;
}

khb_status journal_begin(journal_t *j, pager_t *p){
    khb_status rc;
    if (j==NULL || p==NULL){
        return KHB_ERR_INVALID;
    }
    if (j->active){
        return KHB_ERR_STATE;
    }

    rc = pager_sync(p);
    if(rc != KHB_OK){
        return rc;
    }

    j->orig_page_count = pager_page_count(p);
    j->noted_len = 0;
    journal_path_for(p->path, j->path, sizeof j-> path);

    j->fd = open(j->path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (j->fd < 0){
        return KHB_ERR_IO;
    }

    rc = jrnl_write_header(j->fd, j->orig_page_count);
    if (rc != KHB_OK){
        goto fail;
    }

    rc = pager_fsync_fd(p, j->fd);
    if (rc != KHB_OK){
        goto fail;
    }

    rc = pager_sync_dir(p);
    if (rc != KHB_OK){
        goto fail;
    }
    j->active = 1;
    rc = journal_note_page(j, p, 0);
    if (rc != KHB_OK) {
        j->active = 0;
        goto fail;
    }

    return KHB_OK;

fail:
    close(j->fd);
    j->fd = -1;
    unlink(j->path);
    return rc;
}

khb_status journal_note_page(journal_t *j, pager_t *p, uint32_t page_id){
    uint8_t image[KHB_PAGE_SIZE];
    size_t pos;
    khb_status rc;

    if (j == NULL||p == NULL){
        return KHB_ERR_INVALID;
    }

    if (!j->active){
        return KHB_ERR_STATE;
    }

    if (page_id>= j->orig_page_count){
        return KHB_OK;
    }

    if (noted_find(j, page_id, &pos)){
        return KHB_OK;
    }

    rc = pager_read(p, page_id, image);
    if (rc != KHB_OK){
        return rc;
    }

    rc = jrnl_append_record(j, page_id, image);
    if (rc != KHB_OK){
        return rc;
    }

    khb_crashpoint();

    rc = pager_fsync_fd(p, j->fd);
    if (rc != KHB_OK){
        return rc;
    }

    khb_crashpoint();

    return noted_insert(j, page_id, pos);
}

khb_status journal_commit(journal_t *j, pager_t *p){
    khb_status rc;
    if (j == NULL || p == NULL)
        return KHB_ERR_INVALID;
    if (!j->active)
        return KHB_ERR_STATE;
    
    rc = pager_sync(p);
    if (rc != KHB_OK){
        return rc;
    }

    khb_crashpoint();
    close(j->fd);
    j->fd = -1;

    rc = jrnl_discard(p, j->path);
    if (rc != KHB_OK){
        return rc;
    }

    khb_crashpoint();

    j->active = 0;
    j->noted_len = 0;
    return KHB_OK;
}

khb_status journal_rollback(journal_t *j, pager_t *p){
    khb_status rc;
    if (j == NULL || p == NULL)
        return KHB_ERR_INVALID;
    if (!j->active)
        return KHB_ERR_STATE;
    rc = jrnl_replay(j->fd, p);
    close(j->fd);
    j->fd = -1;

    if (rc == KHB_OK){
        rc = jrnl_discard(p, j->path);
    }
    else{
        (void)jrnl_discard(p, j->path);
    }

    j->active = 0;
    j->noted_len = 0;
    return rc;
}

khb_status journal_recover(pager_t *p){
    char jpath[PATH_MAX];
    journal_header_t jh;
    int fd;
    khb_status rc;

    if (p==NULL){
        return KHB_ERR_INVALID;
    }
    journal_path_for(p->path, jpath, sizeof jpath);
    fd = open(jpath, O_RDWR);
    if (fd<0){
        if (errno == ENOENT){
            return KHB_OK;
        }
        return KHB_ERR_IO;
    }
    rc = jrnl_read_header(fd, &jh);
    if (rc == KHB_ERR_CORRUPT) {
        close(fd);
        return jrnl_discard(p, jpath);
    }
    if (rc != KHB_OK) {
        close(fd);
        return rc;
    }

    rc = jrnl_replay(fd, p);
    close(fd);
    if (rc != KHB_OK)
        return rc;

    return jrnl_discard(p, jpath);
}