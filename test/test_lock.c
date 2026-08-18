#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_util.h"
#include "lockmgr.h"


static int open_file(const char *path)
{
    return open(path, O_RDWR | O_CREAT, 0644);
}

static void child_hold_lock(const char *path, lock_state want, int report_fd)
{
    int    fd;
    lock_t l;
    char   msg;

    fd = open(path, O_RDWR);
    if (fd < 0)
        _exit(2);

    lock_init(&l, fd);
    msg = (lock_acquire(&l, want) == KHB_OK) ? 'R' : 'F';

    if (write(report_fd, &msg, 1) != 1)
        _exit(3);

    
    alarm(30);

    for (;;)
        pause();
}

static int spawn_holder(const char *path, lock_state want, pid_t *out_pid)
{
    int   pipefd[2];
    pid_t pid;
    char  msg;

    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        child_hold_lock(path, want, pipefd[1]);
        _exit(0);                       
    }

    close(pipefd[1]);
    if (read(pipefd[0], &msg, 1) != 1 || msg != 'R') {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    close(pipefd[0]);

    *out_pid = pid;
    return 0;
}

static void test_shared_shared_ok(void)
{
    char   path[PATH_MAX];
    int    fd1, fd2;
    lock_t a, b;

    khb_temp_path(path, sizeof path, "shsh");
    fd1 = open_file(path);
    fd2 = open_file(path);
    CHECK(fd1 >= 0);
    CHECK(fd2 >= 0);

    lock_init(&a, fd1);
    lock_init(&b, fd2);

    CHECK_STATUS(lock_acquire(&a, LOCK_SHARED), KHB_OK);
    CHECK_STATUS(lock_acquire(&b, LOCK_SHARED), KHB_OK);
    CHECK_EQ(lock_current(&a), LOCK_SHARED);
    CHECK_EQ(lock_current(&b), LOCK_SHARED);

    CHECK_STATUS(lock_release(&a), KHB_OK);
    CHECK_STATUS(lock_release(&b), KHB_OK);

    close(fd1);
    close(fd2);
    khb_temp_remove(path);
}

static void test_exclusive_blocks_shared(void)
{
    char   path[PATH_MAX];
    int    fd1, fd2;
    lock_t a, b;

    khb_temp_path(path, sizeof path, "exsh");
    fd1 = open_file(path);
    fd2 = open_file(path);
    CHECK(fd1 >= 0);
    CHECK(fd2 >= 0);

    lock_init(&a, fd1);
    lock_init(&b, fd2);

    CHECK_STATUS(lock_acquire(&a, LOCK_EXCLUSIVE), KHB_OK);
    CHECK_STATUS(lock_acquire(&b, LOCK_SHARED), KHB_ERR_LOCKED);
    CHECK_EQ(lock_current(&b), LOCK_NONE);

    CHECK_STATUS(lock_release(&a), KHB_OK);

    close(fd1);
    close(fd2);
    khb_temp_remove(path);
}

static void test_shared_blocks_exclusive(void)
{
    char   path[PATH_MAX];
    int    fd1, fd2;
    lock_t a, b;

    khb_temp_path(path, sizeof path, "shex");
    fd1 = open_file(path);
    fd2 = open_file(path);
    CHECK(fd1 >= 0);
    CHECK(fd2 >= 0);

    lock_init(&a, fd1);
    lock_init(&b, fd2);

    CHECK_STATUS(lock_acquire(&a, LOCK_SHARED), KHB_OK);
    CHECK_STATUS(lock_acquire(&b, LOCK_EXCLUSIVE), KHB_ERR_LOCKED);

    CHECK_STATUS(lock_release(&a), KHB_OK);

    close(fd1);
    close(fd2);
    khb_temp_remove(path);
}

static void test_exclusive_blocks_exclusive(void)
{
    char   path[PATH_MAX];
    int    fd1, fd2;
    lock_t a, b;

    khb_temp_path(path, sizeof path, "exex");
    fd1 = open_file(path);
    fd2 = open_file(path);
    CHECK(fd1 >= 0);
    CHECK(fd2 >= 0);

    lock_init(&a, fd1);
    lock_init(&b, fd2);

    CHECK_STATUS(lock_acquire(&a, LOCK_EXCLUSIVE), KHB_OK);
    CHECK_STATUS(lock_acquire(&b, LOCK_EXCLUSIVE), KHB_ERR_LOCKED);

    CHECK_STATUS(lock_release(&a), KHB_OK);

    close(fd1);
    close(fd2);
    khb_temp_remove(path);
}

static void test_release_allows_reacquire(void)
{
    char   path[PATH_MAX];
    int    fd1, fd2;
    lock_t a, b;

    khb_temp_path(path, sizeof path, "reacq");
    fd1 = open_file(path);
    fd2 = open_file(path);
    CHECK(fd1 >= 0);
    CHECK(fd2 >= 0);

    lock_init(&a, fd1);
    lock_init(&b, fd2);

    CHECK_STATUS(lock_acquire(&a, LOCK_EXCLUSIVE), KHB_OK);
    CHECK_STATUS(lock_acquire(&b, LOCK_EXCLUSIVE), KHB_ERR_LOCKED);
    CHECK_STATUS(lock_release(&a), KHB_OK);
    CHECK_STATUS(lock_acquire(&b, LOCK_EXCLUSIVE), KHB_OK);
    CHECK_STATUS(lock_release(&b), KHB_OK);

    CHECK_STATUS(lock_acquire(&a, LOCK_SHARED), KHB_OK);
    CHECK_STATUS(lock_release(&a), KHB_OK);

    close(fd1);
    close(fd2);
    khb_temp_remove(path);
}


static void test_dup_fd_shares_lock(void)
{
    char   path[PATH_MAX];
    int    fd1, fd2, fd3;
    lock_t a, b, c;

    khb_temp_path(path, sizeof path, "dupfd");
    fd1 = open_file(path);
    CHECK(fd1 >= 0);
    fd2 = dup(fd1);
    CHECK(fd2 >= 0);
    fd3 = open_file(path);              
    CHECK(fd3 >= 0);

    lock_init(&a, fd1);
    lock_init(&b, fd2);
    lock_init(&c, fd3);

    CHECK_STATUS(lock_acquire(&a, LOCK_EXCLUSIVE), KHB_OK);
    CHECK_STATUS(lock_acquire(&b, LOCK_EXCLUSIVE), KHB_OK);
    CHECK_STATUS(lock_acquire(&c, LOCK_EXCLUSIVE), KHB_ERR_LOCKED);

    CHECK_STATUS(lock_release(&b), KHB_OK);
    CHECK_STATUS(lock_acquire(&c, LOCK_EXCLUSIVE), KHB_OK);

    CHECK_EQ(lock_current(&a), LOCK_EXCLUSIVE);

    CHECK_STATUS(lock_release(&c), KHB_OK);
    close(fd1);
    close(fd2);
    close(fd3);
    khb_temp_remove(path);
}

static void test_double_acquire_rejected(void)
{
    char   path[PATH_MAX];
    int    fd;
    lock_t l;

    khb_temp_path(path, sizeof path, "dbl");
    fd = open_file(path);
    CHECK(fd >= 0);
    lock_init(&l, fd);

    CHECK_STATUS(lock_acquire(&l, LOCK_SHARED), KHB_OK);
    CHECK_STATUS(lock_acquire(&l, LOCK_SHARED), KHB_ERR_STATE);
    CHECK_EQ(lock_current(&l), LOCK_SHARED);

    CHECK_STATUS(lock_release(&l), KHB_OK);
    close(fd);
    khb_temp_remove(path);
}

static void test_upgrade_rejected(void)
{
    char   path[PATH_MAX];
    int    fd;
    lock_t l;

    khb_temp_path(path, sizeof path, "upgrade");
    fd = open_file(path);
    CHECK(fd >= 0);
    lock_init(&l, fd);

    CHECK_STATUS(lock_acquire(&l, LOCK_SHARED), KHB_OK);
    CHECK_STATUS(lock_acquire(&l, LOCK_EXCLUSIVE), KHB_ERR_STATE);
    CHECK_EQ(lock_current(&l), LOCK_SHARED);

    CHECK_STATUS(lock_release(&l), KHB_OK);
    CHECK_STATUS(lock_acquire(&l, LOCK_EXCLUSIVE), KHB_OK);
    CHECK_STATUS(lock_acquire(&l, LOCK_SHARED), KHB_ERR_STATE);

    CHECK_STATUS(lock_release(&l), KHB_OK);
    close(fd);
    khb_temp_remove(path);
}

static void test_release_when_unlocked(void)
{
    char   path[PATH_MAX];
    int    fd;
    lock_t l;

    khb_temp_path(path, sizeof path, "relnone");
    fd = open_file(path);
    CHECK(fd >= 0);
    lock_init(&l, fd);

    CHECK_EQ(lock_current(&l), LOCK_NONE);
    CHECK_STATUS(lock_release(&l), KHB_ERR_STATE);

    CHECK_STATUS(lock_acquire(&l, LOCK_SHARED), KHB_OK);
    CHECK_STATUS(lock_release(&l), KHB_OK);
    CHECK_STATUS(lock_release(&l), KHB_ERR_STATE);

    close(fd);
    khb_temp_remove(path);
}

static void test_invalid_want(void)
{
    char   path[PATH_MAX];
    int    fd;
    lock_t l, bad;

    khb_temp_path(path, sizeof path, "invalid");
    fd = open_file(path);
    CHECK(fd >= 0);

    lock_init(&l, fd);
    CHECK_STATUS(lock_acquire(&l, LOCK_NONE), KHB_ERR_INVALID);
    CHECK_EQ(lock_current(&l), LOCK_NONE);

    lock_init(&bad, -1);
    CHECK_STATUS(lock_acquire(&bad, LOCK_SHARED), KHB_ERR_INVALID);
    CHECK_STATUS(lock_release(&bad), KHB_ERR_INVALID);

    close(fd);
    khb_temp_remove(path);
}

static void test_death_releases_lock(void)
{
    char   path[PATH_MAX];
    int    fd;
    pid_t  pid = -1;
    lock_t l;

    khb_temp_path(path, sizeof path, "death");
    fd = open_file(path);
    CHECK(fd >= 0);

    CHECK_EQ(spawn_holder(path, LOCK_EXCLUSIVE, &pid), 0);

    lock_init(&l, fd);
    CHECK_STATUS(lock_acquire(&l, LOCK_EXCLUSIVE), KHB_ERR_LOCKED);

    CHECK_EQ(kill(pid, SIGKILL), 0);
    CHECK_EQ(waitpid(pid, NULL, 0), pid);

    CHECK_STATUS(lock_acquire(&l, LOCK_EXCLUSIVE), KHB_OK);
    CHECK_STATUS(lock_release(&l), KHB_OK);

    close(fd);
    khb_temp_remove(path);
}

int main(void)
{
    RUN_TEST(test_shared_shared_ok);
    RUN_TEST(test_exclusive_blocks_shared);
    RUN_TEST(test_shared_blocks_exclusive);
    RUN_TEST(test_exclusive_blocks_exclusive);
    RUN_TEST(test_release_allows_reacquire);
    RUN_TEST(test_dup_fd_shares_lock);
    RUN_TEST(test_double_acquire_rejected);
    RUN_TEST(test_upgrade_rejected);
    RUN_TEST(test_release_when_unlocked);
    RUN_TEST(test_invalid_want);
    RUN_TEST(test_death_releases_lock);

    return TEST_SUMMARY();
}