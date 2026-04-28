#include "debug.h"
#include <limits.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/dev.h"

static struct fd *at_fd(fd_t f) {
    if (f == AT_FDCWD_)
        return AT_PWD;
    return f_get(f);
}

static void apply_umask(mode_t_ *mode) {
    struct fs_info *fs = current->fs;
    lock(&fs->lock);
    *mode &= ~fs->umask;
    unlock(&fs->lock);
}

int access_check(struct statbuf *stat, int check) {
    if (superuser()) return 0;
    if (check == 0) return 0;
    // Align check with the correct bits in mode
    if (current->fsuid == stat->uid) {
        check <<= 6;
    } else if (current->fsgid == stat->gid) {
        check <<= 3;
    }
    if (!(stat->mode & check))
        return _EACCES;
    return 0;
}

// TODO ENAMETOOLONG

#define AT_EACCESS_ 0x200
dword_t sys_access(addr_t path_addr, dword_t mode) {
    return sys_faccessat(AT_FDCWD_, path_addr, mode, 0);
}
dword_t sys_faccessat(fd_t at_f, addr_t path_addr, mode_t_ mode, dword_t flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    STRACE("faccessat(%d, \"%s\", 0x%x, %d)", at_f, path, mode, flags);

    if (flags & AT_EACCESS_)
        return generic_accessat(at, path, mode);

    uid_t_ uid_tmp = current->fsuid;
    uid_t_ gid_tmp = current->fsgid;
    current->fsuid = current->uid;
    current->fsgid = current->gid;
    int err = generic_accessat(at, path, mode);
    current->fsuid = uid_tmp;
    current->fsgid = gid_tmp;
    return err;
}

fd_t sys_openat(fd_t at_f, addr_t path_addr, dword_t flags, mode_t_ mode) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("openat(%d, \"%s\", 0x%x, 0x%x)", at_f, path, flags, mode);

    if (flags & O_CREAT_)
        apply_umask(&mode);

    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    struct fd *fd = generic_openat(at, path, flags, mode);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    return f_install(fd, flags);
}

struct open_how_ {
    qword_t flags;
    qword_t mode;
    qword_t resolve;
};

fd_t sys_openat2(fd_t at_f, addr_t path_addr, addr_t how_addr, dword_t size) {
    struct open_how_ how = {};
    if (size < sizeof(how))
        return _EINVAL;
    if (user_read(how_addr, &how, sizeof(how)))
        return _EFAULT;
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("openat2(%d, \"%s\", flags=0x%llx, mode=0x%llx, resolve=0x%llx, size=%u)",
            at_f, path, how.flags, how.mode, how.resolve, size);
    if ((how.flags >> 32) != 0 || (how.mode >> 32) != 0)
        return _EINVAL;
    mode_t_ mode = how.mode;
    dword_t flags = how.flags;
    if (flags & O_CREAT_)
        apply_umask(&mode);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    struct fd *fd = generic_openat_resolve(at, path, flags, mode, how.resolve);
    if (IS_ERR(fd))
        return PTR_ERR(fd);
    return f_install(fd, flags);
}

fd_t sys_open(addr_t path_addr, dword_t flags, mode_t_ mode) {
    return sys_openat(AT_FDCWD_, path_addr, flags, mode);
}

dword_t sys_readlinkat(fd_t at_f, addr_t path_addr, addr_t buf_addr, dword_t bufsize) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("readlinkat(%d, \"%s\", %#x, %#x)", at_f, path, buf_addr, bufsize);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    char buf[bufsize];
    ssize_t size = generic_readlinkat(at, path, buf, bufsize);
    if (size >= 0) {
        STRACE(" \"%.*s\"", size, buf);
        if (user_write(buf_addr, buf, size))
            return _EFAULT;
    }
    return size;
}

dword_t sys_readlink(addr_t path_addr, addr_t buf_addr, dword_t bufsize) {
    return sys_readlinkat(AT_FDCWD_, path_addr, buf_addr, bufsize);
}

dword_t sys_linkat(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr) {
    char src[MAX_PATH];
    if (user_read_string(src_addr, src, sizeof(src)))
        return _EFAULT;
    char dst[MAX_PATH];
    if (user_read_string(dst_addr, dst, sizeof(dst)))
        return _EFAULT;
    STRACE("linkat(%d, \"%s\", %d, \"%s\")", src_at_f, src, dst_at_f, dst);
    struct fd *src_at = at_fd(src_at_f);
    if (src_at == NULL)
        return _EBADF;
    struct fd *dst_at = at_fd(dst_at_f);
    if (dst_at == NULL)
        return _EBADF;
    return generic_linkat(src_at, src, dst_at, dst);
}

dword_t sys_link(addr_t src_addr, addr_t dst_addr) {
    return sys_linkat(AT_FDCWD_, src_addr, AT_FDCWD_, dst_addr);
}

#define AT_REMOVEDIR_ 0x200
dword_t sys_unlinkat(fd_t at_f, addr_t path_addr, int_t flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("unlinkat(%d, \"%s\", %d)", at_f, path, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    if (flags & AT_REMOVEDIR_)
        return generic_rmdirat(at, path);
    else
        return generic_unlinkat(at, path);
}

dword_t sys_unlink(addr_t path_addr) {
    return sys_unlinkat(AT_FDCWD_, path_addr, 0);
}

dword_t sys_renameat2(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr, int_t flags) {
    if (flags != 0)
        return _EINVAL;
    char src[MAX_PATH];
    if (user_read_string(src_addr, src, sizeof(src)))
        return _EFAULT;
    char dst[MAX_PATH];
    if (user_read_string(dst_addr, dst, sizeof(dst)))
        return _EFAULT;
    STRACE("renameat(%d, \"%s\", %d, \"%s\")", src_at_f, src, dst_at_f, dst);
    struct fd *src_at = at_fd(src_at_f);
    if (src_at == NULL)
        return _EBADF;
    struct fd *dst_at = at_fd(dst_at_f);
    if (dst_at == NULL)
        return _EBADF;
    return generic_renameat(src_at, src, dst_at, dst);
}

dword_t sys_renameat(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr) {
    return sys_renameat2(src_at_f, src_addr, dst_at_f, dst_addr, 0);
}

dword_t sys_rename(addr_t src_addr, addr_t dst_addr) {
    return sys_renameat2(AT_FDCWD_, src_addr, AT_FDCWD_, dst_addr, 0);
}

dword_t sys_symlinkat(addr_t target_addr, fd_t at_f, addr_t link_addr) {
    char target[MAX_PATH];
    if (user_read_string(target_addr, target, sizeof(target)))
        return _EFAULT;
    char link[MAX_PATH];
    if (user_read_string(link_addr, link, sizeof(link)))
        return _EFAULT;
    STRACE("symlinkat(\"%s\", %d, \"%s\")", target, at_f, link);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    return generic_symlinkat(target, at, link);
}

dword_t sys_symlink(addr_t target_addr, addr_t link_addr) {
    return sys_symlinkat(target_addr, AT_FDCWD_, link_addr);
}

dword_t sys_mknodat(fd_t at_f, addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("mknodat(%d, \"%s\", %#x, %#x)", at_f, path, mode, dev);
    apply_umask(&mode);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    return generic_mknodat(at, path, mode, dev);
}

dword_t sys_mknod(addr_t path_addr, mode_t_ mode, dev_t_ dev) {
    return sys_mknodat(AT_FDCWD_, path_addr, mode, dev);
}

#define SYSCALL_IO_BUFFER_SIZE (64 * 1024)
#define DIRECT_IO_MIN_SIZE (16 * 1024)
#define DIRECT_IOV_MAX 64

static _Thread_local char syscall_io_buffer[SYSCALL_IO_BUFFER_SIZE];

static int build_user_iov(addr_t addr, size_t size, int prot, struct iovec *iov, int *iovcnt) {
    addr_t p = addr;
    int count = 0;
    size_t remaining = size;
    while (remaining != 0 && count < DIRECT_IOV_MAX) {
        addr_t chunk_end = (PAGE(p) + 1) << PAGE_BITS;
        size_t chunk = chunk_end - p;
        if (chunk > remaining)
            chunk = remaining;
        void *ptr = mem_ptr(current->mem, p, prot);
        if (ptr == NULL)
            return _EFAULT;
        iov[count].iov_base = ptr;
        iov[count].iov_len = chunk;
        count++;
        p += chunk;
        remaining -= chunk;
    }
    *iovcnt = count;
    return 0;
}

static ssize_t sys_read_direct(struct fd *fd, addr_t buf_addr, size_t size) {
    if (!S_ISREG(fd->type) || fd->ops->readv == NULL || size < DIRECT_IO_MIN_SIZE)
        return _ENOSYS;

    ssize_t total = 0;
    while ((size_t) total < size) {
        struct iovec iov[DIRECT_IOV_MAX];
        int iovcnt = 0;
        read_wrlock(&current->mem->lock);
        int err = build_user_iov(buf_addr + total, size - total, MEM_WRITE, iov, &iovcnt);
        if (err < 0) {
            read_wrunlock(&current->mem->lock);
            return total != 0 ? total : err;
        }
        ssize_t res = fd->ops->readv(fd, iov, iovcnt);
        read_wrunlock(&current->mem->lock);
        if (res <= 0)
            return total != 0 ? total : res;
        total += res;
        if (res < (ssize_t) iov[0].iov_len || iovcnt == 1)
            break;
    }
    return total;
}

static ssize_t sys_write_direct(struct fd *fd, addr_t buf_addr, size_t size) {
    if (!S_ISREG(fd->type) || fd->ops->writev == NULL || size < DIRECT_IO_MIN_SIZE)
        return _ENOSYS;

    ssize_t total = 0;
    while ((size_t) total < size) {
        struct iovec iov[DIRECT_IOV_MAX];
        int iovcnt = 0;
        read_wrlock(&current->mem->lock);
        int err = build_user_iov(buf_addr + total, size - total, MEM_READ, iov, &iovcnt);
        if (err < 0) {
            read_wrunlock(&current->mem->lock);
            return total != 0 ? total : err;
        }
        ssize_t res = fd->ops->writev(fd, iov, iovcnt);
        read_wrunlock(&current->mem->lock);
        if (res <= 0)
            return total != 0 ? total : res;
        total += res;
        if (res < (ssize_t) iov[0].iov_len || iovcnt == 1)
            break;
    }
    return total;
}

static ssize_t sys_read_fd(struct fd *fd, void *buf, size_t size) {
    if (S_ISDIR(fd->type))
        return _EISDIR;

    ssize_t res;
    if (fd->ops->read) {
        res = fd->ops->read(fd, buf, size);
    } else if (fd->ops->pread) {
        res = fd->ops->pread(fd, buf, size, fd->offset);
        if (res > 0) {
            fd->ops->lseek(fd, res, LSEEK_CUR);
        }
    } else {
        return _EBADF;
    }

    if (res >= 0) {
        size_t print_size = res;
        if (print_size > 100) print_size = 100;
        STRACE(" \"%.*s\"", print_size, buf);
    }
    return res;
}

static ssize_t sys_read_buf(fd_t fd_no, void *buf, size_t size) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    return sys_read_fd(fd, buf, size);
}

dword_t sys_read(fd_t fd_no, addr_t buf_addr, dword_t size) {
    STRACE("read(%d, 0x%x, %d)", fd_no, buf_addr, size);
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    if (S_ISDIR(fd->type))
        return _EISDIR;
    if (fd->ops->zero_read)
        return user_memset_bytes(buf_addr, 0, size) ? _EFAULT : size;
    ssize_t direct_res = sys_read_direct(fd, buf_addr, size);
    if (direct_res != _ENOSYS)
        return direct_res;

    char *buf = syscall_io_buffer;
    if (size > SYSCALL_IO_BUFFER_SIZE)
        buf = malloc(size);
    if (buf == NULL)
        return _ENOMEM;
    int_t res = sys_read_fd(fd, buf, size);
    if (res >= 0) {
        if (user_write(buf_addr, buf, res))
            res = _EFAULT;
    }
    if (buf != syscall_io_buffer)
        free(buf);
    return res;
}

static ssize_t sys_write_fd(struct fd *fd, void *buf, size_t size) {
    ssize_t res;
    if (fd->ops->write) {
        res = fd->ops->write(fd, buf, size);
    } else if (fd->ops->pwrite) {
        res = fd->ops->pwrite(fd, buf, size, fd->offset);
        if (res > 0) {
            fd->ops->lseek(fd, res, LSEEK_CUR);
        }
    } else {
        return _EBADF;
    }
    return res;
}

static ssize_t sys_write_buf(fd_t fd_no, void *buf, size_t size) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    return sys_write_fd(fd, buf, size);
}

dword_t sys_write(fd_t fd_no, addr_t buf_addr, dword_t size) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops->discard_write)
        return sys_write_fd(fd, NULL, size);
    ssize_t direct_res = sys_write_direct(fd, buf_addr, size);
    if (direct_res != _ENOSYS)
        return direct_res;

    char *buf = syscall_io_buffer;
    if (size > SYSCALL_IO_BUFFER_SIZE)
        buf = malloc(size);
    if (buf == NULL)
        return _ENOMEM;
    dword_t res = _EFAULT;
    if (user_read(buf_addr, buf, size))
        goto out;

    size_t print_size = size;
    if (print_size > 100) print_size = 100;
    STRACE("write(%d, \"%.*s\", %d)", fd_no, print_size, buf, size);

    res = sys_write_fd(fd, buf, size);
out:
    if (buf != syscall_io_buffer)
        free(buf);
    return res;
}

// The vector operations work by flattening the vector into a malloc buffer.
// This at least isn't much worse than what it was before, which copied each
// element of the vector into a malloc buffer. The perfect solution would be to
// construct a vector with an entry for each page of the buffer. I haven't done
// that yet because it's more work and the efficiency gain from that is dwarfed
// by the inefficiency of the emulator.

#define GUEST_IOV_MAX 1024

static struct iovec_ *read_iovec(addr_t iovec_addr, unsigned iovec_count) {
    if (iovec_count > GUEST_IOV_MAX)
        return ERR_PTR(_EINVAL);
    size_t iovec_size = sizeof(struct iovec_) * iovec_count;
    struct iovec_ *iovec = malloc(iovec_size);
    if (iovec == NULL && iovec_size != 0)
        return ERR_PTR(_ENOMEM);
    if (user_read(iovec_addr, iovec, iovec_size)) {
        free(iovec);
        return ERR_PTR(_EFAULT);
    }
    return iovec;
}

static ssize_t iovec_size(struct iovec_ *iovec, unsigned iovec_count) {
    size_t size = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        if (iovec[i].len > SSIZE_MAX || size > SSIZE_MAX - (size_t) iovec[i].len)
            return _EINVAL;
        size += iovec[i].len;
    }
    return size;
}

dword_t sys_readv(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count) {
    STRACE("readv(%d, %#x, %d)", fd_no, iovec_addr, iovec_count);
    struct iovec_ *iovec = read_iovec(iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);
    ssize_t io_size_res = iovec_size(iovec, iovec_count);
    if (io_size_res < 0) {
        free(iovec);
        return io_size_res;
    }
    size_t io_size = io_size_res;
    char *buf = malloc(io_size);
    if (buf == NULL) {
        free(iovec);
        return _ENOMEM;
    }
    ssize_t res = sys_read_buf(fd_no, buf, io_size);
    if (res < 0)
        goto error;

    size_t offset = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        size_t print_size = iovec[i].len;
        if (print_size > 100) print_size = 100;
        STRACE(" {\"%.*s\", %zu}", print_size, buf + offset, (size_t) iovec[i].len);

        if (user_write(iovec[i].base, buf + offset, iovec[i].len)) {
            res = _EFAULT;
            goto error;
        }
        offset += iovec[i].len;
    }

error:
    free(buf);
    free(iovec);
    return res;
}

dword_t sys_writev(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count) {
    STRACE("writev(%d, %#x, %d)", fd_no, iovec_addr, iovec_count);
    struct iovec_ *iovec = read_iovec(iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);
    ssize_t io_size_res = iovec_size(iovec, iovec_count);
    if (io_size_res < 0) {
        free(iovec);
        return io_size_res;
    }
    size_t io_size = io_size_res;
    char *buf = malloc(io_size);
    if (buf == NULL) {
        free(iovec);
        return _ENOMEM;
    }

    ssize_t res = 0;
    size_t offset = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        if (user_read(iovec[i].base, buf + offset, iovec[i].len)) {
            res = _EFAULT;
            goto error;
        }

        size_t print_size = iovec[i].len;
        if (print_size > 100) print_size = 100;
        STRACE(" {\"%.*s\", %zu}", print_size, buf + offset, (size_t) iovec[i].len);
        offset += iovec[i].len;
    }
    res = sys_write_buf(fd_no, buf, io_size);

error:
    free(buf);
    free(iovec);
    return res;
}

dword_t sys__llseek(fd_t f, dword_t off_high, dword_t off_low, addr_t res_addr, dword_t whence) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (!fd->ops->lseek)
        return _ESPIPE;
    lock(&fd->lock);
    off_t_ off = ((qword_t) off_high << 32) | off_low;
    STRACE("llseek(%d, %lu, %#x, %d)", f, off, res_addr, whence);
    off_t_ res = fd->ops->lseek(fd, off, whence);
    STRACE(" -> %lu", res);
    unlock(&fd->lock);
    if (res < 0)
        return res;
    if (user_put(res_addr, res))
        return _EFAULT;
    return 0;
}

dword_t sys_lseek(fd_t f, dword_t off, dword_t whence) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (!fd->ops->lseek)
        return _ESPIPE;
    lock(&fd->lock);
    off_t res = fd->ops->lseek(fd, off, whence);
    unlock(&fd->lock);
    if ((dword_t) res != res)
        return _EOVERFLOW;
    return res;
}

dword_t sys_pread(fd_t f, addr_t buf_addr, dword_t size, off_t_ off) {
    STRACE("pread(%d, 0x%x, %d, %d)", f, buf_addr, size, off);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    char *buf = syscall_io_buffer;
    if (size > SYSCALL_IO_BUFFER_SIZE)
        buf = malloc(size);
    if (buf == NULL)
        return _ENOMEM;
    lock(&fd->lock);
    ssize_t res;
    if (fd->ops->pread) {
        res = fd->ops->pread(fd, buf, size, off);
    } else {
        off_t_ saved_off = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if ((res = fd->ops->lseek(fd, off, LSEEK_SET)) < 0) {
            goto out;
        }
        res = fd->ops->read(fd, buf, size);
        // This really shouldn't fail. The lseek man page lists these reasons:
        // EBADF, ESPIPE: can't happen because the last lseek wouldn't have succeeded.
        // EOVERFLOW: can't happen for LSEEK_SET.
        // EINVAL: can't happen other than typoing LSEEK_SET, because we know saved_off is not negative.
        off_t_ lseek_res = fd->ops->lseek(fd, saved_off, LSEEK_SET);
        assert(lseek_res >= 0);
    }
    if (res >= 0) {
        size_t print_size = res;
        if (print_size > 100) print_size = 100;
        STRACE(" \"%.*s\"", print_size, buf);
        if (user_write(buf_addr, buf, res))
            res = _EFAULT;
    }
out:
    unlock(&fd->lock);
    if (buf != syscall_io_buffer)
        free(buf);
    return res;
}

dword_t sys_pwrite(fd_t f, addr_t buf_addr, dword_t size, off_t_ off) {
    STRACE("pwrite(%d, 0x%x, %d, %d)", f, buf_addr, size, off);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    char *buf = syscall_io_buffer;
    if (size > SYSCALL_IO_BUFFER_SIZE)
        buf = malloc(size);
    if (buf == NULL)
        return _ENOMEM;
    if (user_read(buf_addr, buf, size)) {
        if (buf != syscall_io_buffer)
            free(buf);
        return _EFAULT;
    }
    lock(&fd->lock);
    ssize_t res;
    if (fd->ops->pwrite) {
        res = fd->ops->pwrite(fd, buf, size, off);
    } else {
        off_t_ saved_off = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if ((res = fd->ops->lseek(fd, off, LSEEK_SET)) >= 0) {
            res = fd->ops->write(fd, buf, size);
            // This really shouldn't fail. The lseek man page lists these reasons:
            // EBADF, ESPIPE: can't happen because the last lseek wouldn't have succeeded.
            // EOVERFLOW: can't happen for LSEEK_SET.
            // EINVAL: can't happen other than typoing LSEEK_SET, because we know saved_off is not negative.
            off_t_ lseek_res = fd->ops->lseek(fd, saved_off, LSEEK_SET);
            assert(lseek_res >= 0);
        }
    }
    unlock(&fd->lock);
    if (buf != syscall_io_buffer)
        free(buf);
    return res;
}

dword_t sys_preadv(fd_t f, addr_t iovec_addr, dword_t iovec_count, off_t_ off) {
    STRACE("preadv(%d, %#x, %d, %lld)", f, iovec_addr, iovec_count, (long long) off);
    struct iovec_ *iovec = read_iovec(iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);

    ssize_t total = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        if (iovec[i].len > UINT32_MAX || off < 0) {
            total = _EINVAL;
            break;
        }
        dword_t res = sys_pread(f, iovec[i].base, (dword_t) iovec[i].len, off);
        if ((int_t) res < 0) {
            if (total == 0)
                total = (int_t) res;
            break;
        }
        total += res;
        off += res;
        if (res < iovec[i].len)
            break;
    }

    free(iovec);
    return total;
}

dword_t sys_pwritev(fd_t f, addr_t iovec_addr, dword_t iovec_count, off_t_ off) {
    STRACE("pwritev(%d, %#x, %d, %lld)", f, iovec_addr, iovec_count, (long long) off);
    struct iovec_ *iovec = read_iovec(iovec_addr, iovec_count);
    if (IS_ERR(iovec))
        return PTR_ERR(iovec);

    ssize_t total = 0;
    for (unsigned i = 0; i < iovec_count; i++) {
        if (iovec[i].len > UINT32_MAX || off < 0) {
            total = _EINVAL;
            break;
        }
        dword_t res = sys_pwrite(f, iovec[i].base, (dword_t) iovec[i].len, off);
        if ((int_t) res < 0) {
            if (total == 0)
                total = (int_t) res;
            break;
        }
        total += res;
        off += res;
        if (res < iovec[i].len)
            break;
    }

    free(iovec);
    return total;
}

static int fd_ioctl(struct fd *fd, dword_t cmd, dword_t arg) {
    ssize_t size = -1;
    if (fd->ops->ioctl_size)
        size = fd->ops->ioctl_size(cmd);
    if (size < 0)
        return _ENOTTY;
    if (size == 0)
        return fd->ops->ioctl(fd, cmd, (void *) (long) arg);

    // praying that this won't break
    char buf[size];
    if (user_read(arg, buf, size))
        return _EFAULT;
    int res = fd->ops->ioctl(fd, cmd, buf);
    if (res < 0)
        return res;
    if (user_write(arg, buf, size))
        return _EFAULT;
    return res;
}

static int set_nonblock(struct fd *fd, addr_t nb_addr) {
    dword_t nonblock;
    if (user_get(nb_addr, nonblock))
        return _EFAULT;
    int flags = fd_getflags(fd);
    if (nonblock)
        flags |= O_NONBLOCK_;
    else
        flags &= ~O_NONBLOCK_;
    return fd_setflags(fd, flags);
}

dword_t sys_ioctl(fd_t f, dword_t cmd, dword_t arg) {
    STRACE("ioctl(%d, 0x%x, 0x%x)", f, cmd, arg);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;

    switch (cmd) {
        case FIONBIO_:
            return set_nonblock(fd, arg);
        case FIOCLEX_:
            bit_set(f, current->files->cloexec);
            return 0;
        case FIONCLEX_:
            bit_clear(f, current->files->cloexec);
            return 0;
    }
    return fd_ioctl(fd, cmd, arg);
}

dword_t sys_getcwd(addr_t buf_addr, dword_t size) {
    STRACE("getcwd(%#x, %#x)", buf_addr, size);
    lock(&current->fs->lock);
    struct fd *wd = current->fs->pwd;
    char pwd[MAX_PATH + 1];
    int err = generic_getpath(wd, pwd);
    unlock(&current->fs->lock);
    if (err < 0)
        return err;

    if (strlen(pwd) + 1 > size)
        return _ERANGE;
    size = strlen(pwd) + 1;
    char *buf = malloc(size);
    if (buf == NULL)
        return _ENOMEM;
    strcpy(buf, pwd);
    STRACE(" \"%.*s\"", size, buf);
    dword_t res = size;
    if (user_write(buf_addr, buf, size))
        res = _EFAULT;
    free(buf);
    return res;
}

static struct fd *open_dir(const char *path) {
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, true);
    if (err < 0)
        return ERR_PTR(err);
    if (!(stat.mode & S_IFDIR))
        return ERR_PTR(_ENOTDIR);

    return generic_open(path, O_RDONLY_, 0);
}

void fs_chdir(struct fs_info *fs, struct fd *fd) {
    lock(&fs->lock);
    fd_close(fs->pwd);
    fs->pwd = fd;
    unlock(&fs->lock);
}

dword_t sys_chdir(addr_t path_addr) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("chdir(\"%s\")", path);

    struct fd *dir = open_dir(path);
    if (IS_ERR(dir))
        return PTR_ERR(dir);
    fs_chdir(current->fs, dir);
    return 0;
}

dword_t sys_fchdir(fd_t f) {
    STRACE("fchdir(%d)", f);
    struct fd *dir = f_get(f);
    if (dir == NULL)
        return _EBADF;
    dir->refcount++;
    fs_chdir(current->fs, dir);
    return 0;
}

dword_t sys_chroot(addr_t path_addr) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("chroot(\"%s\")", path);

    struct fd *dir = open_dir(path);
    if (IS_ERR(dir))
        return PTR_ERR(dir);
    lock(&current->fs->lock);
    fd_close(current->fs->root);
    current->fs->root = dir;
    unlock(&current->fs->lock);
    return 0;
}

dword_t sys_umask(dword_t mask) {
    STRACE("umask(0%o)", mask);
    struct fs_info *fs = current->fs;
    lock(&fs->lock);
    mode_t_ old_umask = fs->umask;
    fs->umask = ((mode_t_) mask) & 0777;
    unlock(&fs->lock);
    return old_umask;
}

static int mount_statfs(struct mount *mount, struct statfsbuf *stat) {
    int err = 0;
    if (mount->fs->statfs)
        err = mount->fs->statfs(mount, stat);
    if (stat->type == 0)
        stat->type = mount->fs->magic;
    return err;
}

static int_t statfs_mount(struct mount *mount, addr_t buf_addr) {
    struct statfsbuf buf = {};
    int err = mount_statfs(mount, &buf);
    if (err < 0)
        return err;
    struct statfs_ out_buf = {
        .type = buf.type,
        .bsize = buf.bsize,
        .blocks = buf.blocks,
        .bfree = buf.bfree,
        .bavail = buf.bavail,
        .files = buf.files,
        .ffree = buf.ffree,
        .fsid = buf.fsid,
        .namelen = buf.namelen,
        .frsize = buf.frsize,
        .flags = buf.flags,
    };
    if (user_put(buf_addr, out_buf))
        return _EFAULT;
    return 0;
}

static int_t statfs64_mount(struct mount *mount, addr_t buf_addr) {
    struct statfsbuf buf = {};
    int err = mount_statfs(mount, &buf);
    if (err < 0)
        return err;
    struct statfs64_ out_buf = {
        .type = buf.type,
        .bsize = buf.bsize,
        .blocks = buf.blocks,
        .bfree = buf.bfree,
        .bavail = buf.bavail,
        .files = buf.files,
        .ffree = buf.ffree,
        .fsid = buf.fsid,
        .namelen = buf.namelen,
        .frsize = buf.frsize,
        .flags = buf.flags,
    };
    if (user_put(buf_addr, out_buf))
        return _EFAULT;
    return 0;
}

dword_t sys_statfs(addr_t path_addr, addr_t buf_addr) {
    char path_raw[MAX_PATH];
    if (user_read_string(path_addr, path_raw, sizeof(path_raw)))
        return _EFAULT;
    STRACE("statfs(\"%s\", %#x)", path_raw, buf_addr);
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = mount_find(path);
    err = statfs_mount(mount, buf_addr);
    mount_release(mount);
    return err;
}

dword_t sys_statfs64(addr_t path_addr, dword_t buf_size, addr_t buf_addr) {
    char path_raw[MAX_PATH];
    if (user_read_string(path_addr, path_raw, sizeof(path_raw)))
        return _EFAULT;
    STRACE("statfs64(\"%s\", %d, %#x)", path_raw, buf_size, buf_addr);
    if (buf_size != sizeof(struct statfs64_))
        return _EINVAL;
    char path[MAX_PATH];
    int err = path_normalize(AT_PWD, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = mount_find(path);
    err = statfs64_mount(mount, buf_addr);
    mount_release(mount);
    return err;
}

dword_t sys_fstatfs(fd_t f, addr_t buf_addr) {
    return statfs_mount(f_get(f)->mount, buf_addr);
}

dword_t sys_fstatfs64(fd_t f, addr_t buf_addr) {
    return statfs64_mount(f_get(f)->mount, buf_addr);
}

dword_t sys_flock(fd_t f, dword_t operation) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    // TODO: POSIX doesn't allow flock to fail in this way. The check is here
    // because a segfault is worse.
    if (fd->mount->fs->flock == NULL)
        return _EBADF;
    return fd->mount->fs->flock(fd, operation);
}

static dword_t sys_utime_common(fd_t at_f, addr_t path_addr, struct timespec atime, struct timespec mtime, dword_t flags) {
    char path[MAX_PATH];
    if (path_addr != 0)
        if (user_read_string(path_addr, path, sizeof(path)))
            return _EFAULT;
    STRACE("utimensat(%d, %s, {{%d, %d}, {%d, %d}}, %d)", at_f, path,
            atime.tv_sec, atime.tv_nsec, mtime.tv_sec, mtime.tv_nsec, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;

    bool follow_links = flags & AT_SYMLINK_NOFOLLOW_ ? false : true;
    return generic_utime(at, path_addr != 0 ? path : ".", atime, mtime, follow_links);
}

dword_t sys_utimensat(fd_t at_f, addr_t path_addr, addr_t times_addr, dword_t flags) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        struct timespec_ times[2];
        if (user_get(times_addr, times))
            return _EFAULT;
        atime = convert_timespec(times[0]);
        mtime = convert_timespec(times[1]);
    }
    return sys_utime_common(at_f, path_addr, atime, mtime, flags);
}

dword_t sys_utimes(addr_t path_addr, addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        struct timeval_ times[2];
        if (user_get(times_addr, times))
            return _EFAULT;
        atime = convert_timeval(times[0]);
        mtime = convert_timeval(times[1]);
    }
    return sys_utime_common(AT_FDCWD_, path_addr, atime, mtime, 0);
}

dword_t sys_utime(addr_t path_addr, addr_t times_addr) {
    struct timespec atime;
    struct timespec mtime;
    if (times_addr == 0) {
        atime = mtime = timespec_now(CLOCK_REALTIME);
    } else {
        struct utimbuf_ {
            time_t_ actime;
            time_t_ modtime;
        } times;
        if (user_get(times_addr, times))
            return _EFAULT;
        atime.tv_sec = times.actime;
        atime.tv_nsec = 0;
        mtime.tv_sec = times.modtime;
        mtime.tv_nsec = 0;
    }
    return sys_utime_common(AT_FDCWD_, path_addr, atime, mtime, 0);
}

static int generic_fsetattr(struct fd *fd, struct attr attr) {
    if (fd->mount->fs->fsetattr == NULL)
        return _EPERM;
    return fd->mount->fs->fsetattr(fd, attr);
}

dword_t sys_fchmod(fd_t f, dword_t mode) {
    STRACE("fchmod(%d, %o)", f, mode);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    mode &= ~S_IFMT;
    return generic_fsetattr(fd, make_attr(mode, mode));
}

dword_t sys_fchmodat2(fd_t at_f, addr_t path_addr, dword_t mode, dword_t flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("fchmodat2(%d, \"%s\", %o, %#x)", at_f, path, mode, flags);
    if (flags & ~(AT_SYMLINK_NOFOLLOW_ | AT_EMPTY_PATH_))
        return _EINVAL;
    if (path[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        return sys_fchmod(at_f, mode);
    }
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    mode &= ~S_IFMT;
    return generic_setattrat(at, path, make_attr(mode, mode), !(flags & AT_SYMLINK_NOFOLLOW_));
}

dword_t sys_fchmodat(fd_t at_f, addr_t path_addr, dword_t mode) {
    return sys_fchmodat2(at_f, path_addr, mode, 0);
}

dword_t sys_chmod(addr_t path_addr, dword_t mode) {
    return sys_fchmodat(AT_FDCWD_, path_addr, mode);
}

dword_t sys_fchown32(fd_t f, uid_t_ owner, uid_t_ group) {
    STRACE("fchown(%d, %d, %d)", f, owner, group);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    int err;
    if (owner != (uid_t) -1) {
        err = generic_fsetattr(fd, make_attr(uid, owner));
        if (err < 0)
            return err;
    }
    if (group != (uid_t) -1) {
        err = generic_fsetattr(fd, make_attr(gid, group));
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_fchownat(fd_t at_f, addr_t path_addr, dword_t owner, dword_t group, int flags) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("fchownat(%d, \"%s\", %d, %d, %d)", at_f, path, owner, group, flags);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    int err;
    bool follow_links = flags & AT_SYMLINK_NOFOLLOW_ ? false : true;
    if (owner != (uid_t) -1) {
        err = generic_setattrat(at, path, make_attr(uid, owner), follow_links);
        if (err < 0)
            return err;
    }
    if (group != (uid_t) -1) {
        err = generic_setattrat(at, path, make_attr(gid, group), follow_links);
        if (err < 0)
            return err;
    }
    return 0;
}

dword_t sys_chown32(addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat(AT_FDCWD_, path_addr, owner, group, 0);
}

dword_t sys_lchown(addr_t path_addr, uid_t_ owner, uid_t_ group) {
    return sys_fchownat(AT_FDCWD_, path_addr, owner, group, AT_SYMLINK_NOFOLLOW_);
}

dword_t sys_truncate64(addr_t path_addr, dword_t size_low, dword_t size_high) {
    off_t_ size = ((qword_t) size_high << 32) | size_low;
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    return generic_setattrat(NULL, path, make_attr(size, size), true);
}

dword_t sys_ftruncate64(fd_t f, dword_t size_low, dword_t size_high) {
    off_t_ size = ((qword_t) size_high << 32) | size_low;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    return generic_fsetattr(fd, make_attr(size, size));
}

dword_t sys_fallocate(fd_t f, dword_t UNUSED(mode), dword_t offset_low, dword_t offset_high, dword_t len_low, dword_t len_high) {
    off_t_ offset = ((qword_t) offset_high << 32) | offset_low;
    off_t_ len = ((qword_t) len_high << 32) | len_low;
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    struct statbuf statbuf;
    int err = fd->mount->fs->fstat(fd, &statbuf);
    if (err < 0)
        return err;
    if ((uint64_t) offset + (uint64_t) len > statbuf.size)
        return generic_fsetattr(fd, make_attr(size, offset + len));
    return 0;
}

dword_t sys_mkdirat(fd_t at_f, addr_t path_addr, mode_t_ mode) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("mkdirat(%d, %s, 0%o)", at_f, path, mode);
    struct fd *at = at_fd(at_f);
    if (at == NULL)
        return _EBADF;
    apply_umask(&mode);
    mode &= 0777;
    return generic_mkdirat(at, path, mode);
}

dword_t sys_mkdir(addr_t path_addr, mode_t_ mode) {
    return sys_mkdirat(AT_FDCWD_, path_addr, mode);
}

dword_t sys_rmdir(addr_t path_addr) {
    char path[MAX_PATH];
    if (user_read_string(path_addr, path, sizeof(path)))
        return _EFAULT;
    STRACE("rmdir(%s)", path);
    return generic_rmdirat(AT_PWD, path);
}

dword_t sys_fsync(fd_t f) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    int err = 0;
    if (fd->ops->fsync)
        err = fd->ops->fsync(fd);
    return err;
}

// a few stubs
dword_t sys_sendfile(fd_t UNUSED(out_fd), fd_t UNUSED(in_fd), addr_t UNUSED(offset_addr), dword_t UNUSED(count)) {
    return _EINVAL;
}
dword_t sys_sendfile64(fd_t UNUSED(out_fd), fd_t UNUSED(in_fd), addr_t UNUSED(offset_addr), dword_t UNUSED(count)) {
    return _EINVAL;
}
dword_t sys_splice(fd_t UNUSED(in_fd), addr_t UNUSED(in_off_addr), fd_t UNUSED(out_fd), addr_t UNUSED(out_off_addr), dword_t UNUSED(count), dword_t UNUSED(flags)) {
    return _EINVAL;
}
dword_t sys_copy_file_range(fd_t UNUSED(in_fd), addr_t UNUSED(in_off), fd_t UNUSED(out_fd),
        addr_t UNUSED(out_off), dword_t UNUSED(len), uint_t UNUSED(flags)) {
    return _EPERM; // good enough for ruby
}

dword_t sys_xattr_stub(addr_t UNUSED(path_addr), addr_t UNUSED(name_addr),
        addr_t UNUSED(value_addr), dword_t UNUSED(size), dword_t UNUSED(flags)) {
    return _ENOTSUP;
}
