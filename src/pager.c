#include "pager.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "stats.h"

static khb_status full_pread_raw(int fd, void *buf, size_t n, off_t off);
static khb_status full_pwrite_raw(int fd, const void *buf, size_t n, off_t off);
static khb_status pager_fsync_fd_raw(const pager_t *p, int fd);

static khb_status full_pread(int fd, void *buf, size_t n, off_t off)
{
    uint64_t   w0 = khb_now_ns(), c0 = khb_cpu_ns();
    khb_status rc = full_pread_raw(fd, buf, n, off);

    khb_stat.reads++;
    khb_stat_span(&khb_stat.read_ns, &khb_stat.read_wait_ns, w0, c0);
    return rc;
}

static khb_status full_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    uint64_t   w0 = khb_now_ns(), c0 = khb_cpu_ns();
    khb_status rc = full_pwrite_raw(fd, buf, n, off);

    khb_stat.writes++;
    khb_stat_span(&khb_stat.write_ns, &khb_stat.write_wait_ns, w0, c0);
    return rc;
}

khb_status pager_fsync_fd(const pager_t *p, int fd)
{
    uint64_t   w0 = khb_now_ns(), c0 = khb_cpu_ns();
    khb_status rc = pager_fsync_fd_raw(p, fd);

    khb_stat.fsyncs++;
    khb_stat_span(&khb_stat.fsync_ns, &khb_stat.fsync_wait_ns, w0, c0);
    return rc;
}

static off_t page_offset(uint32_t page_id){
    return (off_t)page_id * (off_t)KHB_PAGE_SIZE;
}

static khb_status full_pread_raw(int fd, void *buf, size_t n, off_t off){
    uint8_t *p = (uint8_t *)buf;
    while (n>0){
        ssize_t r = pread(fd, p, n, off);
        if (r<0){
            if (errno == EINTR){
                continue;
            }
            return KHB_ERR_IO;
        }
        if (r==0){
            return KHB_ERR_IO;
        }
        p += r;
        off += r;
        n -= (size_t)r;
    }
    return KHB_OK;
}

static khb_status full_pwrite_raw(int fd, const void *buf, size_t n, off_t off){
    const uint8_t *p = (const uint8_t *)buf;
    while (n>0){
        ssize_t w = pwrite(fd, p, n, off);
        if (w<0){
            if (errno == EINTR){
                continue;
            }
            return KHB_ERR_IO;
        }
        if (w==0){
            return KHB_ERR_IO;
        }
        p += w;
        off += w;
        n -= (size_t)w;
    }
    return KHB_OK;
}

static khb_status pager_fsync_fd_raw(const pager_t *p, int fd)
{
    if (p == NULL || fd < 0)
        return KHB_ERR_INVALID;

#ifdef __APPLE__
    if (p->use_full_fsync) {
        if (fcntl(fd, F_FULLFSYNC, 0) == 0)
            return KHB_OK;
        /* Some filesystems don't implement it; anything else is a real error. */
        if (errno != ENOTSUP && errno != EINVAL && errno != EOPNOTSUPP)
            return KHB_ERR_IO;
    }
#endif
    return (fsync(fd) == 0) ? KHB_OK : KHB_ERR_IO;
}

static void dir_of(const char *path, char *out, size_t cap){
    const char *slash = strrchr(path, '/');
    size_t      n;

    if (slash == NULL) {
        snprintf(out, cap, ".");
        return;
    }
    if (slash == path) {
        snprintf(out, cap, "/");
        return;
    }

    n = (size_t)(slash - path);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}


khb_status pager_open(pager_t *p, const char *path, int create){
    uint8_t buf[KHB_PAGE_SIZE];
    struct stat st;
    khb_status rc;
    int flags = O_RDWR;

    if (p == NULL || path == NULL || strlen(path) >= PATH_MAX){
        return KHB_ERR_INVALID;
    }

    memset(p, 0, sizeof *p);
    p->fd = -1;
    snprintf(p->path, sizeof p->path, "%s", path);
    p->use_full_fsync = (getenv("KHB_NO_FULLYSYNC") == NULL);

    if (create){
        flags |= O_CREAT;
    }

    p->fd = open(path, flags, 0644);
    if (p->fd<0){
        return KHB_ERR_IO;
    }

    if (fstat(p->fd, &st)!=0){
        rc = KHB_ERR_IO;
        goto fail;
    }

    if (st.st_size ==0){
        if (!create){
            rc = KHB_ERR_CORRUPT;
            goto fail;
        }

        file_header_init(buf);
        page_finalize(buf);

        rc = full_pwrite(p->fd, buf, KHB_PAGE_SIZE, 0);
        if (rc != KHB_OK){
            goto fail;
        }

        rc = pager_fsync_fd(p, p->fd);
        if (rc != KHB_OK){
            goto fail;
        }

        p->page_count = 1;
        p->freelist_head = 0;
        p->catalog_root = 0;
        
        rc = pager_sync_dir(p);
        if (rc != KHB_OK){
            goto fail;
        }
    } else {
        const file_header_t *fh;

        if (st.st_size % KHB_PAGE_SIZE!=0){
            rc = KHB_ERR_CORRUPT;
            goto fail;
        }
        rc = full_pread(p->fd, buf, KHB_PAGE_SIZE,0);
        if (rc != KHB_OK){
            goto fail;
        }

        rc = full_pread(p->fd, buf, KHB_PAGE_SIZE, 0);
        if (rc != KHB_OK){
            goto fail;
        }
        rc = page_verify(buf, 0);
        if (rc != KHB_OK){
            goto fail;
        }

        rc = file_header_validate(buf);
        if (rc != KHB_OK){
            goto fail;
        }

        fh = (const file_header_t *)buf;

        if (fh->page_count == 0 || (off_t)fh-> page_count * KHB_PAGE_SIZE > st.st_size){
            rc = KHB_ERR_CORRUPT;
            goto fail;
        }

        p->page_count = fh->page_count;
        p->freelist_head = fh->freelist_head;
        p->catalog_root = fh->catalog_root;
    }
    p->header_dirty = 0;
    return KHB_OK;

fail:
    close(p->fd);
    p->fd = -1;
    return rc;
}

khb_status pager_close(pager_t *p){
    khb_status rc;
    khb_status rc_close;
    if (p == NULL){
        return KHB_ERR_INVALID;
    }
    if (p->fd <0){
        return KHB_OK;
    }

    rc = pager_sync(p);
    rc_close = (close(p->fd)==0)? KHB_OK : KHB_ERR_IO;
    p->fd = -1;

    return (rc != KHB_OK) ? rc : rc_close;
}

khb_status pager_read(pager_t *p, uint32_t page_id, void *buf){
    khb_status rc;

    if (p==NULL || buf == NULL){
        return KHB_ERR_INVALID;
    }
    if (p->fd <0){
        return KHB_ERR_STATE;
    }

    if (page_id >= p-> page_count){
        return KHB_ERR_INVALID;
    }

    rc = full_pread(p->fd, buf, KHB_PAGE_SIZE, page_offset(page_id));
    if (rc!=KHB_OK){
        return rc;
    }
    return page_verify(buf, page_id);
}

khb_status pager_write(pager_t *p, uint32_t page_id, void *buf){
    const page_header_t *h;
    if (p == NULL || buf == NULL){
        return KHB_ERR_INVALID;}
    if (p->fd < 0){
        return KHB_ERR_STATE;}
    if (page_id >= p->page_count){
        return KHB_ERR_INVALID;}
    h = (const page_header_t *)buf;
    if (h->page_id != page_id){
        return KHB_ERR_INVALID;
    }
    page_finalize(buf);
    return full_pwrite(p->fd, buf, KHB_PAGE_SIZE, page_offset(page_id));
}

khb_status pager_alloc(pager_t *p, uint32_t *out_page_id)
{
    uint8_t    buf[KHB_PAGE_SIZE];
    khb_status rc;
    uint32_t   id;

    if (p == NULL || out_page_id == NULL)
        return KHB_ERR_INVALID;
    if (p->fd < 0)
        return KHB_ERR_STATE;

    if (p->freelist_head != 0) {
        id = p->freelist_head;

        rc = pager_read(p, id, buf);
        if (rc != KHB_OK)
            return rc;
        if (page_type_of(buf) != PAGE_TYPE_FREE)
            return KHB_ERR_CORRUPT;

        p->freelist_head = ((const free_page_t *)buf)->next_free;
        p->header_dirty  = 1;
        *out_page_id     = id;
        return KHB_OK;
    }

    if (p->page_count == UINT32_MAX)
        return KHB_ERR_FULL;

    id = p->page_count;

    page_init(buf, id, PAGE_TYPE_FREE);
    page_finalize(buf);
    rc = full_pwrite(p->fd, buf, KHB_PAGE_SIZE, page_offset(id));
    if (rc != KHB_OK)
        return rc;

    p->page_count++;
    p->header_dirty = 1;
    *out_page_id    = id;
    return KHB_OK;
}

khb_status pager_free(pager_t *p, uint32_t page_id)
{
    uint8_t      buf[KHB_PAGE_SIZE];
    free_page_t *fp;
    khb_status   rc;

    if (p == NULL)
        return KHB_ERR_INVALID;
    if (p->fd < 0)
        return KHB_ERR_STATE;
    if (page_id == 0)
        return KHB_ERR_INVALID;
    if (page_id >= p->page_count)
        return KHB_ERR_INVALID;

    page_init(buf, page_id, PAGE_TYPE_FREE);
    fp = (free_page_t *)buf;
    fp->next_free = p->freelist_head;
    page_finalize(buf);

    rc = full_pwrite(p->fd, buf, KHB_PAGE_SIZE, page_offset(page_id));
    if (rc != KHB_OK)
        return rc;

    p->freelist_head = page_id;
    p->header_dirty  = 1;
    return KHB_OK;
}

khb_status pager_flush_header(pager_t *p)
{
    uint8_t        buf[KHB_PAGE_SIZE];
    file_header_t *fh;
    khb_status     rc;

    if (p == NULL)
        return KHB_ERR_INVALID;
    if (p->fd < 0)
        return KHB_ERR_STATE;
    if (!p->header_dirty)
        return KHB_OK;

    file_header_init(buf);
    fh = (file_header_t *)buf;
    fh->page_count    = p->page_count;
    fh->freelist_head = p->freelist_head;
    fh->catalog_root  = p->catalog_root;
    page_finalize(buf);

    rc = full_pwrite(p->fd, buf, KHB_PAGE_SIZE, 0);
    if (rc != KHB_OK)
        return rc;

    p->header_dirty = 0;
    return KHB_OK;
}

khb_status pager_sync(pager_t *p)
{
    khb_status rc;

    if (p == NULL)
        return KHB_ERR_INVALID;
    if (p->fd < 0)
        return KHB_ERR_STATE;

    rc = pager_flush_header(p);
    if (rc != KHB_OK)
        return rc;

    return pager_fsync_fd(p, p->fd);
}

khb_status pager_sync_dir(pager_t *p)
{
    char       dir[PATH_MAX];
    int        dfd;
    khb_status rc;

    if (p == NULL)
        return KHB_ERR_INVALID;

    dir_of(p->path, dir, sizeof dir);

    dfd = open(dir, O_RDONLY);
    if (dfd < 0)
        return KHB_ERR_IO;

    rc = (fsync(dfd) == 0) ? KHB_OK : KHB_ERR_IO;
    close(dfd);
    return rc;
}

khb_status pager_truncate(pager_t *p, uint32_t page_count)
{
    if (p == NULL)
        return KHB_ERR_INVALID;
    if (p->fd < 0)
        return KHB_ERR_STATE;
    if (page_count == 0)
        return KHB_ERR_INVALID;
    if (page_count > p->page_count)
        return KHB_ERR_INVALID;

    if (ftruncate(p->fd, (off_t)page_count * KHB_PAGE_SIZE) != 0)
        return KHB_ERR_IO;

    p->page_count   = page_count;
    p->header_dirty = 1;
    return KHB_OK;
}
uint32_t pager_page_count(const pager_t *p){ 
    return p->page_count;    
}
uint32_t pager_freelist_head(const pager_t *p){
    return p->freelist_head; 
}
uint32_t pager_catalog_root(const pager_t *p){
    return p->catalog_root;
}

void pager_set_catalog_root(pager_t *p, uint32_t page_id)
{
    p->catalog_root = page_id;
    p->header_dirty = 1;
}

khb_status pager_reload_header(pager_t *p){
    uint8_t buf[KHB_PAGE_SIZE];
    const file_header_t *fh;
    khb_status rc;

    if (p == NULL){
        return KHB_ERR_INVALID;
    }

    if (p->fd<0){
        return KHB_ERR_STATE;
    }

    rc = full_pread(p->fd, buf, KHB_PAGE_SIZE, 0);
    if (rc!=KHB_OK){
        return rc;
    }
    rc = file_header_validate(buf);
    if (rc != KHB_OK){
        return rc;
    }

    fh = (const file_header_t *)buf;
    p->page_count = fh->page_count;
    p->freelist_head = fh->freelist_head;
    p->catalog_root = fh->catalog_root;

    p->header_dirty = 0;
    return KHB_OK;
}