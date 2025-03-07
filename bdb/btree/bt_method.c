/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1999-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: bt_method.c,v 1.1 2006/01/28 00:09:27 kurt Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>
#endif

#include "db_int.h"
#include "dbinc/db_page.h"
#include "dbinc/btree.h"
#include "dbinc/qam.h"

static int __bam_set_bt_compare
	       __P((DB *, int (*)(DB *, const DBT *, const DBT *)));
static int __bam_set_bt_maxkey __P((DB *, u_int32_t));
static int __bam_set_bt_minkey __P((DB *, u_int32_t));
static int __bam_set_bt_prefix
	       __P((DB *, size_t(*)(DB *, const DBT *, const DBT *)));
static int __ram_set_re_delim __P((DB *, int));
static int __ram_set_re_len __P((DB *, u_int32_t));
static int __ram_set_re_pad __P((DB *, int));
static int __ram_set_re_source __P((DB *, const char *));

/*
 * __bam_db_create --
 *	Btree specific initialization of the DB structure.
 *
 * PUBLIC: int __bam_db_create __P((DB *));
 */
int
__bam_db_create(dbp)
	DB *dbp;
{
	BTREE *t;
	int ret;

	/* Allocate and initialize the private btree structure. */
	if ((ret = __os_calloc(dbp->dbenv, 1, sizeof(BTREE), &t)) != 0)
		return (ret);
	dbp->bt_internal = t;

	t->bt_minkey = DEFMINKEYPAGE;		/* Btree */
	t->bt_compare = __bam_defcmp;
	t->bt_prefix = __bam_defpfx;

	dbp->set_bt_compare = __bam_set_bt_compare;
	dbp->set_bt_maxkey = __bam_set_bt_maxkey;
	dbp->set_bt_minkey = __bam_set_bt_minkey;
	dbp->set_bt_prefix = __bam_set_bt_prefix;

	t->re_pad = ' ';			/* Recno */
	t->re_delim = '\n';
	t->re_eof = 1;

	dbp->set_re_delim = __ram_set_re_delim;
	dbp->set_re_len = __ram_set_re_len;
	dbp->set_re_pad = __ram_set_re_pad;
	dbp->set_re_source = __ram_set_re_source;

	return (0);
}

/*
 * __bam_db_close --
 *	Btree specific discard of the DB structure.
 *
 * PUBLIC: int __bam_db_close __P((DB *));
 */
int
__bam_db_close(dbp)
	DB *dbp;
{
	BTREE *t;

	if ((t = dbp->bt_internal) == NULL)
		return (0);
						/* Recno */
	/* Close any backing source file descriptor. */
	if (t->re_fp != NULL)
		(void)fclose(t->re_fp);

	/* Free any backing source file name. */
	if (t->re_source != NULL)
		__os_free(dbp->dbenv, t->re_source);

	__os_free(dbp->dbenv, t);
	dbp->bt_internal = NULL;

	return (0);
}

/*
 * __bam_set_flags --
 *	Set Btree specific flags.
 *
 * PUBLIC: int __bam_set_flags __P((DB *, u_int32_t *flagsp));
 */
int
__bam_set_flags(dbp, flagsp)
	DB *dbp;
	u_int32_t *flagsp;
{
	u_int32_t flags;

	flags = *flagsp;
	if (LF_ISSET(DB_DUP | DB_DUPSORT | DB_RECNUM | DB_REVSPLITOFF)) {
		DB_ILLEGAL_AFTER_OPEN(dbp, "DB->set_flags");

		/*
		 * The DB_DUP and DB_DUPSORT flags are shared by the Hash
		 * and Btree access methods.
		 */
		if (LF_ISSET(DB_DUP | DB_DUPSORT))
			DB_ILLEGAL_METHOD(dbp, DB_OK_BTREE | DB_OK_HASH);

		if (LF_ISSET(DB_RECNUM | DB_REVSPLITOFF))
			DB_ILLEGAL_METHOD(dbp, DB_OK_BTREE);

		if (LF_ISSET(DB_DUP | DB_DUPSORT)) {
			/* DB_DUP/DB_DUPSORT is incompatible with DB_RECNUM. */
			if (F_ISSET(dbp, DB_AM_RECNUM))
				goto incompat;

			if (LF_ISSET(DB_DUPSORT)) {
				if (dbp->dup_compare == NULL)
					dbp->dup_compare = __bam_defcmp;
				F_SET(dbp, DB_AM_DUPSORT);
			}

			F_SET(dbp, DB_AM_DUP);
			LF_CLR(DB_DUP | DB_DUPSORT);
		}

		if (LF_ISSET(DB_RECNUM)) {
			/* DB_RECNUM is incompatible with DB_DUP/DB_DUPSORT. */
			if (F_ISSET(dbp, DB_AM_DUP))
				goto incompat;

			F_SET(dbp, DB_AM_RECNUM);
			LF_CLR(DB_RECNUM);
		}

		if (LF_ISSET(DB_REVSPLITOFF)) {
			F_SET(dbp, DB_AM_REVSPLITOFF);
			LF_CLR(DB_REVSPLITOFF);
		}

		*flagsp = flags;
	}
	return (0);

incompat:
	return (__db_ferr(dbp->dbenv, "DB->set_flags", 1));
}

/*
 * __bam_set_bt_compare --
 *	Set the comparison function.
 */
static int
__bam_set_bt_compare(dbp, func)
	DB *dbp;
	int (*func) __P((DB *, const DBT *, const DBT *));
{
	BTREE *t;

	DB_ILLEGAL_AFTER_OPEN(dbp, "set_bt_compare");
	DB_ILLEGAL_METHOD(dbp, DB_OK_BTREE);

	t = dbp->bt_internal;

	/*
	 * Can't default the prefix routine if the user supplies a comparison
	 * routine; shortening the keys can break their comparison algorithm.
	 */
	t->bt_compare = func;
	if (t->bt_prefix == __bam_defpfx)
		t->bt_prefix = NULL;

	return (0);
}

/*
 * __bam_set_bt_maxkey --
 *	Set the maximum keys per page.
 */
static int
__bam_set_bt_maxkey(dbp, bt_maxkey)
	DB *dbp;
	u_int32_t bt_maxkey;
{
	BTREE *t;

	DB_ILLEGAL_AFTER_OPEN(dbp, "set_bt_maxkey");
	DB_ILLEGAL_METHOD(dbp, DB_OK_BTREE);

	t = dbp->bt_internal;

	if (bt_maxkey < 1) {
		__db_err(dbp->dbenv, "minimum bt_maxkey value is 1");
		return (EINVAL);
	}

	t->bt_maxkey = bt_maxkey;
	return (0);
}

/*
 * __bam_set_bt_minkey --
 *	Set the minimum keys per page.
 */
static int
__bam_set_bt_minkey(dbp, bt_minkey)
	DB *dbp;
	u_int32_t bt_minkey;
{
	BTREE *t;

	DB_ILLEGAL_AFTER_OPEN(dbp, "set_bt_minkey");
	DB_ILLEGAL_METHOD(dbp, DB_OK_BTREE);

	t = dbp->bt_internal;

	if (bt_minkey < 2) {
		__db_err(dbp->dbenv, "minimum bt_minkey value is 2");
		return (EINVAL);
	}

	t->bt_minkey = bt_minkey;
	return (0);
}

/*
 * __bam_set_bt_prefix --
 *	Set the prefix function.
 */
static int
__bam_set_bt_prefix(dbp, func)
	DB *dbp;
	size_t (*func) __P((DB *, const DBT *, const DBT *));
{
	BTREE *t;

	DB_ILLEGAL_AFTER_OPEN(dbp, "set_bt_prefix");
	DB_ILLEGAL_METHOD(dbp, DB_OK_BTREE);

	t = dbp->bt_internal;

	t->bt_prefix = func;
	return (0);
}

/*
 * __ram_set_flags --
 *	Set Recno specific flags.
 *
 * PUBLIC: int __ram_set_flags __P((DB *, u_int32_t *flagsp));
 */
int
__ram_set_flags(dbp, flagsp)
	DB *dbp;
	u_int32_t *flagsp;
{
	u_int32_t flags;

	flags = *flagsp;
	if (LF_ISSET(DB_RENUMBER | DB_SNAPSHOT)) {
		DB_ILLEGAL_AFTER_OPEN(dbp, "DB->set_flags");

		DB_ILLEGAL_METHOD(dbp, DB_OK_RECNO);

		if (LF_ISSET(DB_RENUMBER)) {
			F_SET(dbp, DB_AM_RENUMBER);
			LF_CLR(DB_RENUMBER);
		}

		if (LF_ISSET(DB_SNAPSHOT)) {
			F_SET(dbp, DB_AM_SNAPSHOT);
			LF_CLR(DB_SNAPSHOT);
		}

		*flagsp = flags;
	}
	return (0);
}

/*
 * __ram_set_re_delim --
 *	Set the variable-length input record delimiter.
 */
static int
__ram_set_re_delim(dbp, re_delim)
	DB *dbp;
	int re_delim;
{
	BTREE *t;

	DB_ILLEGAL_AFTER_OPEN(dbp, "set_re_delim");
	DB_ILLEGAL_METHOD(dbp, DB_OK_RECNO);

	t = dbp->bt_internal;

	t->re_delim = re_delim;
	F_SET(dbp, DB_AM_DELIMITER);

	return (0);
}

/*
 * __ram_set_re_len --
 *	Set the variable-length input record length.
 */
static int
__ram_set_re_len(dbp, re_len)
	DB *dbp;
	u_int32_t re_len;
{
	BTREE *t;
	QUEUE *q;

	DB_ILLEGAL_AFTER_OPEN(dbp, "set_re_len");
	DB_ILLEGAL_METHOD(dbp, DB_OK_QUEUE | DB_OK_RECNO);

	t = dbp->bt_internal;
	t->re_len = re_len;

	q = dbp->q_internal;
	q->re_len = re_len;

	F_SET(dbp, DB_AM_FIXEDLEN);

	return (0);
}

/*
 * __ram_set_re_pad --
 *	Set the fixed-length record pad character.
 */
static int
__ram_set_re_pad(dbp, re_pad)
	DB *dbp;
	int re_pad;
{
	BTREE *t;
	QUEUE *q;

	DB_ILLEGAL_AFTER_OPEN(dbp, "set_re_pad");
	DB_ILLEGAL_METHOD(dbp, DB_OK_QUEUE | DB_OK_RECNO);

	t = dbp->bt_internal;
	t->re_pad = re_pad;

	q = dbp->q_internal;
	q->re_pad = re_pad;

	F_SET(dbp, DB_AM_PAD);

	return (0);
}

/*
 * __ram_set_re_source --
 *	Set the backing source file name.
 */
static int
__ram_set_re_source(dbp, re_source)
	DB *dbp;
	const char *re_source;
{
	BTREE *t;

	DB_ILLEGAL_AFTER_OPEN(dbp, "set_re_source");
	DB_ILLEGAL_METHOD(dbp, DB_OK_RECNO);

	t = dbp->bt_internal;

	return (__os_strdup(dbp->dbenv, re_source, &t->re_source));
}
