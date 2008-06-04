/*
   Copyright (c) 2008 Z RESEARCH, Inc. <http://www.zresearch.com>
   This file is part of GlusterFS.

   GlusterFS is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3 of the License,
   or (at your option) any later version.

   GlusterFS is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see
   <http://www.gnu.org/licenses/>.
*/

/* bdb based storage translator - named as 'bdb' translator
 * 
 * 
 * There can be only two modes for files existing on bdb translator:
 * 1. DIRECTORY - directories are stored by bdb as regular directories on background file-system. 
 *                directories also have an entry in the ns_db.db of their parent directory.
 * 2. REGULAR FILE - regular files are stored as records in the storage_db.db present in the directory.
 *                   regular files also have an entry in ns_db.db
 *
 * Internally bdb has a maximum of three different types of logical files associated with each directory:
 * 1. storage_db.db - storage database, used to store the data corresponding to regular files in the
 *                   form of key/value pair. file-name is the 'key' and data is 'value'.
 * 2. directory (all subdirectories) - any subdirectory will have a regular directory entry.
 */
#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#define __XOPEN_SOURCE 500

#include <stdint.h>
#include <sys/time.h>
#include <errno.h>
#include <ftw.h>
#include <libgen.h>

#include "glusterfs.h"
#include "dict.h"
#include "logging.h"
#include "bdb.h"
#include "xlator.h"
#include "lock.h"
#include "defaults.h"
#include "common-utils.h"

/* to be used only by fops, nobody else */
#define BDB_ENV(this) ((((struct bdb_private *)this->private)->b_table)->dbenv)
#define B_TABLE(this) (((struct bdb_private *)this->private)->b_table)

int32_t 
bdb_mknod (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc,
	   mode_t mode,
	   dev_t dev)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *db_path = NULL;
  bctx_t *bctx = NULL;
  struct stat stbuf = {0,};

  if (S_ISREG(mode)) {
    if (((bctx = bctx_parent (B_TABLE(this), loc->path)) != NULL)) {
      char *key_string = NULL;

      MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
      
      lstat (db_path, &stbuf);
      MAKE_KEY_FROM_PATH (key_string, loc->path);
      op_ret = bdb_storage_put (bctx, NULL, key_string, NULL, 0, 0, 0);
      if (!op_ret) {
	/* create successful */
	lstat (db_path, &stbuf);
	stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
	stbuf.st_mode  = mode;
	stbuf.st_size = 0;
	stbuf.st_blocks = BDB_COUNT_BLOCKS (stbuf.st_size, stbuf.st_blksize);
      } else {
	gf_log (this->name,
		GF_LOG_ERROR,
		"bdb_storage_get() failed for path: %s", loc->path);
	op_ret = -1;
	op_errno = ENOENT;
      }/* if (!op_ret)...else */
      bctx_unref (bctx);
    } else {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to get bctx for path: %s", loc->path);
      op_ret = -1;
      op_errno = ENOENT;
    }/* if(bctx=bdb_get_bctx_from()...)...else */
  } else {
    gf_log (this->name,
	    GF_LOG_DEBUG,
	    "mknod for non-regular file");
    op_ret = -1;
    op_errno = EPERM;
  } /* if (S_ISREG(mode))...else */

  frame->root->rsp_refs = NULL;  
  
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf);
  return 0;
}


int32_t 
bdb_rename (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *oldloc,
	    loc_t *newloc)
{
  bctx_t *oldbctx = NULL, *newbctx = NULL;
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;
  int32_t read_size = 0;
  struct stat stbuf = {0,};
  char *real_newpath = NULL;
  DB_TXN *txnid = NULL;

  if (S_ISREG (oldloc->inode->st_mode)) {
    char *oldkey = NULL, *newkey = NULL;
    char *buf = NULL;
    
    oldbctx = bctx_parent (B_TABLE(this), oldloc->path);
    MAKE_REAL_PATH (real_newpath, this, newloc->path);
    op_ret = lstat (real_newpath, &stbuf);
    if ((op_ret == 0) && (S_ISDIR (stbuf.st_mode))) {
      op_ret = -1;
      op_errno = EISDIR;
    } else if (op_ret == 0) {
      /* destination is a symlink */
      MAKE_KEY_FROM_PATH (oldkey, oldloc->path);
      MAKE_KEY_FROM_PATH (newkey, newloc->path);

      op_ret = unlink (real_newpath);
      newbctx = bctx_parent (B_TABLE (this), newloc->path);

      op_ret = bdb_txn_begin (BDB_ENV(this), &txnid);

      if ((read_size = bdb_storage_get (oldbctx, txnid, oldkey, &buf, 0, 0)) < 0) {
	bdb_txn_abort (txnid);
      } else if ((op_ret = bdb_storage_del (oldbctx, txnid, oldkey)) != 0) {
	bdb_txn_abort (txnid);
      } else if ((op_ret = bdb_storage_put (newbctx, txnid, newkey, buf, read_size, 0, 0)) != 0) {
	bdb_txn_abort (txnid);
      } else {
	bdb_txn_commit (txnid);
      }
      
      bctx_unref (newbctx);
    } else {
      /* destination doesn't exist or a regular file */
      MAKE_KEY_FROM_PATH (oldkey, oldloc->path);
      MAKE_KEY_FROM_PATH (newkey, newloc->path);

      newbctx = bctx_parent (B_TABLE (this), newloc->path);
      op_ret = bdb_txn_begin (BDB_ENV(this), &txnid);

      if ((read_size = bdb_storage_get (oldbctx, txnid, oldkey, &buf, 0, 0)) < 0) {
	bdb_txn_abort (txnid);
      } else if ((op_ret = bdb_storage_del (oldbctx, txnid, oldkey)) != 0) {
	bdb_txn_abort (txnid);
      } else if ((op_ret = bdb_storage_put (newbctx, txnid, newkey, buf, read_size, 0, 0)) != 0) {
	bdb_txn_abort (txnid);
      } else {
	bdb_txn_commit (txnid);
      }
      
      bctx_unref (newbctx);
    }
    bctx_unref (oldbctx);
  } else if (S_ISLNK (oldloc->inode->st_mode)) {
    MAKE_REAL_PATH (real_newpath, this, newloc->path);
    op_ret = lstat (real_newpath, &stbuf);
    if ((op_ret == 0) && (S_ISDIR (stbuf.st_mode))) {
      op_ret = -1;
      op_errno = EISDIR;
    } else if (op_ret == 0){
      char *real_oldpath = NULL;
      MAKE_REAL_PATH (real_oldpath, this, oldloc->path);
      op_ret = rename (real_oldpath, real_newpath);
      op_errno = errno;
    } else {
      char *newkey = NULL;
      char *real_oldpath = NULL;
      MAKE_REAL_PATH (real_oldpath, this, oldloc->path);
      MAKE_KEY_FROM_PATH (newkey, newloc->path);
      newbctx = bctx_parent (B_TABLE (this), newloc->path);
      if ((op_ret = bdb_storage_del (newbctx, txnid, newkey)) != 0) {
	/* no problem */
      } 
      op_ret = rename (real_oldpath, real_newpath);
      op_errno = errno;

      bctx_unref (newbctx);
    }
  }
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);
  return 0;
}

int32_t 
bdb_link (call_frame_t *frame, 
	  xlator_t *this,
	  loc_t *oldloc,
	  const char *newpath)
{
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, -1, EPERM, NULL, NULL);
  return 0;
}


int32_t 
bdb_create (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc,
	    int32_t flags,
	    mode_t mode,
	    fd_t *fd)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *db_path = NULL;
  struct stat stbuf = {0,};
  bctx_t *bctx = NULL;
  struct bdb_private *private = this->private;

  if (((bctx = bctx_parent (B_TABLE(this), loc->path)) != NULL)) {
    char *key_string = NULL;
    
    MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
    
    lstat (db_path, &stbuf);
    MAKE_KEY_FROM_PATH (key_string, loc->path);
    op_ret = bdb_storage_put (bctx, NULL, key_string, NULL, 0, 0, 0);
    if (!op_ret) {
      /* create successful */
      struct bdb_fd *bfd = calloc (1, sizeof (*bfd));
      ERR_ABORT (bfd);
      
      /* NOTE: bdb_get_bctx_from () returns bctx with a ref */
      bfd->ctx = bctx; 
      bfd->key = strdup (key_string);

      BDB_SET_BFD (this, fd, bfd);
      
      lstat (db_path, &stbuf);
      stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
      stbuf.st_mode = private->file_mode;
      stbuf.st_size = 0;
      stbuf.st_nlink = 1;
      stbuf.st_blocks = BDB_COUNT_BLOCKS (stbuf.st_size, stbuf.st_blksize);
    } else {
      op_ret = -1;
      op_errno = EINVAL;
    }/* if (!op_ret)...else */
  } else {
    op_ret = -1;
    op_errno = ENOENT;
  }/* if(bctx_data...)...else */

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, fd, loc->inode, &stbuf);

  return 0;
}


/* bdb_open
 *
 * as input parameters bdb_open gets the file name, i.e key. bdb_open should effectively 
 * do: store key, open storage db, store storage-db pointer.
 *
 */
int32_t 
bdb_open (call_frame_t *frame,
	  xlator_t *this,
	  loc_t *loc,
	  int32_t flags,
	  fd_t *fd)
{
  int32_t op_ret = 0;
  int32_t op_errno = 0;
  bctx_t *bctx = NULL;
  struct bdb_fd  *bfd = NULL;

  if (((bctx = bctx_parent (B_TABLE(this), loc->path)) == NULL)) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific data", this->name);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    char *key_string = NULL;
    /* we are ready to go now, wat do we do??? do nothing... just place the same ctx which is there in 
     * inode->ctx to fd->ctx... ashTe ashTe anta open storage_db for this directory and place that pointer too,
     * in ctx, check if someone else has opened the same storage db already. */

    /* successfully opened storage db */
    bfd = calloc (1, sizeof (*bfd));
    if (!bfd) {
      op_ret = -1;
      op_errno = ENOMEM;
    } else {
      /* NOTE: bctx_parent () returns bctx with a ref */
      bfd->ctx = bctx;
      
      MAKE_KEY_FROM_PATH (key_string, loc->path);
      bfd->key = strdup (key_string);
      
      BDB_SET_BFD (this, fd, bfd);
    }/* if(!bfd)...else */
  } /* if((inode->ctx == NULL)...)...else */

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, fd);

  return 0;
}

int32_t 
bdb_readv (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd,
	   size_t size,
	   off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = EBADFD;
  struct iovec vec = {0,};
  struct stat stbuf = {0,};
  struct bdb_fd *bfd = NULL;  
  dict_t *reply_dict = NULL;
  char *buf = NULL;

  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific information from fd:%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    /* we are ready to go */
    op_ret = bdb_storage_get (bfd->ctx, NULL, bfd->key, &buf, size, offset);
    if (op_ret == -1) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to do db_storage_get()");
      op_ret = -1;
      op_errno = ENOENT;
    } else if (op_ret) {
      data_t *buf_data = get_new_data ();
      char *db_path = NULL;
      reply_dict = get_new_dict ();
      
      reply_dict->is_locked = 1;
      buf_data->is_locked = 1;
      buf_data->data      = buf;
      if (size < op_ret) {
	op_ret = size;
      }
      buf_data->len       = op_ret;
      
      dict_set (reply_dict, NULL, buf_data);
      
      frame->root->rsp_refs = dict_ref (reply_dict);
      vec.iov_base = buf;
      vec.iov_len = op_ret;
      
      MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bfd->ctx->directory);
      lstat (db_path, &stbuf);
      stbuf.st_ino = fd->inode->ino;
      stbuf.st_size = op_ret ; 
      stbuf.st_blocks = BDB_COUNT_BLOCKS (stbuf.st_size, stbuf.st_blksize);
    } /* if(op_ret == -1)...else */
  }/* if((fd->ctx == NULL)...)...else */
  
  STACK_UNWIND (frame, op_ret, op_errno, &vec, 1, &stbuf);

  if (reply_dict)
    dict_unref (reply_dict);

  return 0;
}


int32_t 
bdb_writev (call_frame_t *frame,
	    xlator_t *this,
	    fd_t *fd,
	    struct iovec *vector,
	    int32_t count,
	    off_t offset)
{
  int32_t op_ret = 0;
  int32_t op_errno = EPERM;
  struct stat stbuf = {0,};
  struct bdb_fd *bfd = NULL;
 
  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific information from fd:%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    /* we are ready to go */
    int32_t idx = 0;
    off_t c_off = offset;
    int32_t c_ret = -1;
    
    for (idx = 0; idx < count; idx++) {
      c_ret = bdb_storage_put (bfd->ctx, NULL, bfd->key, 
			       vector[idx].iov_base, vector[idx].iov_len, 
			       c_off, 0);
      if (c_ret) {
	gf_log (this->name,
		GF_LOG_ERROR,
		"failed to do bdb_storage_put at offset: %d for file: %s", c_off, bfd->key);
	break;
      } else {
	c_off += vector[idx].iov_len;
      }
      op_ret += vector[idx].iov_len;
    } /* for(idx=0;...)... */
    
    if (c_ret) {
      /* write failed */
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to do bdb_storage_put(): %s", db_strerror (op_ret));
      op_ret = -1;
      op_errno = EBADFD; /* TODO: search for a more meaningful errno */
    } else {
      char *db_path = NULL;
      MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bfd->ctx->directory);
      lstat (db_path, &stbuf);
      /* NOTE: we want to increment stbuf->st_size, as stored in db */
      stbuf.st_size = op_ret;
      stbuf.st_blocks = BDB_COUNT_BLOCKS (stbuf.st_size, stbuf.st_blksize);
      op_errno = 0;
    }/* if(op_ret)...else */
  }/* if((fd->ctx == NULL)...)...else */
  
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);
  return 0;
}

int32_t 
bdb_flush (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct bdb_fd *bfd = NULL;

  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL) {
    gf_log (this->name, 
	    GF_LOG_ERROR, 
	    "failed to extract fd data from fd=%p", fd);
    op_ret = -1;
    op_errno = EBADF;
  } else {
    /* do nothing, as posix says */
    op_ret = 0;
    op_errno = 0;
  } /* if((fd == NULL)...)...else */
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

int32_t 
bdb_close (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd)
{
  int32_t op_ret = -1;
  int32_t op_errno = EBADFD;
  struct bdb_fd *bfd = NULL;
  
  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL){
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific information from fd:%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    dict_del (fd->ctx, this->name);
    
    bctx_unref (bfd->ctx);
    bfd->ctx = NULL; 
    
    if (bfd->key)
      free (bfd->key); /* we did strdup() in bdb_open() */
    free (bfd);
    op_ret = 0;
    op_errno = 0;
  } /* if((fd->ctx == NULL)...)...else */

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}/* bdb_close */


int32_t 
bdb_fsync (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd,
	   int32_t datasync)
{
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, 0, 0);
  return 0;
}/* bdb_fsync */

int32_t 
bdb_lk (call_frame_t *frame,
	xlator_t *this,
	fd_t *fd,
	int32_t cmd,
	struct flock *lock)
{
  struct flock nullock = {0, };

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, -1, EPERM, &nullock);
  return 0;
}/* bdb_lk */

int32_t
bdb_forget (call_frame_t *frame,
	    xlator_t *this,
	    inode_t *inode)
{
  return 0;
}/* bdb_forget */


/* bdb_lookup
 *
 * bdb_lookup looks up for a pathname in ns_db.db and returns the struct stat as read from ns_db.db,
 * if required
 */
int32_t
bdb_lookup (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc,
	    int32_t need_xattr)
{
  struct stat stbuf = {0, };
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;
  dict_t *xattr = NULL;
  char *pathname = NULL, *directory = NULL, *real_path = NULL;
  bctx_t *bctx = NULL;
  char *db_path = NULL;
  struct bdb_private *private = this->private;

  MAKE_REAL_PATH (real_path, this, loc->path);
  pathname = strdup (loc->path);
  directory = dirname (pathname);

  if (!strcmp (directory, loc->path)) {
    /* SPECIAL CASE: looking up root */
    op_ret = lstat (real_path, &stbuf);
    op_errno = errno;
    
    if (op_ret == 0) {
      /* bctx_lookup() returns NULL only when its time to wind up, we should shutdown functioning */
      bctx = bctx_lookup (B_TABLE(this), (char *)loc->path);
      if (bctx != NULL) {
	stbuf.st_ino = 1;
	stbuf.st_mode = private->dir_mode;
	bctx_unref (bctx);
      } else {
	gf_log (this->name,
		GF_LOG_CRITICAL,
		"bctx_lookup failed: out of memory");
	op_ret = -1;
	op_errno = ENOMEM;
      }
    } else {
      /* lstat failed, no way we can exist */
      gf_log (this->name,
	      GF_LOG_CRITICAL,
	      "failed to lookup root of this fs");
      op_ret = -1;
      op_errno = ENOTCONN;
    }/* if(op_ret == 0)...else */
  } else {
    char *key_string = NULL;
    
    MAKE_KEY_FROM_PATH (key_string, loc->path);
    op_ret = lstat (real_path, &stbuf);
    if ((op_ret == 0) && (S_ISDIR (stbuf.st_mode))){
      bctx = bctx_lookup (B_TABLE(this), (char *)loc->path);
      if (bctx != NULL) {
	if (loc->inode->ino) {
	/* revalidating directory inode */
	  gf_log (this->name,
		  GF_LOG_DEBUG,
		  "revalidating directory %s", (char *)loc->path);
	  stbuf.st_ino = loc->inode->ino;
	} else {
	  stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
	}
	bctx_unref (bctx);
      } else {
	gf_log (this->name,
		GF_LOG_CRITICAL,
		"bctx_lookup failed: out of memory");
	op_ret = -1;
	op_errno = ENOMEM;
      }
      stbuf.st_mode = private->dir_mode;
    } else if (op_ret == 0) {
      /* a symlink */
      gf_log (this->name,
	      GF_LOG_DEBUG,
	      "lookup called for symlink: %s", loc->path);
      if ((bctx = bctx_parent (B_TABLE(this), loc->path)) != NULL){
	if (loc->inode->ino) {
	  stbuf.st_ino = loc->inode->ino;
	} else {
	  stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
	}
	stbuf.st_mode = private->symlink_mode;
	bctx_unref (bctx);
      } else {
	gf_log (this->name,
		GF_LOG_DEBUG,
		"failed to get bctx for symlink %s's parent", loc->path);
	op_ret = -1;
	op_errno = ENOENT;
      }
    } else{
      if ((bctx = bctx_parent (B_TABLE(this), loc->path)) != NULL){
	int32_t entry_size = 0;
	char *file_content = NULL;
	
	if (need_xattr) {
	  op_ret = entry_size = bdb_storage_get (bctx, NULL, loc->path, &file_content, 0, 0);
	} else {
	  op_ret = entry_size = bdb_storage_get (bctx, NULL, loc->path, NULL, 0, 0);
	}

	if (op_ret == -1) {
	  /* lookup failed, entry doesn't exist */
	  op_ret = -1;
	  op_errno = ENOENT;
	} else {
	  MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
	  op_ret = lstat (db_path, &stbuf);
	  op_errno = errno;

	  if (need_xattr >= entry_size && entry_size && file_content) {
	    data_t *file_content_data = data_from_dynptr (file_content, entry_size);
	    xattr = get_new_dict ();
	    dict_set (xattr, "glusterfs.content", file_content_data);
	  } else {
	    if (file_content)
	      free (file_content);
	  }

	  if (loc->inode->ino) {
	    /* revalidate */
	    stbuf.st_ino = loc->inode->ino;
	    stbuf.st_size = entry_size;
	    stbuf.st_blocks = BDB_COUNT_BLOCKS (stbuf.st_size, stbuf.st_blksize);
	  } else {
	    /* fresh lookup, create an inode number */
	    stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
	    stbuf.st_size = entry_size;
	    stbuf.st_blocks = BDB_COUNT_BLOCKS (stbuf.st_size, stbuf.st_blksize);
	  }/* if(inode->ino)...else */
	  stbuf.st_nlink = 1;
	  stbuf.st_mode = private->file_mode;
	}/* if(op_ret == DB_NOTFOUND)...else, after lstat() */
	bctx_unref (bctx);
      }/* if(bctx = ...)...else, after bdb_ns_get() */
    } /* if(op_ret == 0)...else, after lstat(real_path, ...) */
  }/* if(loc->parent)...else */
  
  frame->root->rsp_refs = NULL;

  if (pathname)
    free (pathname);
  
  if (xattr)
    dict_ref (xattr);
  
  /* NOTE: ns_database of database which houses this entry is kept open */
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf, xattr);
  
  if (xattr)
    dict_unref (xattr);
  
  
  return 0;
  
}/* bdb_lookup */

int32_t
bdb_stat (call_frame_t *frame,
	  xlator_t *this,
	  loc_t *loc)
{
 
  struct stat stbuf = {0,};
  char *real_path = NULL;
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;
  struct bdb_private *private = this->private;

  MAKE_REAL_PATH (real_path, this, loc->path);

  op_ret = lstat (real_path, &stbuf);
  op_errno = errno;
  
  if (op_ret == 0) {
    /* directory or symlink */
    stbuf.st_ino = loc->inode->ino;
    if (S_ISDIR(stbuf.st_mode))
      stbuf.st_mode = private->dir_mode;
    else
      stbuf.st_mode = private->symlink_mode;
  } else {
    bctx_t *bctx = NULL;

    if ((bctx = bctx_parent (B_TABLE(this), loc->path)) == NULL) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to get bctx for %s", loc->path);
      op_ret = -1;
      op_errno = ENOENT; /* TODO: better errno */
    } else {
      char *db_path = NULL;
      MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
      op_ret = lstat (db_path, &stbuf);
      
      if (op_ret == -1) {
	op_errno = errno;
      } else {
	op_errno = errno;
	stbuf.st_size = bdb_storage_get (bctx, NULL, loc->path, NULL, 0, 0);
	stbuf.st_blocks = BDB_COUNT_BLOCKS (stbuf.st_size, stbuf.st_blksize);
	stbuf.st_ino = loc->inode->ino;
      }
      bctx_unref (bctx);
    }    
  }

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}/* bdb_stat */



/* bdb_opendir - in the world of bdb, open/opendir is all about opening correspondind databases.
 *               opendir in particular, opens the database for the directory which is
 *               to be opened. after opening the database, a cursor to the database is also created.
 *               cursor helps us get the dentries one after the other, and cursor maintains the state
 *               about current positions in directory. pack 'pointer to db', 'pointer to the
 *               cursor' into struct bdb_dir and store it in fd->ctx, we get from our parent xlator.
 *
 * @frame: call frame
 * @this:  our information, as we filled during init()
 * @loc:   location information
 * @fd:    file descriptor structure (glusterfs internal)
 *
 * return value - immaterial, async call.
 *
 */
int32_t 
bdb_opendir (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc, 
	     fd_t *fd)
{
  char *real_path;
  int32_t op_ret = 0;
  int32_t op_errno = 0;
  bctx_t *bctx = NULL;
  
  MAKE_REAL_PATH (real_path, this, loc->path);

  if ((bctx = bctx_lookup (B_TABLE(this), (char *)loc->path)) == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific data from private data", this->name);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    struct bdb_dir *bfd = calloc (1, sizeof (*bfd));
    
    if (!bfd) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to allocate memory for bfd. out of memory. :O");
      op_ret = -1;
      op_errno = ENOMEM;
    } else {
      bfd->dir = opendir (real_path);
      /* NOTE: bctx_lookup() return bctx with ref */
      bfd->ctx = bctx; 
      bfd->path = strdup (real_path);
      BDB_SET_BFD (this, fd, bfd);
    } /* if(!bfd)...else */
  } /* if(bctx=...)...else */
  
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, fd);

  return 0;
}/* bdb_opendir */


int32_t
bdb_getdents (call_frame_t *frame,
	      xlator_t     *this,
	      fd_t         *fd,
	      size_t        size,
	      off_t         off,
	      int32_t       flag)
{
  int32_t         op_ret   = 0;
  int32_t         op_errno = 0;
  char           *real_path = NULL;
  dir_entry_t     entries = {0, };
  dir_entry_t    *tmp = NULL;
  DIR            *dir = NULL;
  struct dirent  *dirent = NULL;
  int32_t         real_path_len = 0;
  int32_t         entry_path_len = 0;
  char           *entry_path = NULL;
  int32_t         count = 0;
  struct bdb_dir *bfd = NULL;

  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL) {
    gf_log (this->name, GF_LOG_ERROR,
	    "failed to extract %s specific fd information from fd=%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    op_ret = 0;
    op_errno = 0;
    MAKE_REAL_PATH (real_path, this, bfd->path);
    dir = bfd->dir;

    while ((dirent = readdir (dir))) {
      if (!dirent)
	break;
      
      struct stat buf = {0,};
      int32_t ret = -1;

      if (!IS_BDB_PRIVATE_FILE(dirent->d_name)) {
	BDB_DO_LSTAT(real_path, &buf, dirent);
	
	if ((flag == GF_GET_DIR_ONLY) && (ret != -1 && !S_ISDIR(buf.st_mode))) {
	  continue;
	}

	tmp = calloc (1, sizeof (*tmp));
	ERR_ABORT (tmp);
	tmp->name = strdup (dirent->d_name);
	if (entry_path_len < real_path_len + 1 + strlen (tmp->name) + 1) {
	  entry_path_len = real_path_len + strlen (tmp->name) + 1024;
	  entry_path = realloc (entry_path, entry_path_len);
	  ERR_ABORT (entry_path);
	}
	strcpy (&entry_path[real_path_len+1], tmp->name);
	lstat (entry_path, &tmp->buf);
	if (S_ISLNK(tmp->buf.st_mode)) {
	  char linkpath[PATH_MAX] = {0,};
	  ret = readlink (entry_path, linkpath, PATH_MAX);
	  if (ret != -1) {
	    linkpath[ret] = '\0';
	    tmp->link = strdup (linkpath);
	  }
	} else {
	  tmp->link = "";
	}

	count++;
	
	tmp->next = entries.next;
	entries.next = tmp;
	/* if size is 0, count can never be = size, so entire dir is read */
      } else {
	/* do nothing */
      }
      if (count == size)
	break;
    }
    
    if (flag != GF_GET_DIR_ONLY && count < size) {
      /* read from db */
      DBC *cursorp = NULL;
      op_ret = bdb_open_db_cursor (bfd->ctx, &cursorp);

      if (op_ret == -1) {
	gf_log (this->name,
		GF_LOG_ERROR,
		"failed to open cursorp for directory %s", bfd->ctx->directory);
	op_ret = -1;
	op_errno = ENOENT;
      } else {
	char *db_path = NULL;
	struct stat db_stbuf = {0,};
	
	MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bfd->ctx->directory);
	lstat (db_path, &db_stbuf);
	/* read all the entries in database, one after the other and put into dictionary */
	while (1) {
	  DBT key = {0,}, value = {0,};
	  
	  key.flags = DB_DBT_MALLOC;
	  value.flags = DB_DBT_MALLOC;
	  op_ret = bdb_cursor_get (cursorp, &key, &value, DB_NEXT);
	  
	  if (op_ret == DB_NOTFOUND) {
	    gf_log (this->name,
		    GF_LOG_DEBUG,
		    "end of list of key/value pair in db for directory: %s", bfd->ctx->directory);
	    op_ret = 0;
	    op_errno = 0;
	    break;
	  } else if (op_ret == 0){
	    /* successfully read */
	    tmp = calloc (1, sizeof (*tmp));
	    ERR_ABORT (tmp);
	    tmp->name = calloc (1, key.size + 1);
	    ERR_ABORT (tmp->name);
	    memcpy (tmp->name, key.data, key.size);
	    tmp->buf = db_stbuf;
	    tmp->buf.st_size = bdb_storage_get (bfd->ctx, NULL, tmp->name, NULL, 0, 0);
	    tmp->buf.st_blocks = BDB_COUNT_BLOCKS (tmp->buf.st_size, tmp->buf.st_blksize);
	    /* FIXME: wat will be the effect of this? */
	    tmp->buf.st_ino = bdb_inode_transform (db_stbuf.st_ino, bfd->ctx); 
	    count++;
	
	    tmp->next = entries.next;
	    tmp->link = "";
	    entries.next = tmp;
	    /* if size is 0, count can never be = size, so entire dir is read */
	    if (count == size)
	      break;

	    free (key.data);
	  } else {
	    gf_log (this->name,
		    GF_LOG_ERROR,
		    "failed to do cursor get for directory %s: %s", bfd->ctx->directory, db_strerror (op_ret));
	    op_ret = -1;
	    op_errno = ENOENT;
	    break;
	  }/* if(op_ret == DB_NOTFOUND)...else if...else */
	} /* while(1){ } */
	bdb_close_db_cursor (bfd->ctx, cursorp);
      }
    } else {
      /* do nothing */
    }
    FREE (entry_path);
  }

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &entries, count);

  while (entries.next) {
    tmp = entries.next;
    entries.next = entries.next->next;
    FREE (tmp->name);
    FREE (tmp);
  }
  return 0;
}/* bdb_getdents */


int32_t 
bdb_closedir (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd)
{
  int32_t op_ret = 0;
  int32_t op_errno = 0;
  struct bdb_dir *bfd = NULL;

  frame->root->rsp_refs = NULL;
  
  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL) {
    gf_log (this->name, 
	    GF_LOG_ERROR, 
	    "failed to extract fd data from fd=%p", fd);
    op_ret = -1;
    op_errno = EBADF;
  } else {
    dict_del (fd->ctx, this->name);
	
    if (bfd->path) {
      free (bfd->path);
    } else {
      gf_log (this->name, GF_LOG_ERROR, "bfd->path was NULL. fd=%p bfd=%p",
	      fd, bfd);
    }
    
    if (bfd->dir) {
      closedir (bfd->dir);
    } else {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "bfd->dir is NULL.");
    }
    if (bfd->ctx) {
      bctx_unref (bfd->ctx);
    } else {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "bfd->ctx is NULL");
    }
    free (bfd);
  }

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}/* bdb_closedir */


int32_t 
bdb_readlink (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      size_t size)
{
  char *dest = NULL;
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path = NULL;

  dest = alloca (size + 1);
  ERR_ABORT (dest);


  MAKE_REAL_PATH (real_path, this, loc->path);
  
  op_ret = readlink (real_path, dest, size);
  
  if (op_ret > 0)
    dest[op_ret] = 0;
  op_errno = errno;
  
  if (op_ret == -1) {
    gf_log (this->name,
	    GF_LOG_DEBUG,
	    "readlink failed on %s: %s", loc->path, strerror (op_errno));
  }
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, dest);

  return 0;
}/* bdb_readlink */


int32_t 
bdb_mkdir (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc,
	   mode_t mode)
{
  int32_t op_ret = -1;
  int32_t op_errno = EEXIST;
  char *real_path = NULL;
  struct stat stbuf = {0, };
  bctx_t *bctx = NULL;

  MAKE_REAL_PATH (real_path, this, loc->path);
  
  op_ret = mkdir (real_path, mode);
  op_errno = errno;
  
  if (op_ret == 0) {
    chown (real_path, frame->root->uid, frame->root->gid);
    op_ret = lstat (real_path, &stbuf);
    
    if ((op_ret == 0) && 
	(bctx = bctx_lookup (B_TABLE(this), (char *)loc->path)) != NULL) {
      stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
      bctx_unref (bctx);
    } else {
      gf_log (this->name,
	      GF_LOG_CRITICAL,
	      "bctx_lookup failed: out of memory");
      op_ret = -1;
      op_errno = ENOMEM;
    }
  } else {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to create directory: %s", loc->path);
  }
  
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf);

  return 0;
}/* bdb_mkdir */


int32_t 
bdb_unlink (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  bctx_t *bctx = NULL;
  
  if (((bctx = bctx_parent (B_TABLE(this), loc->path)) == NULL)) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific data", this->name);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    op_ret = bdb_storage_del (bctx, NULL, loc->path);
    if (op_ret == DB_NOTFOUND) {
      char *real_path = NULL;
      MAKE_REAL_PATH (real_path, this, loc->path);
      op_ret = unlink (real_path);
      op_errno = errno;
      
      if (op_ret == -1) {
	gf_log (this->name,
		GF_LOG_DEBUG,
		"unlinking symlink failed for %s", loc->path);
      }
    } else {
      op_errno = 0;
    }
    bctx_unref (bctx);
  }

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}/* bdb_unlink */


int32_t
bdb_rmelem (call_frame_t *frame,
	    xlator_t *this,
	    const char *path)
{
  int32_t op_ret = -1, op_errno = EPERM;

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
} /* bdb_rmelm */


static inline int32_t
is_dir_empty (xlator_t *this,
	      loc_t *loc)
{
  /* TODO: check for subdirectories */
  int32_t ret = 1;
  DBC *cursorp = NULL;
  bctx_t *bctx = NULL;
  DIR *dir = NULL;
  char *real_path = NULL;

  MAKE_REAL_PATH (real_path, this, loc->path);
  if ((dir = opendir (real_path)) != NULL) {
    struct dirent *entry = NULL;
    while ((entry = readdir (dir))) {
      if ((!IS_BDB_PRIVATE_FILE(entry->d_name)) && (!IS_DOT_DOTDOT(entry->d_name))) {
	gf_log (this->name,
		GF_LOG_DEBUG,
		"directory (%s) not empty, has a dirent", loc->path);
	ret = 0;
	break;
      }/* if(!IS_BDB_PRIVATE_FILE()) */
    } /* while(true) */
    closedir (dir);
  } else {
    gf_log (this->name,
	    GF_LOG_DEBUG,
	    "failed to opendir(%s)", loc->path);
    ret = 0;
  } /* if((dir=...))...else */
  
  if (ret) {
    bctx = bctx_lookup (B_TABLE(this), loc->path);
    if (bctx != NULL) {
      if ((ret = bdb_open_db_cursor (bctx, &cursorp)) != -1) {
	DBT key = {0,};
	DBT value = {0,};
	
	ret = bdb_cursor_get (cursorp, &key, &value, DB_NEXT);
	if (ret == DB_NOTFOUND) {
	  /* directory empty */
	  gf_log (this->name,
		  GF_LOG_DEBUG,
		  "no entry found in db for dir %s", loc->path);
	  ret = 1;
	} else {
	  /* we have at least one entry in db */
	  gf_log (this->name,
		  GF_LOG_DEBUG,
		  "directory not empty");
	  ret = 0;
	} /* if(ret == DB_NOTFOUND) */
	bdb_close_db_cursor (bctx, cursorp);
      } else {
	/* failed to open cursorp */
	gf_log (this->name,
		GF_LOG_ERROR,
		"failed to db cursor for directory %s", loc->path);
	ret = 0;
      } /* if((ret=...)...)...else */
      bctx_unref (bctx);
    } else {
      gf_log (this->name,
	      GF_LOG_DEBUG, 
	      "failed to get bctx from inode for dir: %s, assuming empty directory", loc->path);
      ret = 1;
    }
  } /* if(!ret) */
  
  if (!ret) {
    /* directory empty, we need to close the dbp */
    LOCK (&bctx->lock);
    bctx->dbp->close (bctx->dbp, 0);
    bctx->dbp = NULL;
    UNLOCK (&bctx->lock);
  }
  return ret;
}

int32_t
bdb_remove (const char *path,
	    const struct stat *stbuf,
	    int32_t typeflag,
	    struct FTW *ftw)
{
  int32_t ret = -1;

  if (typeflag & FTW_DP)
    ret = rmdir (path);
  else
    ret = unlink (path);
  
  return ret;
}

int32_t
bdb_do_rmdir (xlator_t *this,
	      loc_t *loc)
{
  char *real_path = NULL;
  int32_t ret = -1;
  bctx_t *bctx = NULL;

  MAKE_REAL_PATH (real_path, this, loc->path);

  bctx = bctx_lookup (B_TABLE(this), loc->path);

  if (bctx == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to fetch bctx for path: %s", loc->path);
    ret = -1;
  } else {
    LOCK(&bctx->lock);
    if (bctx->dbp) {
      DB *dbp = NULL;

      bctx->dbp->close (bctx->dbp, 0);
      db_create (&dbp, bctx->table->dbenv, 0);
      ret = dbp->remove (dbp, bctx->db_path, NULL, 0);
      bctx->dbp = NULL;
    }
    UNLOCK(&bctx->lock);
    
    if (ret) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to remove db %s: %s", bctx->db_path, db_strerror (ret));
      ret = -1;
    } else {
      gf_log (this->name,
	      GF_LOG_DEBUG,
	      "removed db %s", bctx->db_path);
      ret = rmdir (real_path);
    }
    /*
    if ((ret = BDB_ENV(this)->dbremove (BDB_ENV(this), 
					NULL, bctx->db_path, NULL, DB_AUTO_COMMIT)) == 0) {
      ret = rmdir (real_path);
    } else if (ret == DB_LOCK_DEADLOCK) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to remove db for directory %s: DB_LOCK_DEADLOCK", loc->path);
      ret = -1;
    } else {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to remove db for directory %s: %s", loc->path, db_strerror (ret));
      ret = -1;
      } */
    bctx_unref (bctx);
  }
  return ret;
}

int32_t 
bdb_rmdir (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc)
{
  int32_t op_ret = -1, op_errno = ENOTEMPTY;

#if 1
  STACK_UNWIND (frame, -1, EPERM);
  return 0;
#endif

#if 0
  if (is_dir_empty (this, loc)) {
    op_ret = bdb_do_rmdir (this, loc);
    if (op_ret < 0) {
      gf_log (this->name,
	      GF_LOG_DEBUG,
	      "failed to remove directory %s", loc->path);
      op_errno = -op_ret;
      op_ret = -1;
    } else {
      /* do nothing */
      op_ret = 0;
      op_errno = 0;
    }    
  } else {
    gf_log (this->name,
	    GF_LOG_DEBUG,
	    "rmdir: directory %s not empty", loc->path);
    op_errno = ENOTEMPTY;
    op_ret = -1;
  }
#endif
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
} /* bdb_rmdir */

int32_t 
bdb_symlink (call_frame_t *frame,
	     xlator_t *this,
	     const char *linkname,
	     loc_t *loc)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path = NULL;
  struct stat stbuf = {0,};
  struct bdb_private *private = this->private;
  
  MAKE_REAL_PATH (real_path, this, loc->path);
  op_ret = symlink (linkname, real_path);
  op_errno = errno;
  
  if (op_ret == 0) {
    bctx_t *bctx = NULL;

    lstat (real_path, &stbuf);
    if ((bctx = bctx_parent (B_TABLE(this), loc->path)) == NULL) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to get bctx for %s", loc->path);
      unlink (real_path);
      op_ret = -1;
      op_errno = ENOENT;
    } else {
      stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
      stbuf.st_mode = private->symlink_mode;
      bctx_unref (bctx);
    }
  }

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf);
  return 0;
} /* bdb_symlink */

int32_t 
bdb_chmod (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc,
	   mode_t mode)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path;
  struct stat stbuf = {0,};

  MAKE_REAL_PATH (real_path, this, loc->path);
  
  op_ret = lstat (real_path, &stbuf);
  
  if (op_ret == 0) {
    /* directory */
    op_ret = chmod (real_path, mode);
    op_errno = errno;
  } else {
    op_ret = -1;
    op_errno = EPERM;
  }/* if(op_ret == 0)...else */
    
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}/* bdb_chmod */


int32_t 
bdb_chown (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc,
	   uid_t uid,
	   gid_t gid)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path;
  struct stat stbuf = {0,};

  MAKE_REAL_PATH (real_path, this, loc->path);
  
  op_ret = lstat (real_path, &stbuf);
  if (op_ret == 0) {
    /* directory */
    op_ret = lchown (real_path, uid, gid);
    op_errno = errno;
  } else {
    /* not a directory */
    op_ret = -1;
    op_errno = EPERM;
  }/* if(op_ret == 0)...else */
    
  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}/* bdb_chown */


int32_t 
bdb_truncate (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path;
  struct stat stbuf = {0,};
  char *db_path = NULL;
  bctx_t *bctx = NULL;
  char *key_string = NULL;

  if ((bctx = bctx_parent (B_TABLE(this), loc->path)) == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to fetch bctx for path: %s", loc->path);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    MAKE_REAL_PATH (real_path, this, loc->path);
    MAKE_KEY_FROM_PATH (key_string, loc->path);
    
    /* now truncate */
    MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
    lstat (db_path, &stbuf);
    if (loc->inode->ino) {
      stbuf.st_ino = loc->inode->ino;
    }else {
      stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
    }
    
    op_ret = bdb_storage_put (bctx, NULL, key_string, NULL, 0, 1, 0);
    if (op_ret == -1) {
      gf_log (this->name,
	      GF_LOG_DEBUG,
	      "failed to do bdb_storage_put");
      op_ret = -1;
      op_errno = ENOENT; /* TODO: better errno */
    } else {
      /* do nothing */
    }/* if(op_ret == -1)...else */
    bctx_unref (bctx);
  }
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);
  
  return 0;
}/* bdb_truncate */


int32_t 
bdb_utimens (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     struct timespec ts[2])
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path = NULL;
  struct stat stbuf = {0,};
  
  MAKE_REAL_PATH (real_path, this, loc->path);
  if ((op_ret = lstat (real_path, &stbuf)) == 0) {
    /* directory */
    struct timeval tv[2] = {{0,},};
    tv[0].tv_sec = ts[0].tv_sec;
    tv[0].tv_usec = ts[0].tv_nsec / 1000;
    tv[1].tv_sec = ts[1].tv_sec;
    tv[1].tv_usec = ts[1].tv_nsec / 1000;
    
    op_ret = lutimes (real_path, tv);
    if (op_ret == -1 && errno == ENOSYS) {
      op_ret = utimes (real_path, tv);
    }
    op_errno = errno;
    if (op_ret == -1) {
      gf_log (this->name, GF_LOG_WARNING, 
	      "utimes on %s: %s", loc->path, strerror (op_errno));
    }
    
    if (op_ret == 0) {
      lstat (real_path, &stbuf);
      stbuf.st_ino = loc->inode->ino;
    }
    
  } else {
    op_ret = -1;
    op_errno = EPERM;
  }
  
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);
  
  return 0;
}/* bdb_utimens */

int32_t 
bdb_statfs (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc)

{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct statvfs buf = {0, };

  MAKE_REAL_PATH (real_path, this, loc->path);

  op_ret = statvfs (real_path, &buf);
  op_errno = errno;

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &buf);
  return 0;
}/* bdb_statfs */

int32_t
bdb_incver (call_frame_t *frame,
	    xlator_t *this,
	    const char *path,
	    fd_t *fd)
{
  /* TODO: version exists for directory, version is consistent for every entry in the directory */
  char *real_path;
  char version[50];
  int32_t size = 0;
  int32_t ver = 0;

  MAKE_REAL_PATH (real_path, this, path);

  size = lgetxattr (real_path, GLUSTERFS_VERSION, version, 50);
  if ((size == -1) && (errno != ENODATA)) {
    STACK_UNWIND (frame, -1, errno);
    return 0;
  } else {
    version[size] = '\0';
    ver = strtoll (version, NULL, 10);
  }
  ver++;
  sprintf (version, "%u", ver);
  lsetxattr (real_path, GLUSTERFS_VERSION, version, strlen (version), 0);
  STACK_UNWIND (frame, ver, 0);

  return 0;
}/* bdb_incver */

int32_t 
bdb_setxattr (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      dict_t *dict,
	      int flags)
{
  int32_t ret = -1;
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;
  data_pair_t *trav = dict->members_list;
  bctx_t *bctx = NULL;
  char *real_path = NULL;
  
  MAKE_REAL_PATH (real_path, this, loc->path);
  if (S_ISDIR (loc->inode->st_mode)) {
    while (trav) {
      if (GF_FILE_CONTENT_REQUEST(trav->key) ) {
	char *key = NULL;
	
	bctx = bctx_lookup (B_TABLE(this), loc->path);
	
	key = &(trav->key[15]);

	if (flags & XATTR_REPLACE) {
	  /* replace only if previously exists, otherwise error out */
	  op_ret = bdb_storage_get (bctx, NULL, key,
				    NULL, 0, 0);
	  if (op_ret == -1) {
	    /* key doesn't exist in database */
	    op_ret = -1;
	    op_errno = ENOENT;
	  } else {
	    op_ret = bdb_storage_put (bctx, NULL, key, 
				      trav->value->data, trav->value->len, 
				      op_ret, BDB_TRUNCATE_RECORD);
	    if (op_ret != 0) {
	      op_ret   = -1;
	      op_errno = ret;
	      break;
	    } else {
	      op_ret = 0;
	      op_errno = 0;
	    } /* if(op_ret!=0)...else */
	  }/* if(op_ret==-1)...else */
	} else {
	  /* fresh create */
	  op_ret = bdb_storage_put (bctx, NULL, key, 
				    trav->value->data, trav->value->len, 
				    0, 0);
	  if (op_ret != 0) {
	    op_ret   = -1;
	    op_errno = ret;
	    break;
	  } else {
	    op_ret = 0;
	    op_errno = 0;
	  } /* if(op_ret!=0)...else */
	} /* if(flags&XATTR_REPLACE)...else */
	if (bctx)
	  bctx_unref (bctx);
      } else {
	/* do plain setxattr */
	op_ret = lsetxattr (real_path, 
			    trav->key, 
			    trav->value->data, 
			    trav->value->len, 
			    flags);
	op_errno = errno;
	if ((op_ret == -1) && (op_errno != ENOENT)) {
	  gf_log (this->name, GF_LOG_WARNING, 
		  "%s: %s", loc->path, strerror (op_errno));
	  break;
	}
      } /* if(GF_FILE_CONTENT_REQUEST())...else */
      trav = trav->next;
    }/* while(trav) */
  } else {
    op_ret   = -1;
    op_errno = EPERM;
  }/* if(S_ISDIR())...else */

  frame->root->rsp_refs = NULL;

  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;  
}/* bdb_setxattr */

int32_t 
bdb_getxattr (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      const char *name)
{
  int32_t op_ret = 0, op_errno = 0;
  dict_t *dict = get_new_dict ();
  bctx_t *bctx = NULL; 

  if (S_ISDIR (loc->inode->st_mode)) {
    if (name && GF_FILE_CONTENT_REQUEST(name)) {
      char *buf = NULL;
      char *key = NULL;

      bctx = bctx_lookup (B_TABLE(this), loc->path);

      key = (char *)&(name[15]);

      op_ret = bdb_storage_get (bctx, NULL, key, &buf, 0, 0);
      if (op_ret == -1) {
	gf_log (this->name,
		GF_LOG_DEBUG,
		"failed to db get on directory: %s for key: %s", bctx->directory, name);
	op_ret   = -1;
	op_errno = ENODATA;
      } else {
	dict_set (dict, (char *)name, data_from_dynptr (buf, op_ret));
      } /* if(op_ret==-1)...else */
      if(bctx)
	bctx_unref (bctx);
    } else {
      int32_t list_offset = 0;
      size_t size = 0;
      size_t remaining_size = 0;
      char *real_path = NULL;
      char key[1024] = {0,};
      char *value = NULL;
      char *list = NULL;
      
      MAKE_REAL_PATH (real_path, this, loc->path);
      size = llistxattr (real_path, NULL, 0);
      op_errno = errno;
      if (size <= 0) {
	/* There are no extended attributes, send an empty dictionary */
	if (dict) {
	  dict_ref (dict);
	}
	if (size == -1 && op_errno != ENODATA) {
	  gf_log (this->name, GF_LOG_WARNING, 
		  "%s: %s", loc->path, strerror (op_errno));
	} 
	op_ret = -1;
	op_errno = ENODATA;
      } else {
	list = alloca (size + 1);
	ERR_ABORT (list);
	size = llistxattr (real_path, list, size);
	
	remaining_size = size;
	list_offset = 0;
	while (remaining_size > 0) {
	  if(*(list+list_offset) == '\0')
	    break;
	  strcpy (key, list + list_offset);
	  op_ret = lgetxattr (real_path, key, NULL, 0);
	  if (op_ret == -1)
	    break;
	  value = calloc (op_ret + 1, sizeof(char));
	  ERR_ABORT (value);
	  op_ret = lgetxattr (real_path, key, value, op_ret);
	  if (op_ret == -1)
	    break;
	  value [op_ret] = '\0';
	  dict_set (dict, key, data_from_dynptr (value, op_ret));
	  remaining_size -= strlen (key) + 1;
	  list_offset += strlen (key) + 1;
	} /* while(remaining_size>0) */
      } /* if(size <= 0)...else */
    } /* if(name...)...else */
  } else {
    gf_log (this->name,
	    GF_LOG_DEBUG,
	    "operation not permitted on a non-directory file: %s", loc->path);
    op_ret   = -1;
    op_errno = ENODATA;
  } /* if(S_ISDIR(...))...else */

  if (dict)
    dict_ref (dict);

  STACK_UNWIND (frame, op_ret, op_errno, dict);

  if (dict)
    dict_unref (dict);
  
  return 0;
}/* bdb_getxattr */


int32_t 
bdb_removexattr (call_frame_t *frame,
		 xlator_t *this,
		 loc_t *loc,
                 const char *name)
{
  int32_t op_ret = -1, op_errno = EPERM;
  bctx_t *bctx = NULL;

  if (S_ISDIR(loc->inode->st_mode)) {
    if (GF_FILE_CONTENT_REQUEST(name)) {
      bctx = bctx_lookup (B_TABLE(this), loc->path);

      op_ret = bdb_storage_del (bctx, NULL, name);
      
      if (op_ret == -1) {
	op_errno = ENOENT;
      } else {
	op_ret = 0;
	op_errno = 0;
      } /* if(op_ret == -1)...else */
      if (bctx)
	bctx_unref (bctx);
    } else {
      char *real_path = NULL;
      MAKE_REAL_PATH(real_path, this, loc->path);
      op_ret = lremovexattr (real_path, name);
      op_errno = errno;
      if (op_ret == -1) {
	gf_log (this->name, GF_LOG_WARNING, 
		"%s: %s", loc->path, strerror (op_errno));
      } /* if(op_ret == -1) */
    } /* if (GF_FILE_CONTENT_REQUEST(name))...else */
  } else {
    gf_log (this->name,
	    GF_LOG_WARNING,
	    "operation not permitted on non-directory files");
    op_ret = -1;
    op_errno = EPERM;
  } /* if(S_ISDIR(...))...else */
  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}/* bdb_removexattr */


int32_t 
bdb_fsyncdir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		int datasync)
{
  int32_t op_ret;
  int32_t op_errno;
  struct bdb_fd *bfd = NULL;

  frame->root->rsp_refs = NULL;

  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL) {
    gf_log (this->name, GF_LOG_ERROR,
	    "bfd is NULL fd=%p", fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {

    op_ret = 0;
    op_errno = errno;
  }

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}/* bdb_fsycndir */


int32_t 
bdb_access (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t mask)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;

  MAKE_REAL_PATH (real_path, this, loc->path);

  op_ret = access (real_path, mask);
  op_errno = errno;

  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}/* bdb_access */


int32_t 
bdb_ftruncate (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd,
		 off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct stat buf = {0,};

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}

int32_t 
bdb_fchown (call_frame_t *frame,
	    xlator_t *this,
	    fd_t *fd,
	    uid_t uid,
	    gid_t gid)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct stat buf = {0,};

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}


int32_t 
bdb_fchmod (call_frame_t *frame,
	    xlator_t *this,
	    fd_t *fd,
	    mode_t mode)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct stat buf = {0,};

  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}

int32_t 
bdb_setdents (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      int32_t flags,
	      dir_entry_t *entries,
	      int32_t count)
{
  int32_t op_ret = 0, op_errno = 0;
  char *entry_path = NULL;
  int32_t real_path_len = 0;
  int32_t entry_path_len = 0;
  int32_t ret = 0;
  struct bdb_dir *bfd = NULL;

  frame->root->rsp_refs = NULL;

  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL) {
    gf_log (this->name, GF_LOG_ERROR, "bfd is NULL on fd=%p", fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    real_path_len = strlen (bfd->path);
    entry_path_len = real_path_len + 256;
    entry_path = calloc (1, entry_path_len);
    
    if (!entry_path) {
      op_ret = -1;
      op_errno = ENOMEM;
    } else {      
      strcpy (entry_path, bfd->path);
      entry_path[real_path_len] = '/';
      
      dir_entry_t *trav = entries->next;
      while (trav) {
	char pathname[4096] = {0,};
	strcpy (pathname, entry_path);
	strcat (pathname, trav->name);
	
	if (S_ISDIR(trav->buf.st_mode)) {
	  /* If the entry is directory, create it by calling 'mkdir'. If 
	   * directory is not present, it will be created, if its present, 
	   * no worries even if it fails.
	   */
	  ret = mkdir (pathname, trav->buf.st_mode);
	  if (!ret || (errno == EEXIST)) {
	    gf_log (this->name, 
		    GF_LOG_DEBUG, 
		    "Creating directory %s with mode (0%o)", 
		    pathname,
		    trav->buf.st_mode);
	    /* Change the mode */
	    chmod (pathname, trav->buf.st_mode);
	    /* change the ownership */
	    chown (pathname, trav->buf.st_uid, trav->buf.st_gid);
	  } else {
	    gf_log (this->name,
		    GF_LOG_DEBUG,
		    "failed to created directory %s: %s", pathname, strerror(errno));
	  }
	} else if (flags == GF_SET_IF_NOT_PRESENT || flags != GF_SET_DIR_ONLY) {
	  /* Create a 0 byte file here */
	  if (S_ISREG (trav->buf.st_mode)) {
	    op_ret = bdb_storage_put (bfd->ctx, NULL, trav->name, NULL, 0, 0, 0);
	    if (!op_ret) {
	      /* create successful */
	      gf_log (this->name,
		      GF_LOG_DEBUG,
		      "creating file %s",
		      pathname);
	    } /* if (!op_ret)...else */
	  } else {
	    gf_log (this->name,
		    GF_LOG_ERROR,
		    "storage/bdb allows to create regular files only");
	  } /* if(S_ISREG())...else */
	} /* if(S_ISDIR())...else if */
	
	  /* consider the next entry */
	trav = trav->next;
      } /* while(trav) */
    } /* if(!entry)...else */
  } /* if((bfd = ...))...else */

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);
  
  FREE (entry_path);
  return 0;
}

int32_t 
bdb_fstat (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd)
{
  int32_t op_ret = 0;
  int32_t op_errno = ENOENT;
  struct stat stbuf = {0,};
  struct bdb_fd *bfd = NULL;

  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL){
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific information from fd:%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    bctx_t *bctx = bfd->ctx;
    char *db_path = NULL;

    MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
    lstat (db_path, &stbuf);

    stbuf.st_ino = fd->inode->ino;
    stbuf.st_size = bdb_storage_get (bctx, NULL, bfd->key, NULL, 0, 0);
    stbuf.st_blocks = BDB_COUNT_BLOCKS (stbuf.st_size, stbuf.st_blksize);
  }

  frame->root->rsp_refs = NULL;

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);
  return 0;
}


int32_t
bdb_readdir (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd,
	     size_t size,
	     off_t off)
{
  struct bdb_dir *bfd = NULL;
  int32_t this_size = 0;
  int32_t op_ret = -1, op_errno = 0;
  char *buf = NULL;
  size_t filled = 0;
  struct stat stbuf = {0,};


  if ((bfd = bdb_extract_bfd (fd, this->name)) == NULL) {
    gf_log (this->name, GF_LOG_ERROR,
	    "failed to extract %s specific fd information from fd=%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    buf = calloc (size, 1); /* readdir buffer needs 0 padding */
    
    if (!buf) {
      gf_log (this->name, GF_LOG_ERROR,
	      "calloc (%d) returned NULL", size);
      op_ret = -1;
      op_errno = ENOMEM;
    } else {

      while (filled <= size) {
	gf_dirent_t *this_entry = NULL;
	struct dirent *entry = NULL;
	off_t in_case = 0;
	int32_t this_size = 0;
	
	in_case = telldir (bfd->dir);
	entry = readdir (bfd->dir);
	if (!entry)
	  break;
	
	this_size = dirent_size (entry);
	
	if (this_size + filled > size) {
	  seekdir (bfd->dir, in_case);
	  break;
	}
	
	if (!IS_BDB_PRIVATE_FILE(entry->d_name)) {
	  /* TODO - consider endianness here */
	  this_entry = (void *)(buf + filled);
	  this_entry->d_ino = entry->d_ino;
	  
	  this_entry->d_off = entry->d_off;
	  
	  this_entry->d_type = entry->d_type;
	  this_entry->d_len = entry->d_reclen;
	  strncpy (this_entry->d_name, entry->d_name, this_entry->d_len);
	  
	  filled += this_size;
	}/* if(!IS_BDB_PRIVATE_FILE()) */
      }
      
      lstat (bfd->path, &stbuf);
      
      if (filled < size) {
	/* hungry kyaa? */
	DBC *cursorp = NULL;
	op_ret = bdb_open_db_cursor (bfd->ctx, &cursorp);
	if (op_ret != 0) {
	  gf_log (this->name,
		  GF_LOG_ERROR,
		  "failed to open db cursor for %s", bfd->path);
	  op_ret = -1;
	  op_errno = EBADF;
	} else {
	  if (strlen (bfd->offset)) {
	    DBT key = {0,}, value = {0,};
	    key.data = bfd->offset;
	    key.size = strlen (bfd->offset);
	    key.flags = DB_DBT_USERMEM;
	    value.dlen = 0;
	    value.doff = 0;
	    value.flags = DB_DBT_PARTIAL;
	    op_ret = bdb_cursor_get (cursorp, &key, &value, DB_SET);
	  } else {
	    /* first time or last time, do nothing */
	  }
	  while (filled <= size) {
	    gf_dirent_t *this_entry = NULL;
	    DBT key = {0,}, value = {0,};
	    
	    key.flags = DB_DBT_MALLOC;
	    value.dlen = 0;
	    value.doff = 0; 
	    value.flags = DB_DBT_PARTIAL;
	    op_ret = bdb_cursor_get (cursorp, &key, &value, DB_NEXT);
	    
	    if (op_ret == DB_NOTFOUND) {
	      /* we reached end of the directory */
	      break;
	    } else if (op_ret == 0){
	      
	      if (key.data) {
		this_size = bdb_dirent_size (&key);
		if (this_size + filled > size)
		  break;
		/* TODO - consider endianness here */
		this_entry = (void *)(buf + filled);
		/* FIXME: bug, if someone is going to use ->d_ino */
		this_entry->d_ino = -1;
		this_entry->d_off = 0;
		this_entry->d_type = 0;
		this_entry->d_len = key.size;
		strncpy (this_entry->d_name, (char *)key.data, key.size);
		
		if (key.data) {
		  strncpy (bfd->offset, key.data, key.size);
		  bfd->offset [key.size] = '\0';
		  free (key.data);
		}

		filled += this_size;
	      } else {
		/* NOTE: currently ignore when we get key.data == NULL, TODO: we should not get key.data = NULL */
		gf_log (this->name,
			GF_LOG_DEBUG,
			"null key read from db");
	      }/* if(key.data)...else */
	    } else {
	      gf_log (this->name,
		      GF_LOG_DEBUG,
		      "database error during readdir");
	      op_ret = -1;
	      op_errno = ENOENT;
	      break;
	    } /* if (op_ret == DB_NOTFOUND)...else if...else */
	  }/* while */
	  bdb_close_db_cursor (bfd->ctx, cursorp);
	} 
      }
    }
  }
  frame->root->rsp_refs = NULL;
  gf_log (this->name,
	  GF_LOG_DEBUG,
	  "read %d bytes", filled);
  STACK_UNWIND (frame, filled, op_errno, buf);

  free (buf);
    
  return 0;
}


int32_t 
bdb_stats (call_frame_t *frame,
	   xlator_t *this,
	   int32_t flags)

{
  int32_t op_ret = 0;
  int32_t op_errno = 0;

  struct xlator_stats xlstats = {0, }, *stats = &xlstats;
  struct statvfs buf;
  struct timeval tv;
  struct bdb_private *priv = (struct bdb_private *)this->private;
  int64_t avg_read = 0;
  int64_t avg_write = 0;
  int64_t _time_ms = 0; 

    
  op_ret = statvfs (priv->export_path, &buf);
  op_errno = errno;
    
  
  stats->nr_files = priv->stats.nr_files;
  stats->nr_clients = priv->stats.nr_clients; /* client info is maintained at FSd */
  stats->free_disk = buf.f_bfree * buf.f_bsize; /* Number of Free block in the filesystem. */
  stats->total_disk_size = buf.f_blocks * buf.f_bsize; /* */
  stats->disk_usage = (buf.f_blocks - buf.f_bavail) * buf.f_bsize;

  /* Calculate read and write usage */
  gettimeofday (&tv, NULL);
  
  /* Read */
  _time_ms = (tv.tv_sec - priv->init_time.tv_sec) * 1000 +
             ((tv.tv_usec - priv->init_time.tv_usec) / 1000);

  avg_read = (_time_ms) ? (priv->read_value / _time_ms) : 0; /* KBps */
  avg_write = (_time_ms) ? (priv->write_value / _time_ms) : 0; /* KBps */
  
  _time_ms = (tv.tv_sec - priv->prev_fetch_time.tv_sec) * 1000 +
             ((tv.tv_usec - priv->prev_fetch_time.tv_usec) / 1000);
  if (_time_ms && ((priv->interval_read / _time_ms) > priv->max_read)) {
    priv->max_read = (priv->interval_read / _time_ms);
  }
  if (_time_ms && ((priv->interval_write / _time_ms) > priv->max_write)) {
    priv->max_write = priv->interval_write / _time_ms;
  }

  stats->read_usage = avg_read / priv->max_read;
  stats->write_usage = avg_write / priv->max_write;

  gettimeofday (&(priv->prev_fetch_time), NULL);
  priv->interval_read = 0;
  priv->interval_write = 0;

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, stats);
  return 0;
}

int32_t 
bdb_checksum (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t flag)
{
  char *real_path = NULL;
  DIR *dir = NULL;
  struct dirent *dirent = NULL;
  uint8_t file_checksum[4096] = {0,};
  uint8_t dir_checksum[4096] = {0,};
  int32_t op_ret = -1;
  int32_t op_errno = 2;
  int32_t i = 0, length = 0;
  bctx_t *bctx = NULL;
  DBC *cursorp = NULL;

  MAKE_REAL_PATH (real_path, this, loc->path);

  {
    dir = opendir (real_path);
    if (!dir){
      gf_log (this->name, GF_LOG_DEBUG, 
	      "checksum: opendir() failed for `%s'", real_path);
      frame->root->rsp_refs = NULL;
      op_ret = -1;
      op_errno = ENOENT;
      return 0;
    } else {
      while ((dirent = readdir (dir))) {
	if (!dirent)
	  break;
	
	length = strlen (dirent->d_name);
	if (!IS_BDB_PRIVATE_FILE(dirent->d_name)) {
	  for (i = 0; i < length; i++)
	    dir_checksum[i] ^= dirent->d_name[i];
	} else {
	  /* do nothing */
	} /* if(!IS_BDB_PRIVATE_FILE(...))...else */
      } /* while((dirent...)) */
      closedir (dir);
    } /* if(!dir)...else */
  }
  {
    if ((bctx = bctx_lookup (B_TABLE(this), (char *)loc->path)) == NULL) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to extract %s specific data from private data", this->name);
      op_ret = -1;
      op_errno = ENOENT;
    } else {
      op_ret = bdb_open_db_cursor (bctx, &cursorp);
      if (op_ret == -1) {
	gf_log (this->name,
		GF_LOG_ERROR,
		"failed to open cursor for db %s", bctx->directory);
	op_ret = -1;
	op_errno = EBADFD;
      } else {
	while (1) {
	  DBT key = {0,}, value = {0,};
	  
	  key.flags = DB_DBT_MALLOC;
	  value.doff = 0;
	  value.dlen = 0;
	  op_ret = bdb_cursor_get (cursorp, &key, &value, DB_NEXT);
	  
	  if (op_ret == DB_NOTFOUND) {
	    gf_log (this->name,
		    GF_LOG_DEBUG,
		    "end of list of key/value pair in db for directory: %s", bctx->directory);
	    op_ret = 0;
	    op_errno = 0;
	    break;
	  } else if (op_ret == 0){
	    /* successfully read */
	    char *data = key.data;
	    
	    length = key.size;
	    for (i = 0; i < length; i++)
	      file_checksum[i] ^= data[i];
	    
	    free (key.data);
	  } else {
	    gf_log (this->name,
		    GF_LOG_ERROR,
		    "failed to do cursor get for directory %s: %s", bctx->directory, db_strerror (op_ret));
	    op_ret = -1;
	    op_errno = ENOENT;
	    break;
	  }/* if(op_ret == DB_NOTFOUND)...else if...else */
	} /* while(1) */
	bdb_close_db_cursor (bctx, cursorp);
      } /* if(op_ret==-1)...else */
      bctx_unref (bctx);
    } /* if(bctx=...)...else */
  }

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, file_checksum, dir_checksum);

  return 0;
}

/**
 * notify - when parent sends PARENT_UP, send CHILD_UP event from here
 */
int32_t
notify (xlator_t *this,
        int32_t event,
        void *data,
        ...)
{
  switch (event)
    {
    case GF_EVENT_PARENT_UP:
      {
	/* Tell the parent that bdb xlator is up */
	assert ((this->private != NULL) && 
		(BDB_ENV(this) != NULL));
	default_notify (this, GF_EVENT_CHILD_UP, data);
      }
      break;
    default:
      /* */
      break;
    }
  return 0;
}



/**
 * init - 
 */
int32_t 
init (xlator_t *this)
{
  int32_t ret;
  struct stat buf;
  struct bdb_private *_private = NULL;
  data_t *directory = dict_get (this->options, "directory");

  _private = calloc (1, sizeof (*_private));
  ERR_ABORT (_private);

  if (this->children) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "FATAL: storage/bdb cannot have subvolumes");
    FREE (_private);
    return -1;
  }

  if (!directory) {
    gf_log (this->name, GF_LOG_ERROR,
	    "export directory not specified in spec file");
    FREE (_private);
    return -1;
  }
  umask (000); // umask `masking' is done at the client side
  if (mkdir (directory->data, 0777) == 0) {
    gf_log (this->name, GF_LOG_WARNING,
	    "directory specified not exists, created");
  }
  
  /* Check whether the specified directory exists, if not create it. */
  ret = stat (directory->data, &buf);
  if ((ret != 0) || !S_ISDIR (buf.st_mode)) {
    gf_log (this->name, GF_LOG_ERROR, 
	    "Specified directory doesn't exists, Exiting");
    FREE (_private);
    return -1;
  }


  _private->export_path = strdup (directory->data);
  _private->export_path_length = strlen (_private->export_path);

  {
    /* Stats related variables */
    gettimeofday (&_private->init_time, NULL);
    gettimeofday (&_private->prev_fetch_time, NULL);
    _private->max_read = 1;
    _private->max_write = 1;
  }

  this->private = (void *)_private;
  {
    ret = bdb_init_db (this, this->options);
    
    if (ret == -1){
      gf_log (this->name,
	      GF_LOG_DEBUG,
	      "failed to initialize database");
      return -1;
    } else {
      bctx_t *bctx = bctx_lookup (_private->b_table, "/");
      /* NOTE: we are not doing bctx_unref() for root bctx, 
       *      let it remain in active list forever */
      if (!bctx) {
	gf_log (this->name,
		GF_LOG_ERROR,
		"failed to allocate memory for root (/) bctx: out of memory");
	return -1;
      }
    }
  }
  return 0;
}

void 
bctx_cleanup (struct list_head *head)
{
  bctx_t *trav = NULL, *tmp = NULL;
  DB *storage = NULL;

  list_for_each_entry_safe (trav, tmp, head, list) {
    LOCK (&trav->lock);
    storage = trav->dbp;
    trav->dbp = NULL;
    list_del_init (&trav->list);
    UNLOCK (&trav->lock);
    
    if (storage) {
      storage->close (storage, 0);
      storage = NULL;
    }
  }
  
  return;
}

void
fini (xlator_t *this)
{
  struct bdb_private *private = this->private;
  if (B_TABLE(this)) {
    int32_t idx = 0;
    /* close all the dbs from lru list */
    bctx_cleanup (&(B_TABLE(this)->b_lru));
    for (idx = 0; idx < B_TABLE(this)->hash_size; idx++)
      bctx_cleanup (&(B_TABLE(this)->b_hash[idx]));
    
    if (BDB_ENV(this)) {
      /* TODO: pick each of the 'struct bctx' from private->b_hash
       * and close all the databases that are open */
      BDB_ENV(this)->close (BDB_ENV(this), 0);
    } else {
      /* impossible to reach here */
    }

    FREE (B_TABLE(this));
  }
  FREE (private);
  return;
}

struct xlator_mops mops = {
  .stats    = bdb_stats,
  .lock     = mop_lock_impl,
  .unlock   = mop_unlock_impl,
  .checksum = bdb_checksum,
};

struct xlator_fops fops = {
  .lookup      = bdb_lookup,
  .forget      = bdb_forget,
  .stat        = bdb_stat,
  .opendir     = bdb_opendir,
  .readdir     = bdb_readdir,
  .closedir    = bdb_closedir,
  .readlink    = bdb_readlink,
  .mknod       = bdb_mknod,
  .mkdir       = bdb_mkdir,
  .unlink      = bdb_unlink,
  .rmelem      = bdb_rmelem,
  .rmdir       = bdb_rmdir,
  .symlink     = bdb_symlink,
  .rename      = bdb_rename,
  .link        = bdb_link,
  .chmod       = bdb_chmod,
  .chown       = bdb_chown,
  .truncate    = bdb_truncate,
  .utimens     = bdb_utimens,
  .create      = bdb_create,
  .open        = bdb_open,
  .readv       = bdb_readv,
  .writev      = bdb_writev,
  .statfs      = bdb_statfs,
  .flush       = bdb_flush,
  .close       = bdb_close,
  .fsync       = bdb_fsync,
  .incver      = bdb_incver,
  .setxattr    = bdb_setxattr,
  .getxattr    = bdb_getxattr,
  .removexattr = bdb_removexattr,
  .fsyncdir    = bdb_fsyncdir,
  .access      = bdb_access,
  .ftruncate   = bdb_ftruncate,
  .fstat       = bdb_fstat,
  .lk          = bdb_lk,
  .fchown      = bdb_fchown,
  .fchmod      = bdb_fchmod,
  .setdents    = bdb_setdents,
  .getdents    = bdb_getdents,
};
