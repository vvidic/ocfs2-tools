/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * expanddir.c
 *
 * Expands an OCFS2 directory.  For the OCFS2 userspace library.
 *
 * Copyright (C) 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License, version 2,  as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 *  This code is a port of e2fsprogs/lib/ext2fs/expanddir.c
 *  Copyright (C) 1993, 1999 Theodore Ts'o.
 */

#define _XOPEN_SOURCE 600 /* Triggers magic in features.h */
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "ocfs2.h"

/*
 * ocfs2_expand_dir()
 *
 */
errcode_t ocfs2_expand_dir(ocfs2_filesys *fs,
			   uint64_t dir,
			   uint64_t parent_dir)
{
	errcode_t ret = 0;
	ocfs2_cached_inode *cinode = NULL;
	uint64_t used_blks;
	uint64_t totl_blks;
	uint64_t new_blk;
	int contig;
	char *buf = NULL;

	if (!(fs->fs_flags & OCFS2_FLAG_RW))
		return OCFS2_ET_RO_FILESYS;

	/* ensure it is a dir */
	ret = ocfs2_check_directory(fs, dir);
	if (ret)
		goto bail;

	/* read inode */
	ret = ocfs2_read_cached_inode(fs, dir, &cinode);
	if (ret)
		goto bail;

	/* This relies on the fact that i_size of a directory is a
	 * multiple of blocksize */
	used_blks = cinode->ci_inode->i_size >>
	       			OCFS2_RAW_SB(fs->fs_super)->s_blocksize_bits;
	totl_blks = ocfs2_clusters_to_blocks(fs, cinode->ci_inode->i_clusters);

	if (used_blks >= totl_blks) {
		ocfs2_free_cached_inode(fs, cinode);

		/* extend the directory */
		ret = ocfs2_extend_allocation(fs, dir, 1);
		if (ret)
			goto bail;

		ret = ocfs2_read_cached_inode(fs, dir, &cinode);
		if (ret)
			goto bail;
	}

	ret = ocfs2_extent_map_init(fs, cinode);
	if (ret)
		goto bail;

	/* get the next free block */
	ret = ocfs2_extent_map_get_blocks(cinode, used_blks, 1,
					  &new_blk, &contig);
	if (ret) 
		goto bail;

	/* init new dir block, with dotty entries if it's first */
	if (used_blks == 0)
		ret = ocfs2_new_dir_block(fs, dir, parent_dir, &buf);
	else
		ret = ocfs2_new_dir_block(fs, 0, 0, &buf);
	if (ret)
		goto bail;

	/* write new dir block */
	ret = ocfs2_write_dir_block(fs, new_blk, buf);
	if (ret)
		goto bail;

	/* did we just add a '..' reference to a parent?  if so, update
	 * them */
	if (used_blks == 0) {
		ocfs2_dinode *parent = (ocfs2_dinode *)buf;
		ret = ocfs2_read_inode(fs, parent_dir, buf);
		if (ret)
			goto bail;
		parent->i_links_count++;
		ret = ocfs2_write_inode(fs, parent_dir, buf);
		if (ret)
			goto bail;
	}

	/* increase the size */
	cinode->ci_inode->i_size += fs->fs_blocksize;

	/* update the size of the inode */
	ret = ocfs2_write_cached_inode(fs, cinode);
	if (ret)
		goto bail;

bail:
	if (buf)
		ocfs2_free(&buf);

	if (cinode)
		ocfs2_free_cached_inode(fs, cinode);

	return ret;
}