#include "io61.hh"
#include <climits>
#include <cerrno>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <sys/types.h>
#include <sys/stat.h>
#include <thread>

// io61.cc
#define FINELOCKSIZE 16 // size of region locks

// io61_file
//    Data structure for io61 file wrappers.

struct map_region {
    int reg_num;            // region values from file_region()
    std::thread::id owner;  // owner of region
    unsigned locked;        // number of times locked
};

struct io61_file {
    int fd = -1;     // file descriptor
    int mode;        // O_RDONLY, O_WRONLY, or O_RDWR
    bool seekable;   // is this file seekable?

    // Single-slot cache
    static constexpr off_t cbufsz = 8192;
    unsigned char cbuf[cbufsz];
    off_t tag;       // offset of first character in `cbuf`
    off_t pos_tag;   // next offset to read or write (non-positioned mode)
    off_t end_tag;   // offset one past last valid character in `cbuf`

    // Positioned mode
    std::atomic<bool> dirty = false;       // has cache been written?
    bool positioned = false;  // is cache in positioned mode?
    std::recursive_mutex mutex;
    std::condition_variable_any cv;
    std::vector<map_region> rmap; // stores the region numbers, ID, and num of locks

};

// static int file_region(off)
//    calculates region number of file that off falls in

static int file_region(off_t off) {
    return (int) (off/FINELOCKSIZE);
}


// int find(map, n)
//    Returns the index of region that has region values from file_region() n.
//    Returns -1 if not found

int find(std::vector<map_region> map, int n) {
    for (int i = 0; (size_t)i < map.size(); i++) {
        if (map.at(i).reg_num == n) return i;
    }
    return -1;
}


// bool may_overlap_with_other_lock(f, start, len)
//    returns whether a new lock at [start, start+len) would overlap with
//    a lock from a different thread
//    overlapping locks in the same thread are fine; in this case, the
//    'locked' attribute would be >1

bool may_overlap_with_other_lock(io61_file* f, off_t start, off_t len) {
    int rstart = file_region(start);
    int rend = file_region(start + len - 1);

    for (int ri = rstart; ri <= rend; ++ri) {

        // find index of the region with reg_num ri
        int find_index = find(f->rmap, ri);
        if (find_index == -1) {
            // if doesn't exist (no overlap issue), insert 
            f->rmap.push_back({ri, std::this_thread::get_id(), 0});
        }
        else {
            auto region = f->rmap.at(find_index);
            if (region.locked > 0 && region.owner != std::this_thread::get_id()) {
                return true;
            }
        }
    }
    return false;
}


// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file, O_WRONLY for a write-only file,
//    or O_RDWR for a read/write file.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    assert((mode & O_APPEND) == 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f->mode = mode & O_ACCMODE;
    off_t off = lseek(fd, 0, SEEK_CUR);
    if (off != -1) {
        f->seekable = true;
        f->tag = f->pos_tag = f->end_tag = off;
    } else {
        f->seekable = false;
        f->tag = f->pos_tag = f->end_tag = 0;
    }
    f->dirty = f->positioned = false;
    return f;
}


// io61_close(f)
//    Closes the io61_file `f` and releases all its resources.

int io61_close(io61_file* f) {
    io61_flush(f);
    int r = close(f->fd);
    delete f;
    return r;
}


// NORMAL READING AND WRITING FUNCTIONS

// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.

static int io61_fill(io61_file* f);

int io61_readc(io61_file* f) {
    assert(!f->positioned);
    f->mutex.lock();
    if (f->pos_tag == f->end_tag) {
        io61_fill(f);
        if (f->pos_tag == f->end_tag) {
            return -1;
        }
    }
    unsigned char ch = f->cbuf[f->pos_tag - f->tag];
    ++f->pos_tag;
    f->mutex.unlock();
    return ch;
}


// io61_read(f, buf, sz)
//    Reads up to `sz` bytes from `f` into `buf`. Returns the number of
//    bytes read on success. Returns 0 if end-of-file is encountered before
//    any bytes are read, and -1 if an error is encountered before any
//    bytes are read.
//
//    Note that the return value might be positive, but less than `sz`,
//    if end-of-file or error is encountered before all `sz` bytes are read.
//    This is called a “short read.”

ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
    assert(!f->positioned);
    size_t nread = 0;
    // Modify the cache contents and/or position tag here
    f->mutex.lock();
    while (nread != sz) {
        if (f->pos_tag == f->end_tag) {
            int r = io61_fill(f);
            if (r == -1 && nread == 0) {
                return -1;
            } else if (f->pos_tag == f->end_tag) {
                break;
            }
        }
        size_t nleft = f->end_tag - f->pos_tag;
        size_t ncopy = std::min(sz - nread, nleft);
        memcpy(&buf[nread], &f->cbuf[f->pos_tag - f->tag], ncopy);
        nread += ncopy;
        f->pos_tag += ncopy;
    }     
    f->mutex.unlock();
    return nread;
}


// io61_writec(f)
//    Write a single character `c` to `f` (converted to unsigned char).
//    Returns 0 on success and -1 on error.

int io61_writec(io61_file* f, int c) {
    assert(!f->positioned);
    f->mutex.lock();
    if (f->pos_tag == f->tag + f->cbufsz) {
        int r = io61_flush(f);
        if (r == -1) {
            return -1;
        }
    }
    f->cbuf[f->pos_tag - f->tag] = c;
    ++f->pos_tag;
    ++f->end_tag;
    f->dirty = true;
    f->mutex.unlock();
    return 0;
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    assert(!f->positioned);
    size_t nwritten = 0;
    f->mutex.lock();
    while (nwritten != sz) {
        if (f->end_tag == f->tag + f->cbufsz) {
            int r = io61_flush(f);
            if (r == -1 && nwritten == 0) {
                return -1;
            } else if (r == -1) {
                break;
            }
        }
        size_t nleft = f->tag + f->cbufsz - f->pos_tag;
        size_t ncopy = std::min(sz - nwritten, nleft);
        memcpy(&f->cbuf[f->pos_tag - f->tag], &buf[nwritten], ncopy);
        f->pos_tag += ncopy;
        f->end_tag += ncopy;
        f->dirty = true;
        nwritten += ncopy;
    }
    f->mutex.unlock();
    return nwritten;
}


// io61_flush(f)
//    If `f` was opened for writes, `io61_flush(f)` forces a write of any
//    cached data written to `f`. Returns 0 on success; returns -1 if an error
//    is encountered before all cached data was written.
//
//    If `f` was opened read-only and is seekable, `io61_flush(f)` drops any
//    data cached for reading and seeks to the logical file position.

static int io61_flush_dirty(io61_file* f);
static int io61_flush_dirty_positioned(io61_file* f);
static int io61_flush_clean(io61_file* f);

int io61_flush(io61_file* f) {
    if (f->dirty && f->positioned) {
        return io61_flush_dirty_positioned(f);
    } else if (f->dirty) {
        return io61_flush_dirty(f);
    } else {
        return io61_flush_clean(f);
    }
}


// io61_seek(f, off)
//    Changes the file pointer for file `f` to `off` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t off) {
    f->mutex.lock();
    int r = io61_flush(f);
    if (r == -1) {
        return -1;
    }
    off_t roff = lseek(f->fd, off, SEEK_SET);
    if (roff == -1) {
        return -1;
    }
    f->tag = f->pos_tag = f->end_tag = off;
    f->positioned = false;
    f->mutex.unlock();
    return 0;
}


// Helper functions

// io61_fill(f)
//    Fill the cache by reading from the file. Returns 0 on success,
//    -1 on error. Used only for non-positioned files.

static int io61_fill(io61_file* f) {
    assert(f->tag == f->end_tag && f->pos_tag == f->end_tag);
    ssize_t nr;
    f->mutex.lock();
    while (true) {
        nr = read(f->fd, f->cbuf, f->cbufsz);
        if (nr >= 0) {
            break;
        } else if (errno != EINTR && errno != EAGAIN) {
            return -1;
        }
    }
    f->end_tag += nr;
    f->mutex.unlock();
    return 0;
}


// io61_flush_*(f)
//    Helper functions for io61_flush.

static int io61_flush_dirty(io61_file* f) {
    // Called when `f`’s cache is dirty and not positioned.
    // Uses `write`; assumes that the initial file position equals `f->tag`.
    off_t flush_tag = f->tag;
    f->mutex.lock();
    while (flush_tag != f->end_tag) {
        ssize_t nw = write(f->fd, &f->cbuf[flush_tag - f->tag],
                           f->end_tag - flush_tag);
        if (nw >= 0) {
            flush_tag += nw;
        } else if (errno != EINTR && errno != EINVAL) {
            return -1;
        }
    }
    f->dirty = false;
    f->tag = f->pos_tag = f->end_tag;
    f->mutex.unlock();
    return 0;
}

static int io61_flush_dirty_positioned(io61_file* f) {
    // Called when `f`’s cache is dirty and positioned.
    // Uses `pwrite`; does not change file position.
    f->mutex.lock();
    off_t flush_tag = f->tag;
    while (flush_tag != f->end_tag) {
        ssize_t nw = pwrite(f->fd, &f->cbuf[flush_tag - f->tag],
                            f->end_tag - flush_tag, flush_tag);
        if (nw >= 0) {
            flush_tag += nw;
        } else if (errno != EINTR && errno != EINVAL) {
            return -1;
        }
    }
    f->dirty = false;
    f->mutex.unlock();
    return 0;
}

static int io61_flush_clean(io61_file* f) {
    // Called when `f`’s cache is clean.
    f->mutex.lock();
    if (!f->positioned && f->seekable) {
        if (lseek(f->fd, f->pos_tag, SEEK_SET) == -1) {
            return -1;
        }
        f->tag = f->end_tag = f->pos_tag;
    }
    f->mutex.unlock();
    return 0;
}



// POSITIONED I/O FUNCTIONS

// io61_pread(f, buf, sz, off)
//    Read up to `sz` bytes from `f` into `buf`, starting at offset `off`.
//    Returns the number of characters read or -1 on error.
//
//    This function can only be called when `f` was opened in read/write
//    more (O_RDWR).

static int io61_pfill(io61_file* f, off_t off);

ssize_t io61_pread(io61_file* f, unsigned char* buf, size_t sz,
                   off_t off) {
    f->mutex.lock();
    if (!f->positioned || off < f->tag || off >= f->end_tag) {
        if (io61_pfill(f, off) == -1) {
            return -1;
        }
    }
    size_t nleft = f->end_tag - off;
    size_t ncopy = std::min(sz, nleft);
    memcpy(buf, &f->cbuf[off - f->tag], ncopy);
    f->mutex.unlock();
    return ncopy;
}


// io61_pwrite(f, buf, sz, off)
//    Write up to `sz` bytes from `buf` into `f`, starting at offset `off`.
//    Returns the number of characters written or -1 on error.
//
//    This function can only be called when `f` was opened in read/write
//    more (O_RDWR).

ssize_t io61_pwrite(io61_file* f, const unsigned char* buf, size_t sz,
                    off_t off) {
    f->mutex.lock();
    if (!f->positioned || off < f->tag || off >= f->end_tag) {
        if (io61_pfill(f, off) == -1) {
            return -1;
        }
    }
    size_t nleft = f->end_tag - off;
    size_t ncopy = std::min(sz, nleft);
    memcpy(&f->cbuf[off - f->tag], buf, ncopy);
    f->dirty = true;
    f->mutex.unlock();
    return ncopy;
}


// io61_pfill(f, off)
//    Fill the single-slot cache with data including offset `off`.
//    The handout code rounds `off` down to a multiple of 8192.

static int io61_pfill(io61_file* f, off_t off) {
    assert(f->mode == O_RDWR);
    f->mutex.lock();
    if (f->dirty && io61_flush(f) == -1) {
        return -1;
    }

    off = off - (off % 8192);
    ssize_t nr = pread(f->fd, f->cbuf, f->cbufsz, off);
    if (nr == -1) {
        return -1;
    }
    f->tag = off;
    f->end_tag = off + nr;
    f->positioned = true;
    f->mutex.unlock();
    return 0;
}



// FILE LOCKING FUNCTIONS

// io61_try_lock(f, start, len, locktype)
//    Attempts to acquire a lock on offsets `[start, len)` in file `f`.
//    `locktype` must be `LOCK_SH`, which requests a shared lock,
//    or `LOCK_EX`, which requests an exclusive lock.
//
//    Returns 0 if the lock was acquired and -1 if it was not. Does not
//    block: if the lock cannot be acquired, it returns -1 right away.

int io61_try_lock(io61_file* f, off_t start, off_t len, int locktype) {
    (void) f;
    assert(start >= 0 && len >= 0);
    assert(locktype == LOCK_EX || locktype == LOCK_SH);
    if (len == 0) {
        return 0;
    }
    std::unique_lock guard(f->mutex);
    if (may_overlap_with_other_lock(f, start, len)) {
        return -1;
    }
    for (int ri = file_region(start); ri <= file_region(start + len - 1); ++ri) {
        // find the index and element for each region.
        int find_index = find (f->rmap, ri);
        if(find_index != -1) {
            return -1;
        }
        map_region reg = f->rmap.at(find_index);

        // update local
        ++reg.locked;
        reg.owner = std::this_thread::get_id();

        // replace region
        f->rmap.erase(f->rmap.begin()+find_index);
        f->rmap.insert(f->rmap.end(),reg);
    }
    return 0;
}


// io61_lock(f, start, len, locktype)
//    Acquire a lock on offsets `[start, len)` in file `f`.
//    `locktype` must be `LOCK_SH`, which requests a shared lock,
//    or `LOCK_EX`, which requests an exclusive lock.
//
//    Returns 0 if the lock was acquired and -1 on error. Blocks until
//    the lock can be acquired; the -1 return value is reserved for true
//    error conditions, such as EDEADLK (a deadlock was detected).

int io61_lock(io61_file* f, off_t start, off_t len, int locktype) {
    assert(start >= 0 && len >= 0);
    assert(locktype == LOCK_EX || locktype == LOCK_SH);
    if (len == 0) {
        return 0;
    }
    std::unique_lock guard(f->mutex);
    while (may_overlap_with_other_lock(f, start, len)) {
        f->cv.wait(guard);
    }
    for (int ri = file_region(start); ri <= file_region(start + len - 1); ++ri) {
        // find the index as well as element for regions
        int find_index = find (f->rmap, ri);
        assert(find_index != -1); 
        map_region reg = f->rmap.at(find_index);

        // update reg
        ++reg.locked;
        reg.owner = std::this_thread::get_id();

        // replace region
        f->rmap.erase(f->rmap.begin()+find_index);
        f->rmap.insert(f->rmap.end(),reg);
    }
    return 0;
}


// io61_unlock(f, start, len)
//    Release the lock on offsets `[start,len)` in file `f`.
//    Returns 0 on success and -1 on error.

int io61_unlock(io61_file* f, off_t start, off_t len) {
    (void) f;
    assert(start >= 0 && len >= 0);
    if (len == 0) {
        return 0;
    }
    std::unique_lock guard(f->mutex);
    for (int ri = file_region(start); ri <= file_region(start + len - 1); ++ri) {
        // find the index
        int find_index = find (f->rmap, ri);
        if (find_index == -1) {
            return -1;
        }
        //find elements for regions
        map_region reg = f->rmap.at(find_index);

        if (reg.owner != std::this_thread::get_id()) {
            return -1;
        }
        if (reg.locked == 0) {
            return -1;
        }

        //update reg
        --reg.locked;

        f->rmap.erase(f->rmap.begin()+find_index);

        // region is fully unlocked.
        // otherwise only insert new region if locked dne 0
        if (reg.locked == 0) {
            f->cv.notify_all();
        }
        else {
            f->rmap.insert(f->rmap.end(),reg);
        }
    }
    return 0;
}



// HELPER FUNCTIONS
// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Opens the file corresponding to `filename` and returns its io61_file.
//    If `!filename`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != nullptr` and the named file cannot be opened.

io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_fileno(f)
//    Returns the file descriptor associated with `f`.

int io61_fileno(io61_file* f) {
    return f->fd;
}


// io61_filesize(f)
//    Returns the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).

off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}
