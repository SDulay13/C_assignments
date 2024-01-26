#include "io61.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>
 
 
// io61_file
//    Data structure for io61 file wrappers. Add your own stuff.
 
struct io61_file {
    int fd = -1;     // file descriptor
    // `bufsiz` is the cache block size
    static constexpr off_t bufsize = 8192;
    // Cached data is stored in `cbuf`
    unsigned char cbuf[bufsize];
    int mode;

    // File offset of first byte of cached data (0 when file is opened).
    off_t tag;
    // File offset one past the last byte of cached data (0 when file is opened).
    off_t end_tag;
    // Cache position: file offset of the cache.
    off_t pos_tag;
};
 
// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file or O_WRONLY for a write-only file.
//    You need not support read/write files.
 
io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f -> mode = mode;
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
 
int io61_fill(io61_file* f) {
    // Fill the read cache with new data, starting from file offset `end_tag`.
    // Only called for read caches.
 
    // Reset the cache to empty.
    f->tag = f->pos_tag = f->end_tag;

    // Read data.
    ssize_t n = read(f->fd, f->cbuf, f->bufsize);
    if (n >= 0) {
        f->end_tag = f->tag + n;
        return 0;
    }
 
   // Check invariants.
   assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
   assert(f->end_tag - f->pos_tag <= f->bufsize);

   return -1;
}
 
 
// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.
 
int io61_readc(io61_file* f) {
    if (f->pos_tag == f->end_tag) {
        int result = io61_fill(f);
        if (result < 0 || f->pos_tag == f->end_tag) {
            return -1;
        }
    }
 
    unsigned char c = f->cbuf[f->pos_tag - f->tag];
    ++f->pos_tag;
    return c;
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
	// Check invariants.
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);

    ssize_t sz_read = sz;
    // initialize the number of bytes read	
	ssize_t pos = 0;
	
    while (pos < sz_read) {
        if (f->pos_tag == f->end_tag) {
            io61_fill(f);
            if (f->pos_tag == f->end_tag) {
                break;
            }
        }

		// check if enough space to offset
		if (f->pos_tag < f->end_tag) {
			// find remaining bytes
			ssize_t n = sz_read - pos;

			// check if bytes left exceeds space; readjust if necessary
			if (n > f->end_tag - f->pos_tag) {
				n = f->end_tag - f->pos_tag;
            }

			memcpy(&buf[pos], &f->cbuf[f->pos_tag - f->tag], n);
			f->pos_tag += n;
			pos += n;
		} 
	}
	return pos;
}
 
 
// io61_writec(f)
//    Write a single character `ch` to `f`. Returns 0 on success and
//    -1 on error.
 
int io61_writec(io61_file* f, int ch) {
   if (f->pos_tag == f->end_tag) {
       if(io61_flush(f) == -1) {
           // write error
           return -1;
       }
   }

   f->cbuf[f->pos_tag - f->tag] = ch;
   f->pos_tag++;
   return 0;
}
 
 
// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.
 
ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
	// Check invariants.
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);

    // check if mode is read only
	if (f->mode == O_RDONLY) {
		return -1;
    }
    
	ssize_t sz_write = sz;	
	ssize_t pos = 0;

	while (pos < sz_write) {

        // if the buffer is full, flush
		if (f->pos_tag == f->end_tag) {
            if(io61_flush(f) == -1) {
                // write error
                return -1;
            }
        }
		// check if there is space in the buffer to write data
		if (f->pos_tag - f->tag < f -> bufsize) {
			// calculate bytes left to write
			ssize_t n = sz_write - pos;

			// check if bytes left to write exceeds space
			if (n > f -> bufsize - (f->pos_tag - f->tag)) {
				n = f -> bufsize - (f->pos_tag - f->tag);
            }

			memcpy(&f->cbuf[f->pos_tag - f->tag], &buf[pos], n);
			f->pos_tag += n;
            pos += n;
			
            // adjust end if needed
			if (f->pos_tag > f->end_tag) {
				f->end_tag = f->pos_tag;
            }
		}
	}
	return pos;
}
 
 
// io61_flush(f)
//    Forces a write of any cached data written to `f`. Returns 0 on
//    success. Returns -1 if an error is encountered before all cached
//    data was written.
//
//    If `f` was opened read-only, `io61_flush(f)` returns 0. If may also
//    drop any data cached for reading.
 
int io61_flush(io61_file* f) {

    // Check invariants.
    assert(f->tag <= f->pos_tag && f->pos_tag <= f->end_tag);
    assert(f->end_tag - f->pos_tag <= f->bufsize);

    ssize_t nwanted = f->pos_tag - f->tag;
    ssize_t nwritten = 0;

    while (nwritten < nwanted) {
        // write from tag -> pos_tag
        ssize_t increment = write(f->fd, &f->cbuf[nwritten], nwanted - nwritten);
        if (increment >= 0) {
            nwritten += increment;
        }
        else if (errno != EINTR && errno != EAGAIN) {
            return -1;
        }
    }
    if (nwritten >= 0) {
       // update tags
       f->tag = f->pos_tag;
       f->end_tag += f -> bufsize;
       return 0;
    }
    if (nwritten != nwanted) {
        return -1;
    }
   return -1;
}
 
 
// io61_seek(f, pos)
//    Changes the file pointer for file `f` to `pos` bytes into the file.
//    Returns 0 on success and -1 on failure.
 
int io61_seek(io61_file* f, off_t pos) {
   if (f->mode == O_RDONLY) {
       int offset = pos % f->bufsize;
       if (pos >= f->tag && pos < f->end_tag) {
           f->pos_tag = pos;
           return 0;
       }
 
       off_t new_tag = lseek(f->fd, pos - offset, SEEK_SET);
       if (new_tag == -1) {
           return -1;
       }
 
       f->end_tag = new_tag;
       io61_fill(f);
       f->pos_tag += offset;
 
       return 0;
   }
   
   else if (f->mode == O_WRONLY) {
       io61_flush(f);
       off_t new_tag = lseek(f->fd, pos, SEEK_SET);
       if (new_tag == -1) {
           return -1;
       }
       f->pos_tag = f->tag = new_tag;
       f->end_tag = f->tag + f->bufsize;
 
       return 0;
   }
 
   return -1;
}
 
 
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
 

