/* rio.c is a simple stream-oriented I/O abstraction that provides an interface
 * to write code that can consume/produce data using different concrete input
 * and output devices. For instance the same rdb.c code using the rio
 * abstraction can be used to read and write the RDB format using in-memory
 * buffers or files.
 *
 * A rio object provides the following methods:
 *  read: read from stream.
 *  write: write to stream.
 *  tell: get the current offset.
 *
 * It is also possible to set a 'checksum' method that is used by rio.c in order
 * to compute a checksum of the data written or read, or to query the rio object
 * for the current checksum.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-current, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "fmacros.h"
#include "dsa_batch_c.h"
#include "fpconv_dtoa.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "rio.h"
#include "util.h"
#include "crc64.h"
#include "config.h"
#include "server.h"
#include <libpmem.h>

/* ------------------------- Buffer I/O implementation ----------------------- */

/* Returns 1 or 0 for success/failure. */
static size_t rioBufferWrite(rio *r, const void *buf, size_t len) {
    r->io.buffer.ptr = sdscatlen(r->io.buffer.ptr,(char*)buf,len);
    r->io.buffer.pos += len;
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioBufferRead(rio *r, void *buf, size_t len) {
    if (sdslen(r->io.buffer.ptr)-r->io.buffer.pos < len)
        return 0; /* not enough buffer to return len bytes. */
    memcpy(buf,r->io.buffer.ptr+r->io.buffer.pos,len);
    r->io.buffer.pos += len;
    return 1;
}

/* Returns read/write position in buffer. */
static off_t rioBufferTell(rio *r) {
    return r->io.buffer.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioBufferFlush(rio *r) {
    UNUSED(r);
    return 1; /* Nothing to do, our write just appends to the buffer. */
}

static const rio rioBufferIO = {
    rioBufferRead,
    rioBufferWrite,
    rioBufferTell,
    rioBufferFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* flags */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

void rioInitWithBuffer(rio *r, sds s) {
    *r = rioBufferIO;
    r->io.buffer.ptr = s;
    r->io.buffer.pos = 0;
}

/* --------------------- Stdio file pointer implementation ------------------- */

/* Returns 1 or 0 for success/failure. */
static size_t rioFileWrite(rio *r, const void *buf, size_t len) {
    if (!r->io.file.autosync) return fwrite(buf,len,1,r->io.file.fp);

    size_t nwritten = 0;
    /* Incrementally write data to the file, avoid a single write larger than
     * the autosync threshold (so that the kernel's buffer cache never has too
     * many dirty pages at once). */
    while (len != nwritten) {
        serverAssert(r->io.file.autosync > r->io.file.buffered);
        size_t nalign = (size_t)(r->io.file.autosync - r->io.file.buffered);
        size_t towrite = nalign > len-nwritten ? len-nwritten : nalign;

        if (fwrite((char*)buf+nwritten,towrite,1,r->io.file.fp) == 0) return 0;
        nwritten += towrite;
        r->io.file.buffered += towrite;

        if (r->io.file.buffered >= r->io.file.autosync) {
            fflush(r->io.file.fp); 
            size_t processed = r->processed_bytes + nwritten;
            serverAssert(processed % r->io.file.autosync == 0);
            serverAssert(r->io.file.buffered == r->io.file.autosync);

#if HAVE_SYNC_FILE_RANGE
            /* Start writeout asynchronously. */
            if (sync_file_range(fileno(r->io.file.fp),
                    processed - r->io.file.autosync, r->io.file.autosync,
                    SYNC_FILE_RANGE_WRITE) == -1)
                return 0;

            if (processed >= (size_t)r->io.file.autosync * 2) {
                /* To keep the promise to 'autosync', we should make sure last
                 * asynchronous writeout persists into disk. This call may block
                 * if last writeout is not finished since disk is slow. */
                if (sync_file_range(fileno(r->io.file.fp),
                        processed - r->io.file.autosync*2,
                        r->io.file.autosync, SYNC_FILE_RANGE_WAIT_BEFORE|
                        SYNC_FILE_RANGE_WRITE|SYNC_FILE_RANGE_WAIT_AFTER) == -1)
                    return 0;
            }
#else
            if (redis_fsync(fileno(r->io.file.fp)) == -1) return 0;
#endif
            if (r->io.file.reclaim_cache) {
                /* In Linux sync_file_range just issue a writeback request to
                 * OS, and when posix_fadvise is called, the dirty page may
                 * still be in flushing, which means it would be ignored by
                 * posix_fadvise.
                 * 
                 * So we posix_fadvise the whole file, and the writeback-ed 
                 * pages will have other chances to be reclaimed. */
                reclaimFilePageCache(fileno(r->io.file.fp), 0, 0);
            }
            r->io.file.buffered = 0;
        }
    }
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioFileRead(rio *r, void *buf, size_t len) {
    return fread(buf,len,1,r->io.file.fp);
}

/* Returns read/write position in file. */
static off_t rioFileTell(rio *r) {
    return ftello(r->io.file.fp);
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioFileFlush(rio *r) {
    return (fflush(r->io.file.fp) == 0) ? 1 : 0;
}

static const rio rioFileIO = {
    rioFileRead,
    rioFileWrite,
    rioFileTell,
    rioFileFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* flags */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

void rioInitWithFile(rio *r, FILE *fp) {
    *r = rioFileIO;
    r->io.file.fp = fp;
    r->io.file.buffered = 0;
    r->io.file.autosync = 0;
    r->io.file.reclaim_cache = 0;
}

/* ------------------- Connection implementation -------------------
 * We use this RIO implementation when reading an RDB file directly from
 * the connection to the memory via rdbLoadRio(), thus this implementation
 * only implements reading from a connection that is, normally,
 * just a socket. */

static size_t rioConnWrite(rio *r, const void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not yet support writing. */
}

/* Returns 1 or 0 for success/failure. */
static size_t rioConnRead(rio *r, void *buf, size_t len) {
    size_t avail = sdslen(r->io.conn.buf)-r->io.conn.pos;

    /* If the buffer is too small for the entire request: realloc. */
    if (sdslen(r->io.conn.buf) + sdsavail(r->io.conn.buf) < len)
        r->io.conn.buf = sdsMakeRoomFor(r->io.conn.buf, len - sdslen(r->io.conn.buf));

    /* If the remaining unused buffer is not large enough: memmove so that we
     * can read the rest. */
    if (len > avail && sdsavail(r->io.conn.buf) < len - avail) {
        sdsrange(r->io.conn.buf, r->io.conn.pos, -1);
        r->io.conn.pos = 0;
    }

    /* Make sure the caller didn't request to read past the limit.
     * If they didn't we'll buffer till the limit, if they did, we'll
     * return an error. */
    if (r->io.conn.read_limit != 0 && r->io.conn.read_limit < r->io.conn.read_so_far + len) {
        errno = EOVERFLOW;
        return 0;
    }

    /* If we don't already have all the data in the sds, read more */
    while (len > sdslen(r->io.conn.buf) - r->io.conn.pos) {
        size_t buffered = sdslen(r->io.conn.buf) - r->io.conn.pos;
        size_t needs = len - buffered;
        /* Read either what's missing, or PROTO_IOBUF_LEN, the bigger of
         * the two. */
        size_t toread = needs < PROTO_IOBUF_LEN ? PROTO_IOBUF_LEN: needs;
        if (toread > sdsavail(r->io.conn.buf)) toread = sdsavail(r->io.conn.buf);
        if (r->io.conn.read_limit != 0 &&
            r->io.conn.read_so_far + buffered + toread > r->io.conn.read_limit)
        {
            toread = r->io.conn.read_limit - r->io.conn.read_so_far - buffered;
        }
        int retval = connRead(r->io.conn.conn,
                          (char*)r->io.conn.buf + sdslen(r->io.conn.buf),
                          toread);
        if (retval == 0) {
            return 0;
        } else if (retval < 0) {
            if (connLastErrorRetryable(r->io.conn.conn)) continue;
            if (errno == EWOULDBLOCK) errno = ETIMEDOUT;
            return 0;
        }
        sdsIncrLen(r->io.conn.buf, retval);
    }

    memcpy(buf, (char*)r->io.conn.buf + r->io.conn.pos, len);
    r->io.conn.read_so_far += len;
    r->io.conn.pos += len;
    return len;
}

/* Returns read/write position in file. */
static off_t rioConnTell(rio *r) {
    return r->io.conn.read_so_far;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioConnFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioConnWrite(r,NULL,0);
}

static const rio rioConnIO = {
    rioConnRead,
    rioConnWrite,
    rioConnTell,
    rioConnFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* flags */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

/* Create an RIO that implements a buffered read from an fd
 * read_limit argument stops buffering when the reaching the limit. */
void rioInitWithConn(rio *r, connection *conn, size_t read_limit) {
    *r = rioConnIO;
    r->io.conn.conn = conn;
    r->io.conn.pos = 0;
    r->io.conn.read_limit = read_limit;
    r->io.conn.read_so_far = 0;
    r->io.conn.buf = sdsnewlen(NULL, PROTO_IOBUF_LEN);
    sdsclear(r->io.conn.buf);
}

/* Release the RIO stream. Optionally returns the unread buffered data
 * when the SDS pointer 'remaining' is passed. */
void rioFreeConn(rio *r, sds *remaining) {
    if (remaining && (size_t)r->io.conn.pos < sdslen(r->io.conn.buf)) {
        if (r->io.conn.pos > 0) sdsrange(r->io.conn.buf, r->io.conn.pos, -1);
        *remaining = r->io.conn.buf;
    } else {
        sdsfree(r->io.conn.buf);
        if (remaining) *remaining = NULL;
    }
    r->io.conn.buf = NULL;
}

/* ------------------- File descriptor implementation ------------------
 * This target is used to write the RDB file to pipe, when the master just
 * streams the data to the replicas without creating an RDB on-disk image
 * (diskless replication option).
 * It only implements writes. */

/* Returns 1 or 0 for success/failure.
 *
 * When buf is NULL and len is 0, the function performs a flush operation
 * if there is some pending buffer, so this function is also used in order
 * to implement rioFdFlush(). */
static size_t rioFdWrite(rio *r, const void *buf, size_t len) {
    ssize_t retval;
    unsigned char *p = (unsigned char*) buf;
    int doflush = (buf == NULL && len == 0);

    /* For small writes, we rather keep the data in user-space buffer, and flush
     * it only when it grows. however for larger writes, we prefer to flush
     * any pre-existing buffer, and write the new one directly without reallocs
     * and memory copying. */
    if (len > PROTO_IOBUF_LEN) {
        /* First, flush any pre-existing buffered data. */
        if (sdslen(r->io.fd.buf)) {
            if (rioFdWrite(r, NULL, 0) == 0)
                return 0;
        }
        /* Write the new data, keeping 'p' and 'len' from the input. */
    } else {
        if (len) {
            r->io.fd.buf = sdscatlen(r->io.fd.buf,buf,len);
            if (sdslen(r->io.fd.buf) > PROTO_IOBUF_LEN)
                doflush = 1;
            if (!doflush)
                return 1;
        }
        /* Flushing the buffered data. set 'p' and 'len' accordingly. */
        p = (unsigned char*) r->io.fd.buf;
        len = sdslen(r->io.fd.buf);
    }

    size_t nwritten = 0;
    while(nwritten != len) {
        retval = write(r->io.fd.fd,p+nwritten,len-nwritten);
        if (retval <= 0) {
            if (retval == -1 && errno == EINTR) continue;
            /* With blocking io, which is the sole user of this
             * rio target, EWOULDBLOCK is returned only because of
             * the SO_SNDTIMEO socket option, so we translate the error
             * into one more recognizable by the user. */
            if (retval == -1 && errno == EWOULDBLOCK) errno = ETIMEDOUT;
            return 0; /* error. */
        }
        nwritten += retval;
    }

    r->io.fd.pos += len;
    sdsclear(r->io.fd.buf);
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioFdRead(rio *r, void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not support reading. */
}

/* Returns read/write position in file. */
static off_t rioFdTell(rio *r) {
    return r->io.fd.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
static int rioFdFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioFdWrite(r,NULL,0);
}

static const rio rioFdIO = {
    rioFdRead,
    rioFdWrite,
    rioFdTell,
    rioFdFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* flags */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

void rioInitWithFd(rio *r, int fd) {
    *r = rioFdIO;
    r->io.fd.fd = fd;
    r->io.fd.pos = 0;
    r->io.fd.buf = sdsempty();
}

/* release the rio stream. */
void rioFreeFd(rio *r) {
    sdsfree(r->io.fd.buf);
}


/* ---------------- Persist Memory File implementation ------------------
 * This target is used to write the RDB file to NVM.
 * It use DSA when possible to write the data to the NVM.  */

static int pm_file_extend( rio_pmem_t *handler , size_t extend_size ) { 
    size_t new_size = handler->file_size + handler->extend_size ;
    // size_t old_size = handler->file_size ;
    if( extend_size ) new_size = handler->file_size + extend_size ;
    if( handler->batch == NULL ) {
        pmem_drain() ;
    } else {
        DSAbatch_wait( handler->batch ) ;
    }
    pmem_unmap(handler->pmem_addr, handler->file_size);
    
    handler->pmem_addr = (char*)pmem_map_file(handler->file_path, new_size, 
                                PMEM_FILE_CREATE , 0666, &handler->file_size, &handler->is_pmem);
    if (handler->pmem_addr == NULL) {
        serverLog(LL_WARNING, "pmem_file %s extend failed", handler->file_path );
        return 0 ; // 扩展失败
    }
    // for( size_t i = old_size ; i < new_size ; i += 4096 ) { 
    //     handler->pmem_addr[i] = 0 ;
    // }
    // serverLog(LL_NOTICE, "pmem_file %s extend to %zu", handler->file_path, new_size ) ;
    return 1 ;
} 

static int pm_file_append( rio_pmem_t *handler , const void *data , size_t len , int use_cpu ) { 
    if (handler->used_size + len > handler->file_size) {
        size_t delta = ( len + handler->used_size + handler->extend_size - 1 ) / handler->extend_size * handler->extend_size - handler->file_size ;
        if ( pm_file_extend(handler, delta) == 0 ) {
            serverLog(LL_WARNING, "pmem_file %s append failed", handler->file_path );
            return 0 ; // 扩展失败
        }
    }
    if( handler->batch == NULL){
        memcpy(handler->pmem_addr + handler->used_size, data, len);
        pmem_flush( handler->pmem_addr + handler->used_size, len ); 
    } else if( use_cpu ){
        memcpy(handler->pmem_addr + handler->used_size, data, len);
        pmem_flush( handler->pmem_addr + handler->used_size, len );
    } else {
        // DSA submit
        DSAbatch_submit_memmove( handler->batch, handler->pmem_addr + handler->used_size, data, len );
    } 
    handler->used_size += len;
    return 1;
}

static int rioPmFlush(rio *r) ;
/* Returns 1 or 0 for success/failure. */
static size_t rioPmWrite(rio *r, const void *buf, size_t len) {
    // first fill the buffer 
    int buf_used = r->io.pmem_file.buffer_offset ;
    int this_write = 0 ;
    size_t buffer_write_len = 0 ;
#define USE_ALIGN
#if defined(USE_ALIGN)
    buffer_write_len = ( len + buf_used > PMEM_BUFFER_SIZE ) ? (size_t)PMEM_BUFFER_SIZE - buf_used : len ;
    if( len > 64 ){
        size_t align_need = 64 - ( buf_used + r->io.pmem_file.used_size ) % 64 ;
        buffer_write_len = align_need ; 
        this_write = 1 ;
    }
#endif
    // size_t buffer_write_len = 0 ;
    size_t direct_write_len = len - buffer_write_len ;
    if( buffer_write_len > 0 ) {
        memcpy( r->io.pmem_file.write_buffer + buf_used, buf, buffer_write_len );
        r->io.pmem_file.buffer_offset += buffer_write_len ;
    }
    if( this_write || r->io.pmem_file.buffer_offset >= PMEM_BUFFER_SIZE - 64 ) {
        if( pm_file_append(&r->io.pmem_file, r->io.pmem_file.write_buffer, r->io.pmem_file.buffer_offset , 1 ) == 0 ) {
            serverLog(LL_WARNING, "rioPmWrite %s failed: %s", r->io.pmem_file.file_path, strerror(errno));
            return 0;
        }
        r->io.pmem_file.buffer_offset = 0 ;
    }
    if( direct_write_len > 0 ) {
        if( pm_file_append(&r->io.pmem_file, (char*)buf + buffer_write_len, direct_write_len , 0 ) == 0 ) {
            serverLog(LL_WARNING, "rioPmWrite %s failed: %s", r->io.pmem_file.file_path, strerror(errno));
            return 0;
        }
    }
    return 1;
}

/* Returns 1 or 0 for success/failure. */
static size_t rioPmRead(rio *r, void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not support reading. */
}

/* Returns read/write position in file. */
static off_t rioPmTell(rio *r) {
    return r->io.pmem_file.used_size ;
}

static int rioPmFlush(rio *r) {
    if( r->io.pmem_file.buffer_offset > 0 ) {
        if( pm_file_append(&r->io.pmem_file, r->io.pmem_file.write_buffer, r->io.pmem_file.buffer_offset , 1 ) == 0 ) {
            serverLog(LL_WARNING, "rioPmFlush %s failed: %s", r->io.pmem_file.file_path, strerror(errno));
            return 0;
        }
        r->io.pmem_file.buffer_offset = 0 ;
    }
    pmem_drain() ;
    if( r->io.pmem_file.batch != NULL ) {
        DSAbatch_wait( r->io.pmem_file.batch ) ;  
    } 
    UNUSED(r);
    return 1; /* Nothing to do*/
}

static const rio rioPmIO = {
    rioPmRead,
    rioPmWrite,
    rioPmTell,
    rioPmFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* flags */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL } }    /* union for io-specific vars */
};

int rioInitWithPmFile(rio *r, const char* filename, int use_dsa ) { 
    *r = rioPmIO; 
    r->io.pmem_file.file_path = sdsnew(filename) ; 
    if( r->io.pmem_file.batch == NULL && use_dsa ) {
        serverLog(LL_NOTICE, "pmem_file %s enable dsa", r->io.pmem_file.file_path);
        r->io.pmem_file.batch = DSAbatch_create( 32 , 20 ) ;
    }
    r->io.pmem_file.used_size = 0 ;
    r->io.pmem_file.extend_size = 256 * 1024 * 1024 ; // 256MB
    r->io.pmem_file.pmem_addr = (char*)pmem_map_file(r->io.pmem_file.file_path , r->io.pmem_file.extend_size, 
                                 PMEM_FILE_CREATE, 0666, &r->io.pmem_file.file_size , &r->io.pmem_file.is_pmem);
    if (r->io.pmem_file.pmem_addr == NULL) {
        serverLog(LL_WARNING, "pmem file init %s failed: %s", filename, strerror(errno));
        sdsfree(r->io.pmem_file.file_path);
        return 0 ;
    }
    r->io.pmem_file.write_buffer = (char*)aligned_alloc(64, PMEM_BUFFER_SIZE); // buffer size
    r->io.pmem_file.buffer_offset = 0 ;
    return 1 ;
}

/* release the rio stream. */
void rioFreePm(rio *r) {
    if( r->io.pmem_file.pmem_addr ) { 
        if( r->io.pmem_file.batch != NULL ) {
            DSAbatch_printstats( r->io.pmem_file.batch ) ;
        }
        pmem_unmap(r->io.pmem_file.pmem_addr, r->io.pmem_file.file_size);
        int fd = open(r->io.pmem_file.file_path, O_RDWR);
        if (fd == -1){
            serverLog(LL_WARNING, "rioFreePm shrink %s failed: %s", r->io.pmem_file.file_path, strerror(errno));
            return ;
        }
        if (ftruncate(fd, r->io.pmem_file.used_size) != 0)
            serverLog(LL_WARNING, "rioFreePm shrink %s failed: %s", r->io.pmem_file.file_path, strerror(errno));
        close(fd);
        sdsfree( r->io.pmem_file.file_path );
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            free(r->io.pmem_file.write_buffer);
        #pragma GCC diagnostic pop 
    }
}


/* ---------------------------- Generic functions ---------------------------- */

/* This function can be installed both in memory and file streams when checksum
 * computation is needed. */
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len) {
    r->cksum = crc64(r->cksum,buf,len);
}

/* Set the file-based rio object to auto-fsync every 'bytes' file written.
 * By default this is set to zero that means no automatic file sync is
 * performed.
 *
 * This feature is useful in a few contexts since when we rely on OS write
 * buffers sometimes the OS buffers way too much, resulting in too many
 * disk I/O concentrated in very little time. When we fsync in an explicit
 * way instead the I/O pressure is more distributed across time. */
void rioSetAutoSync(rio *r, off_t bytes) {
    if(r->write != rioFileIO.write) return;
    r->io.file.autosync = bytes;
}

/* Set the file-based rio object to reclaim cache after every auto-sync.
 * In the Linux implementation POSIX_FADV_DONTNEED skips the dirty
 * pages, so if auto sync is unset this option will have no effect.
 * 
 * This feature can reduce the cache footprint backed by the file. */
void rioSetReclaimCache(rio *r, int enabled) {
    r->io.file.reclaim_cache = enabled;
}

/* Check the type of rio. */
uint8_t rioCheckType(rio *r) {
    if (r->read == rioFileRead) {
        return RIO_TYPE_FILE;
    } else if (r->read == rioBufferRead) {
        return RIO_TYPE_BUFFER;
    } else if (r->read == rioConnRead) {
        return RIO_TYPE_CONN;
    } else if (r->read == rioPmRead) {
        return RIO_TYPE_PMEM;
    } else {
        /* r->read == rioFdRead */
        return RIO_TYPE_FD;
    }
}

/* --------------------------- Higher level interface --------------------------
 *
 * The following higher level functions use lower level rio.c functions to help
 * generating the Redis protocol for the Append Only File. */

/* Write multi bulk count in the format: "*<count>\r\n". */
size_t rioWriteBulkCount(rio *r, char prefix, long count) {
    char cbuf[128];
    int clen;

    cbuf[0] = prefix;
    clen = 1+ll2string(cbuf+1,sizeof(cbuf)-1,count);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    if (rioWrite(r,cbuf,clen) == 0) return 0;
    return clen;
}

/* Write binary-safe string in the format: "$<count>\r\n<payload>\r\n". */
size_t rioWriteBulkString(rio *r, const char *buf, size_t len) {
    size_t nwritten;

    if ((nwritten = rioWriteBulkCount(r,'$',len)) == 0) return 0;
    if (len > 0 && rioWrite(r,buf,len) == 0) return 0;
    if (rioWrite(r,"\r\n",2) == 0) return 0;
    return nwritten+len+2;
}

/* Write a long long value in format: "$<count>\r\n<payload>\r\n". */
size_t rioWriteBulkLongLong(rio *r, long long l) {
    char lbuf[32];
    unsigned int llen;

    llen = ll2string(lbuf,sizeof(lbuf),l);
    return rioWriteBulkString(r,lbuf,llen);
}

/* Write a double value in the format: "$<count>\r\n<payload>\r\n" */
size_t rioWriteBulkDouble(rio *r, double d) {
    char dbuf[128];
    unsigned int dlen;
    dlen = fpconv_dtoa(d, dbuf);
    dbuf[dlen] = '\0';
    return rioWriteBulkString(r,dbuf,dlen);
}
