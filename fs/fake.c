#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sqlite3.h>

#include "debug.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/inode.h"
#include "fs/real.h"
#define ISH_INTERNAL
#include "fs/fake.h"

// TODO document database

// this exists only to override readdir to fix the returned inode numbers
static struct fd_ops fakefs_fdops;

static void fakefs_fd_cache_stat(struct fd *fd, struct fakefs_db *fs, const struct ish_stat *stat) {
    fd->fake_ishstat = *stat;
    fd->fake_ishstat_generation = fs->cache_generation;
    fd->fake_ishstat_valid = true;
}

static void fakefs_record_or_flush_deferred_create(struct mount *mount, int fd, const struct ish_stat *stat) {
    struct fakefs_db *fs = &mount->fakefs;
    if (fakefs_record_deferred_create(mount->root_fd, fd, stat))
        return;
    sqlite3_mutex_enter(fs->lock);
    db_flush_deferred(fs);
    sqlite3_mutex_leave(fs->lock);
}

static const char *fakefs_builtin_symlink(const char *path) {
    if (strcmp(path, "/dev/fd") == 0)
        return "/proc/self/fd";
    if (strcmp(path, "/dev/stdin") == 0)
        return "/proc/self/fd/0";
    if (strcmp(path, "/dev/stdout") == 0)
        return "/proc/self/fd/1";
    if (strcmp(path, "/dev/stderr") == 0)
        return "/proc/self/fd/2";
    return NULL;
}

static ino_t fakefs_builtin_symlink_inode(const char *path) {
    if (strcmp(path, "/dev/fd") == 0)
        return 0x7ffff001;
    if (strcmp(path, "/dev/stdin") == 0)
        return 0x7ffff002;
    if (strcmp(path, "/dev/stdout") == 0)
        return 0x7ffff003;
    if (strcmp(path, "/dev/stderr") == 0)
        return 0x7ffff004;
    return 0;
}

static void fakefs_builtin_symlink_stat(struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->mode = S_IFLNK | 0777;
    stat->nlink = 1;
    stat->uid = 0;
    stat->gid = 0;
}

static struct fd *fakefs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct fakefs_db *fs = &mount->fakefs;
    struct fd *fd = realfs.open(mount, path, flags, 0666);
    if (IS_ERR(fd))
        return fd;
    bool may_create = flags & O_CREAT_;
    bool exclusive_create = (flags & O_EXCL_) && may_create;
    struct ish_stat ishstat;
    if (exclusive_create) {
        ishstat.mode = mode | S_IFREG;
        ishstat.uid = current->fsuid;
        ishstat.gid = current->fsgid;
        ishstat.rdev = 0;
        sqlite3_mutex_enter(fs->lock);
        fd->fake_inode = path_defer_create(fs, path, &ishstat);
        sqlite3_mutex_leave(fs->lock);
        if (fd->fake_inode == 0) {
            fd_close(fd);
            realfs.unlink(mount, path);
            return ERR_PTR(_ENOMEM);
        }
        fakefs_record_or_flush_deferred_create(mount, fd->real_fd, &ishstat);
        fakefs_fd_cache_stat(fd, fs, &ishstat);
        fd->ops = &fakefs_fdops;
        return fd;
    }
    if (may_create)
        db_begin_write(fs);
    else
        db_begin_read(fs);
    fd->fake_inode = path_get_inode(fs, path);
    bool created = false;
    if (may_create) {
        ishstat.mode = mode | S_IFREG;
        ishstat.uid = current->fsuid;
        ishstat.gid = current->fsgid;
        ishstat.rdev = 0;
        if (fd->fake_inode == 0) {
            fd->fake_inode = path_create(fs, path, &ishstat);
            created = true;
        }
    }
    if (fd->fake_inode != 0 && !created)
        if (!path_read_stat(fs, path, &ishstat, NULL))
            fd->fake_inode = 0;
    db_commit(fs);
    if (fd->fake_inode == 0) {
        // metadata for this file is missing
        // TODO unlink the real file
        fd_close(fd);
        return ERR_PTR(_ENOENT);
    }
    fakefs_fd_cache_stat(fd, fs, &ishstat);
    fd->ops = &fakefs_fdops;
    return fd;
}

// WARNING: giant hack, just for file providerws
struct fd *fakefs_open_inode(struct mount *mount, ino_t inode) {
    struct fakefs_db *fs = &mount->fakefs;
    sqlite3_mutex_enter(fs->lock);
    db_flush_deferred(fs);
    sqlite3_mutex_leave(fs->lock);
    db_begin_read(fs);
    sqlite3_stmt *stmt = fs->stmt.path_from_inode;
    sqlite3_bind_int64(stmt, 1, inode);
step:
    if (!db_exec(fs, stmt)) {
        db_reset(fs, stmt);
        db_rollback(fs);
        return ERR_PTR(_ENOENT);
    }
    const char *path = (const char *) sqlite3_column_text(stmt, 0);
    struct fd *fd = realfs.open(mount, path, O_RDWR_, 0);
    if (PTR_ERR(fd) == _EISDIR)
        fd = realfs.open(mount, path, O_RDONLY_, 0);
    if (PTR_ERR(fd) == _ENOENT)
        goto step;
    db_reset(fs, stmt);
    db_commit(fs);
    fd->fake_inode = inode;
    fd->ops = &fakefs_fdops;
    db_begin_read(fs);
    struct ish_stat ishstat;
    if (inode_read_stat_if_exist(fs, inode, &ishstat))
        fakefs_fd_cache_stat(fd, fs, &ishstat);
    db_commit(fs);
    return fd;
}

static int fakefs_link(struct mount *mount, const char *src, const char *dst) {
    struct fakefs_db *fs = &mount->fakefs;
    sqlite3_mutex_enter(fs->lock);
    db_flush_deferred(fs);
    sqlite3_mutex_leave(fs->lock);
    db_begin_write(fs);
    int err = realfs.link(mount, src, dst);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    path_link(fs, src, dst);
    db_commit(fs);
    return 0;
}

static int fakefs_unlink(struct mount *mount, const char *path) {
    struct fakefs_db *fs = &mount->fakefs;
    sqlite3_mutex_enter(fs->lock);
    db_flush_deferred(fs);
    sqlite3_mutex_leave(fs->lock);
    db_begin_write(fs);
    int err = realfs.unlink(mount, path);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    ino_t ino = path_unlink(fs, path);
    db_commit(fs);
    inode_check_orphaned(mount, ino);
    return 0;
}

static int fakefs_rmdir(struct mount *mount, const char *path) {
    struct fakefs_db *fs = &mount->fakefs;
    sqlite3_mutex_enter(fs->lock);
    db_flush_deferred(fs);
    sqlite3_mutex_leave(fs->lock);
    db_begin_write(fs);
    int err = realfs.rmdir(mount, path);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    ino_t ino = path_unlink(fs, path);
    db_commit(fs);
    inode_check_orphaned(mount, ino);
    return 0;
}

static int fakefs_rename(struct mount *mount, const char *src, const char *dst) {
    struct fakefs_db *fs = &mount->fakefs;
    sqlite3_mutex_enter(fs->lock);
    db_flush_deferred(fs);
    sqlite3_mutex_leave(fs->lock);
    db_begin_write(fs);
    path_rename(fs, src, dst);
    int err = realfs.rename(mount, src, dst);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    db_commit(fs);
    return 0;
}

static int fakefs_symlink(struct mount *mount, const char *target, const char *link) {
    struct fakefs_db *fs = &mount->fakefs;
    // create a file containing the target
    int fd = openat(mount->root_fd, fix_path(link), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        return errno_map();
    }
    ssize_t res = write(fd, target, strlen(target));
    if (res < 0) {
        int saved_errno = errno;
        close(fd);
        unlinkat(mount->root_fd, fix_path(link), 0);
        errno = saved_errno;
        return errno_map();
    }

    // customize the stat info so it looks like a link
    struct ish_stat ishstat;
    ishstat.mode = S_IFLNK | 0777; // symlinks always have full permissions
    ishstat.uid = current->fsuid;
    ishstat.gid = current->fsgid;
    ishstat.rdev = 0;
    sqlite3_mutex_enter(fs->lock);
    inode_t inode = path_defer_create(fs, link, &ishstat);
    sqlite3_mutex_leave(fs->lock);
    if (inode == 0) {
        close(fd);
        unlinkat(mount->root_fd, fix_path(link), 0);
        return _ENOMEM;
    }
    fakefs_record_or_flush_deferred_create(mount, fd, &ishstat);
    close(fd);
    return 0;
}

static int fakefs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev) {
    struct fakefs_db *fs = &mount->fakefs;
    mode_t_ real_mode = 0666;
    if (S_ISBLK(mode) || S_ISCHR(mode) || S_ISSOCK(mode))
        real_mode |= S_IFREG;
    else
        real_mode |= mode & S_IFMT;
    db_begin_write(fs);
    int err = realfs.mknod(mount, path, real_mode, 0);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    struct ish_stat stat;
    stat.mode = mode;
    stat.uid = current->fsuid;
    stat.gid = current->fsgid;
    stat.rdev = 0;
    if (S_ISBLK(mode) || S_ISCHR(mode))
        stat.rdev = dev;
    path_create(fs, path, &stat);
    db_commit(fs);
    return err;
}

static int fakefs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat) {
    const char *builtin_link = fakefs_builtin_symlink(path);
    if (builtin_link != NULL) {
        fakefs_builtin_symlink_stat(fake_stat);
        fake_stat->inode = fakefs_builtin_symlink_inode(path);
        fake_stat->size = strlen(builtin_link);
        return 0;
    }

    struct fakefs_db *fs = &mount->fakefs;
    struct ish_stat ishstat;
    ino_t inode;
    if (!path_read_stat_cached(fs, path, &ishstat, &inode))
        return _ENOENT;
    int err = realfs.stat(mount, path, fake_stat);
    if (err < 0)
        return err;
    fake_stat->inode = inode;
    fake_stat->mode = ishstat.mode;
    fake_stat->uid = ishstat.uid;
    fake_stat->gid = ishstat.gid;
    fake_stat->rdev = ishstat.rdev;
    return 0;
}

static int fakefs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    struct fakefs_db *fs = &fd->mount->fakefs;
    int err = realfs.fstat(fd, fake_stat);
    if (err < 0)
        return err;
    struct ish_stat ishstat;
    if (fd->fake_ishstat_valid && fd->fake_ishstat_generation == fs->cache_generation) {
        ishstat = fd->fake_ishstat;
    } else {
        db_begin_read(fs);
        if (!inode_read_stat_if_exist(fs, fd->fake_inode, &ishstat)) {
            db_rollback(fs);
            return _ENOENT;
        }
        db_commit(fs);
        fakefs_fd_cache_stat(fd, fs, &ishstat);
    }
    fake_stat->inode = fd->fake_inode;
    fake_stat->mode = ishstat.mode;
    fake_stat->uid = ishstat.uid;
    fake_stat->gid = ishstat.gid;
    fake_stat->rdev = ishstat.rdev;
    return 0;
}

static void fake_stat_setattr(struct ish_stat *ishstat, struct attr attr) {
    switch (attr.type) {
        case attr_uid:
            ishstat->uid = attr.uid;
            break;
        case attr_gid:
            ishstat->gid = attr.gid;
            break;
        case attr_mode:
            ishstat->mode = (ishstat->mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            die("attr_size should be handled by realfs");
    }
}

static int fakefs_setattr(struct mount *mount, const char *path, struct attr attr) {
    struct fakefs_db *fs = &mount->fakefs;
    if (attr.type == attr_size)
        return realfs.setattr(mount, path, attr);
    db_begin_read(fs);
    struct ish_stat ishstat;
    ino_t inode;
    if (!path_read_stat(fs, path, &ishstat, &inode)) {
        db_rollback(fs);
        return _ENOENT;
    }
    fake_stat_setattr(&ishstat, attr);
    inode_write_stat(fs, inode, &ishstat);
    db_commit(fs);
    return 0;
}

static int fakefs_fsetattr(struct fd *fd, struct attr attr) {
    struct fakefs_db *fs = &fd->mount->fakefs;
    if (attr.type == attr_size)
        return realfs.fsetattr(fd, attr);
    sqlite3_mutex_enter(fs->lock);
    struct ish_stat ishstat;
    if (fd->fake_ishstat_valid && fd->fake_ishstat_generation == fs->cache_generation)
        ishstat = fd->fake_ishstat;
    else
        inode_read_stat_or_die(fs, fd->fake_inode, &ishstat);
    fake_stat_setattr(&ishstat, attr);
    inode_defer_write_stat(fs, fd->fake_inode, &ishstat);
    sqlite3_mutex_leave(fs->lock);
    fakefs_fd_cache_stat(fd, fs, &ishstat);
    return 0;
}

static int fakefs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    struct fakefs_db *fs = &mount->fakefs;
    int err = realfs.mkdir(mount, path, 0777);
    if (err < 0)
        return err;
    struct ish_stat ishstat;
    ishstat.mode = mode | S_IFDIR;
    ishstat.uid = current->fsuid;
    ishstat.gid = current->fsgid;
    ishstat.rdev = 0;
    sqlite3_mutex_enter(fs->lock);
    inode_t inode = path_defer_create(fs, path, &ishstat);
    sqlite3_mutex_leave(fs->lock);
    if (inode == 0) {
        realfs.rmdir(mount, path);
        return _ENOMEM;
    }
    int fd = openat(mount->root_fd, fix_path(path), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fakefs_record_or_flush_deferred_create(mount, fd, &ishstat);
        close(fd);
    } else {
        sqlite3_mutex_enter(fs->lock);
        db_flush_deferred(fs);
        sqlite3_mutex_leave(fs->lock);
    }
    return 0;
}

static ssize_t file_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    // broken symlinks can't be included in an iOS app or else Xcode craps out
    int fd = openat(mount->root_fd, fix_path(path), O_RDONLY);
    if (fd < 0)
        return errno_map();
    int err = read(fd, buf, bufsize);
    close(fd);
    if (err < 0)
        return errno_map();
    return err;
}

static ssize_t fakefs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    const char *builtin_link = fakefs_builtin_symlink(path);
    if (builtin_link != NULL) {
        size_t len = strlen(builtin_link);
        if (bufsize > len)
            bufsize = len;
        memcpy(buf, builtin_link, bufsize);
        return bufsize;
    }

    struct fakefs_db *fs = &mount->fakefs;
    struct ish_stat ishstat;
    if (!path_read_stat_cached(fs, path, &ishstat, NULL))
        return _ENOENT;
    if (!S_ISLNK(ishstat.mode)) {
        return _EINVAL;
    }

    ssize_t err = realfs.readlink(mount, path, buf, bufsize);
    if (err == _EINVAL)
        err = file_readlink(mount, path, buf, bufsize);
    return err;
}

static int fakefs_readdir(struct fd *fd, struct dir_entry *entry) {
    assert(fd->ops == &fakefs_fdops);
    int res;
retry:
    res = realfs_fdops.readdir(fd, entry);
    if (res <= 0)
        return res;

    // this is annoying
    char entry_path[MAX_PATH + 1];
    if (fd->fake_dir_path == NULL) {
        realfs_getpath(fd, entry_path);
        fd->fake_dir_path = strdup(entry_path);
    } else {
        strcpy(entry_path, fd->fake_dir_path);
    }
    if (strcmp(entry->name, "..") == 0) {
        if (strcmp(entry_path, "") != 0) {
            *strrchr(entry_path, '/') = '\0';
        }
    } else if (strcmp(entry->name, ".") != 0) {
        // god I don't know what to do if this would overflow
        strcat(entry_path, "/");
        strcat(entry_path, entry->name);
    }

    struct fakefs_db *fs = &fd->mount->fakefs;
    entry->inode = path_get_inode_cached(fs, entry_path);
    // it's quite possible that due to some mishap there's no metadata for this file
    // so just skip this entry, instead of crashing the program, so there's hope for recovery
    if (entry->inode == 0)
        goto retry;
    return res;
}

static int fakefs_close_fd(struct fd *fd) {
    free(fd->fake_dir_path);
    fd->fake_dir_path = NULL;
    return 0;
}

static struct fd_ops fakefs_fdops;
static void __attribute__((constructor)) init_fake_fdops() {
    fakefs_fdops = realfs_fdops;
    fakefs_fdops.readdir = fakefs_readdir;
    fakefs_fdops.close = fakefs_close_fd;
}

static int fakefs_mount(struct mount *mount) {
    char db_path[PATH_MAX];
    strcpy(db_path, mount->source);
    char *basename = strrchr(db_path, '/') + 1;
    assert(strcmp(basename, "data") == 0);
    strcpy(basename, "meta.db");

    // do this now so rebuilding can use root_fd
    int err = realfs.mount(mount);
    if (err < 0)
        return err;

    err = fake_db_init(&mount->fakefs, db_path, mount->root_fd);
    if (err < 0)
        return err;

    return 0;
}

static int fakefs_umount(struct mount *mount) {
    int err = fake_db_deinit(&mount->fakefs);
    if (err != SQLITE_OK) {
        printk("sqlite failed to close: %d\n", err);
    }
    /* return realfs.umount(mount); */
    return 0;
}

static void fakefs_inode_orphaned(struct mount *mount, ino_t inode) {
    struct fakefs_db *fs = &mount->fakefs;
    db_begin_write(fs);
    sqlite3_bind_int64(fs->stmt.try_cleanup_inode, 1, inode);
    db_exec_reset(fs, fs->stmt.try_cleanup_inode);
    db_commit(fs);
}

const struct fs_ops fakefs = {
    .name = "fake", .magic = 0x66616b65,
    .mount = fakefs_mount,
    .umount = fakefs_umount,
    .statfs = realfs_statfs,
    .open = fakefs_open,
    .readlink = fakefs_readlink,
    .link = fakefs_link,
    .unlink = fakefs_unlink,
    .rename = fakefs_rename,
    .symlink = fakefs_symlink,
    .mknod = fakefs_mknod,

    .close = realfs_close,
    .stat = fakefs_stat,
    .fstat = fakefs_fstat,
    .flock = realfs_flock,
    .setattr = fakefs_setattr,
    .fsetattr = fakefs_fsetattr,
    .getpath = realfs_getpath,
    .utime = realfs_utime,

    .mkdir = fakefs_mkdir,
    .rmdir = fakefs_rmdir,

    .inode_orphaned = fakefs_inode_orphaned,
};
