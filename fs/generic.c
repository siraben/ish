#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/dev.h"
#include "kernel/task.h"
#include "kernel/errno.h"

struct mount *find_mount_and_trim_path(char *path) {
    struct mount *mount = mount_find(path);
    char *dst = path;
    const char *src = path + strlen(mount->point);
    while (*src != '\0')
        *dst++ = *src++;
    *dst = '\0';
    return mount;
}

bool contains_mount_point(const char *path) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        int n = strlen(path);
        if (strncmp(path, mount->point, n) == 0 &&
                (mount->point[n] == '\0' || mount->point[n] == '/'))
            return true;
    }
    return false;
}

static bool parse_fd_path(const char *path, fd_t *fd_out) {
    const char *fd_str = NULL;
    if (strncmp(path, "/dev/fd/", 8) == 0) {
        fd_str = path + 8;
    } else if (strncmp(path, "/proc/self/fd/", 14) == 0) {
        fd_str = path + 14;
    } else {
        return false;
    }

    if (*fd_str == '\0')
        return false;

    fd_t fd = 0;
    for (const char *p = fd_str; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return false;
        fd = fd * 10 + (*p - '0');
    }
    *fd_out = fd;
    return true;
}

static struct fd *open_fd_path(const char *path) {
    fd_t fd_no;
    if (!parse_fd_path(path, &fd_no))
        return NULL;

    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return ERR_PTR(_ENOENT);
    fd_retain(fd);
    return fd;
}

static bool path_starts_with_component(const char *path, const char *prefix) {
    size_t len = strlen(prefix);
    if (strcmp(prefix, "/") == 0)
        return path[0] == '/';
    return strncmp(path, prefix, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

#define RESOLVE_NO_XDEV_ 0x01
#define RESOLVE_NO_MAGICLINKS_ 0x02
#define RESOLVE_NO_SYMLINKS_ 0x04
#define RESOLVE_BENEATH_ 0x08
#define RESOLVE_IN_ROOT_ 0x10
#define RESOLVE_CACHED_ 0x20

struct fd *generic_openat_resolve(struct fd *at, const char *path_raw, int flags, int mode, qword_t resolve) {
    int err;
    if (flags & O_RDWR_ && flags & O_WRONLY_)
        return ERR_PTR(_EINVAL);
    if (resolve & ~(RESOLVE_NO_XDEV_ | RESOLVE_NO_MAGICLINKS_ | RESOLVE_NO_SYMLINKS_ |
                RESOLVE_BENEATH_ | RESOLVE_IN_ROOT_ | RESOLVE_CACHED_))
        return ERR_PTR(_EINVAL);
    if ((resolve & RESOLVE_BENEATH_) && (resolve & RESOLVE_IN_ROOT_))
        return ERR_PTR(_EINVAL);
    if (resolve & RESOLVE_CACHED_)
        return ERR_PTR(_EAGAIN);
    if ((resolve & RESOLVE_BENEATH_) && path_raw[0] == '/')
        return ERR_PTR(_EXDEV);

    if (!(resolve & (RESOLVE_NO_MAGICLINKS_ | RESOLVE_NO_SYMLINKS_)) &&
            (at == AT_PWD || path_raw[0] == '/')) {
        struct fd *fd = open_fd_path(path_raw);
        if (fd != NULL)
            return fd;
    }

    // TODO really, really, seriously reconsider what I'm doing with the strings
    char path[MAX_PATH];
    int normalize_flags = flags & O_NOFOLLOW_ ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW;
    if (resolve & RESOLVE_NO_SYMLINKS_)
        normalize_flags = N_SYMLINK_NOFOLLOW | N_SYMLINK_NOFOLLOW_ANY;
    if (flags & O_CREAT_)
        normalize_flags |= N_PARENT_DIR_WRITE;
    char beneath[MAX_PATH];
    if (resolve & (RESOLVE_BENEATH_ | RESOLVE_IN_ROOT_)) {
        struct fd *base = at;
        lock(&current->fs->lock);
        if (base == AT_PWD)
            base = current->fs->pwd;
        unlock(&current->fs->lock);
        err = generic_getpath(base, beneath);
        if (err < 0)
            return ERR_PTR(err);
    }
    if (resolve & RESOLVE_IN_ROOT_) {
        // IN_ROOT needs per-component root confinement for absolute paths and
        // ".."; the generic resolver currently only supports post-resolution
        // containment checks.
        return ERR_PTR(_EINVAL);
    }
    err = path_normalize(at, path_raw, path, normalize_flags);
    if (err < 0)
        return ERR_PTR(err);
    if ((resolve & RESOLVE_BENEATH_) && !path_starts_with_component(path, beneath))
        return ERR_PTR(_EXDEV);
    if (resolve & RESOLVE_NO_XDEV_) {
        char base_path[MAX_PATH];
        if (generic_getpath(at == AT_PWD ? current->fs->pwd : at, base_path) < 0)
            return ERR_PTR(_EXDEV);
        struct mount *base_mount = mount_find(base_path);
        struct mount *target_mount = mount_find(path);
        bool same_mount = base_mount == target_mount;
        mount_release(base_mount);
        mount_release(target_mount);
        if (!same_mount)
            return ERR_PTR(_EXDEV);
    }
    struct mount *mount = find_mount_and_trim_path(path);
    struct fd *fd = mount->fs->open(mount, path, flags, mode);
    if (IS_ERR(fd)) {
        // if an error happens after this point, fd_close will release the
        // mount, but right now we need to do it manually
        mount_release(mount);
        return fd;
    }
    fd->mount = mount;

    lock(&inodes_lock); // TODO: don't do this
    struct statbuf stat;
    err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0) {
        unlock(&inodes_lock);
        goto error;
    }
    fd->inode = inode_get_unlocked(mount, stat.inode);
    unlock(&inodes_lock);
    fd->type = stat.mode & S_IFMT;
    fd->flags = flags;

    int accmode;
    if (flags & O_RDWR_) accmode = AC_R | AC_W;
    else if (flags & O_WRONLY_) accmode = AC_W;
    else accmode = AC_R;
    err = access_check(&stat, accmode);
    if (err < 0)
        goto error;

    assert(!S_ISLNK(fd->type)); // would mean path_normalize didn't do its job
    if (S_ISBLK(fd->type) || S_ISCHR(fd->type)) {
        int type;
        if (S_ISBLK(fd->type))
            type = DEV_BLOCK;
        else
            type = DEV_CHAR;
        err = dev_open(dev_major(stat.rdev), dev_minor(stat.rdev), type, fd);
        if (err < 0)
            goto error;
    }
    err = _ENXIO;
    if (S_ISSOCK(fd->type))
        goto error;
    err = _EISDIR;
    if (S_ISDIR(fd->type) && flags & (O_RDWR_ | O_WRONLY_))
        goto error;
    err = _ENOTDIR;
    if (!S_ISDIR(fd->type) && flags & O_DIRECTORY_)
        goto error;
    return fd;

error:
    fd_close(fd);
    return ERR_PTR(err);
}

struct fd *generic_openat(struct fd *at, const char *path_raw, int flags, int mode) {
    return generic_openat_resolve(at, path_raw, flags, mode, 0);
}

struct fd *generic_open(const char *path, int flags, int mode) {
    return generic_openat(AT_PWD, path, flags, mode);
}

int generic_getpath(struct fd *fd, char *buf) {
    int err = fd->mount->fs->getpath(fd, buf);
    if (err < 0)
        return err;
    if (strlen(buf) + strlen(fd->mount->point) >= MAX_PATH)
        return _ENAMETOOLONG;
    memmove(buf + strlen(fd->mount->point), buf, strlen(buf) + 1);
    memcpy(buf, fd->mount->point, strlen(fd->mount->point));
    if (buf[0] == '\0')
        strcpy(buf, "/");
    return 0;
}

int generic_accessat(struct fd *dirfd, const char *path_raw, int mode) {
    char path[MAX_PATH];
    int err = path_normalize(dirfd, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    struct mount *mount = find_mount_and_trim_path(path);
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    mount_release(mount);
    if (err < 0)
        return err;
    return access_check(&stat, mode);
}

int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->link == NULL)
        err = _EPERM;
    else
        err = mount->fs->link(mount, src, dst);
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_unlinkat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->unlink)
        err = mount->fs->unlink(mount, path);
    mount_release(mount);
    return err;
}

int generic_renameat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(src))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->rename == NULL)
        err = _EPERM;
    else
        err = mount->fs->rename(mount, src, dst);
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

int generic_symlinkat(const char *target, struct fd *at, const char *link_raw) {
    char link[MAX_PATH];
    int err = path_normalize(at, link_raw, link, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(link);
    err = _EPERM;
    if (mount->fs->symlink)
        err = mount->fs->symlink(mount, target, link);
    mount_release(mount);
    return err;
}

int generic_mknodat(struct fd *at, const char *path_raw, mode_t_ mode, dev_t_ dev) {
    if (S_ISDIR(mode) || S_ISLNK(mode))
        return _EINVAL;
    if (!superuser() && (S_ISBLK(mode) || S_ISCHR(mode)))
        return _EPERM;

    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->mknod)
        err = mount->fs->mknod(mount, path, mode, dev);
    mount_release(mount);
    return err;
}

int generic_setattrat(struct fd *at, const char *path_raw, struct attr attr, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->setattr)
        err = mount->fs->setattr(mount, path, attr);
    mount_release(mount);
    return err;
}

int generic_utime(struct fd *at, const char *path_raw, struct timespec atime, struct timespec mtime, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->utime)
        err = mount->fs->utime(mount, path, atime, mtime);
    mount_release(mount);
    return err;
}

ssize_t generic_readlinkat(struct fd *at, const char *path_raw, char *buf, size_t bufsize) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EINVAL;
    if (mount->fs->readlink)
        err = mount->fs->readlink(mount, path, buf, bufsize);
    mount_release(mount);
    return err;
}

int generic_mkdirat(struct fd *at, const char *path_raw, mode_t_ mode) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_FOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->mkdir)
        err = mount->fs->mkdir(mount, path, mode);
    mount_release(mount);
    return err;
}

int generic_rmdirat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_FOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(path))
        return _EBUSY;
    struct mount *mount = find_mount_and_trim_path(path);
    err = _EPERM;
    if (mount->fs->rmdir)
        err = mount->fs->rmdir(mount, path);
    mount_release(mount);
    return err;
}

int generic_seek(struct fd *fd, off_t_ off, int whence, size_t size) {
    off_t_ new_off = fd->offset;
    if (whence == LSEEK_SET) {
        fd->offset = off;
    } else if (whence == LSEEK_CUR) {
        if (__builtin_add_overflow(new_off, off, &new_off) || new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else if (whence == LSEEK_END) {
        new_off = size + off;
        if (new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else {
        return _EINVAL;
    }
    return 0;
}
