/*
  zip_close.c -- close zip archive and update changes
  Copyright (C) 1999-2016 Dieter Baron and Thomas Klausner

  This file is part of libzip, a library to manipulate ZIP archives.
  The authors can be contacted at <libzip@nih.at>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
  1. Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.
  3. The names of the authors may not be used to endorse or promote
     products derived from this software without specific prior
     written permission.
 
  THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS
  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "zipint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif


/* max deflate size increase: size + ceil(size/16k)*5+6 */
#define MAX_DEFLATE_SIZE_32	4293656963u

typedef struct {
    double last_update;  /* last value callback function was called with */

    double start;        /* start of sub-progress setcion */
    double end;          /* end of sub-progress setcion */
} progress_state_t;

static int add_data(zip_t *, zip_source_t *, zip_dirent_t *, progress_state_t *);
static int copy_data(zip_t *, zip_uint64_t, progress_state_t *);
static int copy_source(zip_t *, zip_source_t *, progress_state_t *, zip_int64_t);
static int write_cdir(zip_t *, const zip_filelist_t *, zip_uint64_t);
static void _zip_progress(zip_t *, progress_state_t *, double);

ZIP_EXTERN int
zip_close(zip_t *za)
{
    zip_uint64_t i, j, survivors;
    zip_int64_t off;
    int error;
    zip_filelist_t *filelist;
    int changed;
    progress_state_t progress_state;

    if (za == NULL)
	return -1;

    changed = _zip_changed(za, &survivors);

    /* don't create zip files with no entries */
    if (survivors == 0) {
	if ((za->open_flags & ZIP_TRUNCATE) || changed) {
	    if (zip_source_remove(za->src) < 0) {
		_zip_error_set_from_source(&za->error, za->src);
		return -1;
	    }
	}
	zip_discard(za);
	return 0;
    }	       

    if (!changed) {
	zip_discard(za);
	return 0;
    }

    if (survivors > za->nentry) {
        zip_error_set(&za->error, ZIP_ER_INTERNAL, 0);
        return -1;
    }
    
    if ((filelist=(zip_filelist_t *)malloc(sizeof(filelist[0])*(size_t)survivors)) == NULL)
	return -1;

    /* create list of files with index into original archive  */
    for (i=j=0; i<za->nentry; i++) {
	if (za->entry[i].deleted)
	    continue;

        if (j >= survivors) {
            free(filelist);
            zip_error_set(&za->error, ZIP_ER_INTERNAL, 0);
            return -1;
        }
        
	filelist[j].idx = i;
	j++;
    }
    if (j < survivors) {
        free(filelist);
        zip_error_set(&za->error, ZIP_ER_INTERNAL, 0);
        return -1;
    }

    if (zip_source_begin_write(za->src) < 0) {
	_zip_error_set_from_source(&za->error, za->src);
	free(filelist);
	return -1;
    }
    
    if (za->progress_callback) {
	progress_state.last_update = 0.0;
	za->progress_callback(0.0);
    }
    error = 0;
    for (j=0; j<survivors; j++) {
	int new_data;
	zip_entry_t *entry;
	zip_dirent_t *de;

	if (za->progress_callback) {
	    progress_state.start = (double)j / survivors;
	    progress_state.end = (double)(j+1) / survivors;
	    _zip_progress(za, &progress_state, 0.0);
	}

	i = filelist[j].idx;
	entry = za->entry+i;

	new_data = (ZIP_ENTRY_DATA_CHANGED(entry) || ZIP_ENTRY_CHANGED(entry, ZIP_DIRENT_COMP_METHOD) || ZIP_ENTRY_CHANGED(entry, ZIP_DIRENT_ENCRYPTION_METHOD));

	/* create new local directory entry */
	if (entry->changes == NULL) {
	    if ((entry->changes=_zip_dirent_clone(entry->orig)) == NULL) {
                zip_error_set(&za->error, ZIP_ER_MEMORY, 0);
                error = 1;
                break;
	    }
	}
	de = entry->changes;

	if (_zip_read_local_ef(za, i) < 0) {
	    error = 1;
	    break;
	}

        if ((off = zip_source_tell_write(za->src)) < 0) {
            error = 1;
            break;
        }
        de->offset = (zip_uint64_t)off;

	if (new_data) {
	    zip_source_t *zs;

	    zs = NULL;
	    if (!ZIP_ENTRY_DATA_CHANGED(entry)) {
		if ((zs=_zip_source_zip_new(za, za, i, ZIP_FL_UNCHANGED, 0, 0, NULL)) == NULL) {
		    error = 1;
		    break;
		}
	    }

	    /* add_data writes dirent */
	    if (add_data(za, zs ? zs : entry->source, de, &progress_state) < 0) {
		error = 1;
		if (zs)
		    zip_source_free(zs);
		break;
	    }
	    if (zs)
		zip_source_free(zs);
	}
	else {
	    zip_uint64_t offset;

	    /* when copying data, all sizes are known -> no data descriptor needed */
	    de->bitflags &= (zip_uint16_t)~ZIP_GPBF_DATA_DESCRIPTOR;
	    if (_zip_dirent_write(za, de, ZIP_FL_LOCAL) < 0) {
		error = 1;
		break;
	    }
	    if ((offset=_zip_file_get_offset(za, i, &za->error)) == 0) {
		error = 1;
		break;
	    }
	    if (zip_source_seek(za->src, (zip_int64_t)offset, SEEK_SET) < 0) {
		_zip_error_set_from_source(&za->error, za->src);
		error = 1;
		break;
	    }
	    if (copy_data(za, de->comp_size, &progress_state) < 0) {
		error = 1;
		break;
	    }
	}
    }

    if (!error) {
	if (write_cdir(za, filelist, survivors) < 0)
	    error = 1;
    }

    free(filelist);

    if (!error) {
	if (zip_source_commit_write(za->src) != 0) {
	    _zip_error_set_from_source(&za->error, za->src);
	    error = 1;
	}
    }

    if (error) {
	zip_source_rollback_write(za->src);
	return -1;
    }

    if (za->progress_callback && progress_state.last_update < 1.0) {
	za->progress_callback(1.0);
    }

    zip_discard(za);
    
    return 0;
}


static int
add_data(zip_t *za, zip_source_t *src, zip_dirent_t *de, progress_state_t *progress_state)
{
    zip_int64_t offstart, offdata, offend, data_length;
    struct zip_stat st;
    zip_source_t *src_final, *src_tmp;
    int ret;
    int is_zip64;
    zip_flags_t flags;
    bool needs_recompress, needs_decompress, needs_crc, needs_compress, needs_reencrypt, needs_decrypt, needs_encrypt;

    if (zip_source_stat(src, &st) < 0) {
	_zip_error_set_from_source(&za->error, src);
	return -1;
    }

    if ((st.valid & ZIP_STAT_COMP_METHOD) == 0) {
	st.valid |= ZIP_STAT_COMP_METHOD;
	st.comp_method = ZIP_CM_STORE;
    }

    if (ZIP_CM_IS_DEFAULT(de->comp_method) && st.comp_method != ZIP_CM_STORE)
	de->comp_method = st.comp_method;
    else if (de->comp_method == ZIP_CM_STORE && (st.valid & ZIP_STAT_SIZE)) {
	st.valid |= ZIP_STAT_COMP_SIZE;
	st.comp_size = st.size;
    }
    else {
	/* we'll recompress */
	st.valid &= ~ZIP_STAT_COMP_SIZE;
    }

    if ((st.valid & ZIP_STAT_ENCRYPTION_METHOD) == 0) {
	st.valid |= ZIP_STAT_ENCRYPTION_METHOD;
	st.encryption_method = ZIP_EM_NONE;
    }

    flags = ZIP_EF_LOCAL;

    if ((st.valid & ZIP_STAT_SIZE) == 0) {
	flags |= ZIP_FL_FORCE_ZIP64;
	data_length = -1;
    }
    else {
	de->uncomp_size = st.size;
	/* this is technically incorrect (copy_source counts compressed data), but it's the best we have */
	data_length = (zip_int64_t)st.size;
	
	if ((st.valid & ZIP_STAT_COMP_SIZE) == 0) {
	    if (( ((de->comp_method == ZIP_CM_DEFLATE || ZIP_CM_IS_DEFAULT(de->comp_method)) && st.size > MAX_DEFLATE_SIZE_32)
		  || (de->comp_method != ZIP_CM_STORE && de->comp_method != ZIP_CM_DEFLATE && !ZIP_CM_IS_DEFAULT(de->comp_method))))
		flags |= ZIP_FL_FORCE_ZIP64;
	}
	else
	    de->comp_size = st.comp_size;
    }

    if ((offstart = zip_source_tell_write(za->src)) < 0) {
        return -1;
    }

    /* as long as we don't support non-seekable output, clear data descriptor bit */
    de->bitflags &= (zip_uint16_t)~ZIP_GPBF_DATA_DESCRIPTOR;
    if ((is_zip64=_zip_dirent_write(za, de, flags)) < 0)
	return -1;

    needs_recompress = !((st.comp_method == de->comp_method) || (ZIP_CM_IS_DEFAULT(de->comp_method) && st.comp_method == ZIP_CM_DEFLATE));
    needs_decompress = needs_recompress && (st.comp_method != ZIP_CM_STORE);
    needs_crc = (st.comp_method == ZIP_CM_STORE) || needs_decompress;
    needs_compress = needs_recompress && (de->comp_method != ZIP_CM_STORE);

    needs_reencrypt = needs_recompress || (de->changed & ZIP_DIRENT_PASSWORD) || (de->encryption_method != st.encryption_method);
    needs_decrypt = needs_reencrypt && (st.encryption_method != ZIP_EM_NONE);
    needs_encrypt = needs_reencrypt && (de->encryption_method != ZIP_EM_NONE);

    src_final = src;
    zip_source_keep(src_final);

    if (needs_decrypt) {
	zip_encryption_implementation impl;
	
	if ((impl = _zip_get_encryption_implementation(st.encryption_method, ZIP_CODEC_DECODE)) == NULL) {
	    zip_error_set(&za->error, ZIP_ER_ENCRNOTSUPP, 0);
	    zip_source_free(src_final);
	    return -1;
	}
	if ((src_tmp = impl(za, src_final, st.encryption_method, ZIP_CODEC_DECODE, za->default_password)) == NULL) {
	    /* error set by impl */
	    zip_source_free(src_final);
	    return -1;
	}

	zip_source_free(src_final);
	src_final = src_tmp;
    }
    
    if (needs_decompress) {
	zip_compression_implementation comp_impl;
	
	if ((comp_impl = _zip_get_compression_implementation(st.comp_method, ZIP_CODEC_DECODE)) == NULL) {
	    zip_error_set(&za->error, ZIP_ER_COMPNOTSUPP, 0);
	    zip_source_free(src_final);
	    return -1;
	}
	if ((src_tmp = comp_impl(za, src_final, st.comp_method, ZIP_CODEC_DECODE)) == NULL) {
	    /* error set by comp_impl */
	    zip_source_free(src_final);
	    return -1;
	}

	zip_source_free(src_final);
	src_final = src_tmp;
    }

    if (needs_crc) {
	if ((src_tmp = zip_source_crc(za, src_final, 0)) == NULL) {
	    zip_source_free(src_final);
	    return -1;
	}

	zip_source_free(src_final);
	src_final = src_tmp;
    }

    if (needs_compress) {
	zip_compression_implementation comp_impl;

	if ((comp_impl = _zip_get_compression_implementation(de->comp_method, ZIP_CODEC_ENCODE)) == NULL) {
	    zip_error_set(&za->error, ZIP_ER_COMPNOTSUPP, 0);
	    zip_source_free(src_final);
	    return -1;
	}
	if ((src_tmp = comp_impl(za, src_final, de->comp_method, ZIP_CODEC_ENCODE)) == NULL) {
	    zip_source_free(src_final);
	    return -1;
	}
	
	zip_source_free(src_final);
	src_final = src_tmp;
    }

    
    if (needs_encrypt) {
	zip_encryption_implementation impl;
	const char *password = NULL;

	if (de->password) {
	    password = de->password;
	} else if (za->default_password) {
	    password = za->default_password;
	}
	
	if ((impl = _zip_get_encryption_implementation(de->encryption_method, ZIP_CODEC_ENCODE)) == NULL) {
	    zip_error_set(&za->error, ZIP_ER_ENCRNOTSUPP, 0);
	    zip_source_free(src_final);
	    return -1;
	}
	if ((src_tmp = impl(za, src_final, de->encryption_method, ZIP_CODEC_ENCODE, password)) == NULL) {
	    /* error set by impl */
	    zip_source_free(src_final);
	    return -1;
	}

	zip_source_free(src_final);
	src_final = src_tmp;
    }


    if ((offdata = zip_source_tell_write(za->src)) < 0) {
        return -1;
    }

    ret = copy_source(za, src_final, progress_state, data_length);
	
    if (zip_source_stat(src_final, &st) < 0) {
	ret = -1;
    }

    zip_source_free(src_final);

    if (ret < 0) {
	return -1;
    }

    if ((offend = zip_source_tell_write(za->src)) < 0) {
        return -1;
    }

    if (zip_source_seek_write(za->src, offstart, SEEK_SET) < 0) {
	_zip_error_set_from_source(&za->error, za->src);
	return -1;
    }

    if ((st.valid & (ZIP_STAT_COMP_METHOD|ZIP_STAT_CRC|ZIP_STAT_SIZE)) != (ZIP_STAT_COMP_METHOD|ZIP_STAT_CRC|ZIP_STAT_SIZE)) {
	zip_error_set(&za->error, ZIP_ER_INTERNAL, 0);
	return -1;
    }

    if ((de->changed & ZIP_DIRENT_LAST_MOD) == 0) {
        if (st.valid & ZIP_STAT_MTIME)
            de->last_mod = st.mtime;
        else
            time(&de->last_mod);
    }
    de->comp_method = st.comp_method;
    de->crc = st.crc;
    de->uncomp_size = st.size;
    de->comp_size = (zip_uint64_t)(offend - offdata);

    if ((ret=_zip_dirent_write(za, de, flags)) < 0)
	return -1;
 
    if (is_zip64 != ret) {
	/* Zip64 mismatch between preliminary file header written before data and final file header written afterwards */
	zip_error_set(&za->error, ZIP_ER_INTERNAL, 0);
	return -1;
    }

   
    if (zip_source_seek_write(za->src, offend, SEEK_SET) < 0) {
	_zip_error_set_from_source(&za->error, za->src);
	return -1;
    }

    return 0;
}


static int
copy_data(zip_t *za, zip_uint64_t len, progress_state_t *progress_state)
{
    zip_uint8_t buf[BUFSIZE];
    size_t n;
    double total = (double)len;

    while (len > 0) {
	n = len > sizeof(buf) ? sizeof(buf) : len;
	if (_zip_read(za->src, buf, n, &za->error) < 0) {
	    return -1;
	}

	if (_zip_write(za, buf, n) < 0) {
	    return -1;
	}
	
	len -= n;
	
	if (za->progress_callback) {
	    _zip_progress(za, progress_state, (total - len) / total);
	}
    }

    return 0;
}


static int
copy_source(zip_t *za, zip_source_t *src, progress_state_t *progress_state, zip_int64_t data_length)
{
    zip_uint8_t buf[BUFSIZE];
    zip_int64_t n, current;
    int ret;

    if (zip_source_open(src) < 0) {
	_zip_error_set_from_source(&za->error, src);
	return -1;
    }

    ret = 0;
    current = 0;
    while ((n=zip_source_read(src, buf, sizeof(buf))) > 0) {
	if (_zip_write(za, buf, (zip_uint64_t)n) < 0) {
	    ret = -1;
	    break;
	}
	if (n == sizeof(buf) && za->progress_callback && data_length > 0) {
	    current += n;
	    _zip_progress(za, progress_state, current/(double)data_length);
	}
    }
    
    if (n < 0) {
	_zip_error_set_from_source(&za->error, src);
	ret = -1;
    }

    zip_source_close(src);
    
    return ret;
}


static int
write_cdir(zip_t *za, const zip_filelist_t *filelist, zip_uint64_t survivors)
{
    zip_int64_t cd_start, end, size;
    
    if ((cd_start = zip_source_tell_write(za->src)) < 0) {
        return -1;
    }

    if ((size=_zip_cdir_write(za, filelist, survivors)) < 0) {
	return -1;
    }
    
    if ((end = zip_source_tell_write(za->src)) < 0) {
        return -1;
    }

    return 0;
}


int
_zip_changed(const zip_t *za, zip_uint64_t *survivorsp)
{
    int changed;
    zip_uint64_t i, survivors;

    changed = 0;
    survivors = 0;

    if (za->comment_changed || za->ch_flags != za->flags)
	changed = 1;

    for (i=0; i<za->nentry; i++) {
	if (za->entry[i].deleted || za->entry[i].source || (za->entry[i].changes && za->entry[i].changes->changed != 0))
	    changed = 1;
	if (!za->entry[i].deleted)
	    survivors++;
    }

    if (survivorsp)
	*survivorsp = survivors;

    return changed;
}

static void
_zip_progress(zip_t *za, progress_state_t *progress_state, double sub_current)
{
    double current;

    if (za->progress_callback == NULL) {
	return;
    }

    current = ZIP_MIN(ZIP_MAX(sub_current, 0.0), 1.0) * (progress_state->end - progress_state->start) + progress_state->start;

    if (current - progress_state->last_update > 0.001) {
	za->progress_callback(current);
	progress_state->last_update = current;
    }
}
