/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1999-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: qam_open.c,v 1.1 2006/01/28 00:09:28 kurt Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <string.h>
#endif

#include "db_int.h"
#include "dbinc/crypto.h"
#include "dbinc/db_page.h"
#include "dbinc/db_shash.h"
#include "dbinc/db_swap.h"
#include "dbinc/db_am.h"
#include "dbinc/lock.h"
#include "dbinc/qam.h"
#include "dbinc/fop.h"

static int __qam_init_meta __P((DB *, QMETA *));

/*
 * __qam_open
 *
 * PUBLIC: int __qam_open __P((DB *,
 * PUBLIC:     DB_TXN *, const char *, db_pgno_t, int, u_int32_t));
 */
int
__qam_open(dbp, txn, name, base_pgno, mode, flags)
	DB *dbp;
	DB_TXN *txn;
	const char *name;
	db_pgno_t base_pgno;
	int mode;
	u_int32_t flags;
{
	DBC *dbc;
	DB_ENV *dbenv;
	DB_LOCK metalock;
	DB_MPOOLFILE *mpf;
	QMETA *qmeta;
	QUEUE *t;
	int ret, t_ret;

	dbenv = dbp->dbenv;
	mpf = dbp->mpf;
	t = dbp->q_internal;
	ret = 0;
	qmeta = NULL;

	/* Initialize the remaining fields/methods of the DB. */
	dbp->stat = __qam_stat;
	dbp->sync = __qam_sync;
	dbp->db_am_remove = __qam_remove;
	dbp->db_am_rename = __qam_rename;

	/*
	 * Get a cursor.  If DB_CREATE is specified, we may be creating
	 * pages, and to do that safely in CDB we need a write cursor.
	 * In STD_LOCKING mode, we'll synchronize using the meta page
	 * lock instead.
	 */
	if ((ret = dbp->cursor(dbp, txn, &dbc,
	    LF_ISSET(DB_CREATE) && CDB_LOCKING(dbenv) ?  DB_WRITECURSOR : 0))
	    != 0)
		return (ret);

	/*
	 * Get the meta data page.  It must exist, because creates of
	 * files/databases come in through the __qam_new_file interface
	 * and queue doesn't support subdatabases.
	 */
	if ((ret =
	    __db_lget(dbc, 0, base_pgno, DB_LOCK_READ, 0, &metalock)) != 0)
		goto err;
	if ((ret =
	    mpf->get(mpf, &base_pgno, 0, (PAGE **)&qmeta)) != 0)
		goto err;

	/* If the magic number is incorrect, that's a fatal error. */
	if (qmeta->dbmeta.magic != DB_QAMMAGIC) {
		__db_err(dbenv, "%s: unexpected file type or format", name);
		ret = EINVAL;
		goto err;
	}

	/* Setup information needed to open extents. */
	t->page_ext = qmeta->page_ext;

	if (t->page_ext != 0) {
		t->pginfo.db_pagesize = dbp->pgsize;
		t->pginfo.flags =
		    F_ISSET(dbp, (DB_AM_CHKSUM | DB_AM_ENCRYPT | DB_AM_SWAP));
		t->pginfo.type = dbp->type;
		t->pgcookie.data = &t->pginfo;
		t->pgcookie.size = sizeof(DB_PGINFO);

		if ((ret = __os_strdup(dbp->dbenv, name,  &t->path)) != 0)
			return (ret);
		t->dir = t->path;
		if ((t->name = __db_rpath(t->path)) == NULL) {
			t->name = t->path;
			t->dir = PATH_DOT;
		} else
			*t->name++ = '\0';

		if (mode == 0)
			mode = __db_omode("rwrw--");
		t->mode = mode;
	}

	if (name == NULL && t->page_ext != 0) {
		__db_err(dbenv,
	"Extent size may not be specified for in-memory queue database");
		return (EINVAL);
	}

	t->re_pad = qmeta->re_pad;
	t->re_len = qmeta->re_len;
	t->rec_page = qmeta->rec_page;

	t->q_meta = base_pgno;
	t->q_root = base_pgno + 1;

err:	if (qmeta != NULL && (t_ret = mpf->put(mpf, qmeta, 0)) != 0 && ret == 0)
		ret = t_ret;

	/* Don't hold the meta page long term. */
	(void)__LPUT(dbc, metalock);

	if ((t_ret = dbc->c_close(dbc)) != 0 && ret == 0)
		ret = t_ret;

	return (ret);
}

/*
 * __qam_metachk --
 *
 * PUBLIC: int __qam_metachk __P((DB *, const char *, QMETA *));
 */
int
__qam_metachk(dbp, name, qmeta)
	DB *dbp;
	const char *name;
	QMETA *qmeta;
{
	DB_ENV *dbenv;
	u_int32_t vers;
	int ret;

	dbenv = dbp->dbenv;
	ret = 0;

	/*
	 * At this point, all we know is that the magic number is for a Queue.
	 * Check the version, the database may be out of date.
	 */
	vers = qmeta->dbmeta.version;
	if (F_ISSET(dbp, DB_AM_SWAP))
		M_32_SWAP(vers);
	switch (vers) {
	case 1:
	case 2:
		__db_err(dbenv,
		    "%s: queue version %lu requires a version upgrade",
		    name, (u_long)vers);
		return (DB_OLD_VERSION);
	case 3:
	case 4:
		break;
	default:
		__db_err(dbenv,
		    "%s: unsupported qam version: %lu", name, (u_long)vers);
		return (EINVAL);
	}

	/* Swap the page if we need to. */
	if (F_ISSET(dbp, DB_AM_SWAP) && (ret = __qam_mswap((PAGE *)qmeta)) != 0)
		return (ret);

	/* Check the type. */
	if (dbp->type != DB_QUEUE && dbp->type != DB_UNKNOWN)
		return (EINVAL);
	dbp->type = DB_QUEUE;
	DB_ILLEGAL_METHOD(dbp, DB_OK_QUEUE);

	/* Set the page size. */
	dbp->pgsize = qmeta->dbmeta.pagesize;

	/* Copy the file's ID. */
	memcpy(dbp->fileid, qmeta->dbmeta.uid, DB_FILE_ID_LEN);

	/* Set up AM-specific methods that do not require an open. */
	dbp->db_am_rename = __qam_rename;
	dbp->db_am_remove = __qam_remove;

	return (ret);
}

/*
 * __qam_init_meta --
 *	Initialize the meta-data for a Queue database.
 */
static int
__qam_init_meta(dbp, meta)
	DB *dbp;
	QMETA *meta;
{
	QUEUE *t;

	t = dbp->q_internal;

	memset(meta, 0, sizeof(QMETA));
	LSN_NOT_LOGGED(meta->dbmeta.lsn);
	meta->dbmeta.pgno = PGNO_BASE_MD;
	meta->dbmeta.last_pgno = 0;
	meta->dbmeta.magic = DB_QAMMAGIC;
	meta->dbmeta.version = DB_QAMVERSION;
	meta->dbmeta.pagesize = dbp->pgsize;
	if (F_ISSET(dbp, DB_AM_CHKSUM))
		FLD_SET(meta->dbmeta.metaflags, DBMETA_CHKSUM);
	if (F_ISSET(dbp, DB_AM_ENCRYPT)) {
		meta->dbmeta.encrypt_alg =
		   ((DB_CIPHER *)dbp->dbenv->crypto_handle)->alg;
		DB_ASSERT(meta->dbmeta.encrypt_alg != 0);
		meta->crypto_magic = meta->dbmeta.magic;
	}
	meta->dbmeta.type = P_QAMMETA;
	meta->re_pad = t->re_pad;
	meta->re_len = t->re_len;
	meta->rec_page = CALC_QAM_RECNO_PER_PAGE(dbp);
	meta->cur_recno = 1;
	meta->first_recno = 1;
	meta->page_ext = t->page_ext;
	t->rec_page = meta->rec_page;
	memcpy(meta->dbmeta.uid, dbp->fileid, DB_FILE_ID_LEN);

	/* Verify that we can fit at least one record per page. */
	if (QAM_RECNO_PER_PAGE(dbp) < 1) {
		__db_err(dbp->dbenv,
		    "Record size of %lu too large for page size of %lu",
		    (u_long)t->re_len, (u_long)dbp->pgsize);
		return (EINVAL);
	}

	return (0);
}

/*
 * __qam_new_file --
 * Create the necessary pages to begin a new queue database file.
 *
 * This code appears more complex than it is because of the two cases (named
 * and unnamed).  The way to read the code is that for each page being created,
 * there are three parts: 1) a "get page" chunk (which either uses malloc'd
 * memory or calls mpf->get), 2) the initialization, and 3) the "put page"
 * chunk which either does a fop write or an mpf->put.
 *
 * PUBLIC: int __qam_new_file __P((DB *, DB_TXN *, DB_FH *, const char *));
 */
int
__qam_new_file(dbp, txn, fhp, name)
	DB *dbp;
	DB_TXN *txn;
	DB_FH *fhp;
	const char *name;
{
	QMETA *meta;
	DB_ENV *dbenv;
	DB_MPOOLFILE *mpf;
	DB_PGINFO pginfo;
	DBT pdbt;
	db_pgno_t pgno;
	int ret;
	void *buf;

	dbenv = dbp->dbenv;
	mpf = dbp->mpf;
	buf = NULL;
	meta = NULL;

	/* Build meta-data page. */

	if (name == NULL) {
		pgno = PGNO_BASE_MD;
		ret = mpf->get(mpf, &pgno, DB_MPOOL_CREATE, &meta);
	} else {
		ret = __os_calloc(dbp->dbenv, 1, dbp->pgsize, &buf);
		meta = (QMETA *)buf;
	}
	if (ret != 0)
		return (ret);

	if ((ret = __qam_init_meta(dbp, meta)) != 0)
		goto err;

	if (name == NULL)
		ret = mpf->put(mpf, meta, DB_MPOOL_DIRTY);
	else {
		pginfo.db_pagesize = dbp->pgsize;
		pginfo.flags =
		    F_ISSET(dbp, (DB_AM_CHKSUM | DB_AM_ENCRYPT | DB_AM_SWAP));
		pginfo.type = DB_QUEUE;
		pdbt.data = &pginfo;
		pdbt.size = sizeof(pginfo);
		if ((ret = __db_pgout(dbenv, PGNO_BASE_MD, meta, &pdbt)) != 0)
			goto err;
		ret = __fop_write(dbenv,
		    txn, name, DB_APP_DATA, fhp, 0, buf, dbp->pgsize, 1);
	}
	if (ret != 0)
		goto err;
	meta = NULL;

err:	if (name != NULL)
		__os_free(dbenv, buf);
	else if (meta != NULL)
		(void)mpf->put(mpf, meta, 0);
	return (ret);
}
