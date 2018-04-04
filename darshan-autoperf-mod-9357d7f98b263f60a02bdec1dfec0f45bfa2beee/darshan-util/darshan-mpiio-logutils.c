/*
 * Copyright (C) 2015 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

#define _GNU_SOURCE
#include "darshan-util-config.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "darshan-logutils.h"

/* counter name strings for the MPI-IO module */
#define X(a) #a,
char *mpiio_counter_names[] = {
    MPIIO_COUNTERS
};

char *mpiio_f_counter_names[] = {
    MPIIO_F_COUNTERS
};
#undef X

static int darshan_log_get_mpiio_file(darshan_fd fd, void** mpiio_buf_p);
static int darshan_log_put_mpiio_file(darshan_fd fd, void* mpiio_buf);
static void darshan_log_print_mpiio_file(void *file_rec,
    char *file_name, char *mnt_pt, char *fs_type);
static void darshan_log_print_mpiio_description(int ver);
static void darshan_log_print_mpiio_file_diff(void *file_rec1, char *file_name1,
    void *file_rec2, char *file_name2);
static void darshan_log_agg_mpiio_files(void *rec, void *agg_rec, int init_flag);
static void darshan_log_mpiio_accum_file(void *mfile, hash_entry_t *hfile, int64_t nprocs);
static void darshan_log_mpiio_accum_perf(void *mfile, perf_data_t *pdata);
static void darshan_log_mpiio_calc_file(hash_entry_t *file_hash, file_data_t *fdata);
static void darshan_log_mpiio_print_total_file(void *mfile, int mpiio_ver);
static void darshan_log_mpiio_file_list(hash_entry_t *file_hash, struct darshan_name_record_ref *name_hash, int detail_flag);

struct darshan_mod_logutil_funcs mpiio_logutils =
{
    .log_get_record = &darshan_log_get_mpiio_file,
    .log_put_record = &darshan_log_put_mpiio_file,
    .log_print_record = &darshan_log_print_mpiio_file,
    .log_print_description = &darshan_log_print_mpiio_description,
    .log_print_diff = &darshan_log_print_mpiio_file_diff,
    .log_agg_records = &darshan_log_agg_mpiio_files,
    .log_accum_file = &darshan_log_mpiio_accum_file,
    .log_accum_perf = &darshan_log_mpiio_accum_perf,
    .log_calc_file = &darshan_log_mpiio_calc_file,
    .log_print_total_file = &darshan_log_mpiio_print_total_file,
    .log_file_list = &darshan_log_mpiio_file_list,
    .log_calc_perf = &darshan_calc_perf
};

static int darshan_log_get_mpiio_file(darshan_fd fd, void** mpiio_buf_p)
{
    struct darshan_mpiio_file *file = *((struct darshan_mpiio_file **)mpiio_buf_p);
    int i;
    int ret;

    if(fd->mod_map[DARSHAN_MPIIO_MOD].len == 0)
        return(0);

    if(*mpiio_buf_p == NULL)
    {
        file = malloc(sizeof(*file));
        if(!file)
            return(-1);
    }
    
    ret = darshan_log_get_mod(fd, DARSHAN_MPIIO_MOD, file,
        sizeof(struct darshan_mpiio_file));

    if(*mpiio_buf_p == NULL)
    {
        if(ret == sizeof(struct darshan_mpiio_file))
            *mpiio_buf_p = file;
        else
            free(file);
    }

    if(ret < 0)
        return(-1);
    else if(ret < sizeof(struct darshan_mpiio_file))
        return(0);
    else
    {
        /* if the read was successful, do any necessary byte-swapping */
        if(fd->swap_flag)
        {
            DARSHAN_BSWAP64(&(file->base_rec.id));
            DARSHAN_BSWAP64(&(file->base_rec.rank));
            for(i=0; i<MPIIO_NUM_INDICES; i++)
                DARSHAN_BSWAP64(&file->counters[i]);
            for(i=0; i<MPIIO_F_NUM_INDICES; i++)
                DARSHAN_BSWAP64(&file->fcounters[i]);
        }

        return(1);
    }
}

static int darshan_log_put_mpiio_file(darshan_fd fd, void* mpiio_buf)
{
    struct darshan_mpiio_file *file = (struct darshan_mpiio_file *)mpiio_buf;
    int ret;

    ret = darshan_log_put_mod(fd, DARSHAN_MPIIO_MOD, file,
        sizeof(struct darshan_mpiio_file), DARSHAN_MPIIO_VER);
    if(ret < 0)
        return(-1);

    return(0);
}

static void darshan_log_print_mpiio_file(void *file_rec, char *file_name,
    char *mnt_pt, char *fs_type)
{
    int i;
    struct darshan_mpiio_file *mpiio_file_rec =
        (struct darshan_mpiio_file *)file_rec;

    for(i=0; i<MPIIO_NUM_INDICES; i++)
    {
        DARSHAN_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
            mpiio_file_rec->base_rec.rank, mpiio_file_rec->base_rec.id,
            mpiio_counter_names[i], mpiio_file_rec->counters[i],
            file_name, mnt_pt, fs_type);
    }

    for(i=0; i<MPIIO_F_NUM_INDICES; i++)
    {
        DARSHAN_F_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
            mpiio_file_rec->base_rec.rank, mpiio_file_rec->base_rec.id,
            mpiio_f_counter_names[i], mpiio_file_rec->fcounters[i],
            file_name, mnt_pt, fs_type);
    }

    return;
}

static void darshan_log_print_mpiio_description(int ver)
{
    printf("\n# description of MPIIO counters:\n");
    printf("#   MPIIO_INDEP_*: MPI independent operation counts.\n");
    printf("#   MPIIO_COLL_*: MPI collective operation counts.\n");
    printf("#   MPIIO_SPLIT_*: MPI split collective operation counts.\n");
    printf("#   MPIIO_NB_*: MPI non blocking operation counts.\n");
    printf("#   READS,WRITES,and OPENS are types of operations.\n");
    printf("#   MPIIO_SYNCS: MPI file sync operation counts.\n");
    printf("#   MPIIO_HINTS: number of times MPI hints were used.\n");
    printf("#   MPIIO_VIEWS: number of times MPI file views were used.\n");
    printf("#   MPIIO_MODE: MPI-IO access mode that file was opened with.\n");
    printf("#   MPIIO_BYTES_*: total bytes read and written at MPI-IO layer.\n");
    printf("#   MPIIO_RW_SWITCHES: number of times access alternated between read and write.\n");
    printf("#   MPIIO_MAX_*_TIME_SIZE: size of the slowest read and write operations.\n");
    printf("#   MPIIO_SIZE_*_AGG_*: histogram of MPI datatype total sizes for read and write operations.\n");
    printf("#   MPIIO_ACCESS*_ACCESS: the four most common total access sizes.\n");
    printf("#   MPIIO_ACCESS*_COUNT: count of the four most common total access sizes.\n");
    printf("#   MPIIO_*_RANK: rank of the processes that were the fastest and slowest at I/O (for shared files).\n");
    printf("#   MPIIO_*_RANK_BYTES: total bytes transferred at MPI-IO layer by the fastest and slowest ranks (for shared files).\n");
    printf("#   MPIIO_F_OPEN_TIMESTAMP: timestamp of first open.\n");
    printf("#   MPIIO_F_*_START_TIMESTAMP: timestamp of first MPI-IO read/write.\n");
    printf("#   MPIIO_F_*_END_TIMESTAMP: timestamp of last MPI-IO read/write.\n");
    printf("#   MPIIO_F_CLOSE_TIMESTAMP: timestamp of last close.\n");
    printf("#   MPIIO_F_READ/WRITE/META_TIME: cumulative time spent in MPI-IO read, write, or metadata operations.\n");
    printf("#   MPIIO_F_MAX_*_TIME: duration of the slowest MPI-IO read and write operations.\n");
    printf("#   MPIIO_F_*_RANK_TIME: fastest and slowest I/O time for a single rank (for shared files).\n");
    printf("#   MPIIO_F_VARIANCE_RANK_*: variance of total I/O time and bytes moved for all ranks (for shared files).\n");

    if(ver < 2)
    {
        printf("\n# WARNING: MPIIO module log format version 1 has the following limitations:\n");
        printf("# - MPIIO_F_WRITE_START_TIMESTAMP may not be accurate.\n");
    }

    return;
}

static void darshan_log_print_mpiio_file_diff(void *file_rec1, char *file_name1,
    void *file_rec2, char *file_name2)
{
    struct darshan_mpiio_file *file1 = (struct darshan_mpiio_file *)file_rec1;
    struct darshan_mpiio_file *file2 = (struct darshan_mpiio_file *)file_rec2;
    int i;

    /* NOTE: we assume that both input records are the same module format version */

    for(i=0; i<MPIIO_NUM_INDICES; i++)
    {
        if(!file2)
        {
            printf("- ");
            DARSHAN_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
                file1->base_rec.rank, file1->base_rec.id, mpiio_counter_names[i],
                file1->counters[i], file_name1, "", "");

        }
        else if(!file1)
        {
            printf("+ ");
            DARSHAN_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
                file2->base_rec.rank, file2->base_rec.id, mpiio_counter_names[i],
                file2->counters[i], file_name2, "", "");
        }
        else if(file1->counters[i] != file2->counters[i])
        {
            printf("- ");
            DARSHAN_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
                file1->base_rec.rank, file1->base_rec.id, mpiio_counter_names[i],
                file1->counters[i], file_name1, "", "");
            printf("+ ");
            DARSHAN_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
                file2->base_rec.rank, file2->base_rec.id, mpiio_counter_names[i],
                file2->counters[i], file_name2, "", "");
        }
    }

    for(i=0; i<MPIIO_F_NUM_INDICES; i++)
    {
        if(!file2)
        {
            printf("- ");
            DARSHAN_F_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
                file1->base_rec.rank, file1->base_rec.id, mpiio_f_counter_names[i],
                file1->fcounters[i], file_name1, "", "");

        }
        else if(!file1)
        {
            printf("+ ");
            DARSHAN_F_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
                file2->base_rec.rank, file2->base_rec.id, mpiio_f_counter_names[i],
                file2->fcounters[i], file_name2, "", "");
        }
        else if(file1->fcounters[i] != file2->fcounters[i])
        {
            printf("- ");
            DARSHAN_F_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
                file1->base_rec.rank, file1->base_rec.id, mpiio_f_counter_names[i],
                file1->fcounters[i], file_name1, "", "");
            printf("+ ");
            DARSHAN_F_COUNTER_PRINT(darshan_module_names[DARSHAN_MPIIO_MOD],
                file2->base_rec.rank, file2->base_rec.id, mpiio_f_counter_names[i],
                file2->fcounters[i], file_name2, "", "");
        }
    }

    return;
}

/* simple helper struct for determining time & byte variances */
struct var_t
{
    double n;
    double M;
    double S;
};

static void darshan_log_agg_mpiio_files(void *rec, void *agg_rec, int init_flag)
{
    struct darshan_mpiio_file *mpi_rec = (struct darshan_mpiio_file *)rec;
    struct darshan_mpiio_file *agg_mpi_rec = (struct darshan_mpiio_file *)agg_rec;
    int i, j, k;
    int total_count;
    int64_t tmp_val[4];
    int64_t tmp_cnt[4];
    int tmp_ndx;
    double old_M;
    double mpi_time = mpi_rec->fcounters[MPIIO_F_READ_TIME] +
        mpi_rec->fcounters[MPIIO_F_WRITE_TIME] +
        mpi_rec->fcounters[MPIIO_F_META_TIME];
    double mpi_bytes = (double)mpi_rec->counters[MPIIO_BYTES_READ] +
        mpi_rec->counters[MPIIO_BYTES_WRITTEN];
    struct var_t *var_time_p = (struct var_t *)
        ((char *)rec + sizeof(struct darshan_mpiio_file));
    struct var_t *var_bytes_p = (struct var_t *)
        ((char *)var_time_p + sizeof(struct var_t));

    for(i = 0; i < MPIIO_NUM_INDICES; i++)
    {
        switch(i)
        {
            case MPIIO_INDEP_OPENS:
            case MPIIO_COLL_OPENS:
            case MPIIO_INDEP_READS:
            case MPIIO_INDEP_WRITES:
            case MPIIO_COLL_READS:
            case MPIIO_COLL_WRITES:
            case MPIIO_SPLIT_READS:
            case MPIIO_SPLIT_WRITES:
            case MPIIO_NB_READS:
            case MPIIO_NB_WRITES:
            case MPIIO_SYNCS:
            case MPIIO_HINTS:
            case MPIIO_VIEWS:
            case MPIIO_BYTES_READ:
            case MPIIO_BYTES_WRITTEN:
            case MPIIO_RW_SWITCHES:
            case MPIIO_SIZE_READ_AGG_0_100:
            case MPIIO_SIZE_READ_AGG_100_1K:
            case MPIIO_SIZE_READ_AGG_1K_10K:
            case MPIIO_SIZE_READ_AGG_10K_100K:
            case MPIIO_SIZE_READ_AGG_100K_1M:
            case MPIIO_SIZE_READ_AGG_1M_4M:
            case MPIIO_SIZE_READ_AGG_4M_10M:
            case MPIIO_SIZE_READ_AGG_10M_100M:
            case MPIIO_SIZE_READ_AGG_100M_1G:
            case MPIIO_SIZE_READ_AGG_1G_PLUS:
            case MPIIO_SIZE_WRITE_AGG_0_100:
            case MPIIO_SIZE_WRITE_AGG_100_1K:
            case MPIIO_SIZE_WRITE_AGG_1K_10K:
            case MPIIO_SIZE_WRITE_AGG_10K_100K:
            case MPIIO_SIZE_WRITE_AGG_100K_1M:
            case MPIIO_SIZE_WRITE_AGG_1M_4M:
            case MPIIO_SIZE_WRITE_AGG_4M_10M:
            case MPIIO_SIZE_WRITE_AGG_10M_100M:
            case MPIIO_SIZE_WRITE_AGG_100M_1G:
            case MPIIO_SIZE_WRITE_AGG_1G_PLUS:
                /* sum */
                agg_mpi_rec->counters[i] += mpi_rec->counters[i];
                break;
            case MPIIO_MODE:
                /* just set to the input value */
                agg_mpi_rec->counters[i] = mpi_rec->counters[i];
                break;
            case MPIIO_MAX_READ_TIME_SIZE:
            case MPIIO_MAX_WRITE_TIME_SIZE:
            case MPIIO_FASTEST_RANK:
            case MPIIO_FASTEST_RANK_BYTES:
            case MPIIO_SLOWEST_RANK:
            case MPIIO_SLOWEST_RANK_BYTES:
                /* these are set with the FP counters */
                break;
            case MPIIO_ACCESS1_ACCESS:
                /* increment common value counters */
                if(mpi_rec->counters[i] == 0) break;

                /* first, collapse duplicates */
                for(j = i; j < i + 4; j++)
                {
                    for(k = 0; k < 4; k++)
                    {
                        if(agg_mpi_rec->counters[i + k] == mpi_rec->counters[j])
                        {
                            agg_mpi_rec->counters[i + k + 4] += mpi_rec->counters[j + 4];
                            mpi_rec->counters[j] = mpi_rec->counters[j + 4] = 0;
                        }
                    }
                }

                /* second, add new counters */
                for(j = i; j < i + 4; j++)
                {
                    tmp_ndx = 0;
                    memset(tmp_val, 0, 4 * sizeof(int64_t));
                    memset(tmp_cnt, 0, 4 * sizeof(int64_t));

                    if(mpi_rec->counters[j] == 0) break;
                    for(k = 0; k < 4; k++)
                    {
                        if(agg_mpi_rec->counters[i + k] == mpi_rec->counters[j])
                        {
                            total_count = agg_mpi_rec->counters[i + k + 4] +
                                mpi_rec->counters[j + 4];
                            break;
                        }
                    }
                    if(k == 4) total_count = mpi_rec->counters[j + 4];

                    for(k = 0; k < 4; k++)
                    {
                        if((agg_mpi_rec->counters[i + k + 4] > total_count) ||
                           ((agg_mpi_rec->counters[i + k + 4] == total_count) &&
                            (agg_mpi_rec->counters[i + k] > mpi_rec->counters[j])))
                        {
                            tmp_val[tmp_ndx] = agg_mpi_rec->counters[i + k];
                            tmp_cnt[tmp_ndx] = agg_mpi_rec->counters[i + k + 4];
                            tmp_ndx++;
                        }
                        else break;
                    }
                    if(tmp_ndx == 4) break;

                    tmp_val[tmp_ndx] = mpi_rec->counters[j];
                    tmp_cnt[tmp_ndx] = mpi_rec->counters[j + 4];
                    tmp_ndx++;

                    while(tmp_ndx != 4)
                    {
                        if(agg_mpi_rec->counters[i + k] != mpi_rec->counters[j])
                        {
                            tmp_val[tmp_ndx] = agg_mpi_rec->counters[i + k];
                            tmp_cnt[tmp_ndx] = agg_mpi_rec->counters[i + k + 4];
                            tmp_ndx++;
                        }
                        k++;
                    }
                    memcpy(&(agg_mpi_rec->counters[i]), tmp_val, 4 * sizeof(int64_t));
                    memcpy(&(agg_mpi_rec->counters[i + 4]), tmp_cnt, 4 * sizeof(int64_t));
                }
                break;
            case MPIIO_ACCESS2_ACCESS:
            case MPIIO_ACCESS3_ACCESS:
            case MPIIO_ACCESS4_ACCESS:
            case MPIIO_ACCESS1_COUNT:
            case MPIIO_ACCESS2_COUNT:
            case MPIIO_ACCESS3_COUNT:
            case MPIIO_ACCESS4_COUNT:
                /* these are set all at once with common counters above */
                break;
            default:
                agg_mpi_rec->counters[i] = -1;
                break;
        }
    }

    for(i = 0; i < MPIIO_F_NUM_INDICES; i++)
    {
        switch(i)
        {
            case MPIIO_F_READ_TIME:
            case MPIIO_F_WRITE_TIME:
            case MPIIO_F_META_TIME:
                /* sum */
                agg_mpi_rec->fcounters[i] += mpi_rec->fcounters[i];
                break;
            case MPIIO_F_OPEN_TIMESTAMP:
            case MPIIO_F_READ_START_TIMESTAMP:
            case MPIIO_F_WRITE_START_TIMESTAMP:
                /* minimum non-zero */
                if((mpi_rec->fcounters[i] > 0)  &&
                    ((agg_mpi_rec->fcounters[i] == 0) ||
                    (mpi_rec->fcounters[i] < agg_mpi_rec->fcounters[i])))
                {
                    agg_mpi_rec->fcounters[i] = mpi_rec->fcounters[i];
                }
                break;
            case MPIIO_F_READ_END_TIMESTAMP:
            case MPIIO_F_WRITE_END_TIMESTAMP:
            case MPIIO_F_CLOSE_TIMESTAMP:
                /* maximum */
                if(mpi_rec->fcounters[i] > agg_mpi_rec->fcounters[i])
                {
                    agg_mpi_rec->fcounters[i] = mpi_rec->fcounters[i];
                }
                break;
            case MPIIO_F_MAX_READ_TIME:
                if(mpi_rec->fcounters[i] > agg_mpi_rec->fcounters[i])
                {
                    agg_mpi_rec->fcounters[i] = mpi_rec->fcounters[i];
                    agg_mpi_rec->counters[MPIIO_MAX_READ_TIME_SIZE] =
                        mpi_rec->counters[MPIIO_MAX_READ_TIME_SIZE];
                }
                break;
            case MPIIO_F_MAX_WRITE_TIME:
                if(mpi_rec->fcounters[i] > agg_mpi_rec->fcounters[i])
                {
                    agg_mpi_rec->fcounters[i] = mpi_rec->fcounters[i];
                    agg_mpi_rec->counters[MPIIO_MAX_WRITE_TIME_SIZE] =
                        mpi_rec->counters[MPIIO_MAX_WRITE_TIME_SIZE];
                }
                break;
            case MPIIO_F_FASTEST_RANK_TIME:
                if(init_flag)
                {
                    /* set fastest rank counters according to root rank. these counters
                     * will be determined as the aggregation progresses.
                     */
                    agg_mpi_rec->counters[MPIIO_FASTEST_RANK] = mpi_rec->base_rec.rank;
                    agg_mpi_rec->counters[MPIIO_FASTEST_RANK_BYTES] = mpi_bytes;
                    agg_mpi_rec->fcounters[MPIIO_F_FASTEST_RANK_TIME] = mpi_time;
                }

                if(mpi_time < agg_mpi_rec->fcounters[MPIIO_F_FASTEST_RANK_TIME])
                {
                    agg_mpi_rec->counters[MPIIO_FASTEST_RANK] = mpi_rec->base_rec.rank;
                    agg_mpi_rec->counters[MPIIO_FASTEST_RANK_BYTES] = mpi_bytes;
                    agg_mpi_rec->fcounters[MPIIO_F_FASTEST_RANK_TIME] = mpi_time;
                }
                break;
            case MPIIO_F_SLOWEST_RANK_TIME:
                if(init_flag)
                {
                    /* set slowest rank counters according to root rank. these counters
                     * will be determined as the aggregation progresses.
                     */
                    agg_mpi_rec->counters[MPIIO_SLOWEST_RANK] = mpi_rec->base_rec.rank;
                    agg_mpi_rec->counters[MPIIO_SLOWEST_RANK_BYTES] = mpi_bytes;
                    agg_mpi_rec->fcounters[MPIIO_F_SLOWEST_RANK_TIME] = mpi_time;
                }

                if(mpi_time > agg_mpi_rec->fcounters[MPIIO_F_SLOWEST_RANK_TIME])
                {
                    agg_mpi_rec->counters[MPIIO_SLOWEST_RANK] = mpi_rec->base_rec.rank;
                    agg_mpi_rec->counters[MPIIO_SLOWEST_RANK_BYTES] = mpi_bytes;
                    agg_mpi_rec->fcounters[MPIIO_F_SLOWEST_RANK_TIME] = mpi_time;
                }
                break;
            case MPIIO_F_VARIANCE_RANK_TIME:
                if(init_flag)
                {
                    var_time_p->n = 1;
                    var_time_p->M = mpi_time;
                    var_time_p->S = 0;
                }
                else
                {
                    old_M = var_time_p->M;

                    var_time_p->n++;
                    var_time_p->M += (mpi_time - var_time_p->M) / var_time_p->n;
                    var_time_p->S += (mpi_time - var_time_p->M) * (mpi_time - old_M);

                    agg_mpi_rec->fcounters[MPIIO_F_VARIANCE_RANK_TIME] =
                        var_time_p->S / var_time_p->n;
                }
                break;
            case MPIIO_F_VARIANCE_RANK_BYTES:
                if(init_flag)
                {
                    var_bytes_p->n = 1;
                    var_bytes_p->M = mpi_bytes;
                    var_bytes_p->S = 0;
                }
                else
                {
                    old_M = var_bytes_p->M;

                    var_bytes_p->n++;
                    var_bytes_p->M += (mpi_bytes - var_bytes_p->M) / var_bytes_p->n;
                    var_bytes_p->S += (mpi_bytes - var_bytes_p->M) * (mpi_bytes - old_M);

                    agg_mpi_rec->fcounters[MPIIO_F_VARIANCE_RANK_BYTES] =
                        var_bytes_p->S / var_bytes_p->n;
                }
                break;
            default:
                agg_mpi_rec->fcounters[i] = -1;
                break;
        }
    }

    return;
}

static void darshan_log_mpiio_accum_file(
    void *infile,
    hash_entry_t *hfile,
    int64_t nprocs)
{
    struct darshan_mpiio_file *mfile = infile;
    int i, j;
    int set;
    int min_ndx;
    int64_t min;
    struct darshan_mpiio_file* tmp;

    hfile->procs += 1;

    if(mfile->base_rec.rank == -1)
    {
        hfile->slowest_time = mfile->fcounters[MPIIO_F_SLOWEST_RANK_TIME];
    }
    else
    {
        hfile->slowest_time = max(hfile->slowest_time,
            (mfile->fcounters[MPIIO_F_META_TIME] +
            mfile->fcounters[MPIIO_F_READ_TIME] +
            mfile->fcounters[MPIIO_F_WRITE_TIME]));
    }

    if(mfile->base_rec.rank == -1)
    {
        hfile->procs = nprocs;
        hfile->type |= FILETYPE_SHARED;

    }
    else if(hfile->procs > 1)
    {
        hfile->type &= (~FILETYPE_UNIQUE);
        hfile->type |= FILETYPE_PARTSHARED;
    }
    else
    {
        hfile->type |= FILETYPE_UNIQUE;
    }

    hfile->cumul_time += mfile->fcounters[MPIIO_F_META_TIME] +
                         mfile->fcounters[MPIIO_F_READ_TIME] +
                         mfile->fcounters[MPIIO_F_WRITE_TIME];

    if(hfile->rec_dat == NULL)
    {
        hfile->rec_dat = malloc(sizeof(struct darshan_mpiio_file));
        assert(hfile->rec_dat);
        memset(hfile->rec_dat, 0, sizeof(struct darshan_mpiio_file));
    }
    tmp = (struct darshan_mpiio_file*)hfile->rec_dat;

    for(i = 0; i < MPIIO_NUM_INDICES; i++)
    {
        switch(i)
        {
        case MPIIO_MODE:
            tmp->counters[i] = mfile->counters[i];
            break;
        case MPIIO_ACCESS1_ACCESS:
        case MPIIO_ACCESS2_ACCESS:
        case MPIIO_ACCESS3_ACCESS:
        case MPIIO_ACCESS4_ACCESS:
            /*
             * do nothing here because these will be stored
             * when the _COUNT is accessed.
             */
            break;
        case MPIIO_ACCESS1_COUNT:
        case MPIIO_ACCESS2_COUNT:
        case MPIIO_ACCESS3_COUNT:
        case MPIIO_ACCESS4_COUNT:
            set = 0;
            min_ndx = MPIIO_ACCESS1_COUNT;
            min = tmp->counters[min_ndx];
            for(j = MPIIO_ACCESS1_COUNT; j <= MPIIO_ACCESS4_COUNT; j++)
            {
                if(tmp->counters[j-4] == mfile->counters[i-4])
                {
                    tmp->counters[j] += mfile->counters[i];
                    set = 1;
                    break;
                }
                if(tmp->counters[j] < min)
                {
                    min_ndx = j;
                    min = tmp->counters[j];
                }
            }
            if(!set && (mfile->counters[i] > min))
            {
                tmp->counters[i] = mfile->counters[i];
                tmp->counters[i-4] = mfile->counters[i-4];
            }
            break;
        case MPIIO_FASTEST_RANK:
        case MPIIO_SLOWEST_RANK:
        case MPIIO_FASTEST_RANK_BYTES:
        case MPIIO_SLOWEST_RANK_BYTES:
            tmp->counters[i] = 0;
            break;
        case MPIIO_MAX_READ_TIME_SIZE:
        case MPIIO_MAX_WRITE_TIME_SIZE:
            break;
        default:
            tmp->counters[i] += mfile->counters[i];
            break;
        }
    }

    for(i = 0; i < MPIIO_F_NUM_INDICES; i++)
    {
        switch(i)
        {
            case MPIIO_F_OPEN_TIMESTAMP:
            case MPIIO_F_READ_START_TIMESTAMP:
            case MPIIO_F_WRITE_START_TIMESTAMP:
                if(tmp->fcounters[i] == 0 ||
                    tmp->fcounters[i] > mfile->fcounters[i])
                {
                    tmp->fcounters[i] = mfile->fcounters[i];
                }
                break;
            case MPIIO_F_READ_END_TIMESTAMP:
            case MPIIO_F_WRITE_END_TIMESTAMP:
            case MPIIO_F_CLOSE_TIMESTAMP:
                if(tmp->fcounters[i] == 0 ||
                    tmp->fcounters[i] < mfile->fcounters[i])
                {
                    tmp->fcounters[i] = mfile->fcounters[i];
                }
                break;
            case MPIIO_F_FASTEST_RANK_TIME:
            case MPIIO_F_SLOWEST_RANK_TIME:
            case MPIIO_F_VARIANCE_RANK_TIME:
            case MPIIO_F_VARIANCE_RANK_BYTES:
                tmp->fcounters[i] = 0;
                break;
            case MPIIO_F_MAX_READ_TIME:
                if(tmp->fcounters[i] < mfile->fcounters[i])
                {
                    tmp->fcounters[i] = mfile->fcounters[i];
                    tmp->counters[MPIIO_MAX_READ_TIME_SIZE] =
                        mfile->counters[MPIIO_MAX_READ_TIME_SIZE];
                }
                break;
            case MPIIO_F_MAX_WRITE_TIME:
                if(tmp->fcounters[i] < mfile->fcounters[i])
                {
                    tmp->fcounters[i] = mfile->fcounters[i];
                    tmp->counters[MPIIO_MAX_WRITE_TIME_SIZE] =
                        mfile->counters[MPIIO_MAX_WRITE_TIME_SIZE];
                }
                break;
            default:
                tmp->fcounters[i] += mfile->fcounters[i];
                break;
        }
    }

    return;
}

static void darshan_log_mpiio_accum_perf(
    void *infile,
    perf_data_t *pdata)
{
    struct darshan_mpiio_file *mfile = infile;
    pdata->total_bytes += mfile->counters[MPIIO_BYTES_READ] +
                          mfile->counters[MPIIO_BYTES_WRITTEN];

    /*
     * Calculation of Shared File Time
     *   Four Methods!!!!
     *     by_cumul: sum time counters and divide by nprocs
     *               (inaccurate if lots of variance between procs)
     *     by_open: difference between timestamp of open and close
     *              (inaccurate if file is left open without i/o happening)
     *     by_open_lastio: difference between timestamp of open and the
     *                     timestamp of last i/o
     *                     (similar to above but fixes case where file is left
     *                      open after io is complete)
     *     by_slowest: use slowest rank time from log data
     *                 (most accurate but requires newer log version)
     */
    if(mfile->base_rec.rank == -1)
    {
        /* by_open */
        if(mfile->fcounters[MPIIO_F_CLOSE_TIMESTAMP] >
            mfile->fcounters[MPIIO_F_OPEN_TIMESTAMP])
        {
            pdata->shared_time_by_open +=
                mfile->fcounters[MPIIO_F_CLOSE_TIMESTAMP] -
                mfile->fcounters[MPIIO_F_OPEN_TIMESTAMP];
        }

        /* by_open_lastio */
        if(mfile->fcounters[MPIIO_F_READ_END_TIMESTAMP] >
            mfile->fcounters[MPIIO_F_WRITE_END_TIMESTAMP])
        {
            /* be careful: file may have been opened but not read or written */
            if(mfile->fcounters[MPIIO_F_READ_END_TIMESTAMP] > mfile->fcounters[MPIIO_F_OPEN_TIMESTAMP])
            {
                pdata->shared_time_by_open_lastio +=
                    mfile->fcounters[MPIIO_F_READ_END_TIMESTAMP] -
                    mfile->fcounters[MPIIO_F_OPEN_TIMESTAMP];
            }
        }
        else
        {
            /* be careful: file may have been opened but not read or written */
            if(mfile->fcounters[MPIIO_F_WRITE_END_TIMESTAMP] > mfile->fcounters[MPIIO_F_OPEN_TIMESTAMP])
            {
                pdata->shared_time_by_open_lastio +=
                    mfile->fcounters[MPIIO_F_WRITE_END_TIMESTAMP] -
                    mfile->fcounters[MPIIO_F_OPEN_TIMESTAMP];
            }
        }

        pdata->shared_time_by_cumul +=
            mfile->fcounters[MPIIO_F_META_TIME] +
            mfile->fcounters[MPIIO_F_READ_TIME] +
            mfile->fcounters[MPIIO_F_WRITE_TIME];
        pdata->shared_meta_time += mfile->fcounters[MPIIO_F_META_TIME];

        /* by_slowest */
        pdata->shared_time_by_slowest +=
            mfile->fcounters[MPIIO_F_SLOWEST_RANK_TIME];
    }

    /*
     * Calculation of Unique File Time
     *   record the data for each file and sum it 
     */
    else
    {
        pdata->rank_cumul_io_time[mfile->base_rec.rank] +=
            (mfile->fcounters[MPIIO_F_META_TIME] +
            mfile->fcounters[MPIIO_F_READ_TIME] +
            mfile->fcounters[MPIIO_F_WRITE_TIME]);
        pdata->rank_cumul_md_time[mfile->base_rec.rank] +=
            mfile->fcounters[MPIIO_F_META_TIME];
    }

    return;
}

static void darshan_log_mpiio_calc_file(
    hash_entry_t *file_hash,
    file_data_t *fdata)
{
    hash_entry_t *curr = NULL;
    hash_entry_t *tmp = NULL;
    struct darshan_mpiio_file *file_rec;

    memset(fdata, 0, sizeof(*fdata));
    HASH_ITER(hlink, file_hash, curr, tmp)
    {
        int64_t bytes;
        int64_t r;
        int64_t w;

        file_rec = (struct darshan_mpiio_file*)curr->rec_dat;
        assert(file_rec);

        bytes = file_rec->counters[MPIIO_BYTES_READ] +
                file_rec->counters[MPIIO_BYTES_WRITTEN];

        r = (file_rec->counters[MPIIO_INDEP_READS]+
             file_rec->counters[MPIIO_COLL_READS] +
             file_rec->counters[MPIIO_SPLIT_READS] +
             file_rec->counters[MPIIO_NB_READS]);

        w = (file_rec->counters[MPIIO_INDEP_WRITES]+
             file_rec->counters[MPIIO_COLL_WRITES] +
             file_rec->counters[MPIIO_SPLIT_WRITES] +
             file_rec->counters[MPIIO_NB_WRITES]);

        fdata->total += 1;
        fdata->total_size += bytes;
        fdata->total_max = max(fdata->total_max, bytes);

        if (r && !w)
        {
            fdata->read_only += 1;
            fdata->read_only_size += bytes;
            fdata->read_only_max = max(fdata->read_only_max, bytes);
        }

        if (!r && w)
        {
            fdata->write_only += 1;
            fdata->write_only_size += bytes;
            fdata->write_only_max = max(fdata->write_only_max, bytes);
        }

        if (r && w)
        {
            fdata->read_write += 1;
            fdata->read_write_size += bytes;
            fdata->read_write_max = max(fdata->read_write_max, bytes);
        }

        if ((curr->type & (FILETYPE_SHARED|FILETYPE_PARTSHARED)))
        {
            fdata->shared += 1;
            fdata->shared_size += bytes;
            fdata->shared_max = max(fdata->shared_max, bytes);
        }
        if ((curr->type & (FILETYPE_UNIQUE)))
        {
            fdata->unique += 1;
            fdata->unique_size += bytes;
            fdata->unique_max = max(fdata->unique_max, bytes);
        }
    }

    return;
}

static void darshan_log_mpiio_print_total_file(
    void *infile,
    int mpiio_ver)
{
    struct darshan_mpiio_file *mfile = infile;
    int i;

    mod_logutils[DARSHAN_MPIIO_MOD]->log_print_description(mpiio_ver);
    printf("\n");
    for(i = 0; i < MPIIO_NUM_INDICES; i++)
    {
        printf("total_%s: %"PRId64"\n",
            mpiio_counter_names[i], mfile->counters[i]);
    }
    for(i = 0; i < MPIIO_F_NUM_INDICES; i++)
    {
        printf("total_%s: %lf\n",
            mpiio_f_counter_names[i], mfile->fcounters[i]);
    }
    return;
}

static void darshan_log_mpiio_file_list(
    hash_entry_t *file_hash,
    struct darshan_name_record_ref *name_hash,
    int detail_flag)
{
    hash_entry_t *curr = NULL;
    hash_entry_t *tmp = NULL;
    struct darshan_mpiio_file *file_rec = NULL;
    struct darshan_name_record_ref *ref = NULL;
    int i;

    /* list of columns:
     *
     * normal mode
     * - file id
     * - file name
     * - nprocs
     * - slowest I/O time
     * - average cumulative I/O time
     *
     * detailed mode
     * - first open
     * - first read
     * - first write
     * - last read
     * - last write
     * - last close
     * - MPI indep opens
     * - MPI coll opens
     * - r histogram
     * - w histogram
     */

    if(detail_flag)
        printf("\n# Per-file summary of I/O activity (detailed).\n");
    else
        printf("\n# Per-file summary of I/O activity.\n");
    printf("# -----\n");

    printf("# <record_id>: darshan record id for this file\n");
    printf("# <file_name>: full file name\n");
    printf("# <nprocs>: number of processes that opened the file\n");
    printf("# <slowest>: (estimated) time in seconds consumed in IO by slowest process\n");
    printf("# <avg>: average time in seconds consumed in IO per process\n");
    if(detail_flag)
    {
        printf("# <start_{open/read/write}>: start timestamp of first open, read, or write\n");
        printf("# <end_{read/write/close}>: end timestamp of last read, write, or close\n");
        printf("# <mpi_indep_opens>: independent MPI_File_open calls\n");
        printf("# <mpi_coll_opens>: collective MPI_File_open calls\n");
        printf("# <MPIIO_SIZE_READ_AGG_*>: MPI-IO aggregate read size histogram\n");
        printf("# <MPIIO_SIZE_WRITE_AGG_*>: MPI-IO aggregate write size histogram\n");
    }

    printf("\n# <record_id>\t<file_name>\t<nprocs>\t<slowest>\t<avg>");
    if(detail_flag)
    {
        printf("\t<start_open>\t<start_read>\t<start_write>");
        printf("\t<end_read>\t<end_write>\t<end_close>");
        printf("\t<mpi_indep_opens>\t<mpi_coll_opens>");
        for(i=MPIIO_SIZE_READ_AGG_0_100; i<= MPIIO_SIZE_WRITE_AGG_1G_PLUS; i++)
            printf("\t<%s>", mpiio_counter_names[i]);
    }
    printf("\n");

    HASH_ITER(hlink, file_hash, curr, tmp)
    {
        file_rec = (struct darshan_mpiio_file*)curr->rec_dat;
        assert(file_rec);

        HASH_FIND(hlink, name_hash, &(curr->rec_id), sizeof(darshan_record_id), ref);
        assert(ref);

        printf("%" PRIu64 "\t%s\t%" PRId64 "\t%f\t%f",
            curr->rec_id,
            ref->name_record->name,
            curr->procs,
            curr->slowest_time,
            curr->cumul_time/(double)curr->procs);

        if(detail_flag)
        {
            for(i=MPIIO_F_OPEN_TIMESTAMP; i<=MPIIO_F_CLOSE_TIMESTAMP; i++)
            {
                printf("\t%f", file_rec->fcounters[i]);
            }
            printf("\t%" PRId64 "\t%" PRId64, file_rec->counters[MPIIO_INDEP_OPENS],
                file_rec->counters[MPIIO_COLL_OPENS]);
            for(i=MPIIO_SIZE_READ_AGG_0_100; i<= MPIIO_SIZE_WRITE_AGG_1G_PLUS; i++)
                printf("\t%" PRId64, file_rec->counters[i]);
        }
        printf("\n");
    }

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
