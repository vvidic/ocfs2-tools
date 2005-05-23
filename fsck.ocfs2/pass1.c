/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * Copyright (C) 1993-2004 by Theodore Ts'o.
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
 * --
 *
 * Pass 1 is arguably where the greatest concentration of special rules
 * in fsck live.  It walks through all the inodes that it can get its hands
 * on and verifies them.  For now it only walks the inode allocator groups
 * that pass 0 was able to verify.  One can imagine it getting other potential
 * inodes from other places.
 *
 * The complexity comes in deciding that inodes are valid.  There are different
 * critera for system inodes, allocator inodes, and the usual different 
 * unix inode file types.
 *
 * Pass 1 build up in-memory copies of the inode allocators that are written
 * back as the real inode allocators if inconsistencies are found between
 * the bitmaps and the inodes.  It also builds up many inode-dependent data
 * structures that are used by future passes:
 *  - icount map of inodes to what their current on-disk i_link_count is
 *  - bitmap of which inodes are directories or regular files
 *  - directory blocks that it finds off of directory inodes
 *
 * The end of Pass 1 is when the found block bitmap should contain all the
 * blocks in the system that are in use.  This is used to derive the set of
 * clusters that should be allocated.  The cluster chain allocator is loaded
 * and synced up with this set and potentially written back.  After that point
 * fsck can use libocfs2 to allocate and free clusters as usual.
 *
 * XXX
 * 	check many, many, more i_ fields for each inode type
 * 	make sure the inode's dtime/count/valid match in update_inode_alloc
 * 	more carefully track cluster use in conjunction with pass 0
 * 	free an inodes chains and extents and such if we free it
 */
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "ocfs2.h"
#include "bitops.h"

#include "dirblocks.h"
#include "dirparents.h"
#include "extent.h"
#include "icount.h"
#include "fsck.h"
#include "pass1.h"
#include "problem.h"
#include "util.h"

static const char *whoami = "pass1";

void o2fsck_free_inode_allocs(o2fsck_state *ost)
{
	uint16_t i;

	ocfs2_free_cached_inode(ost->ost_fs, ost->ost_global_inode_alloc);

	for (i = 0; i < OCFS2_RAW_SB(ost->ost_fs->fs_super)->s_max_slots; i++)
		ocfs2_free_cached_inode(ost->ost_fs, ost->ost_inode_allocs[i]);
}

/* update our in memory images of the inode chain alloc bitmaps.  these
 * will be written out at the end of pass1 and the library will read
 * them off disk for use from then on. */
static void update_inode_alloc(o2fsck_state *ost, ocfs2_dinode *di, 
			       uint64_t blkno, int val)
{
	int16_t slot;
	uint16_t max_slots, yn;
	errcode_t ret = OCFS2_ET_INTERNAL_FAILURE;
	ocfs2_cached_inode *cinode;
	int oldval;

	val = !!val;

	if (ost->ost_write_inode_alloc_asked && !ost->ost_write_inode_alloc)
		return;

	max_slots = OCFS2_RAW_SB(ost->ost_fs->fs_super)->s_max_slots;

	for (slot = OCFS2_INVALID_SLOT; slot != max_slots; slot++, 
					   ret = OCFS2_ET_INTERNAL_FAILURE) {

		if (slot == OCFS2_INVALID_SLOT)
			cinode = ost->ost_global_inode_alloc;
		else
			cinode = ost->ost_inode_allocs[slot];

		/* we might have had trouble reading the chains in pass 0 */
		if (cinode == NULL)
			continue;

		ret = ocfs2_chain_force_val(ost->ost_fs, cinode, blkno, val,
					    &oldval);
		if (ret) {
		       	if (ret != OCFS2_ET_INVALID_BIT)
				com_err(whoami, ret, "while trying to set "
					"inode %"PRIu64"'s allocation to '%d' "
					"in slot %"PRId16"'s chain", blkno,
					val, slot);
			continue;
		}

		/* hmm, de hmm.  it would be kind of nice if the bitmaps
		 * didn't use 'int' but rather some real boolean construct */
		oldval = !!oldval;

		/* this slot covers the inode.  see if we've changed the 
		 * bitmap and if the user wants us to keep tracking it and
		 * write back the new map */
		if (oldval != val && !ost->ost_write_inode_alloc_asked) {
			yn = prompt(ost, PY, PR_INODE_ALLOC_REPAIR,
				    "fsck found an inode whose "
				    "allocation does not match the chain "
				    "allocators.  Fix the allocation of this "
				    "and all future inodes?");
			ost->ost_write_inode_alloc_asked = 1;
			ost->ost_write_inode_alloc = !!yn;
			if (!ost->ost_write_inode_alloc)
				o2fsck_free_inode_allocs(ost);
		}
		break;
	}

	if (ret) {
		com_err(whoami, ret, "while trying to set inode %"PRIu64"'s "
			"allocation to '%d'.  None of the slots chain "
			"allocator's had a group covering the inode.",
			blkno, val);
		goto out;
	}

	verbosef("updated inode %"PRIu64" alloc to %d in slot %"PRId16"\n",
		 blkno, val, slot);

	/* make sure the inode's fields are consistent if it's allocated */
	if (val == 1 && slot != di->i_suballoc_slot &&
	    prompt(ost, PY, PR_INODE_SUBALLOC,
		   "Inode %"PRIu64" indicates that it was allocated "
		   "from slot %"PRId16" but slot %"PRId16"'s chain allocator "
		   "covers the inode.  Fix the inode's record of where it is "
		   "allocated?",
		   blkno, di->i_suballoc_slot, slot)) {
		di->i_suballoc_slot = slot;
		o2fsck_write_inode(ost, di->i_blkno, di);
	}
out:
	return;
}

static errcode_t verify_local_alloc(o2fsck_state *ost, ocfs2_dinode *di)
{
	ocfs2_local_alloc *la = &di->id2.i_lab;
	uint32_t max;
	int broken = 0, changed = 0, clear = 0;
	errcode_t ret = 0;

	verbosef("la_bm_off %u size %u total %u used %u\n", la->la_bm_off,
		 la->la_size, di->id1.bitmap1.i_total,
		 di->id1.bitmap1.i_used);

	max = ocfs2_local_alloc_size(ost->ost_fs->fs_blocksize);

	if (la->la_size > max) {
		broken = 1;
		if (prompt(ost, PY, PR_LALLOC_SIZE,
			   "Local alloc inode %"PRIu64" claims to "
			   "have %u bytes of bitmap data but %u bytes is the "
			   "maximum allowed.  Set the inode's count to the "
			   "maximum?", di->i_blkno, la->la_size, max)) {

			la->la_size = max;
			changed = 1;
		}
	}

	if (di->id1.bitmap1.i_total == 0) {
		/* ok, it's not used.  we don't mark these errors as 
		 * 'broken' as the kernel shouldn't care.. right? */
		if (di->id1.bitmap1.i_used != 0) {
			if (prompt(ost, PY, PR_LALLOC_NZ_USED,
				   "Local alloc inode %"PRIu64" "
			    "isn't in use bit its i_used isn't 0.  Set it to "
			    "0?", di->i_blkno)) {

				di->id1.bitmap1.i_used = 0;
				changed = 1;
			}
		}

		if (la->la_bm_off != 0) {
			if (prompt(ost, PY, PR_LALLOC_NZ_BM,
				   "Local alloc inode %"PRIu64" "
			    "isn't in use bit its i_bm_off isn't 0.  Set it "
			    "to 0?", di->i_blkno)) {

				la->la_bm_off = 0;
				changed = 1;
			}
		}

		goto out;
	}

	if (la->la_bm_off >= ost->ost_fs->fs_clusters) {
		broken = 1;
		if (prompt(ost, PY, PR_LALLOC_BM_OVERRUN,
			   "Local alloc inode %"PRIu64" claims to "
			   "contain a bitmap that starts at cluster %u but "
			   "the volume contains %u clusters.  Mark the local "
			   "alloc bitmap as unused?", di->i_blkno,
			   la->la_bm_off, ost->ost_fs->fs_clusters)) {
			clear = 1;
		}
	}

	if (di->id1.bitmap1.i_total > la->la_size * 8) {
		broken = 1;
		if (prompt(ost, PY, PR_LALLOC_BM_SIZE,
			   "Local alloc inode %"PRIu64" claims to "
			   "have a bitmap with %u bits but the inode can only "
			   "fit %u bits.  Clamp the bitmap size to this "
			   "maxmum?", di->i_blkno, di->id1.bitmap1.i_total,
			   la->la_size * 8)) {

			di->id1.bitmap1.i_total = la->la_size * 8;
			changed = 1;
		}
	}

	if (la->la_bm_off + di->id1.bitmap1.i_total >
	    ost->ost_fs->fs_clusters) {
		broken = 1;
		if (prompt(ost, PY, PR_LALLOC_BM_STRADDLE,
			   "Local alloc inode %"PRIu64" claims to "
			   "have a bitmap that covers clusters numbered %u "
			   "through %u but %u is the last valid cluster. "
			   "Mark the local bitmap as unused?",
			   di->i_blkno,
			   la->la_bm_off,
			   la->la_bm_off + di->id1.bitmap1.i_total - 1, 
			   ost->ost_fs->fs_clusters - 1)) {

			clear = 1;
		}
		/* we can't possibly check _used if bm/off and total are
		 * so busted */
		goto out;
	}

	if (di->id1.bitmap1.i_used > di->id1.bitmap1.i_total) {
		broken = 1;
		if (prompt(ost, PY, PR_LALLOC_USED_OVERRUN,
			   "Local alloc inode %"PRIu64" claims to "
			   "contain a bitmap with %u bits and %u used.  Set "
			   "i_used down to %u?", di->i_blkno,
			   di->id1.bitmap1.i_total, di->id1.bitmap1.i_used, 
			   di->id1.bitmap1.i_total)) {

			di->id1.bitmap1.i_used = di->id1.bitmap1.i_total;
			changed = 1;
		}
	}

out:
	if (broken && !clear &&
	    prompt(ost, PY, PR_LALLOC_CLEAR,
		   "Local alloc inode %"PRIu64" contained errors. "
		   "Mark it as unused instead of trying to correct its "
		   "bitmap?", di->i_blkno)) {
		clear = 1;
	}

	if (clear) {
		di->id1.bitmap1.i_total = 0;
		di->id1.bitmap1.i_used = 0;
		la->la_bm_off = 0;
		memset(la->la_bitmap, 0, 
		       ocfs2_local_alloc_size(ost->ost_fs->fs_blocksize));
		changed = 1;
	}

	if (changed) {
		ret = ocfs2_write_inode(ost->ost_fs, di->i_blkno, (char *)di);
		if (ret) {
			com_err(whoami, ret, "while writing local alloc inode "
				    "%"PRIu64, di->i_blkno);
			ost->ost_write_error = 1;
			ret = 0;
		}
	}

	return ret;
}

/* this just makes sure the truncate log contains consistent data, it doesn't
 * do anything with it yet */
static errcode_t verify_truncate_log(o2fsck_state *ost, ocfs2_dinode *di)
{
	ocfs2_truncate_log *tl = &di->id2.i_dealloc;
	uint16_t max, i;
	int changed = 0;
	errcode_t ret = 0;

	verbosef("tl_count %u tl_used %u (tl_reserved1 %u)\n", tl->tl_count,
		 tl->tl_used, tl->tl_reserved1);

	max = ocfs2_truncate_recs_per_inode(ost->ost_fs->fs_blocksize);

	if (tl->tl_count > max &&
	    prompt(ost, PY, PR_DEALLOC_COUNT,
		   "Truncate log inode %"PRIu64" claims space for %u records but only %u "
		   "records are possible.  Set the inode's count to the maximum?",
		   di->i_blkno, tl->tl_count, max)) {

		tl->tl_count = max;
		changed = 1;
	}

	if (tl->tl_used > tl->tl_count &&
	    prompt(ost, PY, PR_DEALLOC_USED,
		   "Truncate log inode %"PRIu64" claims to be using %u records but the "
		   "inode can only hold %u records.  Change the number used to reflect "
		   "the maximum possible in the inode?", di->i_blkno, tl->tl_used,
		   tl->tl_count)) {

		tl->tl_used = tl->tl_count;
		changed = 1;
	}

	for (i = 0; i < ocfs2_min(max, tl->tl_used); i++) {
		ocfs2_truncate_rec *tr = &tl->tl_recs[i];
		int zero = 0;

		verbosef("t_start %u t_clusters %u\n", tr->t_start,
		         tr->t_clusters);

		if (tr->t_start == 0)
			continue;

		if (tr->t_start >= ost->ost_fs->fs_clusters &&
	            prompt(ost, PY, PR_TRUNCATE_REC_START_RANGE,
			   "Truncate record at offset %u in truncate log "
			   "inode %"PRIu64" starts at cluster %u but there "
			   "are %u clusters in the volume. Remove this record "
			   "from the log?", i, di->i_blkno, tr->t_start,
			   ost->ost_fs->fs_clusters)) {
				zero = 1;
		}

		if (tr->t_start + tr->t_clusters < tr->t_start &&
	            prompt(ost, PY, PR_TRUNCATE_REC_WRAP,
			   "Truncate record at offset %u in truncate log "
			   "inode %"PRIu64" starts at cluster %u and contains "
			   "%u clusters.  It can't have this many clusters "
			   "as that overflows the number of possible clusters "
			   "in a volume.  Remove this record from the log?",
			   i, di->i_blkno, tr->t_start, tr->t_clusters)) {
				zero = 1;
		}

		if (tr->t_start + tr->t_clusters > ost->ost_fs->fs_clusters &&
	            prompt(ost, PY, PR_TRUNCATE_REC_RANGE,
			   "Truncate record at offset %u in truncate log "
			   "inode %"PRIu64" starts at cluster %u and contains "
			   "%u clusters.  It can't have this many clusters "
			   "as this volume only has %u clusters. Remove this "
			   "record from the log?",
			   i, di->i_blkno, tr->t_start, tr->t_clusters,
			   ost->ost_fs->fs_clusters)) {
				zero = 1;
		}

		if (zero) {
			tr->t_start = 0;
			tr->t_clusters = 0;
			changed = 1;
		}
	}

	if (changed) {
		ret = ocfs2_write_inode(ost->ost_fs, di->i_blkno, (char *)di);
		if (ret) {
			com_err(whoami, ret, "while writing truncate log inode "
				    "%"PRIu64, di->i_blkno);
			ost->ost_write_error = 1;
			ret = 0;
		}
	}

	return ret;
}

/* Check the basics of the ocfs2_dinode itself.  If we find problems
 * we clear the VALID flag and the caller will see that and update
 * inode allocations and write the inode to disk. 
 *
 * XXX the o2fsck_write_inode helpers need to be fixed here*/
static void o2fsck_verify_inode_fields(ocfs2_filesys *fs, o2fsck_state *ost, 
				       uint64_t blkno, ocfs2_dinode *di)
{
	int clear = 0;

	verbosef("checking inode %"PRIu64"'s fields\n", blkno);

	if (di->i_fs_generation != ost->ost_fs_generation) {
		if (prompt(ost, PY, PR_INODE_GEN,
			   "Inode read from block %"PRIu64" looks "
			   "like it is valid but it has a generation of %x "
			   "that doesn't match the current volume's "
			   "generation of %x.  This is probably a harmless "
			   "old inode.  Mark it deleted?", blkno, 
			   di->i_fs_generation, ost->ost_fs_generation)) {

			clear = 1;
			goto out;
		}
		if (prompt(ost, PY, PR_INODE_GEN_FIX,
			   "Update the inode's generation to match "
			  "the volume?")) {

			di->i_fs_generation = ost->ost_fs_generation;
			o2fsck_write_inode(ost, blkno, di);
		}
	}

	/* do we want to detect and delete corrupt system dir/files here
	 * so we can recreate them later ? */

	/* also make sure the journal inode is ok? */

	/* clamp inodes to > OCFS2_SUPER_BLOCK_BLKNO && < fs->fs_blocks? */

	/* XXX need to compare the lifetime of inodes (uninitialized?
	 * in use?  orphaned?  deleted?  garbage?) to understand what
	 * fsck can do to fix it up */

	if (di->i_blkno != blkno &&
	    prompt(ost, PY, PR_INODE_BLKNO,
		   "Inode read from block %"PRIu64" has i_blkno set "
		   "to %"PRIu64".  Set the inode's i_blkno value to reflect "
		   "its location on disk?", blkno, di->i_blkno)) {

		di->i_blkno = blkno;
		o2fsck_write_inode(ost, blkno, di);
	}


	/* offer to clear a non-directory root inode so that 
	 * pass3:check_root() can re-create it */
	if ((di->i_blkno == fs->fs_root_blkno) && !S_ISDIR(di->i_mode) && 
	    prompt(ost, PY, PR_ROOT_NOTDIR,
		   "Root inode isn't a directory.  Clear it in "
		   "preparation for fixing it?")) {

		clear = 1;
		goto out;
	}

	if (di->i_dtime &&
	    prompt(ost, PY, PR_INODE_NZ_DTIME,
		   "Inode %"PRIu64" is in use but has a non-zero dtime. Reset "
		   "the dtime to 0?",  di->i_blkno)) {

		di->i_dtime = 0ULL;
		o2fsck_write_inode(ost, blkno, di);
	}

	if (S_ISDIR(di->i_mode)) {
		ocfs2_bitmap_set(ost->ost_dir_inodes, blkno, NULL);
		o2fsck_add_dir_parent(&ost->ost_dir_parents, blkno, 0, 0,
				      di->i_flags & OCFS2_ORPHANED_FL);
	} else if (S_ISREG(di->i_mode)) {
		ocfs2_bitmap_set(ost->ost_reg_inodes, blkno, NULL);
	} else if (S_ISLNK(di->i_mode)) {
		/* we only make sure a link's i_size matches
		 * the link names length in the file data later when
		 * we walk the inode's blocks */
	} else {
		if (!S_ISCHR(di->i_mode) && !S_ISBLK(di->i_mode) &&
		    !S_ISFIFO(di->i_mode) && !S_ISSOCK(di->i_mode)) {
			clear = 1;
			goto out;
		}

		/* i_size?  what other sanity testing for devices? */
	}

	/* put this after all opportunities to clear so we don't have to
	 * unwind it */
	if (di->i_links_count)
		o2fsck_icount_set(ost->ost_icount_in_inodes, di->i_blkno,
					di->i_links_count);

	/* orphan inodes are a special case. if -n is given pass4 will try
	 * and assert that their links_count should include the dirent
	 * reference from the orphan dir. */
	if (di->i_flags & OCFS2_ORPHANED_FL && di->i_links_count == 0)
		o2fsck_icount_set(ost->ost_icount_in_inodes, di->i_blkno, 1);

	if (di->i_flags & OCFS2_LOCAL_ALLOC_FL)
		verify_local_alloc(ost, di);
	else if (di->i_flags & OCFS2_DEALLOC_FL)
		verify_truncate_log(ost, di);

out:
	/* XXX when we clear we need to also free whatever blocks may have
	 * hung off this inode that haven't already been reserved.  we want
	 * to do this on the transition from valid to invalid, not just
	 * any time we see an invalid inode (somewhat obviously). */
	if (clear) {
		di->i_flags &= ~OCFS2_VALID_FL;
		o2fsck_write_inode(ost, blkno, di);
	}
}

struct verifying_blocks {
       unsigned		vb_clear:1,
       			vb_saw_link_null:1;

       uint64_t		vb_link_len;
       uint64_t		vb_num_blocks;	
       uint64_t		vb_last_block;	
       o2fsck_state 	*vb_ost;
       ocfs2_dinode	*vb_di;
       errcode_t	vb_ret;
};

/* last_block and num_blocks would be different in a sparse file */
static void vb_saw_block(struct verifying_blocks *vb, uint64_t bcount)
{
	vb->vb_num_blocks++;
	if (bcount > vb->vb_last_block)
		vb->vb_last_block = bcount;
}

static errcode_t process_link_block(struct verifying_blocks *vb,
				    uint64_t blkno)
{
	char *buf, *null;
	errcode_t ret = 0;
	unsigned int blocksize = vb->vb_ost->ost_fs->fs_blocksize;

	if (vb->vb_saw_link_null)
		goto out;

	ret = ocfs2_malloc_blocks(vb->vb_ost->ost_fs->fs_io, 1, &buf);
	if (ret) {
		com_err(whoami, ret, "while allocating room to read a block "
			"of link data");
		goto out;
	}

	ret = io_read_block(vb->vb_ost->ost_fs->fs_io, blkno, 1, buf);
	if (ret)
		goto out;

	null = memchr(buf, 0, blocksize);
	if (null != NULL) {
		vb->vb_link_len += null - buf;
		vb->vb_saw_link_null = 1;
	} else {
		vb->vb_link_len += blocksize;
	}

out:
	ocfs2_free(&buf);
	return ret;
}

static void check_link_data(struct verifying_blocks *vb)
{
	ocfs2_dinode *di = vb->vb_di;
	o2fsck_state *ost = vb->vb_ost;
	uint64_t expected, link_max;
	char *null;

	verbosef("found a link: num %"PRIu64" last %"PRIu64" len "
		"%"PRIu64" null %d\n", vb->vb_num_blocks, 
		vb->vb_last_block, vb->vb_link_len, vb->vb_saw_link_null);

	if (di->i_clusters == 0 && vb->vb_num_blocks > 0 &&
	    prompt(ost, PY, PR_LINK_FAST_DATA,
		   "Symlink inode %"PRIu64" claims to be a fast symlink "
		   "but has file data.  Clear the inode?", di->i_blkno)) {
		vb->vb_clear = 1;
		return;
	}

	/* if we're a fast link we doctor the verifying_blocks book-keeping
	 * to satisfy the following checks */
	if (di->i_clusters == 0) {
		link_max = ost->ost_fs->fs_blocksize - 
			   offsetof(typeof(*di), id2.i_symlink);

		null = memchr(di->id2.i_symlink, 0, link_max);

		if (null != NULL) {
			vb->vb_saw_link_null = 1;
			vb->vb_link_len = (char *)null - 
					  (char *)di->id2.i_symlink;
		} else
			vb->vb_link_len = link_max;

		expected = 0;
	} else
		expected = ocfs2_blocks_in_bytes(ost->ost_fs,
						 vb->vb_link_len + 1);

	/* XXX this could offer to null terminate */
	if (!vb->vb_saw_link_null) {
		if (prompt(ost, PY, PR_LINK_NULLTERM,
			   "The target of symlink inode %"PRIu64" "
			   "isn't null terminated.  Clear the inode?",
			   di->i_blkno)) {
			vb->vb_clear = 1;
			return;
		}
	}


	if (di->i_size != vb->vb_link_len) {
		if (prompt(ost, PY, PR_LINK_SIZE,
			   "The target of symlink inode %"PRIu64" "
			   "is %"PRIu64" bytes long on disk, but i_size is "
			   "%"PRIu64" bytes long.  Update i_size to reflect "
			   "the length on disk?",
			   di->i_blkno, vb->vb_link_len, di->i_size)) {
			di->i_size = vb->vb_link_len;
			o2fsck_write_inode(ost, di->i_blkno, di);
			return;
		}
	}

	/* maybe we don't shrink link target allocations, I don't know,
	 * someone will holler if this is wrong :) */
	if (vb->vb_num_blocks != expected) {
		if (prompt(ost, PN, PR_LINK_BLOCKS,
			   "The target of symlink inode %"PRIu64" "
			   "fits in %"PRIu64" blocks but the inode has "
			   "%"PRIu64" allocated.  Clear the inode?", 
			   di->i_blkno, expected, vb->vb_num_blocks)) {
			vb->vb_clear = 1;
			return;
		}
	}
}

static int verify_block(ocfs2_filesys *fs,
			    uint64_t blkno,
			    uint64_t bcount,
			    void *priv_data)
{
	struct verifying_blocks *vb = priv_data;
	ocfs2_dinode *di = vb->vb_di;
	o2fsck_state *ost = vb->vb_ost;
	errcode_t ret = 0;
	
	/* someday we may want to worry about holes in files here */

	if (S_ISDIR(di->i_mode)) {
		verbosef("adding dir block %"PRIu64"\n", blkno);
		ret = o2fsck_add_dir_block(&ost->ost_dirblocks, di->i_blkno,
					   blkno, bcount);
		if (ret) {
			com_err(whoami, ret, "while trying to track block in "
				"directory inode %"PRIu64, di->i_blkno);
		}
	} else if (S_ISLNK(di->i_mode))
		ret = process_link_block(vb, blkno);

	if (ret) {
		vb->vb_ret = ret;
		return OCFS2_BLOCK_ABORT;
	}

	vb_saw_block(vb, bcount);

	return 0;
}

/* XXX this is only really building up the vb data so that the caller can
 * verify the chain allocator inode's fields.  I wonder if we shouldn't have
 * already done that in pass 0. */
static int check_gd_block(ocfs2_filesys *fs, uint64_t gd_blkno, int chain_num,
			   void *priv_data)
{
	struct verifying_blocks *vb = priv_data;
	verbosef("found gd block %"PRIu64"\n", gd_blkno);
	/* XXX should arguably be verifying that pass 0 marked the group desc
	 * blocks found */
	/* don't have bcount */
	vb_saw_block(vb, vb->vb_num_blocks);
	return 0;
}


static errcode_t o2fsck_check_blocks(ocfs2_filesys *fs, o2fsck_state *ost,
				     uint64_t blkno, ocfs2_dinode *di)
{
	uint64_t expected = 0;
	errcode_t ret;
	struct verifying_blocks vb = {
		.vb_ost = ost,
		.vb_di = di,
	};

	/*
	 * ISLNK && clusters == 0 is the only sign of an inode that doesn't
	 * have an extent list when i_flags would have us believe it did.  
	 * We might be able to be very clever about discovering the 
	 * difference between i_symlink and i_list, but we don't try yet.
	 */
	if (di->i_flags & OCFS2_LOCAL_ALLOC_FL ||
	    di->i_flags & OCFS2_DEALLOC_FL)
		ret = 0;
	else if (di->i_flags & OCFS2_CHAIN_FL)
		ret = ocfs2_chain_iterate(fs, blkno, check_gd_block, &vb);
	else if (S_ISLNK(di->i_mode) && di->i_clusters == 0) 
		ret = 0;
	else {
		ret = o2fsck_check_extents(ost, di);
		if (ret == 0)
			ret = ocfs2_block_iterate_inode(fs, di, 0,
							verify_block, &vb);
		if (vb.vb_ret)
			ret = vb.vb_ret;
	}

	if (ret) {
		com_err(whoami, ret, "while iterating over the blocks for "
			"inode %"PRIu64, di->i_blkno);	
		goto out;
	}

	if (S_ISLNK(di->i_mode))
		check_link_data(&vb);

	if (S_ISDIR(di->i_mode) && vb.vb_num_blocks == 0 &&
	    prompt(ost, PY, PR_DIR_ZERO,
		   "Inode %"PRIu64" is a zero length directory, clear it?",
		   di->i_blkno)) {

		vb.vb_clear = 1;
	}

	/*
	 * XXX we should have a helper that clears an inode and backs it out of
	 * any book-keeping that it might have been included in, as though it
	 * was never seen.  the alternative is to restart pass1 which seems
	 * goofy. 
	 */
	if (vb.vb_clear) {
		di->i_links_count = 0;
		o2fsck_icount_set(ost->ost_icount_in_inodes, di->i_blkno,
				  di->i_links_count);
		di->i_dtime = time(NULL);
		o2fsck_write_inode(ost, di->i_blkno, di);
		/* XXX clear valid flag and stuff? */
	}

#if 0 /* boy, this is just broken */
	if (vb.vb_num_blocks > 0)
		expected = (vb.vb_last_block + 1) * fs->fs_blocksize;

	/* i_size is checked for symlinks elsewhere */
	if (!S_ISLNK(di->i_mode) && di->i_size > expected &&
	    prompt(ost, PY, 0, "Inode %"PRIu64" has a size of %"PRIu64" but has "
		    "%"PRIu64" bytes of actual data. Correct the file size?",
		    di->i_blkno, di->i_size, expected)) {
		di->i_size = expected;
		o2fsck_write_inode(ost, blkno, di);
	}
#endif

	if (vb.vb_num_blocks > 0)
		expected = ocfs2_clusters_in_blocks(fs, vb.vb_last_block + 1);

	if (di->i_clusters < expected &&
	    prompt(ost, PY, PR_INODE_CLUSTERS,
		   "Inode %"PRIu64" has %"PRIu32" clusters but its "
		   "blocks fit in %"PRIu64" clusters.  Correct the number of "
		   "clusters?", di->i_blkno, di->i_clusters, expected)) {
		di->i_clusters = expected;
		o2fsck_write_inode(ost, blkno, di);
	}
out:
	return ret;
}

/* 
 * we just make sure that the bits that are clear in the local
 * alloc are still reserved in the global bitmap.  We leave
 * cleaning of the local windows to recovery in the file system.
 */
static void mark_local_allocs(o2fsck_state *ost)
{
	int16_t slot;
	uint16_t max_slots;
	char *buf = NULL;
	errcode_t ret;
	uint64_t blkno, start, end;
	ocfs2_dinode *di;
	ocfs2_local_alloc *la = &di->id2.i_lab;
	int bit;

	max_slots = OCFS2_RAW_SB(ost->ost_fs->fs_super)->s_max_slots;

	ret = ocfs2_malloc_block(ost->ost_fs->fs_io, &buf);
	if (ret) {
		com_err(whoami, ret, "while allocating an inode buffer to "
			"use when verifying local alloc inode bitmaps.");
		goto out;
	}

	di = (ocfs2_dinode *)buf; 

	for (slot = 0; slot < max_slots; slot++) {
		ret = ocfs2_lookup_system_inode(ost->ost_fs,
						LOCAL_ALLOC_SYSTEM_INODE,
						slot, &blkno);
		if (ret) {
			com_err(whoami, ret, "while looking up local alloc "
				"inode %"PRIu64" to verify its bitmap",
				blkno);
			goto out;
		}

		ret = ocfs2_read_inode(ost->ost_fs, blkno, buf);
		if (ret) {
			com_err(whoami, ret, "while reading local alloc "
				"inode %"PRIu64" to verify its bitmap",
				blkno);
			goto out;
		}

		la = &di->id2.i_lab;

		if (di->id1.bitmap1.i_total == 0)
			continue;

		/* make sure we don't try to work with a crazy bitmap.  It
		 * can only be this crazy if the user wouldn't let us fix
		 * it up.. just ignore it */
		if (la->la_size > 
		    ocfs2_local_alloc_size(ost->ost_fs->fs_blocksize) ||
		    di->id1.bitmap1.i_used > di->id1.bitmap1.i_total ||
		    di->id1.bitmap1.i_total > la->la_size * 8)
			continue;

		start = la->la_bm_off;
		end = la->la_bm_off + di->id1.bitmap1.i_total;

		if (start >= ost->ost_fs->fs_clusters || 
		    end < start ||
		    end > ost->ost_fs->fs_clusters)
			continue;

		/* bits that are clear in the local alloc haven't been 
		 * used by the slot yet, they must still be set in the
		 * main bitmap.  bits that are set might have been used
		 * and already freed in the main bitmap. */
		for(bit = 0; bit < di->id1.bitmap1.i_total; bit++) {
			bit = ocfs2_find_next_bit_clear(la->la_bitmap,
							di->id1.bitmap1.i_total,
							bit);

			if (bit < di->id1.bitmap1.i_total) {
				verbosef("bit %u is clear, reserving "
					 "cluster %u\n", bit,
					 bit + la->la_bm_off);

				o2fsck_mark_cluster_allocated(ost,
							      bit + 
							      la->la_bm_off);
			}
		}
	}
out:
	if (buf)
		ocfs2_free(&buf);
	return;
}

/* 
 * Clusters that are in the truncate logs should still be allocated.  We just
 * make sure our accounting realizes this and let the kernel replay the logs
 * and free them.  This will change someday when fsck learns to fully practice
 * recovery isntead of just making sure that the system is in a coherent
 * recoverable state.
 */
static void mark_truncate_logs(o2fsck_state *ost)
{
	int16_t slot;
	uint16_t max_slots, i, max;
	ocfs2_truncate_log *tl;
	uint64_t blkno;
	ocfs2_dinode *di;
	char *buf = NULL;
	errcode_t ret;

	max_slots = OCFS2_RAW_SB(ost->ost_fs->fs_super)->s_max_slots;
	max = ocfs2_truncate_recs_per_inode(ost->ost_fs->fs_blocksize);

	ret = ocfs2_malloc_block(ost->ost_fs->fs_io, &buf);
	if (ret) {
		com_err(whoami, ret, "while allocating an inode buffer to "
			"use accounting for records in truncate logs");
		goto out;
	}

	di = (ocfs2_dinode *)buf; 

	for (slot = 0; slot < max_slots; slot++) {
		ret = ocfs2_lookup_system_inode(ost->ost_fs,
						TRUNCATE_LOG_SYSTEM_INODE,
						slot, &blkno);
		if (ret) {
			com_err(whoami, ret, "while looking up truncate log "
				"inode %"PRIu64" to account for its records",
				blkno);
			goto out;
		}

		ret = ocfs2_read_inode(ost->ost_fs, blkno, buf);
		if (ret) {
			com_err(whoami, ret, "while reading truncate log "
				"inode %"PRIu64" to account for its records",
				blkno);
			goto out;
		}

		tl = &di->id2.i_dealloc;

		for (i = 0; i < ocfs2_min(tl->tl_used, max); i++) {
			ocfs2_truncate_rec *tr = &tl->tl_recs[i];

			if (tr->t_start == 0)
				continue;

			verbosef("rec [%u, %u] at off %u\n", tr->t_start, tr->t_clusters,
				 i);

			o2fsck_mark_clusters_allocated(ost, tr->t_start,
						       tr->t_clusters);
		}
	}
out:
	if (buf)
		ocfs2_free(&buf);
	return;
}

/* XXX we really need to get the latch stuff straight */
static errcode_t force_cluster_bit(o2fsck_state *ost, 
				   ocfs2_cached_inode *ci,
				   uint64_t bit,
				   int val)
{
	errcode_t ret;
	char *reason;

	if (!val) {
		reason = "Cluster %u is marked in the global cluster "
			 "bitmap but it isn't in use.  Clear its bit "
			 "in the bitmap?";
	} else {
		reason = "Cluster %u is in use but isn't set in the "
			 "global cluster bitmap.  Set its bit in the "
			 "bitmap?";
	}

	if (!prompt(ost, PY, PR_CLUSTER_ALLOC_BIT, reason, bit))
		return 0;

	ret = ocfs2_chain_force_val(ost->ost_fs, ci, bit, !!val, NULL);
	if (ret)
		com_err(whoami, ret, "while trying to %s bit %"PRIu64" in the "
			"cluster bitmap", val ? "set" : "clear", bit);
	return ret;
}

/* once we've iterated all the inodes we should have the current working
 * set of which blocks we think are in use.  we use this to derive the set
 * of clusters that should be allocated in the cluster chain allocators.  we
 * don't iterate over all clusters like we do inodes.. */
static void write_cluster_alloc(o2fsck_state *ost)
{
	ocfs2_cached_inode *ci = NULL;
	errcode_t ret;
	uint64_t blkno, last_cbit, cbit, cbit_found;
	struct ocfs2_cluster_group_sizes cgs;

	ocfs2_calc_cluster_groups(ost->ost_fs->fs_clusters,
				  ost->ost_fs->fs_blocksize, &cgs);

	/* first load the cluster chain alloc so we can compare */
	ret = ocfs2_lookup_system_inode(ost->ost_fs,
					GLOBAL_BITMAP_SYSTEM_INODE, 0, &blkno);
	if (ret) {
		com_err(whoami, ret, "while looking up the cluster bitmap "
			"allocator inode");
		goto out;
	}

	/* load in the cluster chain allocator */
	ret = ocfs2_read_cached_inode(ost->ost_fs, blkno, &ci);
	if (ret) {
		com_err(whoami, ret, "while reading the cluster bitmap "
			"allocator inode from block %"PRIu64, blkno);
		goto out;
	}

	ret = ocfs2_load_chain_allocator(ost->ost_fs, ci);
	if (ret) {
		com_err(whoami, ret, "while loading the cluster bitmap "
			"allocator from block %"PRIu64, blkno);
		goto out;
	}

	/* we walk our found blocks bitmap to find clusters that we think
	 * are in use.  each time we find a block in a cluster we skip ahead
	 * to the first block of the next cluster when looking for the next.
	 *
	 * once we have a cluster we think is allocated we walk the cluster
	 * chain alloc bitmaps from the last cluster we thought was allocated
	 * to make sure that all the bits are cleared on the way.
	 *
	 * we special case the number of clusters as the cluster offset which
	 * indicates that the rest of the bits to the end of the bitmap
	 * should be clear.
	 */
	for (last_cbit = 0, cbit = 0;
	     cbit < ost->ost_fs->fs_clusters; 
	     cbit++, last_cbit = cbit) {

		ret = ocfs2_bitmap_find_next_set(ost->ost_allocated_clusters,
						 cbit, &cbit);

		/* clear to the end */
		if (ret == OCFS2_ET_BIT_NOT_FOUND)
			cbit = ost->ost_fs->fs_clusters;

		ret = ocfs2_bitmap_find_next_set(ci->ci_chains, last_cbit, 
						 &cbit_found);
		if (ret == OCFS2_ET_BIT_NOT_FOUND)
			cbit_found = ost->ost_fs->fs_clusters;

		verbosef("cbit %"PRIu64" last_cbit %"PRIu64" cbit_found "
			 "%"PRIu64"\n", cbit, last_cbit, cbit_found);

		if (cbit_found == cbit)
			continue;

		/* clear set bits that should have been clear up to cbit */
		while (cbit_found < cbit) {
			force_cluster_bit(ost, ci, cbit_found, 0);
			cbit_found++;
			ret = ocfs2_bitmap_find_next_set(ci->ci_chains, cbit, 
							 &cbit_found);
			if (ret == OCFS2_ET_BIT_NOT_FOUND)
				cbit_found = ost->ost_fs->fs_clusters;
		}

		/* make sure cbit is set before moving on */
		if (cbit_found != cbit && cbit != ost->ost_fs->fs_clusters)
			force_cluster_bit(ost, ci, cbit, 1);
	}

	ret = ocfs2_write_chain_allocator(ost->ost_fs, ci);
	if (ret)
		com_err(whoami, ret, "while trying to write back the cluster "
			"bitmap allocator");

out:
	if (ci)
		ocfs2_free_cached_inode(ost->ost_fs, ci);
}

static void write_inode_alloc(o2fsck_state *ost)
{
	int max_slots = OCFS2_RAW_SB(ost->ost_fs->fs_super)->s_max_slots;
	ocfs2_cached_inode **ci;
	errcode_t ret;
	int i;

	if (!ost->ost_write_inode_alloc)
		return;

	for (i = -1; i < max_slots; i++) {
		if (i == OCFS2_INVALID_SLOT)
			ci = &ost->ost_global_inode_alloc;
		else
			ci = &ost->ost_inode_allocs[i];

		if (*ci == NULL)
			continue;

		verbosef("writing slot %d's allocator\n", i);

		ret = ocfs2_write_chain_allocator(ost->ost_fs, *ci);
		if (ret)
			com_err(whoami, ret, "while trying to write back slot "
				"%d's inode allocator", i);
	}

	o2fsck_free_inode_allocs(ost);
}

errcode_t o2fsck_pass1(o2fsck_state *ost)
{
	errcode_t ret;
	uint64_t blkno;
	char *buf;
	ocfs2_dinode *di;
	ocfs2_inode_scan *scan;
	ocfs2_filesys *fs = ost->ost_fs;
	int valid;

	printf("Pass 1: Checking inodes and blocks.\n");

	ret = ocfs2_malloc_block(fs->fs_io, &buf);
	if (ret) {
		com_err(whoami, ret, "while allocating inode buffer");
		goto out;
	}

	di = (ocfs2_dinode *)buf;

	ret = ocfs2_open_inode_scan(fs, &scan);
	if (ret) {
		com_err(whoami, ret, "while opening inode scan");
		goto out_free;
	}

	for(;;) {
		ret = ocfs2_get_next_inode(scan, &blkno, buf);
		if (ret) {
			/* we don't deal with corrupt inode allocation
			 * files yet.  They won't be files for much longer.
			 * In the future the intent is to clean up inode
			 * allocation if scanning returns an error. */
			com_err(whoami, ret,
				"while getting next inode");
			goto out_close_scan;
		}
		if (blkno == 0)
			break;

		valid = 0;

		/* we never consider inodes who don't have a signature.
		 * We only consider inodes whose generations don't match
		 * if the user has asked us to */
		if (!memcmp(di->i_signature, OCFS2_INODE_SIGNATURE,
			    strlen(OCFS2_INODE_SIGNATURE)) &&
		    (ost->ost_fix_fs_gen ||
		    (di->i_fs_generation == ost->ost_fs_generation))) {

			if (di->i_flags & OCFS2_VALID_FL)
				o2fsck_verify_inode_fields(fs, ost, blkno, di);

			if (di->i_flags & OCFS2_VALID_FL) {
				ret = o2fsck_check_blocks(fs, ost, blkno, di);
				if (ret)
					goto out;
			}

			valid = di->i_flags & OCFS2_VALID_FL;
		}

		update_inode_alloc(ost, di, blkno, valid);
	}

	mark_local_allocs(ost);
	mark_truncate_logs(ost);
	write_cluster_alloc(ost);
	write_inode_alloc(ost);

out_close_scan:
	ocfs2_close_inode_scan(scan);
out_free:
	ocfs2_free(&buf);

out:
	return ret;
}
