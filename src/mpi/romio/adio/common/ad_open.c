/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "adio.h"
#include "adio_extern.h"
#include "adio_cb_config_list.h"
#include "hint_fns.h"

static int is_aggregator(int rank, ADIO_File fd);
static int uses_generic_read(ADIO_File fd);
static int uses_generic_write(ADIO_File fd);
static int build_cb_config_list(ADIO_File fd,
                                MPI_Comm orig_comm, MPI_Comm comm,
                                int rank, int procs, int *error_code);

MPI_File ADIO_Open(MPI_Comm orig_comm,
                   MPI_Comm comm, const char *filename, int file_system,
                   ADIOI_Fns * ops,
                   int access_mode, ADIO_Offset disp, MPI_Datatype etype,
                   MPI_Datatype filetype, MPI_Info info, int perm, int *error_code)
{
    MPI_File mpi_fh;
    ADIO_File fd;
    int err, rank, procs;
    static char myname[] = "ADIO_OPEN";
    int max_error_code;
    MPI_Info dupinfo;
    int syshints_processed, can_skip;
    char *p;

    *error_code = MPI_SUCCESS;

    /* obtain MPI_File handle */
    mpi_fh = MPIO_File_create(sizeof(struct ADIOI_FileD));
    if (mpi_fh == MPI_FILE_NULL) {
        fd = MPI_FILE_NULL;
        *error_code = MPIO_Err_create_code(*error_code,
                                           MPIR_ERR_RECOVERABLE,
                                           myname, __LINE__, MPI_ERR_OTHER, "**nomem2", 0);
        goto fn_exit;

    }
    fd = MPIO_File_resolve(mpi_fh);

    fd->cookie = ADIOI_FILE_COOKIE;
    fd->fp_ind = disp;
    fd->fp_sys_posn = 0;
    fd->comm = comm;    /* dup'ed in MPI_File_open */
    fd->filename = ADIOI_Strdup(filename);
    fd->file_system = file_system;
    fd->fs_ptr = NULL;

    fd->fns = ops;

    fd->disp = disp;
    fd->split_coll_count = 0;
    fd->shared_fp_fd = ADIO_FILE_NULL;
    fd->atomicity = 0;
    fd->etype = etype;  /* MPI_BYTE by default */
    fd->filetype = filetype;    /* MPI_BYTE by default */
    fd->etype_size = 1; /* default etype is MPI_BYTE */

    fd->file_realm_st_offs = NULL;
    fd->file_realm_types = NULL;

    fd->perm = perm;

    fd->async_count = 0;

    fd->fortran_handle = -1;

    fd->err_handler = ADIOI_DFLT_ERR_HANDLER;

    fd->io_buf_window = MPI_WIN_NULL;
    fd->io_buf_put_amounts_window = MPI_WIN_NULL;

    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &procs);
/* create and initialize info object */
    fd->hints = (ADIOI_Hints *) ADIOI_Calloc(1, sizeof(struct ADIOI_Hints_struct));
    if (fd->hints == NULL) {
        *error_code = MPIO_Err_create_code(*error_code,
                                           MPIR_ERR_RECOVERABLE,
                                           myname, __LINE__, MPI_ERR_OTHER, "**nomem2", 0);
        goto fn_exit;
    }
    fd->hints->cb_config_list = NULL;
    fd->hints->ranklist = NULL;
    fd->hints->initialized = 0;
    fd->info = MPI_INFO_NULL;

    /* Some ROMIO drivers will support "write tracking": omit an fsync() call
     * if no writes happened.  Drivers supporting that feature will set the bit
     * on write, and clear the bit on open, close, and sync. */
    fd->dirty_write = 1;

    /* move system-wide hint processing *back* into open, but this time the
     * hintfile reader will do a scalable read-and-broadcast.  The global
     * ADIOI_syshints will get initialized at first open.  subsequent open
     * calls will just use result from first open.
     *
     * We have two goals here:
     * 1: avoid processing the hintfile multiple times
     * 2: have all processes participate in hintfile processing (so we can read-and-broadcast)
     *
     * a code might do an "initialize from 0", so we can only skip hint
     * processing once everyone has participated in hint processing */
    if (ADIOI_syshints == MPI_INFO_NULL)
        syshints_processed = 0;
    else
        syshints_processed = 1;

    MPI_Allreduce(&syshints_processed, &can_skip, 1, MPI_INT, MPI_MIN, fd->comm);
    if (!can_skip) {
        if (ADIOI_syshints == MPI_INFO_NULL)
            MPI_Info_create(&ADIOI_syshints);
        ADIOI_process_system_hints(fd, ADIOI_syshints);
    }

    ADIOI_incorporate_system_hints(info, ADIOI_syshints, &dupinfo);
    ADIO_SetInfo(fd, dupinfo, &err);
    if (dupinfo != MPI_INFO_NULL) {
        *error_code = MPI_Info_free(&dupinfo);
        if (*error_code != MPI_SUCCESS)
            goto fn_exit;
    }
    ADIOI_Info_set(fd->info, "romio_filesystem_type", fd->fns->fsname);

    /* The dup'ed comm should report the default memory allocation kinds
     * (if any), since hints are not inherited during MPI_Comm_dup. We
     * just ignore any assertion hints for now. */
    MPI_Info comm_info;
    char *memory_kinds = NULL;
    MPI_Comm_get_info(comm, &comm_info);
    ADIOI_Info_check_and_install_str(fd, comm_info, "mpi_memory_alloc_kinds", &memory_kinds,
                                     myname, error_code);
    if (memory_kinds) {
        ADIOI_Free(memory_kinds);
    }
    MPI_Info_free(&comm_info);

    /* Instead of repeatedly allocating this buffer in collective read/write,
     * allocating up-front might make memory management on small platforms
     * (e.g. Blue Gene) more efficient */

    fd->io_buf = ADIOI_Malloc(fd->hints->cb_buffer_size);


    // BAD MAGIC NUMBER (256)
    int info_flag;
    char data_buffer_size_str[256];
    ADIOI_Info_get(info, "data_buffer_size", 256, data_buffer_size_str, &info_flag);
    fd->data_buffer_size = atoi(data_buffer_size_str);
    
    char shared_buffer_folder[256];
    ADIOI_Info_get(info, "shared_buffer_folder", 256, shared_buffer_folder, &info_flag);

    char metadata_file[256] = {0};
    char data_file[256] = {0};
    sprintf(metadata_file, "%s/metadata-log.%04d", shared_buffer_folder, rank);
    sprintf(data_file, "%s/data-log.%04d", shared_buffer_folder, rank);

    int file, len = sizeof(m_chunk) * M_CHUNK_COUNT;
    file = open(metadata_file, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if(file < 0) perror("Error opening node buffer file!\n");
    ftruncate(file, len);
    fd->metadata_log_buffer = mmap(fd->metadata_log_buffer, len, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);

    file = open(data_file, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if(file < 0) perror("Error opening shared buffer file!\n");
    ftruncate(file, fd->data_buffer_size);
    fd->data_log_buffer = mmap(fd->data_log_buffer, fd->data_buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);

    fd->cur_data_offset = 0;

    // consider keeping these separate, and holding the chunks in ram and then copying them once io is complete
    fd->chunks = fd->metadata_log_buffer;
    for(int i = 0; i != M_CHUNK_COUNT; ++i) {
        fd->chunks[i].free = 1;
        fd->chunks[i].item_count = 0;
        fd->chunks[i].next_chunk = i+1;
    }
    fd->chunks[M_CHUNK_COUNT - 1].next_chunk = 0;
    fd->current_chunk = fd->chunks;

    /* deferred open:
     * we can only do this optimization if 'fd->hints->deferred_open' is set
     * (which means the user hinted 'no_indep_rw' and collective buffering).
     * Furthermore, we only do this if our collective read/write routines use
     * our generic function, and not an fs-specific routine (we can defer opens
     * only if we use our aggregation code). */
    if (fd->hints->deferred_open && !(uses_generic_read(fd)
                                      && uses_generic_write(fd))) {
        fd->hints->deferred_open = 0;
    }
    if (ADIO_Feature(fd, ADIO_SCALABLE_OPEN))
        /* disable deferred open on these fs so that scalable broadcast
         * will always use the proper communicator */
        fd->hints->deferred_open = 0;


    /* on BlueGene, the cb_config_list is built when hints are processed. No
     * one else does that right now */
    if (fd->hints->ranklist == NULL) {
        build_cb_config_list(fd, orig_comm, comm, rank, procs, error_code);
        if (*error_code != MPI_SUCCESS)
            goto fn_exit;
    }
    fd->is_open = 0;
    fd->my_cb_nodes_index = -2;
    fd->is_agg = is_aggregator(rank, fd);
    /* deferred open used to split the communicator to create an "aggregator
     * communicator", but we only used it as a way to indicate that deferred
     * open happened.  fd->is_open and fd->is_agg are sufficient */

    /* actual opens start here */
    /* generic open: one process opens to create the file, all others open */
    /* nfs open: everybody opens or else you'll end up with "file not found"
     * due to stupid nfs consistency semantics */
    /* scalable open: one process opens and broadcasts results to everyone */

    ADIOI_OpenColl(fd, rank, access_mode, error_code);

    /* deferred open consideration: if an independent process lied about
     * "no_indep_rw" and opens the file later (example: HDF5 uses independent
     * i/o for metadata), that deferred open will use the access_mode provided
     * by the user.  CREATE|EXCL only makes sense here -- exclusive access in
     * the deferred open case is going to fail and surprise the user.  Turn off
     * the excl amode bit. Save user's amode for MPI_FILE_GET_AMODE */
    fd->orig_access_mode = access_mode;
    if (fd->access_mode & ADIO_EXCL)
        fd->access_mode ^= ADIO_EXCL;


    /* for debugging, it can be helpful to see the hints selected. Some file
     * systems set up the hints in the open call (e.g. lustre) */
    p = getenv("ROMIO_PRINT_HINTS");
    if (rank == 0 && p != NULL) {
        ADIOI_Info_print_keyvals(fd->info);
    }

  fn_exit:
    MPI_Allreduce(error_code, &max_error_code, 1, MPI_INT, MPI_MAX, comm);
    if (max_error_code != MPI_SUCCESS) {

        /* If the file was successfully opened, close it */
        if (*error_code == MPI_SUCCESS) {

            /* in the deferred open case, only those who have actually
             * opened the file should close it */
            if (fd->hints->deferred_open) {
                if (fd->is_agg) {
                    (*(fd->fns->ADIOI_xxx_Close)) (fd, error_code);
                }
            } else {
                (*(fd->fns->ADIOI_xxx_Close)) (fd, error_code);
            }
        }
        ADIOI_Free(fd->filename);
        if (fd->hints->ranklist != NULL)
            ADIOI_Free(fd->hints->ranklist);
        if (fd->hints->cb_config_list != NULL)
            ADIOI_Free(fd->hints->cb_config_list);
        ADIOI_Free(fd->hints);
        if (fd->info != MPI_INFO_NULL)
            MPI_Info_free(&(fd->info));
        ADIOI_Free(fd->io_buf);
        ADIOI_Free(fd);
        fd = ADIO_FILE_NULL;
        if (*error_code == MPI_SUCCESS) {
            *error_code = MPIO_Err_create_code(MPI_SUCCESS,
                                               MPIR_ERR_RECOVERABLE, myname,
                                               __LINE__, MPI_ERR_IO, "**oremote_fail", 0);
        }
    }

    return fd;
}

/* a simple linear search. possible enhancement: add a my_cb_nodes_index member
 * (index into cb_nodes, else -1 if not aggregator) for faster lookups
 *
 * fd->hints->cb_nodes is the number of aggregators
 * fd->hints->ranklist[] is an array of the ranks of aggregators
 *
 * might want to move this to adio/common/cb_config_list.c
 */
int is_aggregator(int rank, ADIO_File fd)
{
    int i;

    if (fd->my_cb_nodes_index == -2) {
        for (i = 0; i < fd->hints->cb_nodes; i++) {
            if (rank == fd->hints->ranklist[i]) {
                fd->my_cb_nodes_index = i;
                return 1;
            }
        }
        fd->my_cb_nodes_index = -1;
    } else if (fd->my_cb_nodes_index != -1)
        return 1;

    return 0;
}

/*
 * If file system implements some version of two-phase -- doesn't have to be
 * generic -- we can still carry out the deferred open optimization
 */
static int uses_generic_read(ADIO_File fd)
{
    if (ADIO_Feature(fd, ADIO_TWO_PHASE))
        return 1;
    return 0;
}

static int uses_generic_write(ADIO_File fd)
{
    if (ADIO_Feature(fd, ADIO_TWO_PHASE))
        return 1;
    return 0;
}

static int build_cb_config_list(ADIO_File fd,
                                MPI_Comm orig_comm, MPI_Comm comm,
                                int rank, int procs, int *error_code)
{
    ADIO_cb_name_array array;
    int *tmp_ranklist;
    int rank_ct;
    char *value;
    static char myname[] = "ADIO_OPEN cb_config_list";

    /* gather the processor name array if we don't already have it */
    /* this has to be done early in ADIO_Open so that we can cache the name
     * array in both the dup'd communicator (in case we want it later) and the
     * original communicator */
    ADIOI_cb_gather_name_array(orig_comm, comm, &array);

/* parse the cb_config_list and create a rank map on rank 0 */
    if (rank == 0) {
        tmp_ranklist = (int *) ADIOI_Malloc(sizeof(int) * procs);
        if (tmp_ranklist == NULL) {
            *error_code = MPIO_Err_create_code(*error_code,
                                               MPIR_ERR_RECOVERABLE,
                                               myname, __LINE__, MPI_ERR_OTHER, "**nomem2", 0);
            return 0;
        }

        rank_ct = ADIOI_cb_config_list_parse(fd->hints->cb_config_list,
                                             array, tmp_ranklist, fd->hints->cb_nodes);

        /* store the ranklist using the minimum amount of memory */
        if (rank_ct > 0) {
            fd->hints->ranklist = (int *) ADIOI_Malloc(sizeof(int) * rank_ct);
            memcpy(fd->hints->ranklist, tmp_ranklist, sizeof(int) * rank_ct);
        }
        ADIOI_Free(tmp_ranklist);
        fd->hints->cb_nodes = rank_ct;
        /* TEMPORARY -- REMOVE WHEN NO LONGER UPDATING INFO FOR FS-INDEP. */
        value = (char *) ADIOI_Malloc((MPI_MAX_INFO_VAL + 1) * sizeof(char));
        snprintf(value, MPI_MAX_INFO_VAL + 1, "%d", rank_ct);
        ADIOI_Info_set(fd->info, "cb_nodes", value);
        ADIOI_Free(value);
    }

    ADIOI_cb_bcast_rank_map(fd);
    if (fd->hints->cb_nodes <= 0) {
        *error_code = MPIO_Err_create_code(MPI_SUCCESS, MPIR_ERR_RECOVERABLE,
                                           myname, __LINE__, MPI_ERR_IO, "**ioagnomatch", 0);
        fd = ADIO_FILE_NULL;
    }
    return 0;
}
