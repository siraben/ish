#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/real.h"

#define MFD_CLOEXEC_ 0x0001u
#define MFD_ALLOW_SEALING_ 0x0002u
#define MFD_HUGETLB_ 0x0004u
#define MFD_NOEXEC_SEAL_ 0x0008u
#define MFD_EXEC_ 0x0010u

static struct mount memfd_mount;

static int memfd_fstat(struct fd *fd, struct statbuf *fake_stat) {
    struct stat real_stat;
    if (fstat(fd->real_fd, &real_stat) < 0)
        return errno_map();
    memset(fake_stat, 0, sizeof(*fake_stat));
    fake_stat->dev = fd->stat.dev;
    fake_stat->inode = fd->stat.inode;
    fake_stat->mode = S_IFREG | 0777;
    fake_stat->nlink = 0;
    fake_stat->uid = current->fsuid;
    fake_stat->gid = current->fsgid;
    fake_stat->size = real_stat.st_size;
    fake_stat->blksize = real_stat.st_blksize;
    fake_stat->blocks = real_stat.st_blocks;
    fake_stat->atime = real_stat.st_atime;
    fake_stat->mtime = real_stat.st_mtime;
    fake_stat->ctime = real_stat.st_ctime;
#if __APPLE__
#define TIMESPEC(x) st_##x##timespec
#elif __linux__
#define TIMESPEC(x) st_##x##tim
#endif
    fake_stat->atime_nsec = real_stat.TIMESPEC(a).tv_nsec;
    fake_stat->mtime_nsec = real_stat.TIMESPEC(m).tv_nsec;
    fake_stat->ctime_nsec = real_stat.TIMESPEC(c).tv_nsec;
#undef TIMESPEC
    return 0;
}

static int memfd_fsetattr(struct fd *fd, struct attr attr) {
    return realfs_fsetattr(fd, attr);
}

static int memfd_getpath(struct fd *fd, char *buf) {
    const char *name = fd->data != NULL ? fd->data : "";
    snprintf(buf, MAX_PATH, "/memfd:%s (deleted)", name);
    return 0;
}

static int memfd_close(struct fd *fd) {
    free(fd->data);
    fd->data = NULL;
    return realfs_close(fd);
}

static const struct fd_ops memfd_fdops = {
    .read = realfs_read,
    .write = realfs_write,
    .readv = realfs_readv,
    .writev = realfs_writev,
    .pread = realfs_pread,
    .pwrite = realfs_pwrite,
    .preadv = realfs_preadv,
    .pwritev = realfs_pwritev,
    .lseek = realfs_lseek,
    .mmap = realfs_mmap,
    .poll = realfs_poll,
    .ioctl_size = realfs_ioctl_size,
    .ioctl = realfs_ioctl,
    .fsync = realfs_fsync,
    .close = memfd_close,
    .getflags = realfs_getflags,
    .setflags = realfs_setflags,
};

static const struct fs_ops memfd_fs = {
    .name = "memfd",
    .magic = 0x01021994,
    .fstat = memfd_fstat,
    .fsetattr = memfd_fsetattr,
    .getpath = memfd_getpath,
};

static struct mount memfd_mount = {
    .fs = &memfd_fs,
    .point = "",
};

static int create_tmpfile(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0')
        tmpdir = "/tmp";

    char path[MAX_PATH];
    int n = snprintf(path, sizeof(path), "%s/%s", tmpdir, "ish-memfd.XXXXXX");
    if (n < 0 || (size_t) n >= sizeof(path))
        return _ENAMETOOLONG;

    int fd = mkstemp(path);
    if (fd < 0)
        return errno_map();
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int_t sys_memfd_create(addr_t name_addr, dword_t flags) {
    if (flags & ~(MFD_CLOEXEC_ | MFD_ALLOW_SEALING_ | MFD_NOEXEC_SEAL_ | MFD_EXEC_))
        return _EINVAL;
    if (flags & MFD_HUGETLB_)
        return _EINVAL;
    if ((flags & MFD_NOEXEC_SEAL_) && (flags & MFD_EXEC_))
        return _EINVAL;

    char name[256];
    if (user_read_string(name_addr, name, sizeof(name)))
        return _EFAULT;
    STRACE("memfd_create(\"%s\", %#x)", name, flags);

    int real_fd = create_tmpfile();
    if (real_fd < 0)
        return real_fd;

    struct fd *fd = fd_create(&memfd_fdops);
    if (fd == NULL) {
        close(real_fd);
        return _ENOMEM;
    }

    mount_retain(&memfd_mount);
    fd->mount = &memfd_mount;
    fd->real_fd = real_fd;
    fd->dir = NULL;
    fd->type = S_IFREG;
    fd->flags = O_RDWR_;
    fd->data = strdup(name);
    if (fd->data == NULL) {
        fd_close(fd);
        return _ENOMEM;
    }

    static _Atomic ino_t next_inode = 1;
    fd->stat = (struct statbuf) {
        .inode = next_inode++,
        .mode = S_IFREG | 0777,
        .uid = current->fsuid,
        .gid = current->fsgid,
    };

    return f_install(fd, flags & MFD_CLOEXEC_ ? O_CLOEXEC_ : 0);
}
