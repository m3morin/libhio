/* -*- Mode: C; c-basic-offset:2 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "hio_types.h"
#include "hio_component.h"
#include "hio_module.h"

#include <stdlib.h>
#include <stdio.h>
#include <ftw.h>

#include <string.h>

#if defined(HAVE_STRINGS_H)
#include <strings.h>
#endif

#include <errno.h>

#include <dirent.h>
#include <unistd.h>

#if defined(HAVE_SYS_STAT_H)
#include <sys/stat.h>
#endif

/* data types */
typedef struct builtin_posix_module_t {
  hio_module_t base;
  mode_t access_mode;
} builtin_posix_module_t;

#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

typedef struct builtin_posix_module_dataset_t {
  struct hio_dataset_t base;
  pthread_mutex_t      lock;
  FILE                *fh;
  uint64_t             bytes_read;
  uint64_t             bytes_written;
  char                *base_path;
} builtin_posix_module_dataset_t;

/** static functions */
static int builtin_posix_module_dataset_unlink (struct hio_module_t *module, const char *name, int64_t set_id);

static void builtin_posix_dataset_path (struct hio_module_t *module, char *path, size_t path_size,
					const char *name, uint64_t set_id) {
  hio_context_t context = module->context;
  snprintf (path, path_size, "%s/%s.hio/%s/%lu", module->data_root,
	    context->context_object.identifier, name, (unsigned long) set_id);
}

static int builtin_posix_create_dataset_dirs (struct hio_module_t *module, const char *name, uint64_t set_id) {
  builtin_posix_module_t *posix_module = (builtin_posix_module_t *) module;
  mode_t access_mode = posix_module->access_mode;
  hio_context_t context = module->context;
  char path[PATH_MAX];
  int rc;

  if (context->context_rank > 0) {
    return HIO_SUCCESS;
  }

  snprintf (path, PATH_MAX, "%s/%s.hio", module->data_root,
	    context->context_object.identifier);

  rc = mkdir (path, access_mode);
  if (0 > rc && EEXIST != errno) {
    hio_err_push (hioi_err_errno (errno), context, NULL, "Error creating context directory: %s", path);
    return hioi_err_errno (errno);
  }

  snprintf (path, PATH_MAX, "%s/%s.hio/%s", module->data_root,
	    context->context_object.identifier, name);

  rc = mkdir (path, access_mode);
  if (0 > rc && EEXIST != errno) {
    hio_err_push (hioi_err_errno (errno), context, NULL, "Error creating context directory: %s", path);
    return hioi_err_errno (errno);
  }

  builtin_posix_dataset_path (module, path, PATH_MAX, name, set_id);
  rc = mkdir (path, access_mode);
  if (0 > rc) {
    hio_err_push (hioi_err_errno (errno), context, NULL, "Error creating context directory: %s", path);
    return hioi_err_errno (errno);
  }

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "Successfully created dataset directories");

  return HIO_SUCCESS;
}

static int builtin_posix_module_dataset_list (struct hio_module_t *module, const char *name,
                                              int64_t **set_ids, int *set_id_count) {
  hio_context_t context = module->context;
  int num_set_ids = 0, set_id_index = 0;
  struct dirent *dp;
  char path[PATH_MAX];
  DIR *dir;

  *set_ids = NULL;
  *set_id_count = 0;

  snprintf (path, PATH_MAX, "%s/%s.hio/%s/", module->data_root,
            context->context_object.identifier, name);

  dir = opendir (path);
  if (NULL == dir) {
    return HIO_SUCCESS;
  }

  while (NULL != (dp = readdir (dir))) {
    if (dp->d_name[0] != '.') {
      num_set_ids++;
    }
  }

  if (0 == num_set_ids) {
    closedir (dir);
    return HIO_SUCCESS;
  }

  rewinddir (dir);

  *set_ids = calloc (num_set_ids, sizeof (uint64_t));
  if (NULL == *set_ids) {
    closedir (dir);
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  while (NULL != (dp = readdir (dir))) {
    if (dp->d_name[0] != '.') {
      set_ids[0][set_id_index++] = strtol (dp->d_name, NULL, 0);
    }
  }

  *set_id_count = num_set_ids;

  return HIO_SUCCESS;
}

static int builtin_posix_module_dataset_open (struct hio_module_t *module,
                                              hio_dataset_t *set_out, const char *name,
                                              int64_t set_id, hio_flags_t flags,
                                              hio_dataset_mode_t mode) {
  hio_context_t context = module->context;
  builtin_posix_module_dataset_t *posix_dataset;
  char path[PATH_MAX];
  int rc = HIO_SUCCESS;

  hioi_log (context, HIO_VERBOSE_DEBUG_MED, "builtin-posix/dataset_open: opening dataset %s:%lu",
	    name, (unsigned long) set_id);

  posix_dataset = (builtin_posix_module_dataset_t *)
    hioi_dataset_alloc (context, name, set_id, flags, mode,
			sizeof (builtin_posix_module_dataset_t));
  if (NULL == posix_dataset) {
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  rc = asprintf (&posix_dataset->base_path, "%s/%s.hio/%s/%lu", module->data_root,
                 context->context_object.identifier, name, (unsigned long) set_id);
  if (0 > rc) {
    /* out of memory. not much can be done now */
    return HIO_ERR_OUT_OF_RESOURCE;
  }
  rc = HIO_SUCCESS;

  if (!(flags & HIO_FLAG_CREAT)) {
    if (!(flags & HIO_FLAG_TRUNC)) {
      snprintf (path, PATH_MAX, "%s/manifest.basic.xml", posix_dataset->base_path);

      if (!access (path, F_OK)) {
        /* if a basic dataset manifest is found switch to basic mode for reading */
        hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "builtin-posix/dataset_open: detected basic dataset. switching "
                  "to basic mode for reading...");
        posix_dataset->base.dataset_file_mode = HIO_FILE_MODE_BASIC;
      }

      if (HIO_FILE_MODE_BASIC != posix_dataset->base.dataset_file_mode) {
        snprintf (path, PATH_MAX, "%s/manifest%05d.xml", posix_dataset->base_path,
                  context->context_rank);
      }

      rc = hioi_manifest_load (&posix_dataset->base, path);
    } else {
      /* access works with directories on OSX and Linux but not work with directories everywhere */
      if (!context->context_rank && !access (path, F_OK)) {
        hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "builtin-posix/dataset_open: removing existing dataset");
        /* blow away an existing dataset */
        rc = builtin_posix_module_dataset_unlink (module, path, set_id);
      }

      /* ensure we take the create path later */
      flags |= HIO_FLAG_CREAT;
    }
  }

  /* basic datasets write a file per element so there is no dataset backing file */
  if (HIO_FILE_MODE_BASIC != posix_dataset->base.dataset_file_mode) {
    if (!posix_dataset->base.dataset_backing_file) {
      if (HIO_SET_ELEMENT_UNIQUE == posix_dataset->base.dataset_mode) {
        snprintf (path, PATH_MAX, "data.%05d", context->context_rank);
      } else {
        snprintf (path, PATH_MAX, "data");
      }

      posix_dataset->base.dataset_backing_file = strdup (path);
    }
  }

  if ((flags & HIO_FLAG_CREAT) && (HIO_SUCCESS == rc)) {
    rc = builtin_posix_create_dataset_dirs (module, name, set_id);
  }


#if HIO_USE_MPI
  /* need to barrier here to ensure directories are created before we try to open
   * any files. might as well pass the return code and exit if any process failed */
  if (hioi_context_using_mpi (context)) {
    MPI_Allreduce (MPI_IN_PLACE, &rc, 1, MPI_INT, MPI_MIN, context->context_comm);
  }
#endif

  if (HIO_SUCCESS != rc) {
    return rc;
  }

  /* in optimized mode all element data is contained in a single file. in basic mode
   * elements are stored in their own files */
  if (posix_dataset->base.dataset_backing_file) {
    const char *file_mode;

    if (flags == HIO_FLAG_RDONLY) {
      file_mode = "r";
    } else {
      file_mode = "w";
    }

    snprintf (path, PATH_MAX, "%s/%s.hio/%s/%lu/%s", module->data_root, context->context_object.identifier,
              name, (unsigned long) set_id, posix_dataset->base.dataset_backing_file);

    posix_dataset->fh = fopen (path, file_mode);
    if (NULL == posix_dataset->fh) {
      rc = HIO_ERR_NOT_FOUND;
    }
  } else {
    posix_dataset->fh = NULL;
  }


#if HIO_USE_MPI
  if (hioi_context_using_mpi (context)) {
    MPI_Allreduce (MPI_IN_PLACE, &rc, 1, MPI_INT, MPI_MIN, context->context_comm);
  }
#endif

  if (HIO_SUCCESS != rc) {
    if (posix_dataset->fh) {
      fclose (posix_dataset->fh);
    }

    free (posix_dataset->base_path);

    hioi_dataset_release ((hio_dataset_t *) &posix_dataset);
    return rc;
  }

  posix_dataset->base.dataset_module = module;

  pthread_mutex_init (&posix_dataset->lock, NULL);

  *set_out = &posix_dataset->base;

  /* set up performance variables */
  hioi_perf_add (context, &posix_dataset->base.dataset_object, &posix_dataset->bytes_read, "total_bytes_read",
                 HIO_CONFIG_TYPE_UINT64, NULL, "Total number of bytes read in this dataset instance", 0);

  hioi_perf_add (context, &posix_dataset->base.dataset_object, &posix_dataset->bytes_written, "total_bytes_written",
                 HIO_CONFIG_TYPE_UINT64, NULL, "Total number of bytes written in this dataset instance", 0);

  return HIO_SUCCESS;
}

static int builtin_posix_module_dataset_close (struct hio_module_t *module, hio_dataset_t dataset) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) dataset;
  hio_context_t context = module->context;
  hio_element_t element;
  int rc;

  if (posix_dataset->fh) {
    fclose (posix_dataset->fh);
    posix_dataset->fh = NULL;
  }

  hioi_list_foreach(element, dataset->dataset_element_list, struct hio_element_t, element_list) {
    if (element->element_fh) {
      fclose (element->element_fh);
      element->element_fh = NULL;
    }
  }

  if (dataset->dataset_flags & HIO_FLAG_WRONLY) {
    char path[PATH_MAX];

    if (HIO_FILE_MODE_BASIC != dataset->dataset_file_mode) {
      snprintf (path, PATH_MAX, "%s/manifest%05d.xml", posix_dataset->base_path,
                context->context_rank);
    } else if (0 == context->context_rank) {
      snprintf (path, PATH_MAX, "%s/manifest.basic.xml", posix_dataset->base_path);
    }

    if (HIO_FILE_MODE_BASIC != dataset->dataset_file_mode || 0 == context->context_rank) {
      rc = hioi_manifest_save (dataset, path);
      if (HIO_SUCCESS != rc) {
        hio_err_push (rc, context, &dataset->dataset_object, "error writing local manifest");
        return rc;
      }
    }
  }

  if (posix_dataset->base_path) {
    free (posix_dataset->base_path);
    posix_dataset->base_path = NULL;
  }

  context->context_bytes_read = posix_dataset->bytes_read;
  context->context_bytes_written = posix_dataset->bytes_written;

  pthread_mutex_destroy (&posix_dataset->lock);

  return HIO_SUCCESS;
}

static int builtin_posix_unlink_cb (const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
  return remove (path);
}

static int builtin_posix_module_dataset_unlink (struct hio_module_t *module, const char *name, int64_t set_id) {
  char path[PATH_MAX];
  struct stat statinfo;
  int rc;

  if (module->context->context_rank) {
    return HIO_ERR_NOT_AVAILABLE;
  }

  builtin_posix_dataset_path (module, path, PATH_MAX, name, set_id);
  if (stat (path, &statinfo)) {
    return hioi_err_errno (errno);
  }

  rc = nftw (path, builtin_posix_unlink_cb, 32, FTW_DEPTH | FTW_PHYS);
  if (0 > rc) {
    fprintf (stderr, "Could not unlink dataset. errno = %d\n", errno);
    return hioi_err_errno (errno);
  }

  return HIO_SUCCESS;
}

static int builtin_posix_module_element_open (struct hio_module_t *module, hio_dataset_t dataset,
                                              hio_element_t *element_out, const char *element_name,
                                              hio_flags_t flags) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) dataset;
  hio_context_t context = dataset->dataset_context;
  char *file_mode;
  hio_element_t element;

  hioi_list_foreach (element, dataset->dataset_element_list, struct hio_element_t, element_list) {
    if (!strcmp (element->element_object.identifier, element_name)) {
      *element_out = element;
      element->element_is_open = true;
      /* nothing more to do with optimized mode */
      return HIO_SUCCESS;
    }
  }

  element = hioi_element_alloc (dataset, element_name);
  if (NULL == element) {
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  if (HIO_FILE_MODE_BASIC == dataset->dataset_file_mode) {
    char path[PATH_MAX];

    if (HIO_SET_ELEMENT_UNIQUE == dataset->dataset_mode) {
      snprintf (path, PATH_MAX, "element_data.%s.%05d", element_name, context->context_rank);
    } else {
      snprintf (path, PATH_MAX, "element_data.%s", element_name);
    }

    element->element_backing_file = strdup (path);
    if (NULL == element->element_backing_file) {
      hioi_element_release (element);
      return HIO_ERR_OUT_OF_RESOURCE;
    }

    if (HIO_FLAG_WRONLY & dataset->dataset_flags) {
      file_mode = "w";
    } else {
      file_mode = "r";
    }

    snprintf (path, PATH_MAX, "%s/%s", posix_dataset->base_path, element->element_backing_file);

    element->element_fh = fopen (path, file_mode);
    if (NULL == element->element_fh) {
      int hrc = hioi_err_errno (errno);
      hio_err_push (hrc, dataset->dataset_context, &dataset->dataset_object, "Error opening element file %s",
                    path);
      hioi_element_release (element);
      return hrc;
    }
  }

  hioi_dataset_add_element (dataset, element);

  hioi_log (dataset->dataset_context, HIO_VERBOSE_DEBUG_LOW, "Created new element %p (idenfier %s) for dataset %s",
	    element, element_name, dataset->dataset_object.identifier);

  *element_out = element;

  return HIO_SUCCESS;
}

static int builtin_posix_module_element_close (struct hio_module_t *module, hio_element_t element) {
  element->element_is_open = false;
  return HIO_SUCCESS;
}

static int builtin_posix_module_element_write_strided_nb (struct hio_module_t *module, hio_element_t element,
                                                          hio_request_t *request, off_t offset, void *ptr,
                                                          size_t count, size_t size, size_t stride) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) element->element_dataset;
  hio_context_t context = posix_dataset->base.dataset_context;
  hio_request_t new_request;
  size_t items_written;
  long file_offset;
  FILE *fh;

  pthread_mutex_lock (&posix_dataset->lock);

  if (HIO_FILE_MODE_BASIC != posix_dataset->base.dataset_file_mode) {
    file_offset = ftell (posix_dataset->fh);
    fh = posix_dataset->fh;
  } else {
    file_offset = offset;
    fseek (element->element_fh, file_offset, SEEK_SET);
    fh = element->element_fh;
  }

  hioi_log(context, HIO_VERBOSE_DEBUG_LOW, "Writing %lu bytes to file offset %lu (%lu)", count * size, file_offset, ftell (fh));

  if (0 < stride) {
    items_written = 0;

    for (int i = 0 ; i < count ; ++i) {
      items_written += fwrite (ptr, size, 1, fh);
      ptr = (void *)((intptr_t) ptr + size + stride);
    }
  } else {
    items_written = fwrite (ptr, size, count, fh);
  }

  hioi_log(context, HIO_VERBOSE_DEBUG_LOW, "Finished write. bytes written: %lu, file offset is now %lu", items_written * size, ftell (fh));

  posix_dataset->bytes_written += items_written * size;

  if (HIO_FILE_MODE_BASIC != posix_dataset->base.dataset_file_mode) {
    hioi_element_add_segment (element, file_offset, offset, 0, size * count);
  }

  pthread_mutex_unlock (&posix_dataset->lock);

  if (request) {
    new_request = hioi_request_alloc (context);
    if (NULL == new_request) {
      return HIO_ERR_OUT_OF_RESOURCE;
    }

    *request = new_request;
    new_request->request_transferred = items_written * size;;
    new_request->request_complete = true;
  }

  return HIO_SUCCESS;
}

static int builtin_posix_module_element_read_strided_nb (struct hio_module_t *module, hio_element_t element,
                                                         hio_request_t *request, off_t offset, void *ptr,
                                                         size_t count, size_t size, size_t stride) {
  builtin_posix_module_dataset_t *posix_dataset = (builtin_posix_module_dataset_t *) element->element_dataset;
  size_t bytes_read, bytes_available, bytes_requested = count * size;
  hio_context_t context = posix_dataset->base.dataset_context;
  size_t remaining_size = size;
  hio_request_t new_request;
  off_t file_offset;
  int rc = HIO_SUCCESS;
  FILE *fh;

  if (HIO_FILE_MODE_BASIC != posix_dataset->base.dataset_file_mode) {
    fh = posix_dataset->fh;
  } else {
    fh = element->element_fh;
  }

  while (bytes_requested) {
    bytes_available = bytes_requested;
    if (HIO_FILE_MODE_BASIC != posix_dataset->base.dataset_file_mode) {
      rc = hioi_element_find_offset (element, offset, 0, &file_offset, &bytes_available);
      if (HIO_SUCCESS != rc) {
        break;
      }
    } else {
      file_offset = offset;
    }

    errno = 0;
    pthread_mutex_lock (&posix_dataset->lock);
    fseek (fh, file_offset, SEEK_SET);
    if (0 < stride) {
      bytes_read = 0;

      for (int i = 0 ; i < count && bytes_available ; ++i) {
        size_t tmp = (bytes_available < remaining_size) ? bytes_available : remaining_size;
        size_t actual_bytes_read;

        actual_bytes_read = fread (ptr, 1, tmp, fh);
        if (!actual_bytes_read) {
          break;
        }

        bytes_read += actual_bytes_read;

        if (tmp < remaining_size) {
          /* partial read */
          ptr = (void *)((intptr_t) ptr + tmp);
          remaining_size = size - tmp;
        } else {
          ptr = (void *)((intptr_t) ptr + remaining_size + stride);
          remaining_size = size;
        }
      }
    } else {
      bytes_read = fread (ptr, 1, bytes_available, fh);
      ptr = (void *)((intptr_t) ptr + bytes_read);
    }

    pthread_mutex_unlock (&posix_dataset->lock);

    posix_dataset->bytes_read += bytes_read;

    if (0 == bytes_read) {
      rc = hioi_err_errno (errno);
      break;
    }

    offset += bytes_read;
    bytes_requested -= bytes_read;
  }

  if (request) {
    new_request = hioi_request_alloc (context);
    if (NULL == new_request) {
      return HIO_ERR_OUT_OF_RESOURCE;
    }

    *request = new_request;
    new_request->request_transferred = (count * size) - bytes_requested;
    new_request->request_complete = true;
  }

  return rc;
}

static int builtin_posix_module_element_flush (struct hio_module_t *module, hio_element_t element,
                                               hio_flush_mode_t mode) {
  hio_dataset_t dataset = element->element_dataset;

  if (!(dataset->dataset_flags & HIO_FLAG_WRONLY)) {
    return HIO_ERR_PERM;
  }

  return HIO_SUCCESS;
}

static int builtin_posix_module_element_complete (struct hio_module_t *module, hio_element_t element) {
  hio_dataset_t dataset = element->element_dataset;

  if (!(dataset->dataset_flags & HIO_FLAG_RDONLY)) {
    return HIO_ERR_PERM;
  }

  return HIO_SUCCESS;
}

static int builtin_posix_module_fini (struct hio_module_t *module) {
  hioi_log (module->context, HIO_VERBOSE_DEBUG_LOW, "Finalizing posix filesystem module for data root %s",
	    module->data_root);

  free (module->data_root);
  free (module);

  return HIO_SUCCESS;
}

hio_module_t builtin_posix_module_template = {
  .dataset_open     = builtin_posix_module_dataset_open,
  .dataset_close    = builtin_posix_module_dataset_close,
  .dataset_unlink   = builtin_posix_module_dataset_unlink,

  .element_open     = builtin_posix_module_element_open,
  .element_close    = builtin_posix_module_element_close,

  .element_write_strided_nb = builtin_posix_module_element_write_strided_nb,
  .element_read_strided_nb  = builtin_posix_module_element_read_strided_nb,

  .element_flush    = builtin_posix_module_element_flush,
  .element_complete = builtin_posix_module_element_complete,

  .dataset_list     = builtin_posix_module_dataset_list,

  .fini             = builtin_posix_module_fini,
};

static int builtin_posix_component_init (void) {
  /* nothing to do */
  return HIO_SUCCESS;
}

static int builtin_posix_component_fini (void) {
  /* nothing to do */
  return HIO_SUCCESS;
}

static int builtin_posix_component_query (hio_context_t context, const char *data_root,
					  hio_module_t **module) {
  builtin_posix_module_t *new_module;

  if (strncasecmp("posix:", data_root, 6)) {
    hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "Module posix does not match for data root %s",
	      data_root);
    return HIO_ERR_NOT_AVAILABLE;
  }

  /* skip posix: */
  data_root += 6;

  new_module = calloc (1, sizeof (builtin_posix_module_t));
  if (NULL == new_module) {
    return HIO_ERR_OUT_OF_RESOURCE;
  }

  memcpy (new_module, &builtin_posix_module_template, sizeof (builtin_posix_module_template));

  new_module->base.data_root = strdup (data_root);
  new_module->base.context = context;

  /* get the current umask */
  new_module->access_mode = umask (0);
  umask (new_module->access_mode);
  new_module->access_mode ^= 0777;

  hioi_log (context, HIO_VERBOSE_DEBUG_LOW, "Created posix filesystem module for data root %s. access mode %o",
	    data_root, new_module->access_mode);

  *module = &new_module->base;

  return HIO_SUCCESS;
}

hio_component_t builtin_posix_component = {
  .init = builtin_posix_component_init,
  .fini = builtin_posix_component_fini,

  .query = builtin_posix_component_query,
  .flags = 0,
  .priority = 10,
};
