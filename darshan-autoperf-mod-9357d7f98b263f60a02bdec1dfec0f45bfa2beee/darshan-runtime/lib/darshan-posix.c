/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include "darshan-runtime-config.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <search.h>
#include <assert.h>
#include <libgen.h>
#include <aio.h>
#include <pthread.h>

#include "utlist.h"
#include "darshan.h"
#include "darshan-dynamic.h"

#ifndef HAVE_OFF64_T
typedef int64_t off64_t;
#endif
#ifndef HAVE_AIOCB64
#define aiocb64 aiocb
#endif


DARSHAN_FORWARD_DECL(open, int, (const char *path, int flags, ...));
DARSHAN_FORWARD_DECL(open64, int, (const char *path, int flags, ...));
DARSHAN_FORWARD_DECL(creat, int, (const char* path, mode_t mode));
DARSHAN_FORWARD_DECL(creat64, int, (const char* path, mode_t mode));
DARSHAN_FORWARD_DECL(mkstemp, int, (char *template));
DARSHAN_FORWARD_DECL(mkostemp, int, (char *template, int flags));
DARSHAN_FORWARD_DECL(mkstemps, int, (char *template, int suffixlen));
DARSHAN_FORWARD_DECL(mkostemps, int, (char *template, int suffixlen, int flags));
DARSHAN_FORWARD_DECL(read, ssize_t, (int fd, void *buf, size_t count));
DARSHAN_FORWARD_DECL(write, ssize_t, (int fd, const void *buf, size_t count));
DARSHAN_FORWARD_DECL(pread, ssize_t, (int fd, void *buf, size_t count, off_t offset));
DARSHAN_FORWARD_DECL(pwrite, ssize_t, (int fd, const void *buf, size_t count, off_t offset));
DARSHAN_FORWARD_DECL(pread64, ssize_t, (int fd, void *buf, size_t count, off64_t offset));
DARSHAN_FORWARD_DECL(pwrite64, ssize_t, (int fd, const void *buf, size_t count, off64_t offset));
DARSHAN_FORWARD_DECL(readv, ssize_t, (int fd, const struct iovec *iov, int iovcnt));
DARSHAN_FORWARD_DECL(writev, ssize_t, (int fd, const struct iovec *iov, int iovcnt));
DARSHAN_FORWARD_DECL(lseek, off_t, (int fd, off_t offset, int whence));
DARSHAN_FORWARD_DECL(lseek64, off64_t, (int fd, off64_t offset, int whence));
DARSHAN_FORWARD_DECL(__xstat, int, (int vers, const char* path, struct stat *buf));
DARSHAN_FORWARD_DECL(__xstat64, int, (int vers, const char* path, struct stat64 *buf));
DARSHAN_FORWARD_DECL(__lxstat, int, (int vers, const char* path, struct stat *buf));
DARSHAN_FORWARD_DECL(__lxstat64, int, (int vers, const char* path, struct stat64 *buf));
DARSHAN_FORWARD_DECL(__fxstat, int, (int vers, int fd, struct stat *buf));
DARSHAN_FORWARD_DECL(__fxstat64, int, (int vers, int fd, struct stat64 *buf));
#ifdef DARSHAN_WRAP_MMAP
DARSHAN_FORWARD_DECL(mmap, void*, (void *addr, size_t length, int prot, int flags, int fd, off_t offset));
DARSHAN_FORWARD_DECL(mmap64, void*, (void *addr, size_t length, int prot, int flags, int fd, off64_t offset));
#endif /* DARSHAN_WRAP_MMAP */
DARSHAN_FORWARD_DECL(fsync, int, (int fd));
DARSHAN_FORWARD_DECL(fdatasync, int, (int fd));
DARSHAN_FORWARD_DECL(close, int, (int fd));
DARSHAN_FORWARD_DECL(aio_read, int, (struct aiocb *aiocbp));
DARSHAN_FORWARD_DECL(aio_write, int, (struct aiocb *aiocbp));
DARSHAN_FORWARD_DECL(aio_read64, int, (struct aiocb64 *aiocbp));
DARSHAN_FORWARD_DECL(aio_write64, int, (struct aiocb64 *aiocbp));
DARSHAN_FORWARD_DECL(aio_return, ssize_t, (struct aiocb *aiocbp));
DARSHAN_FORWARD_DECL(aio_return64, ssize_t, (struct aiocb64 *aiocbp));
DARSHAN_FORWARD_DECL(lio_listio, int, (int mode, struct aiocb *const aiocb_list[], int nitems, struct sigevent *sevp));
DARSHAN_FORWARD_DECL(lio_listio64, int, (int mode, struct aiocb64 *const aiocb_list[], int nitems, struct sigevent *sevp));

/* The posix_file_record_ref structure maintains necessary runtime metadata
 * for the POSIX file record (darshan_posix_file structure, defined in
 * darshan-posix-log-format.h) pointed to by 'file_rec'. This metadata
 * assists with the instrumenting of specific statistics in the file record.
 *
 * RATIONALE: the POSIX module needs to track some stateful, volatile 
 * information about each open file (like the current file offset, most recent 
 * access time, etc.) to aid in instrumentation, but this information can't be
 * stored in the darshan_posix_file struct because we don't want it to appear in
 * the final darshan log file.  We therefore associate a posix_file_record_ref
 * struct with each darshan_posix_file struct in order to track this information
 * (i.e., the mapping between posix_file_record_ref structs to darshan_posix_file
 * structs is one-to-one).
 *
 * NOTE: we use the 'darshan_record_ref' interface (in darshan-common) to
 * associate different types of handles with this posix_file_record_ref struct.
 * This allows us to index this struct (and the underlying file record) by using
 * either the corresponding Darshan record identifier (derived from the filename)
 * or by a generated file descriptor, for instance. Note that, while there should
 * only be a single Darshan record identifier that indexes a posix_file_record_ref,
 * there could be multiple open file descriptors that index it.
 */
struct posix_file_record_ref
{
    struct darshan_posix_file *file_rec;
    int64_t offset;
    int64_t last_byte_read;
    int64_t last_byte_written;
    enum darshan_io_type last_io_type;
    double last_meta_end;
    double last_read_end;
    double last_write_end;
    void *access_root;
    int access_count;
    void *stride_root;
    int stride_count;
    struct posix_aio_tracker* aio_list;
    int fs_type; /* same as darshan_fs_info->fs_type */
};

/* The posix_runtime structure maintains necessary state for storing
 * POSIX file records and for coordinating with darshan-core at 
 * shutdown time.
 */
struct posix_runtime
{
    void *rec_id_hash;
    void *fd_hash;
    int file_rec_count;
};

/* struct to track information about aio operations in flight */
struct posix_aio_tracker
{
    double tm1;
    void *aiocbp;
    struct posix_aio_tracker *next;
};

static void posix_runtime_initialize(
    void);
static struct posix_file_record_ref *posix_track_new_file_record(
    darshan_record_id rec_id, const char *path);
static void posix_aio_tracker_add(
    int fd, void *aiocbp);
static struct posix_aio_tracker* posix_aio_tracker_del(
    int fd, void *aiocbp);
static void posix_finalize_file_records(
    void *rec_ref_p);
static void posix_record_reduction_op(
    void* infile_v, void* inoutfile_v, int *len, MPI_Datatype *datatype);
static void posix_shared_record_variance(
    MPI_Comm mod_comm, struct darshan_posix_file *inrec_array,
    struct darshan_posix_file *outrec_array, int shared_rec_count);
static void posix_cleanup_runtime(
    void);

static void posix_shutdown(
    MPI_Comm mod_comm, darshan_record_id *shared_recs,
    int shared_rec_count, void **posix_buf, int *posix_buf_sz);

/* extern DXT function defs */
extern void dxt_posix_write(darshan_record_id rec_id, int64_t offset,
    int64_t length, double start_time, double end_time);
extern void dxt_posix_read(darshan_record_id rec_id, int64_t offset,
    int64_t length, double start_time, double end_time);

static struct posix_runtime *posix_runtime = NULL;
static pthread_mutex_t posix_runtime_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int my_rank = -1;
static int darshan_mem_alignment = 1;
static int enable_dxt_io_trace = 0;

#define POSIX_LOCK() pthread_mutex_lock(&posix_runtime_mutex)
#define POSIX_UNLOCK() pthread_mutex_unlock(&posix_runtime_mutex)

#define POSIX_PRE_RECORD() do { \
    POSIX_LOCK(); \
    if(!darshan_core_disabled_instrumentation()) { \
        if(!posix_runtime) { \
            posix_runtime_initialize(); \
        } \
        if(posix_runtime) break; \
    } \
    POSIX_UNLOCK(); \
    return(ret); \
} while(0)

#define POSIX_POST_RECORD() do { \
    POSIX_UNLOCK(); \
} while(0)

#define POSIX_RECORD_OPEN(__ret, __path, __mode, __tm1, __tm2) do { \
    darshan_record_id rec_id; \
    struct posix_file_record_ref *rec_ref; \
    char *newpath; \
    if(__ret < 0) break; \
    newpath = darshan_clean_file_path(__path); \
    if(!newpath) newpath = (char *)__path; \
    if(darshan_core_excluded_path(newpath)) { \
        if(newpath != __path) free(newpath); \
        break; \
    } \
    rec_id = darshan_core_gen_record_id(newpath); \
    rec_ref = darshan_lookup_record_ref(posix_runtime->rec_id_hash, &rec_id, sizeof(darshan_record_id)); \
    if(!rec_ref) rec_ref = posix_track_new_file_record(rec_id, newpath); \
    if(!rec_ref) { \
        if(newpath != __path) free(newpath); \
        break; \
    } \
    if(__mode) \
        rec_ref->file_rec->counters[POSIX_MODE] = __mode; \
    rec_ref->offset = 0; \
    rec_ref->last_byte_written = 0; \
    rec_ref->last_byte_read = 0; \
    rec_ref->file_rec->counters[POSIX_OPENS] += 1; \
    if(rec_ref->file_rec->fcounters[POSIX_F_OPEN_START_TIMESTAMP] == 0 || \
     rec_ref->file_rec->fcounters[POSIX_F_OPEN_START_TIMESTAMP] > __tm1) \
        rec_ref->file_rec->fcounters[POSIX_F_OPEN_START_TIMESTAMP] = __tm1; \
    rec_ref->file_rec->fcounters[POSIX_F_OPEN_END_TIMESTAMP] = __tm2; \
    DARSHAN_TIMER_INC_NO_OVERLAP(rec_ref->file_rec->fcounters[POSIX_F_META_TIME], \
        __tm1, __tm2, rec_ref->last_meta_end); \
    darshan_add_record_ref(&(posix_runtime->fd_hash), &__ret, sizeof(int), rec_ref); \
    darshan_instrument_fs_data(rec_ref->fs_type, newpath, __ret); \
    if(newpath != __path) free(newpath); \
} while(0)

#define POSIX_RECORD_READ(__ret, __fd, __pread_flag, __pread_offset, __aligned, __tm1, __tm2) do { \
    struct posix_file_record_ref* rec_ref; \
    size_t stride; \
    int64_t this_offset; \
    int64_t file_alignment; \
    double __elapsed = __tm2-__tm1; \
    if(__ret < 0) break; \
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &(__fd), sizeof(int)); \
    if(!rec_ref) break; \
    if(__pread_flag) \
        this_offset = __pread_offset; \
    else \
        this_offset = rec_ref->offset; \
    /* DXT to record detailed read tracing information */ \
    if(enable_dxt_io_trace) { \
        dxt_posix_read(rec_ref->file_rec->base_rec.id, this_offset, __ret, __tm1, __tm2); \
    } \
    if(this_offset > rec_ref->last_byte_read) \
        rec_ref->file_rec->counters[POSIX_SEQ_READS] += 1;  \
    if(this_offset == (rec_ref->last_byte_read + 1)) \
        rec_ref->file_rec->counters[POSIX_CONSEC_READS] += 1;  \
    if(this_offset > 0 && this_offset > rec_ref->last_byte_read \
        && rec_ref->last_byte_read != 0) \
        stride = this_offset - rec_ref->last_byte_read - 1; \
    else \
        stride = 0; \
    rec_ref->last_byte_read = this_offset + __ret - 1; \
    rec_ref->offset = this_offset + __ret; \
    if(rec_ref->file_rec->counters[POSIX_MAX_BYTE_READ] < (this_offset + __ret - 1)) \
        rec_ref->file_rec->counters[POSIX_MAX_BYTE_READ] = (this_offset + __ret - 1); \
    rec_ref->file_rec->counters[POSIX_BYTES_READ] += __ret; \
    rec_ref->file_rec->counters[POSIX_READS] += 1; \
    DARSHAN_BUCKET_INC(&(rec_ref->file_rec->counters[POSIX_SIZE_READ_0_100]), __ret); \
    darshan_common_val_counter(&rec_ref->access_root, &rec_ref->access_count, __ret, \
        &(rec_ref->file_rec->counters[POSIX_ACCESS1_ACCESS]), \
        &(rec_ref->file_rec->counters[POSIX_ACCESS1_COUNT])); \
    darshan_common_val_counter(&rec_ref->stride_root, &rec_ref->stride_count, stride, \
        &(rec_ref->file_rec->counters[POSIX_STRIDE1_STRIDE]), \
        &(rec_ref->file_rec->counters[POSIX_STRIDE1_COUNT])); \
    if(!__aligned) \
        rec_ref->file_rec->counters[POSIX_MEM_NOT_ALIGNED] += 1; \
    file_alignment = rec_ref->file_rec->counters[POSIX_FILE_ALIGNMENT]; \
    if(file_alignment > 0 && (this_offset % file_alignment) != 0) \
        rec_ref->file_rec->counters[POSIX_FILE_NOT_ALIGNED] += 1; \
    if(rec_ref->last_io_type == DARSHAN_IO_WRITE) \
        rec_ref->file_rec->counters[POSIX_RW_SWITCHES] += 1; \
    rec_ref->last_io_type = DARSHAN_IO_READ; \
    if(rec_ref->file_rec->fcounters[POSIX_F_READ_START_TIMESTAMP] == 0 || \
     rec_ref->file_rec->fcounters[POSIX_F_READ_START_TIMESTAMP] > __tm1) \
        rec_ref->file_rec->fcounters[POSIX_F_READ_START_TIMESTAMP] = __tm1; \
    rec_ref->file_rec->fcounters[POSIX_F_READ_END_TIMESTAMP] = __tm2; \
    if(rec_ref->file_rec->fcounters[POSIX_F_MAX_READ_TIME] < __elapsed) { \
        rec_ref->file_rec->fcounters[POSIX_F_MAX_READ_TIME] = __elapsed; \
        rec_ref->file_rec->counters[POSIX_MAX_READ_TIME_SIZE] = __ret; } \
    DARSHAN_TIMER_INC_NO_OVERLAP(rec_ref->file_rec->fcounters[POSIX_F_READ_TIME], \
        __tm1, __tm2, rec_ref->last_read_end); \
} while(0)

#define POSIX_RECORD_WRITE(__ret, __fd, __pwrite_flag, __pwrite_offset, __aligned, __tm1, __tm2) do { \
    struct posix_file_record_ref* rec_ref; \
    size_t stride; \
    int64_t this_offset; \
    int64_t file_alignment; \
    double __elapsed = __tm2-__tm1; \
    if(__ret < 0) break; \
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &__fd, sizeof(int)); \
    if(!rec_ref) break; \
    if(__pwrite_flag) \
        this_offset = __pwrite_offset; \
    else \
        this_offset = rec_ref->offset; \
    /* DXT to record detailed write tracing information */ \
    if(enable_dxt_io_trace) { \
        dxt_posix_write(rec_ref->file_rec->base_rec.id, this_offset, __ret, __tm1, __tm2); \
    } \
    if(this_offset > rec_ref->last_byte_written) \
        rec_ref->file_rec->counters[POSIX_SEQ_WRITES] += 1; \
    if(this_offset == (rec_ref->last_byte_written + 1)) \
        rec_ref->file_rec->counters[POSIX_CONSEC_WRITES] += 1; \
    if(this_offset > 0 && this_offset > rec_ref->last_byte_written \
        && rec_ref->last_byte_written != 0) \
        stride = this_offset - rec_ref->last_byte_written - 1; \
    else \
        stride = 0; \
    rec_ref->last_byte_written = this_offset + __ret - 1; \
    rec_ref->offset = this_offset + __ret; \
    if(rec_ref->file_rec->counters[POSIX_MAX_BYTE_WRITTEN] < (this_offset + __ret - 1)) \
        rec_ref->file_rec->counters[POSIX_MAX_BYTE_WRITTEN] = (this_offset + __ret - 1); \
    rec_ref->file_rec->counters[POSIX_BYTES_WRITTEN] += __ret; \
    rec_ref->file_rec->counters[POSIX_WRITES] += 1; \
    DARSHAN_BUCKET_INC(&(rec_ref->file_rec->counters[POSIX_SIZE_WRITE_0_100]), __ret); \
    darshan_common_val_counter(&rec_ref->access_root, &rec_ref->access_count, __ret, \
        &(rec_ref->file_rec->counters[POSIX_ACCESS1_ACCESS]), \
        &(rec_ref->file_rec->counters[POSIX_ACCESS1_COUNT])); \
    darshan_common_val_counter(&rec_ref->stride_root, &rec_ref->stride_count, stride, \
        &(rec_ref->file_rec->counters[POSIX_STRIDE1_STRIDE]), \
        &(rec_ref->file_rec->counters[POSIX_STRIDE1_COUNT])); \
    if(!__aligned) \
        rec_ref->file_rec->counters[POSIX_MEM_NOT_ALIGNED] += 1; \
    file_alignment = rec_ref->file_rec->counters[POSIX_FILE_ALIGNMENT]; \
    if(file_alignment > 0 && (this_offset % file_alignment) != 0) \
        rec_ref->file_rec->counters[POSIX_FILE_NOT_ALIGNED] += 1; \
    if(rec_ref->last_io_type == DARSHAN_IO_READ) \
        rec_ref->file_rec->counters[POSIX_RW_SWITCHES] += 1; \
    rec_ref->last_io_type = DARSHAN_IO_WRITE; \
    if(rec_ref->file_rec->fcounters[POSIX_F_WRITE_START_TIMESTAMP] == 0 || \
     rec_ref->file_rec->fcounters[POSIX_F_WRITE_START_TIMESTAMP] > __tm1) \
        rec_ref->file_rec->fcounters[POSIX_F_WRITE_START_TIMESTAMP] = __tm1; \
    rec_ref->file_rec->fcounters[POSIX_F_WRITE_END_TIMESTAMP] = __tm2; \
    if(rec_ref->file_rec->fcounters[POSIX_F_MAX_WRITE_TIME] < __elapsed) { \
        rec_ref->file_rec->fcounters[POSIX_F_MAX_WRITE_TIME] = __elapsed; \
        rec_ref->file_rec->counters[POSIX_MAX_WRITE_TIME_SIZE] = __ret; } \
    DARSHAN_TIMER_INC_NO_OVERLAP(rec_ref->file_rec->fcounters[POSIX_F_WRITE_TIME], \
        __tm1, __tm2, rec_ref->last_write_end); \
} while(0)

#define POSIX_LOOKUP_RECORD_STAT(__path, __statbuf, __tm1, __tm2) do { \
    darshan_record_id rec_id; \
    struct posix_file_record_ref* rec_ref; \
    char *newpath = darshan_clean_file_path(__path); \
    if(!newpath) newpath = (char *)__path; \
    if(darshan_core_excluded_path(newpath)) { \
        if(newpath != __path) free(newpath); \
        break; \
    } \
    rec_id = darshan_core_gen_record_id(newpath); \
    rec_ref = darshan_lookup_record_ref(posix_runtime->rec_id_hash, &rec_id, sizeof(darshan_record_id)); \
    if(!rec_ref) rec_ref = posix_track_new_file_record(rec_id, newpath); \
    if(newpath != __path) free(newpath); \
    if(rec_ref) { \
        POSIX_RECORD_STAT(rec_ref, __statbuf, __tm1, __tm2); \
    } \
} while(0)

#define POSIX_RECORD_STAT(__rec_ref, __statbuf, __tm1, __tm2) do { \
    (__rec_ref)->file_rec->counters[POSIX_STATS] += 1; \
    DARSHAN_TIMER_INC_NO_OVERLAP((__rec_ref)->file_rec->fcounters[POSIX_F_META_TIME], \
        __tm1, __tm2, (__rec_ref)->last_meta_end); \
} while(0)


/**********************************************************
 *      Wrappers for POSIX I/O functions of interest      * 
 **********************************************************/

int DARSHAN_DECL(open)(const char *path, int flags, ...)
{
    int mode = 0;
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(open);

    if(flags & O_CREAT) 
    {
        va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, int);
        va_end(arg);

        tm1 = darshan_core_wtime();
        ret = __real_open(path, flags, mode);
        tm2 = darshan_core_wtime();
    }
    else
    {
        tm1 = darshan_core_wtime();
        ret = __real_open(path, flags);
        tm2 = darshan_core_wtime();
    }

    POSIX_PRE_RECORD();
    POSIX_RECORD_OPEN(ret, path, mode, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(open64)(const char *path, int flags, ...)
{
    int mode = 0;
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(open64);

    if(flags & O_CREAT)
    {
        va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, int);
        va_end(arg);

        tm1 = darshan_core_wtime();
        ret = __real_open64(path, flags, mode);
        tm2 = darshan_core_wtime();
    }
    else
    {
        tm1 = darshan_core_wtime();
        ret = __real_open64(path, flags);
        tm2 = darshan_core_wtime();
    }

    POSIX_PRE_RECORD();
    POSIX_RECORD_OPEN(ret, path, mode, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(creat)(const char* path, mode_t mode)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(creat);

    tm1 = darshan_core_wtime();
    ret = __real_creat(path, mode);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_OPEN(ret, path, mode, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(creat64)(const char* path, mode_t mode)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(creat64);

    tm1 = darshan_core_wtime();
    ret = __real_creat64(path, mode);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_OPEN(ret, path, mode, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(mkstemp)(char* template)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(mkstemp);

    tm1 = darshan_core_wtime();
    ret = __real_mkstemp(template);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_OPEN(ret, template, 0, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(mkostemp)(char* template, int flags)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(mkostemp);

    tm1 = darshan_core_wtime();
    ret = __real_mkostemp(template, flags);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_OPEN(ret, template, 0, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(mkstemps)(char* template, int suffixlen)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(mkstemps);

    tm1 = darshan_core_wtime();
    ret = __real_mkstemps(template, suffixlen);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_OPEN(ret, template, 0, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(mkostemps)(char* template, int suffixlen, int flags)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(mkostemps);

    tm1 = darshan_core_wtime();
    ret = __real_mkostemps(template, suffixlen, flags);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_OPEN(ret, template, 0, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(read)(int fd, void *buf, size_t count)
{
    ssize_t ret;
    int aligned_flag = 0;
    double tm1, tm2;

    MAP_OR_FAIL(read);

    if((unsigned long)buf % darshan_mem_alignment == 0) aligned_flag = 1;

    tm1 = darshan_core_wtime();
    ret = __real_read(fd, buf, count);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_READ(ret, fd, 0, 0, aligned_flag, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(write)(int fd, const void *buf, size_t count)
{
    ssize_t ret;
    int aligned_flag = 0;
    double tm1, tm2;

    MAP_OR_FAIL(write);

    if((unsigned long)buf % darshan_mem_alignment == 0) aligned_flag = 1;

    tm1 = darshan_core_wtime();
    ret = __real_write(fd, buf, count);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_WRITE(ret, fd, 0, 0, aligned_flag, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(pread)(int fd, void *buf, size_t count, off_t offset)
{
    ssize_t ret;
    int aligned_flag = 0;
    double tm1, tm2;

    MAP_OR_FAIL(pread);

    if((unsigned long)buf % darshan_mem_alignment == 0) aligned_flag = 1;

    tm1 = darshan_core_wtime();
    ret = __real_pread(fd, buf, count, offset);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_READ(ret, fd, 1, offset, aligned_flag, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(pwrite)(int fd, const void *buf, size_t count, off_t offset)
{
    ssize_t ret;
    int aligned_flag = 0;
    double tm1, tm2;

    MAP_OR_FAIL(pwrite);

    if((unsigned long)buf % darshan_mem_alignment == 0) aligned_flag = 1;

    tm1 = darshan_core_wtime();
    ret = __real_pwrite(fd, buf, count, offset);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_WRITE(ret, fd, 1, offset, aligned_flag, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(pread64)(int fd, void *buf, size_t count, off64_t offset)
{
    ssize_t ret;
    int aligned_flag = 0;
    double tm1, tm2;

    MAP_OR_FAIL(pread64);

    if((unsigned long)buf % darshan_mem_alignment == 0) aligned_flag = 1;

    tm1 = darshan_core_wtime();
    ret = __real_pread64(fd, buf, count, offset);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_READ(ret, fd, 1, offset, aligned_flag, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(pwrite64)(int fd, const void *buf, size_t count, off64_t offset)
{
    ssize_t ret;
    int aligned_flag = 0;
    double tm1, tm2;

    MAP_OR_FAIL(pwrite64);

    if((unsigned long)buf % darshan_mem_alignment == 0) aligned_flag = 1;

    tm1 = darshan_core_wtime();
    ret = __real_pwrite64(fd, buf, count, offset);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_WRITE(ret, fd, 1, offset, aligned_flag, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(readv)(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t ret;
    int aligned_flag = 1;
    int i;
    double tm1, tm2;

    MAP_OR_FAIL(readv);

    for(i=0; i<iovcnt; i++)
    {
        if(((unsigned long)iov[i].iov_base % darshan_mem_alignment) != 0)
            aligned_flag = 0;
    }

    tm1 = darshan_core_wtime();
    ret = __real_readv(fd, iov, iovcnt);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_READ(ret, fd, 0, 0, aligned_flag, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(writev)(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t ret;
    int aligned_flag = 1;
    int i;
    double tm1, tm2;

    MAP_OR_FAIL(writev);

    for(i=0; i<iovcnt; i++)
    {
        if(((unsigned long)iov[i].iov_base % darshan_mem_alignment) != 0)
            aligned_flag = 0;
    }

    tm1 = darshan_core_wtime();
    ret = __real_writev(fd, iov, iovcnt);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    POSIX_RECORD_WRITE(ret, fd, 0, 0, aligned_flag, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

off_t DARSHAN_DECL(lseek)(int fd, off_t offset, int whence)
{
    off_t ret;
    struct posix_file_record_ref *rec_ref;
    double tm1, tm2;

    MAP_OR_FAIL(lseek);

    tm1 = darshan_core_wtime();
    ret = __real_lseek(fd, offset, whence);
    tm2 = darshan_core_wtime();

    if(ret >= 0)
    {
        POSIX_PRE_RECORD();
        rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
        if(rec_ref)
        {
            rec_ref->offset = ret;
            DARSHAN_TIMER_INC_NO_OVERLAP(
                rec_ref->file_rec->fcounters[POSIX_F_META_TIME],
                tm1, tm2, rec_ref->last_meta_end);
            rec_ref->file_rec->counters[POSIX_SEEKS] += 1;
        }
        POSIX_POST_RECORD();
    }

    return(ret);
}

off_t DARSHAN_DECL(lseek64)(int fd, off_t offset, int whence)
{
    off_t ret;
    struct posix_file_record_ref *rec_ref;
    double tm1, tm2;

    MAP_OR_FAIL(lseek64);

    tm1 = darshan_core_wtime();
    ret = __real_lseek64(fd, offset, whence);
    tm2 = darshan_core_wtime();

    if(ret >= 0)
    {
        POSIX_PRE_RECORD();
        rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
        if(rec_ref)
        {
            rec_ref->offset = ret;
            DARSHAN_TIMER_INC_NO_OVERLAP(
                rec_ref->file_rec->fcounters[POSIX_F_META_TIME],
                tm1, tm2, rec_ref->last_meta_end);
            rec_ref->file_rec->counters[POSIX_SEEKS] += 1;
        }
        POSIX_POST_RECORD();
    }

    return(ret);
}

int DARSHAN_DECL(__xstat)(int vers, const char *path, struct stat *buf)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(__xstat);

    tm1 = darshan_core_wtime();
    ret = __real___xstat(vers, path, buf);
    tm2 = darshan_core_wtime();

    if(ret < 0 || !S_ISREG(buf->st_mode))
        return(ret);

    POSIX_PRE_RECORD();
    POSIX_LOOKUP_RECORD_STAT(path, buf, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(__xstat64)(int vers, const char *path, struct stat64 *buf)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(__xstat64);

    tm1 = darshan_core_wtime();
    ret = __real___xstat64(vers, path, buf);
    tm2 = darshan_core_wtime();

    if(ret < 0 || !S_ISREG(buf->st_mode))
        return(ret);

    POSIX_PRE_RECORD();
    POSIX_LOOKUP_RECORD_STAT(path, buf, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(__lxstat)(int vers, const char *path, struct stat *buf)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(__lxstat);

    tm1 = darshan_core_wtime();
    ret = __real___lxstat(vers, path, buf);
    tm2 = darshan_core_wtime();

    if(ret < 0 || !S_ISREG(buf->st_mode))
        return(ret);

    POSIX_PRE_RECORD();
    POSIX_LOOKUP_RECORD_STAT(path, buf, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(__lxstat64)(int vers, const char *path, struct stat64 *buf)
{
    int ret;
    double tm1, tm2;

    MAP_OR_FAIL(__lxstat64);

    tm1 = darshan_core_wtime();
    ret = __real___lxstat64(vers, path, buf);
    tm2 = darshan_core_wtime();

    if(ret < 0 || !S_ISREG(buf->st_mode))
        return(ret);

    POSIX_PRE_RECORD();
    POSIX_LOOKUP_RECORD_STAT(path, buf, tm1, tm2);
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(__fxstat)(int vers, int fd, struct stat *buf)
{
    int ret;
    struct posix_file_record_ref *rec_ref;
    double tm1, tm2;

    MAP_OR_FAIL(__fxstat);

    tm1 = darshan_core_wtime();
    ret = __real___fxstat(vers, fd, buf);
    tm2 = darshan_core_wtime();

    if(ret < 0 || !S_ISREG(buf->st_mode))
        return(ret);

    POSIX_PRE_RECORD();
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        POSIX_RECORD_STAT(rec_ref, buf, tm1, tm2);
    }
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(__fxstat64)(int vers, int fd, struct stat64 *buf)
{
    int ret;
    struct posix_file_record_ref *rec_ref;
    double tm1, tm2;

    MAP_OR_FAIL(__fxstat64);

    tm1 = darshan_core_wtime();
    ret = __real___fxstat64(vers, fd, buf);
    tm2 = darshan_core_wtime();

    if(ret < 0 || !S_ISREG(buf->st_mode))
        return(ret);

    POSIX_PRE_RECORD();
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        POSIX_RECORD_STAT(rec_ref, buf, tm1, tm2);
    }
    POSIX_POST_RECORD();

    return(ret);
}

#ifdef DARSHAN_WRAP_MMAP
void* DARSHAN_DECL(mmap)(void *addr, size_t length, int prot, int flags,
    int fd, off_t offset)
{
    void* ret;
    struct posix_file_record_ref *rec_ref;

    MAP_OR_FAIL(mmap);

    if(fd < 0 || (flags & MAP_ANONYMOUS))
    {
        /* mmap is not associated with a backing file; skip all Darshan
         * characterization attempts.
         */
        return(__real_mmap(addr, length, prot, flags, fd, offset));
    }

    ret = __real_mmap(addr, length, prot, flags, fd, offset);
    if(ret == MAP_FAILED)
        return(ret);

    POSIX_PRE_RECORD();
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        rec_ref->file_rec->counters[POSIX_MMAPS] += 1;
    }
    POSIX_POST_RECORD();

    return(ret);
}
#endif /* DARSHAN_WRAP_MMAP */

#ifdef DARSHAN_WRAP_MMAP
void* DARSHAN_DECL(mmap64)(void *addr, size_t length, int prot, int flags,
    int fd, off64_t offset)
{
    void* ret;
    struct posix_file_record_ref *rec_ref;

    MAP_OR_FAIL(mmap64);

    if(fd < 0 || (flags & MAP_ANONYMOUS))
    {
        /* mmap is not associated with a backing file; skip all Darshan
         * characterization attempts.
         */
        return(__real_mmap64(addr, length, prot, flags, fd, offset));
    }

    ret = __real_mmap64(addr, length, prot, flags, fd, offset);
    if(ret == MAP_FAILED)
        return(ret);

    POSIX_PRE_RECORD();
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        rec_ref->file_rec->counters[POSIX_MMAPS] += 1;
    }
    POSIX_POST_RECORD();

    return(ret);
}
#endif /* DARSHAN_WRAP_MMAP */

int DARSHAN_DECL(fsync)(int fd)
{
    int ret;
    struct posix_file_record_ref *rec_ref;
    double tm1, tm2;

    MAP_OR_FAIL(fsync);

    tm1 = darshan_core_wtime();
    ret = __real_fsync(fd);
    tm2 = darshan_core_wtime();

    if(ret < 0)
        return(ret);

    POSIX_PRE_RECORD();
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        DARSHAN_TIMER_INC_NO_OVERLAP(
            rec_ref->file_rec->fcounters[POSIX_F_WRITE_TIME],
            tm1, tm2, rec_ref->last_write_end);
        rec_ref->file_rec->counters[POSIX_FSYNCS] += 1;
    }
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(fdatasync)(int fd)
{
    int ret;
    struct posix_file_record_ref *rec_ref;
    double tm1, tm2;

    MAP_OR_FAIL(fdatasync);

    tm1 = darshan_core_wtime();
    ret = __real_fdatasync(fd);
    tm2 = darshan_core_wtime();

    if(ret < 0)
        return(ret);

    POSIX_PRE_RECORD();
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        DARSHAN_TIMER_INC_NO_OVERLAP(
            rec_ref->file_rec->fcounters[POSIX_F_WRITE_TIME],
            tm1, tm2, rec_ref->last_write_end);
        rec_ref->file_rec->counters[POSIX_FDSYNCS] += 1;
    }
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(close)(int fd)
{
    int ret;
    struct posix_file_record_ref *rec_ref;
    double tm1, tm2;

    MAP_OR_FAIL(close);

    tm1 = darshan_core_wtime();
    ret = __real_close(fd);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        rec_ref->last_byte_written = 0;
        rec_ref->last_byte_read = 0;
        if(rec_ref->file_rec->fcounters[POSIX_F_CLOSE_START_TIMESTAMP] == 0 ||
         rec_ref->file_rec->fcounters[POSIX_F_CLOSE_START_TIMESTAMP] > tm1)
           rec_ref->file_rec->fcounters[POSIX_F_CLOSE_START_TIMESTAMP] = tm1;
        rec_ref->file_rec->fcounters[POSIX_F_CLOSE_END_TIMESTAMP] = tm2;
        DARSHAN_TIMER_INC_NO_OVERLAP(
            rec_ref->file_rec->fcounters[POSIX_F_META_TIME],
            tm1, tm2, rec_ref->last_meta_end);
        darshan_delete_record_ref(&(posix_runtime->fd_hash), &fd, sizeof(int));
    }
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(aio_read)(struct aiocb *aiocbp)
{
    int ret;

    MAP_OR_FAIL(aio_read);

    ret = __real_aio_read(aiocbp);
    if(ret == 0)
    {
        POSIX_PRE_RECORD();
        posix_aio_tracker_add(aiocbp->aio_fildes, aiocbp);
        POSIX_POST_RECORD();
    }

    return(ret);
}

int DARSHAN_DECL(aio_write)(struct aiocb *aiocbp)
{
    int ret;

    MAP_OR_FAIL(aio_write);

    ret = __real_aio_write(aiocbp);
    if(ret == 0)
    {
        POSIX_PRE_RECORD();
        posix_aio_tracker_add(aiocbp->aio_fildes, aiocbp);
        POSIX_POST_RECORD();
    }

    return(ret);
}

int DARSHAN_DECL(aio_read64)(struct aiocb64 *aiocbp)
{
    int ret;

    MAP_OR_FAIL(aio_read64);

    ret = __real_aio_read64(aiocbp);
    if(ret == 0)
    {
        POSIX_PRE_RECORD();
        posix_aio_tracker_add(aiocbp->aio_fildes, aiocbp);
        POSIX_POST_RECORD();
    }

    return(ret);
}

int DARSHAN_DECL(aio_write64)(struct aiocb64 *aiocbp)
{
    int ret;

    MAP_OR_FAIL(aio_write64);

    ret = __real_aio_write64(aiocbp);
    if(ret == 0)
    {
        POSIX_PRE_RECORD();
        posix_aio_tracker_add(aiocbp->aio_fildes, aiocbp);
        POSIX_POST_RECORD();
    }

    return(ret);
}

ssize_t DARSHAN_DECL(aio_return)(struct aiocb *aiocbp)
{
    int ret;
    double tm2;
    struct posix_aio_tracker *tmp;
    int aligned_flag = 0;

    MAP_OR_FAIL(aio_return);

    ret = __real_aio_return(aiocbp);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    tmp = posix_aio_tracker_del(aiocbp->aio_fildes, aiocbp);
    if(tmp)
    {
        if((unsigned long)aiocbp->aio_buf % darshan_mem_alignment == 0)
            aligned_flag = 1;
        if(aiocbp->aio_lio_opcode == LIO_WRITE)
        {
            POSIX_RECORD_WRITE(ret, aiocbp->aio_fildes,
                1, aiocbp->aio_offset, aligned_flag,
                tmp->tm1, tm2);
        }
        else if(aiocbp->aio_lio_opcode == LIO_READ)
        {
            POSIX_RECORD_READ(ret, aiocbp->aio_fildes,
                1, aiocbp->aio_offset, aligned_flag,
                tmp->tm1, tm2);
        }
        free(tmp);
    }
    POSIX_POST_RECORD();

    return(ret);
}

ssize_t DARSHAN_DECL(aio_return64)(struct aiocb64 *aiocbp)
{
    int ret;
    double tm2;
    struct posix_aio_tracker *tmp;
    int aligned_flag = 0;

    MAP_OR_FAIL(aio_return64);

    ret = __real_aio_return64(aiocbp);
    tm2 = darshan_core_wtime();

    POSIX_PRE_RECORD();
    tmp = posix_aio_tracker_del(aiocbp->aio_fildes, aiocbp);
    if(tmp)
    {
        if((unsigned long)aiocbp->aio_buf % darshan_mem_alignment == 0)
            aligned_flag = 1;
        if(aiocbp->aio_lio_opcode == LIO_WRITE)
        {
            POSIX_RECORD_WRITE(ret, aiocbp->aio_fildes,
                1, aiocbp->aio_offset, aligned_flag,
                tmp->tm1, tm2);
        }
        else if(aiocbp->aio_lio_opcode == LIO_READ)
        {
            POSIX_RECORD_READ(ret, aiocbp->aio_fildes,
                1, aiocbp->aio_offset, aligned_flag,
                tmp->tm1, tm2);
        }
        free(tmp);
    }
    POSIX_POST_RECORD();

    return(ret);
}

int DARSHAN_DECL(lio_listio)(int mode, struct aiocb *const aiocb_list[],
    int nitems, struct sigevent *sevp)
{
    int ret;
    int i;

    MAP_OR_FAIL(lio_listio);

    ret = __real_lio_listio(mode, aiocb_list, nitems, sevp);
    if(ret == 0)
    {
        POSIX_PRE_RECORD();
        for(i = 0; i < nitems; i++)
        {
            posix_aio_tracker_add(aiocb_list[i]->aio_fildes, aiocb_list[i]);
        }
        POSIX_POST_RECORD();
    }

    return(ret);
}

int DARSHAN_DECL(lio_listio64)(int mode, struct aiocb64 *const aiocb_list[],
    int nitems, struct sigevent *sevp)
{
    int ret;
    int i;

    MAP_OR_FAIL(lio_listio64);

    ret = __real_lio_listio64(mode, aiocb_list, nitems, sevp);
    if(ret == 0)
    {
        POSIX_PRE_RECORD();
        for(i = 0; i < nitems; i++)
        {
            posix_aio_tracker_add(aiocb_list[i]->aio_fildes, aiocb_list[i]);
        }
        POSIX_POST_RECORD();
    }

    return(ret);
}

/**********************************************************
 * Internal functions for manipulating POSIX module state *
 **********************************************************/

/* initialize internal POSIX module data structures and register with darshan-core */
static void posix_runtime_initialize()
{
    int psx_buf_size;

    /* try and store a default number of records for this module */
    psx_buf_size = DARSHAN_DEF_MOD_REC_COUNT * sizeof(struct darshan_posix_file);

    /* register the POSIX module with darshan core */
    darshan_core_register_module(
        DARSHAN_POSIX_MOD,
        &posix_shutdown,
        &psx_buf_size,
        &my_rank,
        &darshan_mem_alignment);

    /* return if darshan-core does not provide enough module memory */
    if(psx_buf_size < sizeof(struct darshan_posix_file))
    {
        darshan_core_unregister_module(DARSHAN_POSIX_MOD);
        return;
    }

    posix_runtime = malloc(sizeof(*posix_runtime));
    if(!posix_runtime)
    {
        darshan_core_unregister_module(DARSHAN_POSIX_MOD);
        return;
    }
    memset(posix_runtime, 0, sizeof(*posix_runtime));

    /* check if DXT (Darshan extended tracing) should be enabled */
    if (getenv("DXT_ENABLE_IO_TRACE")) {
        enable_dxt_io_trace = 1;
    }

    return;
}

static struct posix_file_record_ref *posix_track_new_file_record(
    darshan_record_id rec_id, const char *path)
{
    struct darshan_posix_file *file_rec = NULL;
    struct posix_file_record_ref *rec_ref = NULL;
    struct darshan_fs_info fs_info;
    int ret;

    rec_ref = malloc(sizeof(*rec_ref));
    if(!rec_ref)
        return(NULL);
    memset(rec_ref, 0, sizeof(*rec_ref));

    /* add a reference to this file record based on record id */
    ret = darshan_add_record_ref(&(posix_runtime->rec_id_hash), &rec_id,
        sizeof(darshan_record_id), rec_ref);
    if(ret == 0)
    {
        free(rec_ref);
        return(NULL);
    }

    /* register the actual file record with darshan-core so it is persisted
     * in the log file
     */
    file_rec = darshan_core_register_record(
        rec_id,
        path,
        DARSHAN_POSIX_MOD,
        sizeof(struct darshan_posix_file),
        &fs_info);

    if(!file_rec)
    {
        darshan_delete_record_ref(&(posix_runtime->rec_id_hash),
            &rec_id, sizeof(darshan_record_id));
        free(rec_ref);
        return(NULL);
    }

    /* registering this file record was successful, so initialize some fields */
    file_rec->base_rec.id = rec_id;
    file_rec->base_rec.rank = my_rank;
    file_rec->counters[POSIX_MEM_ALIGNMENT] = darshan_mem_alignment;
    file_rec->counters[POSIX_FILE_ALIGNMENT] = fs_info.block_size;
#ifndef DARSHAN_WRAP_MMAP
    /* set invalid value here if MMAP instrumentation is disabled */
    file_rec->counters[POSIX_MMAPS] = -1;
#endif /* undefined DARSHAN_WRAP_MMAP */
    rec_ref->fs_type = fs_info.fs_type;
    rec_ref->file_rec = file_rec;
    posix_runtime->file_rec_count++;

    return(rec_ref);
}

/* finds the tracker structure for a given aio operation, removes it from
 * the associated linked list for this file record, and returns a pointer.  
 *
 * returns NULL if aio operation not found
 */
static struct posix_aio_tracker* posix_aio_tracker_del(int fd, void *aiocbp)
{
    struct posix_aio_tracker *tracker = NULL, *iter, *tmp;
    struct posix_file_record_ref *rec_ref;

    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        LL_FOREACH_SAFE(rec_ref->aio_list, iter, tmp)
        {
            if(iter->aiocbp == aiocbp)
            {
                LL_DELETE(rec_ref->aio_list, iter);
                tracker = iter;
                break;
            }
        }
    }

    return(tracker);
}

/* adds a tracker for the given aio operation */
static void posix_aio_tracker_add(int fd, void *aiocbp)
{
    struct posix_aio_tracker* tracker;
    struct posix_file_record_ref *rec_ref;

    rec_ref = darshan_lookup_record_ref(posix_runtime->fd_hash, &fd, sizeof(int));
    if(rec_ref)
    {
        tracker = malloc(sizeof(*tracker));
        if(tracker)
        {
            tracker->tm1 = darshan_core_wtime();
            tracker->aiocbp = aiocbp;
            LL_PREPEND(rec_ref->aio_list, tracker);
        }
    }

    return;
}

static void posix_finalize_file_records(void *rec_ref_p)
{
    struct posix_file_record_ref *rec_ref =
        (struct posix_file_record_ref *)rec_ref_p;

    tdestroy(rec_ref->access_root, free);
    tdestroy(rec_ref->stride_root, free);
    return;
}

static void posix_record_reduction_op(void* infile_v, void* inoutfile_v,
    int *len, MPI_Datatype *datatype)
{
    struct darshan_posix_file tmp_file;
    struct darshan_posix_file *infile = infile_v;
    struct darshan_posix_file *inoutfile = inoutfile_v;
    int i, j, k;

    for(i=0; i<*len; i++)
    {
        memset(&tmp_file, 0, sizeof(struct darshan_posix_file));
        tmp_file.base_rec.id = infile->base_rec.id;
        tmp_file.base_rec.rank = -1;

        /* sum */
        for(j=POSIX_OPENS; j<=POSIX_FDSYNCS; j++)
        {
            tmp_file.counters[j] = infile->counters[j] + inoutfile->counters[j];
        }

        tmp_file.counters[POSIX_MODE] = infile->counters[POSIX_MODE];

        /* sum */
        for(j=POSIX_BYTES_READ; j<=POSIX_BYTES_WRITTEN; j++)
        {
            tmp_file.counters[j] = infile->counters[j] + inoutfile->counters[j];
        }

        /* max */
        for(j=POSIX_MAX_BYTE_READ; j<=POSIX_MAX_BYTE_WRITTEN; j++)
        {
            tmp_file.counters[j] = (
                (infile->counters[j] > inoutfile->counters[j]) ?
                infile->counters[j] :
                inoutfile->counters[j]);
        }

        /* sum */
        for(j=POSIX_CONSEC_READS; j<=POSIX_MEM_NOT_ALIGNED; j++)
        {
            tmp_file.counters[j] = infile->counters[j] + inoutfile->counters[j];
        }

        tmp_file.counters[POSIX_MEM_ALIGNMENT] = infile->counters[POSIX_MEM_ALIGNMENT];

        /* sum */
        for(j=POSIX_FILE_NOT_ALIGNED; j<=POSIX_FILE_NOT_ALIGNED; j++)
        {
            tmp_file.counters[j] = infile->counters[j] + inoutfile->counters[j];
        }

        tmp_file.counters[POSIX_FILE_ALIGNMENT] = infile->counters[POSIX_FILE_ALIGNMENT];

        /* skip POSIX_MAX_*_TIME_SIZE; handled in floating point section */

        for(j=POSIX_SIZE_READ_0_100; j<=POSIX_SIZE_WRITE_1G_PLUS; j++)
        {
            tmp_file.counters[j] = infile->counters[j] + inoutfile->counters[j];
        }

        /* first collapse any duplicates */
        for(j=POSIX_STRIDE1_STRIDE; j<=POSIX_STRIDE4_STRIDE; j++)
        {
            for(k=POSIX_STRIDE1_STRIDE; k<=POSIX_STRIDE4_STRIDE; k++)
            {
                if(infile->counters[j] == inoutfile->counters[k])
                {
                    infile->counters[j+4] += inoutfile->counters[k+4];
                    inoutfile->counters[k] = 0;
                    inoutfile->counters[k+4] = 0;
                }
            }
        }

        /* first set */
        for(j=POSIX_STRIDE1_STRIDE; j<=POSIX_STRIDE4_STRIDE; j++)
        {
            DARSHAN_COMMON_VAL_COUNTER_INC(&(tmp_file.counters[POSIX_STRIDE1_STRIDE]),
                &(tmp_file.counters[POSIX_STRIDE1_COUNT]), infile->counters[j],
                infile->counters[j+4], 0);
        }
        /* second set */
        for(j=POSIX_STRIDE1_STRIDE; j<=POSIX_STRIDE4_STRIDE; j++)
        {
            DARSHAN_COMMON_VAL_COUNTER_INC(&(tmp_file.counters[POSIX_STRIDE1_STRIDE]),
                &(tmp_file.counters[POSIX_STRIDE1_COUNT]), inoutfile->counters[j],
                inoutfile->counters[j+4], 0);
        }

        /* same for access counts */

        /* first collapse any duplicates */
        for(j=POSIX_ACCESS1_ACCESS; j<=POSIX_ACCESS4_ACCESS; j++)
        {
            for(k=POSIX_ACCESS1_ACCESS; k<=POSIX_ACCESS4_ACCESS; k++)
            {
                if(infile->counters[j] == inoutfile->counters[k])
                {
                    infile->counters[j+4] += inoutfile->counters[k+4];
                    inoutfile->counters[k] = 0;
                    inoutfile->counters[k+4] = 0;
                }
            }
        }

        /* first set */
        for(j=POSIX_ACCESS1_ACCESS; j<=POSIX_ACCESS4_ACCESS; j++)
        {
            DARSHAN_COMMON_VAL_COUNTER_INC(&(tmp_file.counters[POSIX_ACCESS1_ACCESS]),
                &(tmp_file.counters[POSIX_ACCESS1_COUNT]), infile->counters[j],
                infile->counters[j+4], 0);
        }
        /* second set */
        for(j=POSIX_ACCESS1_ACCESS; j<=POSIX_ACCESS4_ACCESS; j++)
        {
            DARSHAN_COMMON_VAL_COUNTER_INC(&(tmp_file.counters[POSIX_ACCESS1_ACCESS]),
                &(tmp_file.counters[POSIX_ACCESS1_COUNT]), inoutfile->counters[j],
                inoutfile->counters[j+4], 0);
        }

        /* min non-zero (if available) value */
        for(j=POSIX_F_OPEN_START_TIMESTAMP; j<=POSIX_F_CLOSE_START_TIMESTAMP; j++)
        {
            if((infile->fcounters[j] < inoutfile->fcounters[j] &&
               infile->fcounters[j] > 0) || inoutfile->fcounters[j] == 0)
                tmp_file.fcounters[j] = infile->fcounters[j];
            else
                tmp_file.fcounters[j] = inoutfile->fcounters[j];
        }

        /* max */
        for(j=POSIX_F_OPEN_END_TIMESTAMP; j<=POSIX_F_CLOSE_END_TIMESTAMP; j++)
        {
            if(infile->fcounters[j] > inoutfile->fcounters[j])
                tmp_file.fcounters[j] = infile->fcounters[j];
            else
                tmp_file.fcounters[j] = inoutfile->fcounters[j];
        }

        /* sum */
        for(j=POSIX_F_READ_TIME; j<=POSIX_F_META_TIME; j++)
        {
            tmp_file.fcounters[j] = infile->fcounters[j] + inoutfile->fcounters[j];
        }

        /* max (special case) */
        if(infile->fcounters[POSIX_F_MAX_READ_TIME] >
            inoutfile->fcounters[POSIX_F_MAX_READ_TIME])
        {
            tmp_file.fcounters[POSIX_F_MAX_READ_TIME] =
                infile->fcounters[POSIX_F_MAX_READ_TIME];
            tmp_file.counters[POSIX_MAX_READ_TIME_SIZE] =
                infile->counters[POSIX_MAX_READ_TIME_SIZE];
        }
        else
        {
            tmp_file.fcounters[POSIX_F_MAX_READ_TIME] =
                inoutfile->fcounters[POSIX_F_MAX_READ_TIME];
            tmp_file.counters[POSIX_MAX_READ_TIME_SIZE] =
                inoutfile->counters[POSIX_MAX_READ_TIME_SIZE];
        }

        if(infile->fcounters[POSIX_F_MAX_WRITE_TIME] >
            inoutfile->fcounters[POSIX_F_MAX_WRITE_TIME])
        {
            tmp_file.fcounters[POSIX_F_MAX_WRITE_TIME] =
                infile->fcounters[POSIX_F_MAX_WRITE_TIME];
            tmp_file.counters[POSIX_MAX_WRITE_TIME_SIZE] =
                infile->counters[POSIX_MAX_WRITE_TIME_SIZE];
        }
        else
        {
            tmp_file.fcounters[POSIX_F_MAX_WRITE_TIME] =
                inoutfile->fcounters[POSIX_F_MAX_WRITE_TIME];
            tmp_file.counters[POSIX_MAX_WRITE_TIME_SIZE] =
                inoutfile->counters[POSIX_MAX_WRITE_TIME_SIZE];
        }

        /* min (zeroes are ok here; some procs don't do I/O) */
        if(infile->fcounters[POSIX_F_FASTEST_RANK_TIME] <
           inoutfile->fcounters[POSIX_F_FASTEST_RANK_TIME])
        {
            tmp_file.counters[POSIX_FASTEST_RANK] =
                infile->counters[POSIX_FASTEST_RANK];
            tmp_file.counters[POSIX_FASTEST_RANK_BYTES] =
                infile->counters[POSIX_FASTEST_RANK_BYTES];
            tmp_file.fcounters[POSIX_F_FASTEST_RANK_TIME] =
                infile->fcounters[POSIX_F_FASTEST_RANK_TIME];
        }
        else
        {
            tmp_file.counters[POSIX_FASTEST_RANK] =
                inoutfile->counters[POSIX_FASTEST_RANK];
            tmp_file.counters[POSIX_FASTEST_RANK_BYTES] =
                inoutfile->counters[POSIX_FASTEST_RANK_BYTES];
            tmp_file.fcounters[POSIX_F_FASTEST_RANK_TIME] =
                inoutfile->fcounters[POSIX_F_FASTEST_RANK_TIME];
        }

        /* max */
        if(infile->fcounters[POSIX_F_SLOWEST_RANK_TIME] >
           inoutfile->fcounters[POSIX_F_SLOWEST_RANK_TIME])
        {
            tmp_file.counters[POSIX_SLOWEST_RANK] =
                infile->counters[POSIX_SLOWEST_RANK];
            tmp_file.counters[POSIX_SLOWEST_RANK_BYTES] =
                infile->counters[POSIX_SLOWEST_RANK_BYTES];
            tmp_file.fcounters[POSIX_F_SLOWEST_RANK_TIME] =
                infile->fcounters[POSIX_F_SLOWEST_RANK_TIME];
        }
        else
        {
            tmp_file.counters[POSIX_SLOWEST_RANK] =
                inoutfile->counters[POSIX_SLOWEST_RANK];
            tmp_file.counters[POSIX_SLOWEST_RANK_BYTES] =
                inoutfile->counters[POSIX_SLOWEST_RANK_BYTES];
            tmp_file.fcounters[POSIX_F_SLOWEST_RANK_TIME] =
                inoutfile->fcounters[POSIX_F_SLOWEST_RANK_TIME];
        }

        /* update pointers */
        *inoutfile = tmp_file;
        inoutfile++;
        infile++;
    }

    return;
}

static void posix_shared_record_variance(MPI_Comm mod_comm,
    struct darshan_posix_file *inrec_array, struct darshan_posix_file *outrec_array,
    int shared_rec_count)
{
    MPI_Datatype var_dt;
    MPI_Op var_op;
    int i;
    struct darshan_variance_dt *var_send_buf = NULL;
    struct darshan_variance_dt *var_recv_buf = NULL;

    PMPI_Type_contiguous(sizeof(struct darshan_variance_dt),
        MPI_BYTE, &var_dt);
    PMPI_Type_commit(&var_dt);

    PMPI_Op_create(darshan_variance_reduce, 1, &var_op);

    var_send_buf = malloc(shared_rec_count * sizeof(struct darshan_variance_dt));
    if(!var_send_buf)
        return;

    if(my_rank == 0)
    {
        var_recv_buf = malloc(shared_rec_count * sizeof(struct darshan_variance_dt));

        if(!var_recv_buf)
            return;
    }

    /* get total i/o time variances for shared records */

    for(i=0; i<shared_rec_count; i++)
    {
        var_send_buf[i].n = 1;
        var_send_buf[i].S = 0;
        var_send_buf[i].T = inrec_array[i].fcounters[POSIX_F_READ_TIME] +
                            inrec_array[i].fcounters[POSIX_F_WRITE_TIME] +
                            inrec_array[i].fcounters[POSIX_F_META_TIME];
    }

    PMPI_Reduce(var_send_buf, var_recv_buf, shared_rec_count,
        var_dt, var_op, 0, mod_comm);

    if(my_rank == 0)
    {
        for(i=0; i<shared_rec_count; i++)
        {
            outrec_array[i].fcounters[POSIX_F_VARIANCE_RANK_TIME] =
                (var_recv_buf[i].S / var_recv_buf[i].n);
        }
    }

    /* get total bytes moved variances for shared records */

    for(i=0; i<shared_rec_count; i++)
    {
        var_send_buf[i].n = 1;
        var_send_buf[i].S = 0;
        var_send_buf[i].T = (double)
                            inrec_array[i].counters[POSIX_BYTES_READ] +
                            inrec_array[i].counters[POSIX_BYTES_WRITTEN];
    }

    PMPI_Reduce(var_send_buf, var_recv_buf, shared_rec_count,
        var_dt, var_op, 0, mod_comm);

    if(my_rank == 0)
    {
        for(i=0; i<shared_rec_count; i++)
        {
            outrec_array[i].fcounters[POSIX_F_VARIANCE_RANK_BYTES] =
                (var_recv_buf[i].S / var_recv_buf[i].n);
        }
    }

    PMPI_Type_free(&var_dt);
    PMPI_Op_free(&var_op);
    free(var_send_buf);
    free(var_recv_buf);

    return;
}

static void posix_cleanup_runtime()
{
    darshan_clear_record_refs(&(posix_runtime->fd_hash), 0);
    darshan_clear_record_refs(&(posix_runtime->rec_id_hash), 1);

    free(posix_runtime);
    posix_runtime = NULL;

    return;
}

/* posix module shutdown benchmark routine */
void darshan_posix_shutdown_bench_setup(int test_case)
{
    char filepath[256];
    int *fd_array;
    int64_t *size_array;
    int i;

    if(posix_runtime)
        posix_cleanup_runtime();

    posix_runtime_initialize();

    srand(my_rank);
    fd_array = malloc(1024 * sizeof(int));
    size_array = malloc(DARSHAN_COMMON_VAL_MAX_RUNTIME_COUNT * sizeof(int64_t));
    assert(fd_array && size_array);

    for(i = 0; i < 1024; i++)
        fd_array[i] = i;
    for(i = 0; i < DARSHAN_COMMON_VAL_MAX_RUNTIME_COUNT; i++)
        size_array[i] = rand();

    switch(test_case)
    {
        case 1: /* single file-per-process */
            snprintf(filepath, 256, "fpp-0_rank-%d", my_rank);
            
            POSIX_RECORD_OPEN(fd_array[0], filepath, 777, 0, 1);
            POSIX_RECORD_WRITE(size_array[0], fd_array[0], 0, 0, 1, 1, 2);

            break;
        case 2: /* single shared file */
            snprintf(filepath, 256, "shared-0");

            POSIX_RECORD_OPEN(fd_array[0], filepath, 777, 0, 1);
            POSIX_RECORD_WRITE(size_array[0], fd_array[0], 0, 0, 1, 1, 2);

            break;
        case 3: /* 1024 unique files per proc */
            for(i = 0; i < 1024; i++)
            {
                snprintf(filepath, 256, "fpp-%d_rank-%d", i , my_rank);

                POSIX_RECORD_OPEN(fd_array[i], filepath, 777, 0, 1);
                POSIX_RECORD_WRITE(size_array[i % DARSHAN_COMMON_VAL_MAX_RUNTIME_COUNT],
                    fd_array[i], 0, 0, 1, 1, 2);
            }

            break;
        case 4: /* 1024 shared files per proc */
            for(i = 0; i < 1024; i++)
            {
                snprintf(filepath, 256, "shared-%d", i);

                POSIX_RECORD_OPEN(fd_array[i], filepath, 777, 0, 1);
                POSIX_RECORD_WRITE(size_array[i % DARSHAN_COMMON_VAL_MAX_RUNTIME_COUNT],
                    fd_array[i], 0, 0, 1, 1, 2);
            }

            break;
        default:
            fprintf(stderr, "Error: invalid Darshan benchmark test case.\n");
            return;
    }

    free(fd_array);
    free(size_array);

    return;
}

/********************************************************************************
 * shutdown function exported by this module for coordinating with darshan-core *
 ********************************************************************************/

static void posix_shutdown(
    MPI_Comm mod_comm,
    darshan_record_id *shared_recs,
    int shared_rec_count,
    void **posix_buf,
    int *posix_buf_sz)
{
    struct posix_file_record_ref *rec_ref;
    struct darshan_posix_file *posix_rec_buf = *(struct darshan_posix_file **)posix_buf;
    int posix_rec_count;
    double posix_time;
    struct darshan_posix_file *red_send_buf = NULL;
    struct darshan_posix_file *red_recv_buf = NULL;
    MPI_Datatype red_type;
    MPI_Op red_op;
    int i;

    POSIX_LOCK();
    assert(posix_runtime);

    posix_rec_count = posix_runtime->file_rec_count;

    /* perform any final transformations on POSIX file records before
     * writing them out to log file
     */
    darshan_iter_record_refs(posix_runtime->rec_id_hash, &posix_finalize_file_records);

    /* if there are globally shared files, do a shared file reduction */
    /* NOTE: the shared file reduction is also skipped if the 
     * DARSHAN_DISABLE_SHARED_REDUCTION environment variable is set.
     */
    if(shared_rec_count && !getenv("DARSHAN_DISABLE_SHARED_REDUCTION"))
    {
        /* necessary initialization of shared records */
        for(i = 0; i < shared_rec_count; i++)
        {
            rec_ref = darshan_lookup_record_ref(posix_runtime->rec_id_hash,
                &shared_recs[i], sizeof(darshan_record_id));
            assert(rec_ref);

            posix_time =
                rec_ref->file_rec->fcounters[POSIX_F_READ_TIME] +
                rec_ref->file_rec->fcounters[POSIX_F_WRITE_TIME] +
                rec_ref->file_rec->fcounters[POSIX_F_META_TIME];

            /* initialize fastest/slowest info prior to the reduction */
            rec_ref->file_rec->counters[POSIX_FASTEST_RANK] =
                rec_ref->file_rec->base_rec.rank;
            rec_ref->file_rec->counters[POSIX_FASTEST_RANK_BYTES] =
                rec_ref->file_rec->counters[POSIX_BYTES_READ] +
                rec_ref->file_rec->counters[POSIX_BYTES_WRITTEN];
            rec_ref->file_rec->fcounters[POSIX_F_FASTEST_RANK_TIME] =
                posix_time;

            /* until reduction occurs, we assume that this rank is both
             * the fastest and slowest. It is up to the reduction operator
             * to find the true min and max.
             */
            rec_ref->file_rec->counters[POSIX_SLOWEST_RANK] =
                rec_ref->file_rec->counters[POSIX_FASTEST_RANK];
            rec_ref->file_rec->counters[POSIX_SLOWEST_RANK_BYTES] =
                rec_ref->file_rec->counters[POSIX_FASTEST_RANK_BYTES];
            rec_ref->file_rec->fcounters[POSIX_F_SLOWEST_RANK_TIME] =
                rec_ref->file_rec->fcounters[POSIX_F_FASTEST_RANK_TIME];

            rec_ref->file_rec->base_rec.rank = -1;
        }

        /* sort the array of records so we get all of the shared records
         * (marked by rank -1) in a contiguous portion at end of the array
         */
        darshan_record_sort(posix_rec_buf, posix_rec_count,
            sizeof(struct darshan_posix_file));

        /* make send_buf point to the shared files at the end of sorted array */
        red_send_buf = &(posix_rec_buf[posix_rec_count-shared_rec_count]);

        /* allocate memory for the reduction output on rank 0 */
        if(my_rank == 0)
        {
            red_recv_buf = malloc(shared_rec_count * sizeof(struct darshan_posix_file));
            if(!red_recv_buf)
            {
                POSIX_UNLOCK();
                return;
            }
        }

        /* construct a datatype for a POSIX file record.  This is serving no purpose
         * except to make sure we can do a reduction on proper boundaries
         */
        PMPI_Type_contiguous(sizeof(struct darshan_posix_file),
            MPI_BYTE, &red_type);
        PMPI_Type_commit(&red_type);

        /* register a POSIX file record reduction operator */
        PMPI_Op_create(posix_record_reduction_op, 1, &red_op);

        /* reduce shared POSIX file records */
        PMPI_Reduce(red_send_buf, red_recv_buf,
            shared_rec_count, red_type, red_op, 0, mod_comm);

        /* get the time and byte variances for shared files */
        posix_shared_record_variance(mod_comm, red_send_buf, red_recv_buf,
            shared_rec_count);

        /* clean up reduction state */
        if(my_rank == 0)
        {
            int tmp_ndx = posix_rec_count - shared_rec_count;
            memcpy(&(posix_rec_buf[tmp_ndx]), red_recv_buf,
                shared_rec_count * sizeof(struct darshan_posix_file));
            free(red_recv_buf);
        }
        else
        {
            posix_rec_count -= shared_rec_count;
        }

        PMPI_Type_free(&red_type);
        PMPI_Op_free(&red_op);
    }

    /* update output buffer size to account for shared file reduction */
    *posix_buf_sz = posix_rec_count * sizeof(struct darshan_posix_file);

    /* shutdown internal structures used for instrumenting */
    posix_cleanup_runtime();

    POSIX_UNLOCK();
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
