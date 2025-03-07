/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: bt_stat.c,v 1.1 2006/01/28 00:09:27 kurt Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <string.h>
#endif

#include "db_int.h"
#include "dbinc/db_page.h"
#include "dbinc/db_shash.h"
#include "dbinc/btree.h"
#include "dbinc/lock.h"
#include "dbinc/log.h"

/*
 * __bam_stat --
 *	Gather/print the btree statistics
 *
 * PUBLIC: int __bam_stat __P((DB *, void *, u_int32_t));
 */
int
__bam_stat(dbp, spp, flags)
	DB *dbp;
	void *spp;
	u_int32_t flags;
{
	BTMETA *meta;
	BTREE *t;
	BTREE_CURSOR *cp;
	DBC *dbc;
	DB_BTREE_STAT *sp;
	DB_LOCK lock, metalock;
	DB_MPOOLFILE *mpf;
	PAGE *h;
	db_pgno_t pgno;
	int ret, t_ret, write_meta;

	PANIC_CHECK(dbp->dbenv);
	DB_ILLEGAL_BEFORE_OPEN(dbp, "DB->stat");

	meta = NULL;
	t = dbp->bt_internal;
	sp = NULL;
	LOCK_INIT(metalock);
	LOCK_INIT(lock);
	mpf = dbp->mpf;
	h = NULL;
	ret = 0;
	write_meta = 0;

	/* Check for invalid flags. */
	if ((ret = __db_statchk(dbp, flags)) != 0)
		return (ret);

	/* Acquire a cursor. */
	if ((ret = dbp->cursor(dbp, NULL, &dbc, 0)) != 0)
		return (ret);
	cp = (BTREE_CURSOR *)dbc->internal;

	DEBUG_LWRITE(dbc, NULL, "bam_stat", NULL, NULL, flags);

	/* Allocate and clear the structure. */
	if ((ret = __os_umalloc(dbp->dbenv, sizeof(*sp), &sp)) != 0)
		goto err;
	memset(sp, 0, sizeof(*sp));

	/* Get the metadata page for the entire database. */
	pgno = PGNO_BASE_MD;
	if ((ret = __db_lget(dbc, 0, pgno, DB_LOCK_READ, 0, &metalock)) != 0)
		goto err;
	if ((ret = mpf->get(mpf, &pgno, 0, (PAGE **)&meta)) != 0)
		goto err;

	if (flags == DB_RECORDCOUNT || flags == DB_CACHED_COUNTS)
		flags = DB_FAST_STAT;
	if (flags == DB_FAST_STAT)
		goto meta_only;

	/* Walk the metadata free list, counting pages. */
	for (sp->bt_free = 0, pgno = meta->dbmeta.free; pgno != PGNO_INVALID;) {
		++sp->bt_free;

		if ((ret = mpf->get(mpf, &pgno, 0, &h)) != 0)
			goto err;

		pgno = h->next_pgno;
		if ((ret = mpf->put(mpf, h, 0)) != 0)
			goto err;
		h = NULL;
	}

	/* Get the root page. */
	pgno = cp->root;
	if ((ret = __db_lget(dbc, 0, pgno, DB_LOCK_READ, 0, &lock)) != 0)
		goto err;
	if ((ret = mpf->get(mpf, &pgno, 0, &h)) != 0)
		goto err;

	/* Get the levels from the root page. */
	sp->bt_levels = h->level;

	/* Discard the root page. */
	if ((ret = mpf->put(mpf, h, 0)) != 0)
		goto err;
	h = NULL;
	__LPUT(dbc, lock);

	/* Walk the tree. */
	if ((ret = __bam_traverse(dbc,
	    DB_LOCK_READ, cp->root, __bam_stat_callback, sp)) != 0)
		goto err;

	/*
	 * Get the subdatabase metadata page if it's not the same as the
	 * one we already have.
	 */
	write_meta = !F_ISSET(dbp, DB_AM_RDONLY);
meta_only:
	if (t->bt_meta != PGNO_BASE_MD || write_meta != 0) {
		if ((ret = mpf->put(mpf, meta, 0)) != 0)
			goto err;
		meta = NULL;
		__LPUT(dbc, metalock);

		if ((ret = __db_lget(dbc,
		    0, t->bt_meta, write_meta == 0 ?
		    DB_LOCK_READ : DB_LOCK_WRITE, 0, &metalock)) != 0)
			goto err;
		if ((ret = mpf->get(mpf, &t->bt_meta, 0, (PAGE **)&meta)) != 0)
			goto err;
	}
	if (flags == DB_FAST_STAT) {
		if (dbp->type == DB_RECNO ||
		    (dbp->type == DB_BTREE && F_ISSET(dbp, DB_AM_RECNUM))) {
			if ((ret = __db_lget(dbc, 0,
			    cp->root, DB_LOCK_READ, 0, &lock)) != 0)
				goto err;
			if ((ret =
			    mpf->get(mpf, &cp->root, 0, (PAGE **)&h)) != 0)
				goto err;

			sp->bt_nkeys = RE_NREC(h);
		} else
			sp->bt_nkeys = meta->dbmeta.key_count;
		sp->bt_ndata = meta->dbmeta.record_count;
	}

	/* Get metadata page statistics. */
	sp->bt_metaflags = meta->dbmeta.flags;
	sp->bt_maxkey = meta->maxkey;
	sp->bt_minkey = meta->minkey;
	sp->bt_re_len = meta->re_len;
	sp->bt_re_pad = meta->re_pad;
	sp->bt_pagesize = meta->dbmeta.pagesize;
	sp->bt_magic = meta->dbmeta.magic;
	sp->bt_version = meta->dbmeta.version;

	if (write_meta != 0) {
		meta->dbmeta.key_count = sp->bt_nkeys;
		meta->dbmeta.record_count = sp->bt_ndata;
	}

	*(DB_BTREE_STAT **)spp = sp;

err:	/* Discard the second page. */
	__LPUT(dbc, lock);
	if (h != NULL && (t_ret = mpf->put(mpf, h, 0)) != 0 && ret == 0)
		ret = t_ret;

	/* Discard the metadata page. */
	__LPUT(dbc, metalock);
	if (meta != NULL && (t_ret = mpf->put(
	    mpf, meta, write_meta == 0 ? 0 : DB_MPOOL_DIRTY)) != 0 && ret == 0)
		ret = t_ret;

	if ((t_ret = dbc->c_close(dbc)) != 0 && ret == 0)
		ret = t_ret;

	if (ret != 0 && sp != NULL) {
		__os_ufree(dbp->dbenv, sp);
		*(DB_BTREE_STAT **)spp = NULL;
	}

	return (ret);
}

/*
 * __bam_traverse --
 *	Walk a Btree database.
 *
 * PUBLIC: int __bam_traverse __P((DBC *, db_lockmode_t,
 * PUBLIC:     db_pgno_t, int (*)(DB *, PAGE *, void *, int *), void *));
 */
int
__bam_traverse(dbc, mode, root_pgno, callback, cookie)
	DBC *dbc;
	db_lockmode_t mode;
	db_pgno_t root_pgno;
	int (*callback)__P((DB *, PAGE *, void *, int *));
	void *cookie;
{
	BINTERNAL *bi;
	BKEYDATA *bk;
	DB *dbp;
	DB_LOCK lock;
	DB_MPOOLFILE *mpf;
	PAGE *h;
	RINTERNAL *ri;
	db_indx_t indx;
	int already_put, ret, t_ret;

	dbp = dbc->dbp;
	mpf = dbp->mpf;
	already_put = 0;

	if ((ret = __db_lget(dbc, 0, root_pgno, mode, 0, &lock)) != 0)
		return (ret);
	if ((ret = mpf->get(mpf, &root_pgno, 0, &h)) != 0) {
		__LPUT(dbc, lock);
		return (ret);
	}

	switch (TYPE(h)) {
	case P_IBTREE:
		for (indx = 0; indx < NUM_ENT(h); indx += O_INDX) {
			bi = GET_BINTERNAL(dbp, h, indx);
			if (B_TYPE(bi->type) == B_OVERFLOW &&
			    (ret = __db_traverse_big(dbp,
			    ((BOVERFLOW *)bi->data)->pgno,
			    callback, cookie)) != 0)
				goto err;
			if ((ret = __bam_traverse(
			    dbc, mode, bi->pgno, callback, cookie)) != 0)
				goto err;
		}
		break;
	case P_IRECNO:
		for (indx = 0; indx < NUM_ENT(h); indx += O_INDX) {
			ri = GET_RINTERNAL(dbp, h, indx);
			if ((ret = __bam_traverse(
			    dbc, mode, ri->pgno, callback, cookie)) != 0)
				goto err;
		}
		break;
	case P_LBTREE:
		for (indx = 0; indx < NUM_ENT(h); indx += P_INDX) {
			bk = GET_BKEYDATA(dbp, h, indx);
			if (B_TYPE(bk->type) == B_OVERFLOW &&
			    (ret = __db_traverse_big(dbp,
			    GET_BOVERFLOW(dbp, h, indx)->pgno,
			    callback, cookie)) != 0)
				goto err;
			bk = GET_BKEYDATA(dbp, h, indx + O_INDX);
			if (B_TYPE(bk->type) == B_DUPLICATE &&
			    (ret = __bam_traverse(dbc, mode,
			    GET_BOVERFLOW(dbp, h, indx + O_INDX)->pgno,
			    callback, cookie)) != 0)
				goto err;
			if (B_TYPE(bk->type) == B_OVERFLOW &&
			    (ret = __db_traverse_big(dbp,
			    GET_BOVERFLOW(dbp, h, indx + O_INDX)->pgno,
			    callback, cookie)) != 0)
				goto err;
		}
		break;
	case P_LDUP:
	case P_LRECNO:
		for (indx = 0; indx < NUM_ENT(h); indx += O_INDX) {
			bk = GET_BKEYDATA(dbp, h, indx);
			if (B_TYPE(bk->type) == B_OVERFLOW &&
			    (ret = __db_traverse_big(dbp,
			    GET_BOVERFLOW(dbp, h, indx)->pgno,
			    callback, cookie)) != 0)
				goto err;
		}
		break;
	}

	ret = callback(dbp, h, cookie, &already_put);

err:	if (!already_put && (t_ret = mpf->put(mpf, h, 0)) != 0 && ret != 0)
		ret = t_ret;
	__LPUT(dbc, lock);

	return (ret);
}

/*
 * __bam_stat_callback --
 *	Statistics callback.
 *
 * PUBLIC: int __bam_stat_callback __P((DB *, PAGE *, void *, int *));
 */
int
__bam_stat_callback(dbp, h, cookie, putp)
	DB *dbp;
	PAGE *h;
	void *cookie;
	int *putp;
{
	DB_BTREE_STAT *sp;
	db_indx_t indx, *inp, top;
	u_int8_t type;

	sp = cookie;
	*putp = 0;
	top = NUM_ENT(h);
	inp = P_INP(dbp, h);

	switch (TYPE(h)) {
	case P_IBTREE:
	case P_IRECNO:
		++sp->bt_int_pg;
		sp->bt_int_pgfree += P_FREESPACE(dbp, h);
		break;
	case P_LBTREE:
		/* Correct for on-page duplicates and deleted items. */
		for (indx = 0; indx < top; indx += P_INDX) {
			if (indx + P_INDX >= top ||
			    inp[indx] != inp[indx + P_INDX])
				++sp->bt_nkeys;

			type = GET_BKEYDATA(dbp, h, indx + O_INDX)->type;
			if (!B_DISSET(type) && B_TYPE(type) != B_DUPLICATE)
				++sp->bt_ndata;
		}

		++sp->bt_leaf_pg;
		sp->bt_leaf_pgfree += P_FREESPACE(dbp, h);
		break;
	case P_LRECNO:
		/*
		 * If walking a recno tree, then each of these items is a key.
		 * Otherwise, we're walking an off-page duplicate set.
		 */
		if (dbp->type == DB_RECNO) {
			sp->bt_nkeys += top;

			/*
			 * Correct for deleted items in non-renumbering
			 * Recno databases.
			 */
			if (F_ISSET(dbp, DB_AM_RENUMBER))
				sp->bt_ndata += top;
			else
				for (indx = 0; indx < top; indx += O_INDX) {
					type = GET_BKEYDATA(dbp, h, indx)->type;
					if (!B_DISSET(type))
						++sp->bt_ndata;
				}

			++sp->bt_leaf_pg;
			sp->bt_leaf_pgfree += P_FREESPACE(dbp, h);
		} else {
			sp->bt_ndata += top;

			++sp->bt_dup_pg;
			sp->bt_dup_pgfree += P_FREESPACE(dbp, h);
		}
		break;
	case P_LDUP:
		/* Correct for deleted items. */
		for (indx = 0; indx < top; indx += O_INDX)
			if (!B_DISSET(GET_BKEYDATA(dbp, h, indx)->type))
				++sp->bt_ndata;

		++sp->bt_dup_pg;
		sp->bt_dup_pgfree += P_FREESPACE(dbp, h);
		break;
	case P_OVERFLOW:
		++sp->bt_over_pg;
		sp->bt_over_pgfree += P_OVFLSPACE(dbp, dbp->pgsize, h);
		break;
	default:
		return (__db_pgfmt(dbp->dbenv, h->pgno));
	}
	return (0);
}

/*
 * __bam_key_range --
 *	Return proportion of keys relative to given key.  The numbers are
 *	slightly skewed due to on page duplicates.
 *
 * PUBLIC: int __bam_key_range __P((DB *,
 * PUBLIC:     DB_TXN *, DBT *, DB_KEY_RANGE *, u_int32_t));
 */
int
__bam_key_range(dbp, txn, dbt, kp, flags)
	DB *dbp;
	DB_TXN *txn;
	DBT *dbt;
	DB_KEY_RANGE *kp;
	u_int32_t flags;
{
	BTREE_CURSOR *cp;
	DBC *dbc;
	EPG *sp;
	double factor;
	int exact, ret, t_ret;

	PANIC_CHECK(dbp->dbenv);
	DB_ILLEGAL_BEFORE_OPEN(dbp, "DB->key_range");

	if (flags != 0)
		return (__db_ferr(dbp->dbenv, "DB->key_range", 0));

	/* Check for consistent transaction usage. */
	if ((ret = __db_check_txn(dbp, txn, DB_LOCK_INVALIDID, 1)) != 0)
		return (ret);

	/* Acquire a cursor. */
	if ((ret = dbp->cursor(dbp, txn, &dbc, 0)) != 0)
		return (ret);

	DEBUG_LWRITE(dbc, NULL, "bam_key_range", NULL, NULL, 0);

	if ((ret = __bam_search(dbc, PGNO_INVALID,
	    dbt, S_STK_ONLY, 1, NULL, &exact)) != 0)
		goto err;

	cp = (BTREE_CURSOR *)dbc->internal;
	kp->less = kp->greater = 0.0;

	factor = 1.0;
	/* Correct the leaf page. */
	cp->csp->entries /= 2;
	cp->csp->indx /= 2;
	for (sp = cp->sp; sp <= cp->csp; ++sp) {
		/*
		 * At each level we know that pages greater than indx contain
		 * keys greater than what we are looking for and those less
		 * than indx are less than.  The one pointed to by indx may
		 * have some less, some greater or even equal.  If indx is
		 * equal to the number of entries, then the key is out of range
		 * and everything is less.
		 */
		if (sp->indx == 0)
			kp->greater += factor * (sp->entries - 1)/sp->entries;
		else if (sp->indx == sp->entries)
			kp->less += factor;
		else {
			kp->less += factor * sp->indx / sp->entries;
			kp->greater += factor *
			    (sp->entries - sp->indx - 1) / sp->entries;
		}
		factor *= 1.0/sp->entries;
	}

	/*
	 * If there was an exact match then assign 1 n'th to the key itself.
	 * Otherwise that factor belongs to those greater than the key, unless
	 * the key was out of range.
	 */
	if (exact)
		kp->equal = factor;
	else {
		if (kp->less != 1)
			kp->greater += factor;
		kp->equal = 0;
	}

	BT_STK_CLR(cp);

err:	if ((t_ret = dbc->c_close(dbc)) != 0 && ret == 0)
		ret = t_ret;

	return (ret);
}
