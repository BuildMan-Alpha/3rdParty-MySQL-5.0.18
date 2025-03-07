/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: db_dup.c,v 1.1 2006/01/28 00:09:27 kurt Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <string.h>
#endif

#include "db_int.h"
#include "dbinc/db_page.h"
#include "dbinc/db_shash.h"
#include "dbinc/lock.h"
#include "dbinc/db_am.h"

/*
 * __db_ditem --
 *	Remove an item from a page.
 *
 * PUBLIC:  int __db_ditem __P((DBC *, PAGE *, u_int32_t, u_int32_t));
 */
int
__db_ditem(dbc, pagep, indx, nbytes)
	DBC *dbc;
	PAGE *pagep;
	u_int32_t indx, nbytes;
{
	DB *dbp;
	DBT ldbt;
	db_indx_t cnt, *inp, offset;
	int ret;
	u_int8_t *from;

	dbp = dbc->dbp;
	if (DBC_LOGGING(dbc)) {
		ldbt.data = P_ENTRY(dbp, pagep, indx);
		ldbt.size = nbytes;
		if ((ret = __db_addrem_log(dbp, dbc->txn,
		    &LSN(pagep), 0, DB_REM_DUP, PGNO(pagep),
		    (u_int32_t)indx, nbytes, &ldbt, NULL, &LSN(pagep))) != 0)
			return (ret);
	} else
		LSN_NOT_LOGGED(LSN(pagep));

	/*
	 * If there's only a single item on the page, we don't have to
	 * work hard.
	 */
	if (NUM_ENT(pagep) == 1) {
		NUM_ENT(pagep) = 0;
		HOFFSET(pagep) = dbp->pgsize;
		return (0);
	}

	inp = P_INP(dbp, pagep);
	/*
	 * Pack the remaining key/data items at the end of the page.  Use
	 * memmove(3), the regions may overlap.
	 */
	from = (u_int8_t *)pagep + HOFFSET(pagep);
	DB_ASSERT((int)inp[indx] - HOFFSET(pagep) >= 0);
	memmove(from + nbytes, from, inp[indx] - HOFFSET(pagep));
	HOFFSET(pagep) += nbytes;

	/* Adjust the indices' offsets. */
	offset = inp[indx];
	for (cnt = 0; cnt < NUM_ENT(pagep); ++cnt)
		if (inp[cnt] < offset)
			inp[cnt] += nbytes;

	/* Shift the indices down. */
	--NUM_ENT(pagep);
	if (indx != NUM_ENT(pagep))
		memmove(&inp[indx], &inp[indx + 1],
		    sizeof(db_indx_t) * (NUM_ENT(pagep) - indx));

	return (0);
}

/*
 * __db_pitem --
 *	Put an item on a page.
 *
 * PUBLIC: int __db_pitem
 * PUBLIC:     __P((DBC *, PAGE *, u_int32_t, u_int32_t, DBT *, DBT *));
 */
int
__db_pitem(dbc, pagep, indx, nbytes, hdr, data)
	DBC *dbc;
	PAGE *pagep;
	u_int32_t indx;
	u_int32_t nbytes;
	DBT *hdr, *data;
{
	DB *dbp;
	BKEYDATA bk;
	DBT thdr;
	db_indx_t *inp;
	int ret;
	u_int8_t *p;

	dbp = dbc->dbp;
	if (nbytes > P_FREESPACE(dbp, pagep)) {
		DB_ASSERT(nbytes <= P_FREESPACE(dbp, pagep));
		return (EINVAL);
	}
	/*
	 * Put a single item onto a page.  The logic figuring out where to
	 * insert and whether it fits is handled in the caller.  All we do
	 * here is manage the page shuffling.  We cheat a little bit in that
	 * we don't want to copy the dbt on a normal put twice.  If hdr is
	 * NULL, we create a BKEYDATA structure on the page, otherwise, just
	 * copy the caller's information onto the page.
	 *
	 * This routine is also used to put entries onto the page where the
	 * entry is pre-built, e.g., during recovery.  In this case, the hdr
	 * will point to the entry, and the data argument will be NULL.
	 *
	 * !!!
	 * There's a tremendous potential for off-by-one errors here, since
	 * the passed in header sizes must be adjusted for the structure's
	 * placeholder for the trailing variable-length data field.
	 */
	if (DBC_LOGGING(dbc)) {
		if ((ret = __db_addrem_log(dbp, dbc->txn,
		    &LSN(pagep), 0, DB_ADD_DUP, PGNO(pagep),
		    (u_int32_t)indx, nbytes, hdr, data, &LSN(pagep))) != 0)
			return (ret);
	} else
		LSN_NOT_LOGGED(LSN(pagep));

	if (hdr == NULL) {
		B_TSET(bk.type, B_KEYDATA, 0);
		bk.len = data == NULL ? 0 : data->size;

		thdr.data = &bk;
		thdr.size = SSZA(BKEYDATA, data);
		hdr = &thdr;
	}
	inp = P_INP(dbp, pagep);

	/* Adjust the index table, then put the item on the page. */
	if (indx != NUM_ENT(pagep))
		memmove(&inp[indx + 1], &inp[indx],
		    sizeof(db_indx_t) * (NUM_ENT(pagep) - indx));
	HOFFSET(pagep) -= nbytes;
	inp[indx] = HOFFSET(pagep);
	++NUM_ENT(pagep);

	p = P_ENTRY(dbp, pagep, indx);
	memcpy(p, hdr->data, hdr->size);
	if (data != NULL)
		memcpy(p + hdr->size, data->data, data->size);

	return (0);
}

/*
 * __db_relink --
 *	Relink around a deleted page.
 *
 * PUBLIC: int __db_relink __P((DBC *, u_int32_t, PAGE *, PAGE **, int));
 */
int
__db_relink(dbc, add_rem, pagep, new_next, needlock)
	DBC *dbc;
	u_int32_t add_rem;
	PAGE *pagep, **new_next;
	int needlock;
{
	DB *dbp;
	PAGE *np, *pp;
	DB_LOCK npl, ppl;
	DB_LSN *nlsnp, *plsnp, ret_lsn;
	DB_MPOOLFILE *mpf;
	int ret;

	dbp = dbc->dbp;
	np = pp = NULL;
	LOCK_INIT(npl);
	LOCK_INIT(ppl);
	nlsnp = plsnp = NULL;
	mpf = dbp->mpf;
	ret = 0;

	/*
	 * Retrieve and lock the one/two pages.  For a remove, we may need
	 * two pages (the before and after).  For an add, we only need one
	 * because, the split took care of the prev.
	 */
	if (pagep->next_pgno != PGNO_INVALID) {
		if (needlock && (ret = __db_lget(dbc,
		    0, pagep->next_pgno, DB_LOCK_WRITE, 0, &npl)) != 0)
			goto err;
		if ((ret = mpf->get(mpf, &pagep->next_pgno, 0, &np)) != 0) {
			__db_pgerr(dbp, pagep->next_pgno, ret);
			goto err;
		}
		nlsnp = &np->lsn;
	}
	if (add_rem == DB_REM_PAGE && pagep->prev_pgno != PGNO_INVALID) {
		if (needlock && (ret = __db_lget(dbc,
		    0, pagep->prev_pgno, DB_LOCK_WRITE, 0, &ppl)) != 0)
			goto err;
		if ((ret = mpf->get(mpf, &pagep->prev_pgno, 0, &pp)) != 0) {
			__db_pgerr(dbp, pagep->next_pgno, ret);
			goto err;
		}
		plsnp = &pp->lsn;
	}

	/* Log the change. */
	if (DBC_LOGGING(dbc)) {
		if ((ret = __db_relink_log(dbp, dbc->txn, &ret_lsn, 0, add_rem,
		    pagep->pgno, &pagep->lsn, pagep->prev_pgno, plsnp,
		    pagep->next_pgno, nlsnp)) != 0)
			goto err;
	} else
		LSN_NOT_LOGGED(ret_lsn);
	if (np != NULL)
		np->lsn = ret_lsn;
	if (pp != NULL)
		pp->lsn = ret_lsn;
	if (add_rem == DB_REM_PAGE)
		pagep->lsn = ret_lsn;

	/*
	 * Modify and release the two pages.
	 *
	 * !!!
	 * The parameter new_next gets set to the page following the page we
	 * are removing.  If there is no following page, then new_next gets
	 * set to NULL.
	 */
	if (np != NULL) {
		if (add_rem == DB_ADD_PAGE)
			np->prev_pgno = pagep->pgno;
		else
			np->prev_pgno = pagep->prev_pgno;
		if (new_next == NULL)
			ret = mpf->put(mpf, np, DB_MPOOL_DIRTY);
		else {
			*new_next = np;
			ret = mpf->set(mpf, np, DB_MPOOL_DIRTY);
		}
		if (ret != 0)
			goto err;
		if (needlock)
			(void)__TLPUT(dbc, npl);
	} else if (new_next != NULL)
		*new_next = NULL;

	if (pp != NULL) {
		pp->next_pgno = pagep->next_pgno;
		if ((ret = mpf->put(mpf, pp, DB_MPOOL_DIRTY)) != 0)
			goto err;
		if (needlock)
			(void)__TLPUT(dbc, ppl);
	}
	return (0);

err:	if (np != NULL)
		(void)mpf->put(mpf, np, 0);
	if (needlock)
		(void)__TLPUT(dbc, npl);
	if (pp != NULL)
		(void)mpf->put(mpf, pp, 0);
	if (needlock)
		(void)__TLPUT(dbc, ppl);
	return (ret);
}
