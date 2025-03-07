/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1999-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: tcl_db.c,v 1.1 2006/01/28 00:09:28 kurt Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <stdlib.h>
#include <string.h>
#include <tcl.h>
#endif

#include "db_int.h"
#include "dbinc/db_page.h"
#include "dbinc/db_am.h"
#include "dbinc/tcl_db.h"

/*
 * Prototypes for procedures defined later in this file:
 */
static int	tcl_DbAssociate __P((Tcl_Interp *,
    int, Tcl_Obj * CONST*, DB *));
static int	tcl_DbClose __P((Tcl_Interp *,
    int, Tcl_Obj * CONST*, DB *, DBTCL_INFO *));
static int	tcl_DbDelete __P((Tcl_Interp *, int, Tcl_Obj * CONST*, DB *));
static int	tcl_DbGet __P((Tcl_Interp *, int, Tcl_Obj * CONST*, DB *, int));
static int	tcl_DbKeyRange __P((Tcl_Interp *, int, Tcl_Obj * CONST*, DB *));
static int	tcl_DbPut __P((Tcl_Interp *, int, Tcl_Obj * CONST*, DB *));
static int	tcl_DbStat __P((Tcl_Interp *, int, Tcl_Obj * CONST*, DB *));
static int	tcl_DbTruncate __P((Tcl_Interp *, int, Tcl_Obj * CONST*, DB *));
static int	tcl_DbCursor __P((Tcl_Interp *,
    int, Tcl_Obj * CONST*, DB *, DBC **));
static int	tcl_DbJoin __P((Tcl_Interp *,
    int, Tcl_Obj * CONST*, DB *, DBC **));
static int	tcl_DbGetjoin __P((Tcl_Interp *, int, Tcl_Obj * CONST*, DB *));
static int	tcl_DbCount __P((Tcl_Interp *, int, Tcl_Obj * CONST*, DB *));
static int	tcl_second_call __P((DB *, const DBT *, const DBT *, DBT *));

/*
 * _DbInfoDelete --
 *
 * PUBLIC: void _DbInfoDelete __P((Tcl_Interp *, DBTCL_INFO *));
 */
void
_DbInfoDelete(interp, dbip)
	Tcl_Interp *interp;
	DBTCL_INFO *dbip;
{
	DBTCL_INFO *nextp, *p;
	/*
	 * First we have to close any open cursors.  Then we close
	 * our db.
	 */
	for (p = LIST_FIRST(&__db_infohead); p != NULL; p = nextp) {
		nextp = LIST_NEXT(p, entries);
		/*
		 * Check if this is a cursor info structure and if
		 * it is, if it belongs to this DB.  If so, remove
		 * its commands and info structure.
		 */
		if (p->i_parent == dbip && p->i_type == I_DBC) {
			(void)Tcl_DeleteCommand(interp, p->i_name);
			_DeleteInfo(p);
		}
	}
	(void)Tcl_DeleteCommand(interp, dbip->i_name);
	_DeleteInfo(dbip);
}

/*
 *
 * PUBLIC: int db_Cmd __P((ClientData, Tcl_Interp *, int, Tcl_Obj * CONST*));
 *
 * db_Cmd --
 *	Implements the "db" widget.
 */
int
db_Cmd(clientData, interp, objc, objv)
	ClientData clientData;		/* DB handle */
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
{
	static char *dbcmds[] = {
#if CONFIG_TEST
		"keyrange",
		"pget",
		"rpcid",
		"test",
#endif
		"associate",
		"close",
		"count",
		"cursor",
		"del",
		"get",
		"get_join",
		"get_type",
		"is_byteswapped",
		"join",
		"put",
		"stat",
		"sync",
		"truncate",
		NULL
	};
	enum dbcmds {
#if CONFIG_TEST
		DBKEYRANGE,
		DBPGET,
		DBRPCID,
		DBTEST,
#endif
		DBASSOCIATE,
		DBCLOSE,
		DBCOUNT,
		DBCURSOR,
		DBDELETE,
		DBGET,
		DBGETJOIN,
		DBGETTYPE,
		DBSWAPPED,
		DBJOIN,
		DBPUT,
		DBSTAT,
		DBSYNC,
		DBTRUNCATE
	};
	DB *dbp;
	DBC *dbc;
	DBTCL_INFO *dbip;
	DBTCL_INFO *ip;
	DBTYPE type;
	Tcl_Obj *res;
	int cmdindex, isswapped, result, ret;
	char newname[MSG_SIZE];

	Tcl_ResetResult(interp);
	dbp = (DB *)clientData;
	dbip = _PtrToInfo((void *)dbp);
	memset(newname, 0, MSG_SIZE);
	result = TCL_OK;
	if (objc <= 1) {
		Tcl_WrongNumArgs(interp, 1, objv, "command cmdargs");
		return (TCL_ERROR);
	}
	if (dbp == NULL) {
		Tcl_SetResult(interp, "NULL db pointer", TCL_STATIC);
		return (TCL_ERROR);
	}
	if (dbip == NULL) {
		Tcl_SetResult(interp, "NULL db info pointer", TCL_STATIC);
		return (TCL_ERROR);
	}

	/*
	 * Get the command name index from the object based on the dbcmds
	 * defined above.
	 */
	if (Tcl_GetIndexFromObj(interp,
	    objv[1], dbcmds, "command", TCL_EXACT, &cmdindex) != TCL_OK)
		return (IS_HELP(objv[1]));

	res = NULL;
	switch ((enum dbcmds)cmdindex) {
#if CONFIG_TEST
	case DBKEYRANGE:
		result = tcl_DbKeyRange(interp, objc, objv, dbp);
		break;
	case DBPGET:
		result = tcl_DbGet(interp, objc, objv, dbp, 1);
		break;
	case DBRPCID:
		/*
		 * No args for this.  Error if there are some.
		 */
		if (objc > 2) {
			Tcl_WrongNumArgs(interp, 2, objv, NULL);
			return (TCL_ERROR);
		}
		/*
		 * !!! Retrieve the client ID from the dbp handle directly.
		 * This is for testing purposes only.  It is dbp-private data.
		 */
		res = Tcl_NewLongObj(dbp->cl_id);
		break;
	case DBTEST:
		result = tcl_EnvTest(interp, objc, objv, dbp->dbenv);
		break;
#endif
	case DBASSOCIATE:
		result = tcl_DbAssociate(interp, objc, objv, dbp);
		break;
	case DBCLOSE:
		result = tcl_DbClose(interp, objc, objv, dbp, dbip);
		break;
	case DBDELETE:
		result = tcl_DbDelete(interp, objc, objv, dbp);
		break;
	case DBGET:
		result = tcl_DbGet(interp, objc, objv, dbp, 0);
		break;
	case DBPUT:
		result = tcl_DbPut(interp, objc, objv, dbp);
		break;
	case DBCOUNT:
		result = tcl_DbCount(interp, objc, objv, dbp);
		break;
	case DBSWAPPED:
		/*
		 * No args for this.  Error if there are some.
		 */
		if (objc > 2) {
			Tcl_WrongNumArgs(interp, 2, objv, NULL);
			return (TCL_ERROR);
		}
		_debug_check();
		ret = dbp->get_byteswapped(dbp, &isswapped);
		res = Tcl_NewIntObj(isswapped);
		break;
	case DBGETTYPE:
		/*
		 * No args for this.  Error if there are some.
		 */
		if (objc > 2) {
			Tcl_WrongNumArgs(interp, 2, objv, NULL);
			return (TCL_ERROR);
		}
		_debug_check();
		ret = dbp->get_type(dbp, &type);
		if (type == DB_BTREE)
			res = Tcl_NewStringObj("btree", strlen("btree"));
		else if (type == DB_HASH)
			res = Tcl_NewStringObj("hash", strlen("hash"));
		else if (type == DB_RECNO)
			res = Tcl_NewStringObj("recno", strlen("recno"));
		else if (type == DB_QUEUE)
			res = Tcl_NewStringObj("queue", strlen("queue"));
		else {
			Tcl_SetResult(interp,
			    "db gettype: Returned unknown type\n", TCL_STATIC);
			result = TCL_ERROR;
		}
		break;
	case DBSTAT:
		result = tcl_DbStat(interp, objc, objv, dbp);
		break;
	case DBSYNC:
		/*
		 * No args for this.  Error if there are some.
		 */
		if (objc > 2) {
			Tcl_WrongNumArgs(interp, 2, objv, NULL);
			return (TCL_ERROR);
		}
		_debug_check();
		ret = dbp->sync(dbp, 0);
		res = Tcl_NewIntObj(ret);
		if (ret != 0) {
			Tcl_SetObjResult(interp, res);
			result = TCL_ERROR;
		}
		break;
	case DBCURSOR:
		snprintf(newname, sizeof(newname),
		    "%s.c%d", dbip->i_name, dbip->i_dbdbcid);
		ip = _NewInfo(interp, NULL, newname, I_DBC);
		if (ip != NULL) {
			result = tcl_DbCursor(interp, objc, objv, dbp, &dbc);
			if (result == TCL_OK) {
				dbip->i_dbdbcid++;
				ip->i_parent = dbip;
				Tcl_CreateObjCommand(interp, newname,
				    (Tcl_ObjCmdProc *)dbc_Cmd,
				    (ClientData)dbc, NULL);
				res =
				    Tcl_NewStringObj(newname, strlen(newname));
				_SetInfoData(ip, dbc);
			} else
				_DeleteInfo(ip);
		} else {
			Tcl_SetResult(interp,
			    "Could not set up info", TCL_STATIC);
			result = TCL_ERROR;
		}
		break;
	case DBJOIN:
		snprintf(newname, sizeof(newname),
		    "%s.c%d", dbip->i_name, dbip->i_dbdbcid);
		ip = _NewInfo(interp, NULL, newname, I_DBC);
		if (ip != NULL) {
			result = tcl_DbJoin(interp, objc, objv, dbp, &dbc);
			if (result == TCL_OK) {
				dbip->i_dbdbcid++;
				ip->i_parent = dbip;
				Tcl_CreateObjCommand(interp, newname,
				    (Tcl_ObjCmdProc *)dbc_Cmd,
				    (ClientData)dbc, NULL);
				res =
				    Tcl_NewStringObj(newname, strlen(newname));
				_SetInfoData(ip, dbc);
			} else
				_DeleteInfo(ip);
		} else {
			Tcl_SetResult(interp,
			    "Could not set up info", TCL_STATIC);
			result = TCL_ERROR;
		}
		break;
	case DBGETJOIN:
		result = tcl_DbGetjoin(interp, objc, objv, dbp);
		break;
	case DBTRUNCATE:
		result = tcl_DbTruncate(interp, objc, objv, dbp);
		break;
	}
	/*
	 * Only set result if we have a res.  Otherwise, lower
	 * functions have already done so.
	 */
	if (result == TCL_OK && res)
		Tcl_SetObjResult(interp, res);
	return (result);
}

/*
 * tcl_db_stat --
 */
static int
tcl_DbStat(interp, objc, objv, dbp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
{
	DB_BTREE_STAT *bsp;
	DB_HASH_STAT *hsp;
	DB_QUEUE_STAT *qsp;
	void *sp;
	Tcl_Obj *res, *flaglist, *myobjv[2];
	DBTYPE type;
	u_int32_t flag;
	int result, ret;
	char *arg;

	result = TCL_OK;
	flag = 0;

	if (objc > 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "?-faststat?");
		return (TCL_ERROR);
	}

	if (objc == 3) {
		arg = Tcl_GetStringFromObj(objv[2], NULL);
		if (strcmp(arg, "-faststat") == 0)
			flag = DB_FAST_STAT;
		else {
			Tcl_SetResult(interp,
			    "db stat: unknown arg", TCL_STATIC);
			return (TCL_ERROR);
		}
	}

	_debug_check();
	ret = dbp->stat(dbp, &sp, flag);
	result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret), "db stat");
	if (result == TCL_ERROR)
		return (result);

	(void)dbp->get_type(dbp, &type);
	/*
	 * Have our stats, now construct the name value
	 * list pairs and free up the memory.
	 */
	res = Tcl_NewObj();

	/*
	 * MAKE_STAT_LIST assumes 'res' and 'error' label.
	 */
	if (type == DB_HASH) {
		hsp = (DB_HASH_STAT *)sp;
		MAKE_STAT_LIST("Magic", hsp->hash_magic);
		MAKE_STAT_LIST("Version", hsp->hash_version);
		MAKE_STAT_LIST("Page size", hsp->hash_pagesize);
		MAKE_STAT_LIST("Number of keys", hsp->hash_nkeys);
		MAKE_STAT_LIST("Number of records", hsp->hash_ndata);
		MAKE_STAT_LIST("Fill factor", hsp->hash_ffactor);
		MAKE_STAT_LIST("Buckets", hsp->hash_buckets);
		if (flag != DB_FAST_STAT) {
			MAKE_STAT_LIST("Free pages", hsp->hash_free);
			MAKE_STAT_LIST("Bytes free", hsp->hash_bfree);
			MAKE_STAT_LIST("Number of big pages",
			    hsp->hash_bigpages);
			MAKE_STAT_LIST("Big pages bytes free",
			    hsp->hash_big_bfree);
			MAKE_STAT_LIST("Overflow pages", hsp->hash_overflows);
			MAKE_STAT_LIST("Overflow bytes free",
			    hsp->hash_ovfl_free);
			MAKE_STAT_LIST("Duplicate pages", hsp->hash_dup);
			MAKE_STAT_LIST("Duplicate pages bytes free",
			    hsp->hash_dup_free);
		}
	} else if (type == DB_QUEUE) {
		qsp = (DB_QUEUE_STAT *)sp;
		MAKE_STAT_LIST("Magic", qsp->qs_magic);
		MAKE_STAT_LIST("Version", qsp->qs_version);
		MAKE_STAT_LIST("Page size", qsp->qs_pagesize);
		MAKE_STAT_LIST("Extent size", qsp->qs_extentsize);
		MAKE_STAT_LIST("Number of records", qsp->qs_nkeys);
		MAKE_STAT_LIST("Record length", qsp->qs_re_len);
		MAKE_STAT_LIST("Record pad", qsp->qs_re_pad);
		MAKE_STAT_LIST("First record number", qsp->qs_first_recno);
		MAKE_STAT_LIST("Last record number", qsp->qs_cur_recno);
		if (flag != DB_FAST_STAT) {
			MAKE_STAT_LIST("Number of pages", qsp->qs_pages);
			MAKE_STAT_LIST("Bytes free", qsp->qs_pgfree);
		}
	} else {	/* BTREE and RECNO are same stats */
		bsp = (DB_BTREE_STAT *)sp;
		MAKE_STAT_LIST("Magic", bsp->bt_magic);
		MAKE_STAT_LIST("Version", bsp->bt_version);
		MAKE_STAT_LIST("Number of keys", bsp->bt_nkeys);
		MAKE_STAT_LIST("Number of records", bsp->bt_ndata);
		MAKE_STAT_LIST("Minimum keys per page", bsp->bt_minkey);
		MAKE_STAT_LIST("Fixed record length", bsp->bt_re_len);
		MAKE_STAT_LIST("Record pad", bsp->bt_re_pad);
		MAKE_STAT_LIST("Page size", bsp->bt_pagesize);
		if (flag != DB_FAST_STAT) {
			MAKE_STAT_LIST("Levels", bsp->bt_levels);
			MAKE_STAT_LIST("Internal pages", bsp->bt_int_pg);
			MAKE_STAT_LIST("Leaf pages", bsp->bt_leaf_pg);
			MAKE_STAT_LIST("Duplicate pages", bsp->bt_dup_pg);
			MAKE_STAT_LIST("Overflow pages", bsp->bt_over_pg);
			MAKE_STAT_LIST("Pages on freelist", bsp->bt_free);
			MAKE_STAT_LIST("Internal pages bytes free",
			    bsp->bt_int_pgfree);
			MAKE_STAT_LIST("Leaf pages bytes free",
			    bsp->bt_leaf_pgfree);
			MAKE_STAT_LIST("Duplicate pages bytes free",
			    bsp->bt_dup_pgfree);
			MAKE_STAT_LIST("Bytes free in overflow pages",
			    bsp->bt_over_pgfree);
		}
	}

	/*
	 * Construct a {name {flag1 flag2 ... flagN}} list for the
	 * dbp flags.  These aren't access-method dependent, but they
	 * include all the interesting flags, and the integer value
	 * isn't useful from Tcl--return the strings instead.
	 */
	myobjv[0] = Tcl_NewStringObj("Flags", strlen("Flags"));
	myobjv[1] = _GetFlagsList(interp, dbp->flags, __db_inmemdbflags);
	flaglist = Tcl_NewListObj(2, myobjv);
	if (flaglist == NULL) {
		result = TCL_ERROR;
		goto error;
	}
	if ((result =
	    Tcl_ListObjAppendElement(interp, res, flaglist)) != TCL_OK)
		goto error;

	Tcl_SetObjResult(interp, res);
error:
	free(sp);
	return (result);
}

/*
 * tcl_db_close --
 */
static int
tcl_DbClose(interp, objc, objv, dbp, dbip)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
	DBTCL_INFO *dbip;		/* Info pointer */
{
	static char *dbclose[] = {
		"-nosync", "--", NULL
	};
	enum dbclose {
		TCL_DBCLOSE_NOSYNC,
		TCL_DBCLOSE_ENDARG
	};
	u_int32_t flag;
	int endarg, i, optindex, result, ret;
	char *arg;

	result = TCL_OK;
	endarg = 0;
	flag = 0;
	if (objc > 4) {
		Tcl_WrongNumArgs(interp, 2, objv, "?-nosync?");
		return (TCL_ERROR);
	}

	i = 2;
	while (i < objc) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbclose,
		    "option", TCL_EXACT, &optindex) != TCL_OK) {
			arg = Tcl_GetStringFromObj(objv[i], NULL);
			if (arg[0] == '-')
				return (IS_HELP(objv[i]));
			else
				Tcl_ResetResult(interp);
			break;
		}
		i++;
		switch ((enum dbclose)optindex) {
		case TCL_DBCLOSE_NOSYNC:
			flag = DB_NOSYNC;
			break;
		case TCL_DBCLOSE_ENDARG:
			endarg = 1;
			break;
		}
		/*
		 * If, at any time, parsing the args we get an error,
		 * bail out and return.
		 */
		if (result != TCL_OK)
			return (result);
		if (endarg)
			break;
	}
	_DbInfoDelete(interp, dbip);
	_debug_check();

	/* Paranoia. */
	dbp->api_internal = NULL;

	ret = (dbp)->close(dbp, flag);
	result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret), "db close");
	return (result);
}

/*
 * tcl_db_put --
 */
static int
tcl_DbPut(interp, objc, objv, dbp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
{
	static char *dbputopts[] = {
#if CONFIG_TEST
		"-nodupdata",
#endif
		"-append",
		"-auto_commit",
		"-nooverwrite",
		"-partial",
		"-txn",
		NULL
	};
	enum dbputopts {
#if CONFIG_TEST
		DBGET_NODUPDATA,
#endif
		DBPUT_APPEND,
		DBPUT_AUTO_COMMIT,
		DBPUT_NOOVER,
		DBPUT_PART,
		DBPUT_TXN
	};
	static char *dbputapp[] = {
		"-append",	NULL
	};
	enum dbputapp { DBPUT_APPEND0 };
	DBT key, data;
	DBTYPE type;
	DB_TXN *txn;
	Tcl_Obj **elemv, *res;
	void *dtmp, *ktmp;
	db_recno_t recno;
	u_int32_t flag;
	int auto_commit, elemc, end, freekey, freedata;
	int i, optindex, result, ret;
	char *arg, msg[MSG_SIZE];

	txn = NULL;
	result = TCL_OK;
	flag = 0;
	if (objc <= 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "?-args? key data");
		return (TCL_ERROR);
	}

	freekey = freedata = 0;
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));

	/*
	 * If it is a QUEUE or RECNO database, the key is a record number
	 * and must be setup up to contain a db_recno_t.  Otherwise the
	 * key is a "string".
	 */
	(void)dbp->get_type(dbp, &type);

	/*
	 * We need to determine where the end of required args are.  If we
	 * are using a QUEUE/RECNO db and -append, then there is just one
	 * req arg (data).  Otherwise there are two (key data).
	 *
	 * We preparse the list to determine this since we need to know
	 * to properly check # of args for other options below.
	 */
	end = objc - 2;
	if (type == DB_QUEUE || type == DB_RECNO) {
		i = 2;
		while (i < objc - 1) {
			if (Tcl_GetIndexFromObj(interp, objv[i++], dbputapp,
			    "option", TCL_EXACT, &optindex) != TCL_OK)
				continue;
			switch ((enum dbputapp)optindex) {
			case DBPUT_APPEND0:
				end = objc - 1;
				break;
			}
		}
	}
	Tcl_ResetResult(interp);

	/*
	 * Get the command name index from the object based on the options
	 * defined above.
	 */
	i = 2;
	auto_commit = 0;
	while (i < end) {
		if (Tcl_GetIndexFromObj(interp, objv[i],
		    dbputopts, "option", TCL_EXACT, &optindex) != TCL_OK)
			return (IS_HELP(objv[i]));
		i++;
		switch ((enum dbputopts)optindex) {
#if CONFIG_TEST
		case DBGET_NODUPDATA:
			FLAG_CHECK(flag);
			flag = DB_NODUPDATA;
			break;
#endif
		case DBPUT_TXN:
			if (i > (end - 1)) {
				Tcl_WrongNumArgs(interp, 2, objv, "?-txn id?");
				result = TCL_ERROR;
				break;
			}
			arg = Tcl_GetStringFromObj(objv[i++], NULL);
			txn = NAME_TO_TXN(arg);
			if (txn == NULL) {
				snprintf(msg, MSG_SIZE,
				    "Put: Invalid txn: %s\n", arg);
				Tcl_SetResult(interp, msg, TCL_VOLATILE);
				result = TCL_ERROR;
			}
			break;
		case DBPUT_AUTO_COMMIT:
			auto_commit = 1;
			break;
		case DBPUT_APPEND:
			FLAG_CHECK(flag);
			flag = DB_APPEND;
			break;
		case DBPUT_NOOVER:
			FLAG_CHECK(flag);
			flag = DB_NOOVERWRITE;
			break;
		case DBPUT_PART:
			if (i > (end - 1)) {
				Tcl_WrongNumArgs(interp, 2, objv,
				    "?-partial {offset length}?");
				result = TCL_ERROR;
				break;
			}
			/*
			 * Get sublist as {offset length}
			 */
			result = Tcl_ListObjGetElements(interp, objv[i++],
			    &elemc, &elemv);
			if (elemc != 2) {
				Tcl_SetResult(interp,
				    "List must be {offset length}", TCL_STATIC);
				result = TCL_ERROR;
				break;
			}
			data.flags = DB_DBT_PARTIAL;
			result = _GetUInt32(interp, elemv[0], &data.doff);
			if (result != TCL_OK)
				break;
			result = _GetUInt32(interp, elemv[1], &data.dlen);
			/*
			 * NOTE: We don't check result here because all we'd
			 * do is break anyway, and we are doing that.  If you
			 * add code here, you WILL need to add the check
			 * for result.  (See the check for save.doff, a few
			 * lines above and copy that.)
			 */
			break;
		}
		if (result != TCL_OK)
			break;
	}
	if (auto_commit)
		flag |= DB_AUTO_COMMIT;

	if (result == TCL_ERROR)
		return (result);

	/*
	 * If we are a recno db and we are NOT using append, then the 2nd
	 * last arg is the key.
	 */
	if (type == DB_QUEUE || type == DB_RECNO) {
		key.data = &recno;
		key.ulen = key.size = sizeof(db_recno_t);
		key.flags = DB_DBT_USERMEM;
		if (flag == DB_APPEND)
			recno = 0;
		else {
			result = _GetUInt32(interp, objv[objc-2], &recno);
			if (result != TCL_OK)
				return (result);
		}
	} else {
		ret = _CopyObjBytes(interp, objv[objc-2], &ktmp,
		    &key.size, &freekey);
		if (ret != 0) {
			result = _ReturnSetup(interp, ret,
			    DB_RETOK_DBPUT(ret), "db put");
			return (result);
		}
		key.data = ktmp;
	}
	ret = _CopyObjBytes(interp, objv[objc-1], &dtmp,
	    &data.size, &freedata);
	if (ret != 0) {
		result = _ReturnSetup(interp, ret,
		    DB_RETOK_DBPUT(ret), "db put");
		goto out;
	}
	data.data = dtmp;
	_debug_check();
	ret = dbp->put(dbp, txn, &key, &data, flag);
	result = _ReturnSetup(interp, ret, DB_RETOK_DBPUT(ret), "db put");
	if (ret == 0 &&
	    (type == DB_RECNO || type == DB_QUEUE) && flag == DB_APPEND) {
		res = Tcl_NewLongObj((long)recno);
		Tcl_SetObjResult(interp, res);
	}
out:
	if (freedata)
		(void)__os_free(dbp->dbenv, dtmp);
	if (freekey)
		(void)__os_free(dbp->dbenv, ktmp);
	return (result);
}

/*
 * tcl_db_get --
 */
static int
tcl_DbGet(interp, objc, objv, dbp, ispget)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
	int ispget;			/* 1 for pget, 0 for get */
{
	static char *dbgetopts[] = {
#if CONFIG_TEST
		"-dirty",
		"-multi",
#endif
		"-consume",
		"-consume_wait",
		"-get_both",
		"-glob",
		"-partial",
		"-recno",
		"-rmw",
		"-txn",
		"--",
		NULL
	};
	enum dbgetopts {
#if CONFIG_TEST
		DBGET_DIRTY,
		DBGET_MULTI,
#endif
		DBGET_CONSUME,
		DBGET_CONSUME_WAIT,
		DBGET_BOTH,
		DBGET_GLOB,
		DBGET_PART,
		DBGET_RECNO,
		DBGET_RMW,
		DBGET_TXN,
		DBGET_ENDARG
	};
	DBC *dbc;
	DBT key, pkey, data, save;
	DBTYPE type;
	DB_TXN *txn;
	Tcl_Obj **elemv, *retlist;
	void *dtmp, *ktmp;
	u_int32_t flag, cflag, isdup, mflag, rmw;
	int bufsize, elemc, end, endarg, freekey, freedata, i;
	int optindex, result, ret, useglob, useprecno, userecno;
	char *arg, *pattern, *prefix, msg[MSG_SIZE];
	db_recno_t precno, recno;

	result = TCL_OK;
	freekey = freedata = 0;
	cflag = endarg = flag = mflag = rmw = 0;
	useglob = userecno = useprecno = 0;
	txn = NULL;
	pattern = prefix = NULL;

	if (objc < 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "?-args? key");
		return (TCL_ERROR);
	}

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	memset(&save, 0, sizeof(save));

	/* For the primary key in a pget call. */
	memset(&pkey, 0, sizeof(pkey));

	/*
	 * Get the command name index from the object based on the options
	 * defined above.
	 */
	i = 2;
	(void)dbp->get_type(dbp, &type);
	end = objc;
	while (i < end) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbgetopts, "option",
		    TCL_EXACT, &optindex) != TCL_OK) {
			arg = Tcl_GetStringFromObj(objv[i], NULL);
			if (arg[0] == '-') {
				result = IS_HELP(objv[i]);
				goto out;
			} else
				Tcl_ResetResult(interp);
			break;
		}
		i++;
		switch ((enum dbgetopts)optindex) {
#if CONFIG_TEST
		case DBGET_DIRTY:
			rmw |= DB_DIRTY_READ;
			break;
		case DBGET_MULTI:
			mflag |= DB_MULTIPLE;
			result = Tcl_GetIntFromObj(interp, objv[i], &bufsize);
			if (result != TCL_OK)
				goto out;
			i++;
			break;
#endif
		case DBGET_BOTH:
			/*
			 * Change 'end' and make sure we aren't already past
			 * the new end.
			 */
			if (i > objc - 2) {
				Tcl_WrongNumArgs(interp, 2, objv,
				    "?-get_both key data?");
				result = TCL_ERROR;
				break;
			}
			end = objc - 2;
			FLAG_CHECK(flag);
			flag = DB_GET_BOTH;
			break;
		case DBGET_TXN:
			if (i >= end) {
				Tcl_WrongNumArgs(interp, 2, objv, "?-txn id?");
				result = TCL_ERROR;
				break;
			}
			arg = Tcl_GetStringFromObj(objv[i++], NULL);
			txn = NAME_TO_TXN(arg);
			if (txn == NULL) {
				snprintf(msg, MSG_SIZE,
				    "Get: Invalid txn: %s\n", arg);
				Tcl_SetResult(interp, msg, TCL_VOLATILE);
				result = TCL_ERROR;
			}
			break;
		case DBGET_GLOB:
			useglob = 1;
			end = objc - 1;
			break;
		case DBGET_CONSUME:
			FLAG_CHECK(flag);
			flag = DB_CONSUME;
			break;
		case DBGET_CONSUME_WAIT:
			FLAG_CHECK(flag);
			flag = DB_CONSUME_WAIT;
			break;
		case DBGET_RECNO:
			end = objc - 1;
			userecno = 1;
			if (type != DB_RECNO && type != DB_QUEUE) {
				FLAG_CHECK(flag);
				flag = DB_SET_RECNO;
			}
			break;
		case DBGET_RMW:
			rmw |= DB_RMW;
			break;
		case DBGET_PART:
			end = objc - 1;
			if (i == end) {
				Tcl_WrongNumArgs(interp, 2, objv,
				    "?-partial {offset length}?");
				result = TCL_ERROR;
				break;
			}
			/*
			 * Get sublist as {offset length}
			 */
			result = Tcl_ListObjGetElements(interp, objv[i++],
			    &elemc, &elemv);
			if (elemc != 2) {
				Tcl_SetResult(interp,
				    "List must be {offset length}", TCL_STATIC);
				result = TCL_ERROR;
				break;
			}
			save.flags = DB_DBT_PARTIAL;
			result = _GetUInt32(interp, elemv[0], &save.doff);
			if (result != TCL_OK)
				break;
			result = _GetUInt32(interp, elemv[1], &save.dlen);
			/*
			 * NOTE: We don't check result here because all we'd
			 * do is break anyway, and we are doing that.  If you
			 * add code here, you WILL need to add the check
			 * for result.  (See the check for save.doff, a few
			 * lines above and copy that.)
			 */
			break;
		case DBGET_ENDARG:
			endarg = 1;
			break;
		} /* switch */
		if (result != TCL_OK)
			break;
		if (endarg)
			break;
	}
	if (result != TCL_OK)
		goto out;

	if (type == DB_RECNO || type == DB_QUEUE)
		userecno = 1;

	/*
	 * Check args we have left versus the flags we were given.
	 * We might have 0, 1 or 2 left.  If we have 0, it must
	 * be DB_CONSUME*, if 2, then DB_GET_BOTH, all others should
	 * be 1.
	 */
	if (((flag == DB_CONSUME || flag == DB_CONSUME_WAIT) && i != objc) ||
	    (flag == DB_GET_BOTH && i != objc - 2)) {
		Tcl_SetResult(interp,
		    "Wrong number of key/data given based on flags specified\n",
		    TCL_STATIC);
		result = TCL_ERROR;
		goto out;
	} else if (flag == 0 && i != objc - 1) {
		Tcl_SetResult(interp,
		    "Wrong number of key/data given\n", TCL_STATIC);
		result = TCL_ERROR;
		goto out;
	}

	/*
	 * XXX
	 * We technically shouldn't be looking inside the dbp like this,
	 * but this is the only way to figure out whether the primary
	 * key should also be a recno.
	 */
	if (ispget) {
		if (dbp->s_primary != NULL &&
		    (dbp->s_primary->type == DB_RECNO ||
		    dbp->s_primary->type == DB_QUEUE))
			useprecno = 1;
	}

	/*
	 * Check for illegal combos of options.
	 */
	if (useglob && (userecno || flag == DB_SET_RECNO ||
	    type == DB_RECNO || type == DB_QUEUE)) {
		Tcl_SetResult(interp,
		    "Cannot use -glob and record numbers.\n",
		    TCL_STATIC);
		result = TCL_ERROR;
		goto out;
	}
	if (useglob && flag == DB_GET_BOTH) {
		Tcl_SetResult(interp,
		    "Only one of -glob or -get_both can be specified.\n",
		    TCL_STATIC);
		result = TCL_ERROR;
		goto out;
	}

	if (useglob)
		pattern = Tcl_GetStringFromObj(objv[objc - 1], NULL);

	/*
	 * This is the list we return
	 */
	retlist = Tcl_NewListObj(0, NULL);
	save.flags |= DB_DBT_MALLOC;

	/*
	 * isdup is used to know if we support duplicates.  If not, we
	 * can just do a db->get call and avoid using cursors.
	 * XXX
	 * When there is a db->get_flags method, it should be used.
	 * isdup = dbp->get_flags(dbp) & DB_DUP;
	 * For now we illegally peek.
	 * XXX
	 */
	isdup = dbp->flags & DB_AM_DUP;

	/*
	 * If the database doesn't support duplicates or we're performing
	 * ops that don't require returning multiple items, use DB->get
	 * instead of a cursor operation.
	 */
	if (pattern == NULL && (isdup == 0 || mflag != 0 ||
	    flag == DB_SET_RECNO || flag == DB_GET_BOTH ||
	    flag == DB_CONSUME || flag == DB_CONSUME_WAIT)) {
		if (flag == DB_GET_BOTH) {
			if (userecno) {
				result = _GetUInt32(interp,
				    objv[(objc - 2)], &recno);
				if (result == TCL_OK) {
					key.data = &recno;
					key.size = sizeof(db_recno_t);
				} else
					goto out;
			} else {
				/*
				 * Some get calls (SET_*) can change the
				 * key pointers.  So, we need to store
				 * the allocated key space in a tmp.
				 */
				ret = _CopyObjBytes(interp, objv[objc-2],
				    &ktmp, &key.size, &freekey);
				if (ret != 0) {
					result = _ReturnSetup(interp, ret,
					    DB_RETOK_DBGET(ret), "db get");
					goto out;
				}
				key.data = ktmp;
			}
			/*
			 * Already checked args above.  Fill in key and save.
			 * Save is used in the dbp->get call below to fill in
			 * data.
			 *
			 * If the "data" here is really a primary key--that
			 * is, if we're in a pget--and that primary key
			 * is a recno, treat it appropriately as an int.
			 */
			if (useprecno) {
				result = _GetUInt32(interp,
				    objv[objc - 1], &precno);
				if (result == TCL_OK) {
					save.data = &precno;
					save.size = sizeof(db_recno_t);
				} else
					goto out;
			} else {
				ret = _CopyObjBytes(interp, objv[objc-1],
				    &dtmp, &save.size, &freedata);
				if (ret != 0) {
					result = _ReturnSetup(interp, ret,
					    DB_RETOK_DBGET(ret), "db get");
					goto out;
				}
				save.data = dtmp;
			}
		} else if (flag != DB_CONSUME && flag != DB_CONSUME_WAIT) {
			if (userecno) {
				result = _GetUInt32(
				    interp, objv[(objc - 1)], &recno);
				if (result == TCL_OK) {
					key.data = &recno;
					key.size = sizeof(db_recno_t);
				} else
					goto out;
			} else {
				/*
				 * Some get calls (SET_*) can change the
				 * key pointers.  So, we need to store
				 * the allocated key space in a tmp.
				 */
				ret = _CopyObjBytes(interp, objv[objc-1],
				    &ktmp, &key.size, &freekey);
				if (ret != 0) {
					result = _ReturnSetup(interp, ret,
					    DB_RETOK_DBGET(ret), "db get");
					goto out;
				}
				key.data = ktmp;
			}
			if (mflag & DB_MULTIPLE) {
				if ((ret = __os_malloc(dbp->dbenv,
				    bufsize, &save.data)) != 0) {
					Tcl_SetResult(interp,
					    db_strerror(ret), TCL_STATIC);
					goto out;
				}
				save.ulen = bufsize;
				F_CLR(&save, DB_DBT_MALLOC);
				F_SET(&save, DB_DBT_USERMEM);
			}
		}

		data = save;

		if (ispget) {
			if (flag == DB_GET_BOTH) {
				pkey.data = save.data;
				pkey.size = save.size;
				data.data = NULL;
				data.size = 0;
			}
			F_SET(&pkey, DB_DBT_MALLOC);
			_debug_check();
			ret = dbp->pget(dbp,
			    txn, &key, &pkey, &data, flag | rmw);
		} else {
			_debug_check();
			ret = dbp->get(dbp,
			    txn, &key, &data, flag | rmw | mflag);
		}
		result = _ReturnSetup(interp, ret, DB_RETOK_DBGET(ret),
		    "db get");
		if (ret == 0) {
			/*
			 * Success.  Return a list of the form {name value}
			 * If it was a recno in key.data, we need to convert
			 * into a string/object representation of that recno.
			 */
			if (mflag & DB_MULTIPLE)
				result = _SetMultiList(interp,
				    retlist, &key, &data, type, flag);
			else if (type == DB_RECNO || type == DB_QUEUE)
				if (ispget)
					result = _Set3DBTList(interp,
					    retlist, &key, 1, &pkey,
					    useprecno, &data);
				else
					result = _SetListRecnoElem(interp,
					    retlist, *(db_recno_t *)key.data,
					    data.data, data.size);
			else {
				if (ispget)
					result = _Set3DBTList(interp,
					    retlist, &key, 0, &pkey,
					    useprecno, &data);
				else
					result = _SetListElem(interp, retlist,
					    key.data, key.size,
					    data.data, data.size);
			}
		}
		/*
		 * Free space from DBT.
		 *
		 * If we set DB_DBT_MALLOC, we need to free the space if
		 * and only if we succeeded (and thus if DB allocated
		 * anything).  If DB_DBT_MALLOC is not set, this is a bulk
		 * get buffer, and needs to be freed no matter what.
		 */
		if (F_ISSET(&data, DB_DBT_MALLOC) && ret == 0)
			__os_ufree(dbp->dbenv, data.data);
		else if (!F_ISSET(&data, DB_DBT_MALLOC))
			__os_free(dbp->dbenv, data.data);
		if (ispget && ret == 0)
			__os_ufree(dbp->dbenv, pkey.data);
		if (result == TCL_OK)
			Tcl_SetObjResult(interp, retlist);
		goto out;
	}

	if (userecno) {
		result = _GetUInt32(interp, objv[(objc - 1)], &recno);
		if (result == TCL_OK) {
			key.data = &recno;
			key.size = sizeof(db_recno_t);
		} else
			goto out;
	} else {
		/*
		 * Some get calls (SET_*) can change the
		 * key pointers.  So, we need to store
		 * the allocated key space in a tmp.
		 */
		ret = _CopyObjBytes(interp, objv[objc-1], &ktmp,
		    &key.size, &freekey);
		if (ret != 0) {
			result = _ReturnSetup(interp, ret,
			    DB_RETOK_DBGET(ret), "db get");
			return (result);
		}
		key.data = ktmp;
	}
	ret = dbp->cursor(dbp, txn, &dbc, 0);
	result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret), "db cursor");
	if (result == TCL_ERROR)
		goto out;

	/*
	 * At this point, we have a cursor, if we have a pattern,
	 * we go to the nearest one and step forward until we don't
	 * have any more that match the pattern prefix.  If we have
	 * an exact key, we go to that key position, and step through
	 * all the duplicates.  In either case we build up a list of
	 * the form {{key data} {key data}...} along the way.
	 */
	memset(&data, 0, sizeof(data));
	/*
	 * Restore any "partial" info we have saved.
	 */
	data = save;
	if (pattern) {
		/*
		 * Note, prefix is returned in new space.  Must free it.
		 */
		ret = _GetGlobPrefix(pattern, &prefix);
		if (ret) {
			result = TCL_ERROR;
			Tcl_SetResult(interp,
			    "Unable to allocate pattern space", TCL_STATIC);
			goto out1;
		}
		key.data = prefix;
		key.size = strlen(prefix);
		/*
		 * If they give us an empty pattern string
		 * (i.e. -glob *), go through entire DB.
		 */
		if (strlen(prefix) == 0)
			cflag = DB_FIRST;
		else
			cflag = DB_SET_RANGE;
	} else
		cflag = DB_SET;
	if (ispget) {
		_debug_check();
		F_SET(&pkey, DB_DBT_MALLOC);
		ret = dbc->c_pget(dbc, &key, &pkey, &data, cflag | rmw);
	} else {
		_debug_check();
		ret = dbc->c_get(dbc, &key, &data, cflag | rmw);
	}
	result = _ReturnSetup(interp, ret, DB_RETOK_DBCGET(ret),
	    "db get (cursor)");
	if (result == TCL_ERROR)
		goto out1;
	if (ret == 0 && pattern &&
	    memcmp(key.data, prefix, strlen(prefix)) != 0) {
		/*
		 * Free space from DB_DBT_MALLOC
		 */
		free(data.data);
		goto out1;
	}
	if (pattern)
		cflag = DB_NEXT;
	 else
		cflag = DB_NEXT_DUP;

	while (ret == 0 && result == TCL_OK) {
		/*
		 * Build up our {name value} sublist
		 */
		if (ispget)
			result = _Set3DBTList(interp, retlist, &key, 0,
			    &pkey, useprecno, &data);
		else
			result = _SetListElem(interp, retlist,
			    key.data, key.size, data.data, data.size);
		/*
		 * Free space from DB_DBT_MALLOC
		 */
		if (ispget)
			free(pkey.data);
		free(data.data);
		if (result != TCL_OK)
			break;
		/*
		 * Append {name value} to return list
		 */
		memset(&key, 0, sizeof(key));
		memset(&pkey, 0, sizeof(pkey));
		memset(&data, 0, sizeof(data));
		/*
		 * Restore any "partial" info we have saved.
		 */
		data = save;
		if (ispget) {
			F_SET(&pkey, DB_DBT_MALLOC);
			ret = dbc->c_pget(dbc, &key, &pkey, &data, cflag | rmw);
		} else
			ret = dbc->c_get(dbc, &key, &data, cflag | rmw);
		if (ret == 0 && pattern &&
		    memcmp(key.data, prefix, strlen(prefix)) != 0) {
			/*
			 * Free space from DB_DBT_MALLOC
			 */
			free(data.data);
			break;
		}
	}
out1:
	dbc->c_close(dbc);
	if (result == TCL_OK)
		Tcl_SetObjResult(interp, retlist);
out:
	/*
	 * _GetGlobPrefix(), the function which allocates prefix, works
	 * by copying and condensing another string.  Thus prefix may
	 * have multiple nuls at the end, so we free using __os_free().
	 */
	if (prefix != NULL)
		__os_free(dbp->dbenv, prefix);
	if (freedata)
		(void)__os_free(dbp->dbenv, dtmp);
	if (freekey)
		(void)__os_free(dbp->dbenv, ktmp);
	return (result);
}

/*
 * tcl_db_delete --
 */
static int
tcl_DbDelete(interp, objc, objv, dbp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
{
	static char *dbdelopts[] = {
		"-auto_commit",
		"-glob",
		"-txn",
		NULL
	};
	enum dbdelopts {
		DBDEL_AUTO_COMMIT,
		DBDEL_GLOB,
		DBDEL_TXN
	};
	DBC *dbc;
	DBT key, data;
	DBTYPE type;
	DB_TXN *txn;
	void *ktmp;
	db_recno_t recno;
	int freekey, i, optindex, result, ret;
	u_int32_t flag;
	char *arg, *pattern, *prefix, msg[MSG_SIZE];

	result = TCL_OK;
	freekey = 0;
	flag = 0;
	pattern = prefix = NULL;
	txn = NULL;
	if (objc < 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "?-args? key");
		return (TCL_ERROR);
	}

	memset(&key, 0, sizeof(key));
	/*
	 * The first arg must be -auto_commit, -glob, -txn or a list of keys.
	 */
	i = 2;
	while (i < objc) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbdelopts, "option",
		    TCL_EXACT, &optindex) != TCL_OK) {
			/*
			 * If we don't have a -auto_commit, -glob or -txn,
			 * then the remaining args must be exact keys.
			 * Reset the result so we don't get an errant error
			 * message if there is another error.
			 */
			if (IS_HELP(objv[i]) == TCL_OK)
				return (TCL_OK);
			Tcl_ResetResult(interp);
			break;
		}
		i++;
		switch ((enum dbdelopts)optindex) {
		case DBDEL_TXN:
			if (i == objc) {
				/*
				 * Someone could conceivably have a key of
				 * the same name.  So just break and use it.
				 */
				i--;
				break;
			}
			arg = Tcl_GetStringFromObj(objv[i++], NULL);
			txn = NAME_TO_TXN(arg);
			if (txn == NULL) {
				snprintf(msg, MSG_SIZE,
				    "Delete: Invalid txn: %s\n", arg);
				Tcl_SetResult(interp, msg, TCL_VOLATILE);
				result = TCL_ERROR;
			}
			break;
		case DBDEL_AUTO_COMMIT:
			flag |= DB_AUTO_COMMIT;
			break;
		case DBDEL_GLOB:
			/*
			 * Get the pattern.  Get the prefix and use cursors to
			 * get all the data items.
			 */
			if (i == objc) {
				/*
				 * Someone could conceivably have a key of
				 * the same name.  So just break and use it.
				 */
				i--;
				break;
			}
			pattern = Tcl_GetStringFromObj(objv[i++], NULL);
			break;
		}
		if (result != TCL_OK)
			break;
	}

	if (result != TCL_OK)
		goto out;
	/*
	 * XXX
	 * For consistency with get, we have decided for the moment, to
	 * allow -glob, or one key, not many.  The code was originally
	 * written to take many keys and we'll leave it that way, because
	 * tcl_DbGet may one day accept many disjoint keys to get, rather
	 * than one, and at that time we'd make delete be consistent.  In
	 * any case, the code is already here and there is no need to remove,
	 * just check that we only have one arg left.
	 *
	 * If we have a pattern AND more keys to process, there is an error.
	 * Either we have some number of exact keys, or we have a pattern.
	 *
	 * If we have a pattern and an auto commit flag, there is an error.
	 */
	if (pattern == NULL) {
		if (i != (objc - 1)) {
			Tcl_WrongNumArgs(
			    interp, 2, objv, "?args? -glob pattern | key");
			result = TCL_ERROR;
			goto out;
		}
	} else {
		if (i != objc) {
			Tcl_WrongNumArgs(
			    interp, 2, objv, "?args? -glob pattern | key");
			result = TCL_ERROR;
			goto out;
		}
		if (flag & DB_AUTO_COMMIT) {
			Tcl_SetResult(interp,
			    "Cannot use -auto_commit and patterns.\n",
			    TCL_STATIC);
			result = TCL_ERROR;
			goto out;
		}
	}

	/*
	 * If we have remaining args, they are all exact keys.  Call
	 * DB->del on each of those keys.
	 *
	 * If it is a RECNO database, the key is a record number and must be
	 * setup up to contain a db_recno_t.  Otherwise the key is a "string".
	 */
	(void)dbp->get_type(dbp, &type);
	ret = 0;
	while (i < objc && ret == 0) {
		memset(&key, 0, sizeof(key));
		if (type == DB_RECNO || type == DB_QUEUE) {
			result = _GetUInt32(interp, objv[i++], &recno);
			if (result == TCL_OK) {
				key.data = &recno;
				key.size = sizeof(db_recno_t);
			} else
				return (result);
		} else {
			ret = _CopyObjBytes(interp, objv[i++], &ktmp,
			    &key.size, &freekey);
			if (ret != 0) {
				result = _ReturnSetup(interp, ret,
				    DB_RETOK_DBDEL(ret), "db del");
				return (result);
			}
			key.data = ktmp;
		}
		_debug_check();
		ret = dbp->del(dbp, txn, &key, flag);
		/*
		 * If we have any error, set up return result and stop
		 * processing keys.
		 */
		if (freekey)
			(void)__os_free(dbp->dbenv, ktmp);
		if (ret != 0)
			break;
	}
	result = _ReturnSetup(interp, ret, DB_RETOK_DBDEL(ret), "db del");

	/*
	 * At this point we've either finished or, if we have a pattern,
	 * we go to the nearest one and step forward until we don't
	 * have any more that match the pattern prefix.
	 */
	if (pattern) {
		ret = dbp->cursor(dbp, txn, &dbc, 0);
		if (ret != 0) {
			result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret),
			    "db cursor");
			goto out;
		}
		/*
		 * Note, prefix is returned in new space.  Must free it.
		 */
		memset(&key, 0, sizeof(key));
		memset(&data, 0, sizeof(data));
		ret = _GetGlobPrefix(pattern, &prefix);
		if (ret) {
			result = TCL_ERROR;
			Tcl_SetResult(interp,
			    "Unable to allocate pattern space", TCL_STATIC);
			goto out;
		}
		key.data = prefix;
		key.size = strlen(prefix);
		if (strlen(prefix) == 0)
			flag = DB_FIRST;
		else
			flag = DB_SET_RANGE;
		ret = dbc->c_get(dbc, &key, &data, flag);
		while (ret == 0 &&
		    memcmp(key.data, prefix, strlen(prefix)) == 0) {
			/*
			 * Each time through here the cursor is pointing
			 * at the current valid item.  Delete it and
			 * move ahead.
			 */
			_debug_check();
			ret = dbc->c_del(dbc, 0);
			if (ret != 0) {
				result = _ReturnSetup(interp, ret,
				    DB_RETOK_DBCDEL(ret), "db c_del");
				break;
			}
			/*
			 * Deleted the current, now move to the next item
			 * in the list, check if it matches the prefix pattern.
			 */
			memset(&key, 0, sizeof(key));
			memset(&data, 0, sizeof(data));
			ret = dbc->c_get(dbc, &key, &data, DB_NEXT);
		}
		if (ret == DB_NOTFOUND)
			ret = 0;
		/*
		 * _GetGlobPrefix(), the function which allocates prefix, works
		 * by copying and condensing another string.  Thus prefix may
		 * have multiple nuls at the end, so we free using __os_free().
		 */
		__os_free(dbp->dbenv, prefix);
		dbc->c_close(dbc);
		result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret), "db del");
	}
out:
	return (result);
}

/*
 * tcl_db_cursor --
 */
static int
tcl_DbCursor(interp, objc, objv, dbp, dbcp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
	DBC **dbcp;			/* Return cursor pointer */
{
	static char *dbcuropts[] = {
#if CONFIG_TEST
		"-dirty",
		"-update",
#endif
		"-txn",
		NULL
	};
	enum dbcuropts {
#if CONFIG_TEST
		DBCUR_DIRTY,
		DBCUR_UPDATE,
#endif
		DBCUR_TXN
	};
	DB_TXN *txn;
	u_int32_t flag;
	int i, optindex, result, ret;
	char *arg, msg[MSG_SIZE];

	result = TCL_OK;
	flag = 0;
	txn = NULL;
	i = 2;
	while (i < objc) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbcuropts, "option",
		    TCL_EXACT, &optindex) != TCL_OK) {
			result = IS_HELP(objv[i]);
			goto out;
		}
		i++;
		switch ((enum dbcuropts)optindex) {
#if CONFIG_TEST
		case DBCUR_DIRTY:
			flag |= DB_DIRTY_READ;
			break;
		case DBCUR_UPDATE:
			flag |= DB_WRITECURSOR;
			break;
#endif
		case DBCUR_TXN:
			if (i == objc) {
				Tcl_WrongNumArgs(interp, 2, objv, "?-txn id?");
				result = TCL_ERROR;
				break;
			}
			arg = Tcl_GetStringFromObj(objv[i++], NULL);
			txn = NAME_TO_TXN(arg);
			if (txn == NULL) {
				snprintf(msg, MSG_SIZE,
				    "Cursor: Invalid txn: %s\n", arg);
				Tcl_SetResult(interp, msg, TCL_VOLATILE);
				result = TCL_ERROR;
			}
			break;
		}
		if (result != TCL_OK)
			break;
	}
	if (result != TCL_OK)
		goto out;

	_debug_check();
	ret = dbp->cursor(dbp, txn, dbcp, flag);
	if (ret != 0)
		result = _ErrorSetup(interp, ret, "db cursor");
out:
	return (result);
}

/*
 * tcl_DbAssociate --
 *	Call DB->associate().
 */
static int
tcl_DbAssociate(interp, objc, objv, dbp)
	Tcl_Interp *interp;
	int objc;
	Tcl_Obj *CONST objv[];
	DB *dbp;
{
	static char *dbaopts[] = {
		"-auto_commit",
		"-create",
		"-txn",
		NULL
	};
	enum dbaopts {
		DBA_AUTO_COMMIT,
		DBA_CREATE,
		DBA_TXN
	};
	DB *sdbp;
	DB_TXN *txn;
	DBTCL_INFO *sdbip;
	int i, optindex, result, ret;
	char *arg, msg[MSG_SIZE];
	u_int32_t flag;

	txn = NULL;
	result = TCL_OK;
	flag = 0;
	if (objc < 2) {
		Tcl_WrongNumArgs(interp, 2, objv, "[callback] secondary");
		return (TCL_ERROR);
	}

	i = 2;
	while (i < objc) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbaopts, "option",
		    TCL_EXACT, &optindex) != TCL_OK) {
			result = IS_HELP(objv[i]);
			if (result == TCL_OK)
				return (result);
			result = TCL_OK;
			Tcl_ResetResult(interp);
			break;
		}
		i++;
		switch ((enum dbaopts)optindex) {
		case DBA_AUTO_COMMIT:
			flag |= DB_AUTO_COMMIT;
			break;
		case DBA_CREATE:
			flag |= DB_CREATE;
			break;
		case DBA_TXN:
			if (i > (objc - 1)) {
				Tcl_WrongNumArgs(interp, 2, objv, "?-txn id?");
				result = TCL_ERROR;
				break;
			}
			arg = Tcl_GetStringFromObj(objv[i++], NULL);
			txn = NAME_TO_TXN(arg);
			if (txn == NULL) {
				snprintf(msg, MSG_SIZE,
				    "Associate: Invalid txn: %s\n", arg);
				Tcl_SetResult(interp, msg, TCL_VOLATILE);
				result = TCL_ERROR;
			}
			break;
		}
	}
	if (result != TCL_OK)
		return (result);

	/*
	 * Better be 1 or 2 args left.  The last arg must be the sdb
	 * handle.  If 2 args then objc-2 is the callback proc, else
	 * we have a NULL callback.
	 */
	/* Get the secondary DB handle. */
	arg = Tcl_GetStringFromObj(objv[objc - 1], NULL);
	sdbp = NAME_TO_DB(arg);
	if (sdbp == NULL) {
		snprintf(msg, MSG_SIZE,
		    "Associate: Invalid database handle: %s\n", arg);
		Tcl_SetResult(interp, msg, TCL_VOLATILE);
		return (TCL_ERROR);
	}

	/*
	 * The callback is simply a Tcl object containing the name
	 * of the callback proc, which is the second-to-last argument.
	 *
	 * Note that the callback needs to go in the *secondary* DB handle's
	 * info struct;  we may have multiple secondaries with different
	 * callbacks.
	 */
	sdbip = (DBTCL_INFO *)sdbp->api_internal;
	if (i != objc - 1) {
		/*
		 * We have 2 args, get the callback.
		 */
		sdbip->i_second_call = objv[objc - 2];
		Tcl_IncrRefCount(sdbip->i_second_call);

		/* Now call associate. */
		_debug_check();
		ret = dbp->associate(dbp, txn, sdbp, tcl_second_call, flag);
	} else {
		/*
		 * We have a NULL callback.
		 */
		sdbip->i_second_call = NULL;
		ret = dbp->associate(dbp, txn, sdbp, NULL, flag);
	}
	result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret), "associate");

	return (result);
}

/*
 * tcl_second_call --
 *	Callback function for secondary indices.  Get the callback
 *	out of ip->i_second_call and call it.
 */
static int
tcl_second_call(dbp, pkey, data, skey)
	DB *dbp;
	const DBT *pkey, *data;
	DBT *skey;
{
	DBTCL_INFO *ip;
	Tcl_Interp *interp;
	Tcl_Obj *pobj, *dobj, *objv[3];
	int len, result, ret;
	void *retbuf, *databuf;

	ip = (DBTCL_INFO *)dbp->api_internal;
	interp = ip->i_interp;
	objv[0] = ip->i_second_call;

	/*
	 * Create two ByteArray objects, with the contents of the pkey
	 * and data DBTs that are our inputs.
	 */
	pobj = Tcl_NewByteArrayObj(pkey->data, pkey->size);
	Tcl_IncrRefCount(pobj);
	dobj = Tcl_NewByteArrayObj(data->data, data->size);
	Tcl_IncrRefCount(dobj);

	objv[1] = pobj;
	objv[2] = dobj;

	result = Tcl_EvalObjv(interp, 3, objv, 0);

	Tcl_DecrRefCount(pobj);
	Tcl_DecrRefCount(dobj);

	if (result != TCL_OK) {
		__db_err(dbp->dbenv,
		    "Tcl callback function failed with code %d", result);
		return (EINVAL);
	}

	retbuf =
	    Tcl_GetByteArrayFromObj(Tcl_GetObjResult(interp), &len);

	/*
	 * retbuf is owned by Tcl; copy it into malloc'ed memory.
	 * We need to use __os_umalloc rather than ufree because this will
	 * be freed by DB using __os_ufree--the DB_DBT_APPMALLOC flag
	 * tells DB to free application-allocated memory.
	 */
	if ((ret = __os_umalloc(dbp->dbenv, len, &databuf)) != 0)
		return (ret);
	memcpy(databuf, retbuf, len);

	skey->data = databuf;
	skey->size = len;
	F_SET(skey, DB_DBT_APPMALLOC);

	return (0);
}

/*
 * tcl_db_join --
 */
static int
tcl_DbJoin(interp, objc, objv, dbp, dbcp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
	DBC **dbcp;			/* Cursor pointer */
{
	static char *dbjopts[] = {
		"-nosort",
		NULL
	};
	enum dbjopts {
		DBJ_NOSORT
	};
	DBC **listp;
	u_int32_t flag;
	int adj, i, j, optindex, size, result, ret;
	char *arg, msg[MSG_SIZE];

	result = TCL_OK;
	flag = 0;
	if (objc < 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "curs1 curs2 ...");
		return (TCL_ERROR);
	}

	i = 2;
	adj = i;
	while (i < objc) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbjopts, "option",
		    TCL_EXACT, &optindex) != TCL_OK) {
			result = IS_HELP(objv[i]);
			if (result == TCL_OK)
				return (result);
			result = TCL_OK;
			Tcl_ResetResult(interp);
			break;
		}
		i++;
		switch ((enum dbjopts)optindex) {
		case DBJ_NOSORT:
			flag |= DB_JOIN_NOSORT;
			adj++;
			break;
		}
	}
	if (result != TCL_OK)
		return (result);
	/*
	 * Allocate one more for NULL ptr at end of list.
	 */
	size = sizeof(DBC *) * ((objc - adj) + 1);
	ret = __os_malloc(dbp->dbenv, size, &listp);
	if (ret != 0) {
		Tcl_SetResult(interp, db_strerror(ret), TCL_STATIC);
		return (TCL_ERROR);
	}

	memset(listp, 0, size);
	for (j = 0, i = adj; i < objc; i++, j++) {
		arg = Tcl_GetStringFromObj(objv[i], NULL);
		listp[j] = NAME_TO_DBC(arg);
		if (listp[j] == NULL) {
			snprintf(msg, MSG_SIZE,
			    "Join: Invalid cursor: %s\n", arg);
			Tcl_SetResult(interp, msg, TCL_VOLATILE);
			result = TCL_ERROR;
			goto out;
		}
	}
	listp[j] = NULL;
	_debug_check();
	ret = dbp->join(dbp, listp, dbcp, flag);
	result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret), "db join");

out:
	__os_free(dbp->dbenv, listp);
	return (result);
}

/*
 * tcl_db_getjoin --
 */
static int
tcl_DbGetjoin(interp, objc, objv, dbp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
{
	static char *dbgetjopts[] = {
#if CONFIG_TEST
		"-nosort",
#endif
		"-txn",
		NULL
	};
	enum dbgetjopts {
#if CONFIG_TEST
		DBGETJ_NOSORT,
#endif
		DBGETJ_TXN
	};
	DB_TXN *txn;
	DB *elemdbp;
	DBC **listp;
	DBC *dbc;
	DBT key, data;
	Tcl_Obj **elemv, *retlist;
	void *ktmp;
	u_int32_t flag;
	int adj, elemc, freekey, i, j, optindex, result, ret, size;
	char *arg, msg[MSG_SIZE];

	result = TCL_OK;
	flag = 0;
	freekey = 0;
	if (objc < 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "{db1 key1} {db2 key2} ...");
		return (TCL_ERROR);
	}

	txn = NULL;
	i = 2;
	adj = i;
	while (i < objc) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbgetjopts, "option",
		    TCL_EXACT, &optindex) != TCL_OK) {
			result = IS_HELP(objv[i]);
			if (result == TCL_OK)
				return (result);
			result = TCL_OK;
			Tcl_ResetResult(interp);
			break;
		}
		i++;
		switch ((enum dbgetjopts)optindex) {
#if CONFIG_TEST
		case DBGETJ_NOSORT:
			flag |= DB_JOIN_NOSORT;
			adj++;
			break;
#endif
		case DBGETJ_TXN:
			if (i == objc) {
				Tcl_WrongNumArgs(interp, 2, objv, "?-txn id?");
				result = TCL_ERROR;
				break;
			}
			arg = Tcl_GetStringFromObj(objv[i++], NULL);
			txn = NAME_TO_TXN(arg);
			adj += 2;
			if (txn == NULL) {
				snprintf(msg, MSG_SIZE,
				    "GetJoin: Invalid txn: %s\n", arg);
				Tcl_SetResult(interp, msg, TCL_VOLATILE);
				result = TCL_ERROR;
			}
			break;
		}
	}
	if (result != TCL_OK)
		return (result);
	size = sizeof(DBC *) * ((objc - adj) + 1);
	ret = __os_malloc(NULL, size, &listp);
	if (ret != 0) {
		Tcl_SetResult(interp, db_strerror(ret), TCL_STATIC);
		return (TCL_ERROR);
	}

	memset(listp, 0, size);
	for (j = 0, i = adj; i < objc; i++, j++) {
		/*
		 * Get each sublist as {db key}
		 */
		result = Tcl_ListObjGetElements(interp, objv[i],
		    &elemc, &elemv);
		if (elemc != 2) {
			Tcl_SetResult(interp, "Lists must be {db key}",
			    TCL_STATIC);
			result = TCL_ERROR;
			goto out;
		}
		/*
		 * Get a pointer to that open db.  Then, open a cursor in
		 * that db, and go to the "key" place.
		 */
		elemdbp = NAME_TO_DB(Tcl_GetStringFromObj(elemv[0], NULL));
		if (elemdbp == NULL) {
			snprintf(msg, MSG_SIZE, "Get_join: Invalid db: %s\n",
			    Tcl_GetStringFromObj(elemv[0], NULL));
			Tcl_SetResult(interp, msg, TCL_VOLATILE);
			result = TCL_ERROR;
			goto out;
		}
		ret = elemdbp->cursor(elemdbp, txn, &listp[j], 0);
		if ((result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret),
		    "db cursor")) == TCL_ERROR)
			goto out;
		memset(&key, 0, sizeof(key));
		memset(&data, 0, sizeof(data));
		ret = _CopyObjBytes(interp, elemv[elemc-1], &ktmp,
		    &key.size, &freekey);
		if (ret != 0) {
			result = _ReturnSetup(interp, ret,
			    DB_RETOK_STD(ret), "db join");
			goto out;
		}
		key.data = ktmp;
		ret = (listp[j])->c_get(listp[j], &key, &data, DB_SET);
		if ((result = _ReturnSetup(interp, ret, DB_RETOK_DBCGET(ret),
		    "db cget")) == TCL_ERROR)
			goto out;
	}
	listp[j] = NULL;
	_debug_check();
	ret = dbp->join(dbp, listp, &dbc, flag);
	result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret), "db join");
	if (result == TCL_ERROR)
		goto out;

	retlist = Tcl_NewListObj(0, NULL);
	while (ret == 0 && result == TCL_OK) {
		memset(&key, 0, sizeof(key));
		memset(&data, 0, sizeof(data));
		key.flags |= DB_DBT_MALLOC;
		data.flags |= DB_DBT_MALLOC;
		ret = dbc->c_get(dbc, &key, &data, 0);
		/*
		 * Build up our {name value} sublist
		 */
		if (ret == 0) {
			result = _SetListElem(interp, retlist,
			    key.data, key.size,
			    data.data, data.size);
			free(key.data);
			free(data.data);
		}
	}
	dbc->c_close(dbc);
	if (result == TCL_OK)
		Tcl_SetObjResult(interp, retlist);
out:
	if (freekey)
		(void)__os_free(dbp->dbenv, ktmp);
	while (j) {
		if (listp[j])
			(listp[j])->c_close(listp[j]);
		j--;
	}
	__os_free(dbp->dbenv, listp);
	return (result);
}

/*
 * tcl_DbCount --
 */
static int
tcl_DbCount(interp, objc, objv, dbp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
{
	Tcl_Obj *res;
	DBC *dbc;
	DBT key, data;
	void *ktmp;
	db_recno_t count, recno;
	int freekey, result, ret;

	result = TCL_OK;
	count = 0;
	freekey = 0;
	res = NULL;
	if (objc != 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "key");
		return (TCL_ERROR);
	}

	memset(&key, 0, sizeof(key));

	/*
	 * Get the count for our key.
	 * We do this by getting a cursor for this DB.  Moving the cursor
	 * to the set location, and getting a count on that cursor.
	 */
	ret = 0;
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	/*
	 * If it's a queue or recno database, we must make sure to
	 * treat the key as a recno rather than as a byte string.
	 */
	if (dbp->type == DB_RECNO || dbp->type == DB_QUEUE) {
		result = _GetUInt32(interp, objv[2], &recno);
		if (result == TCL_OK) {
			key.data = &recno;
			key.size = sizeof(db_recno_t);
		} else
			return (result);
	} else {
		ret = _CopyObjBytes(interp, objv[2], &ktmp,
		    &key.size, &freekey);
		if (ret != 0) {
			result = _ReturnSetup(interp, ret,
			    DB_RETOK_STD(ret), "db count");
			return (result);
		}
		key.data = ktmp;
	}
	_debug_check();
	ret = dbp->cursor(dbp, NULL, &dbc, 0);
	if (ret != 0) {
		result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret),
		    "db cursor");
		goto out;
	}
	/*
	 * Move our cursor to the key.
	 */
	ret = dbc->c_get(dbc, &key, &data, DB_SET);
	if (ret == DB_NOTFOUND)
		count = 0;
	else {
		ret = dbc->c_count(dbc, &count, 0);
		if (ret != 0) {
			result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret),
			    "db c count");
			goto out;
		}
	}
	res = Tcl_NewLongObj((long)count);
	Tcl_SetObjResult(interp, res);
out:
	if (freekey)
		(void)__os_free(dbp->dbenv, ktmp);
	(void)dbc->c_close(dbc);
	return (result);
}

#if CONFIG_TEST
/*
 * tcl_DbKeyRange --
 */
static int
tcl_DbKeyRange(interp, objc, objv, dbp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
{
	static char *dbkeyropts[] = {
		"-txn",
		NULL
	};
	enum dbkeyropts {
		DBKEYR_TXN
	};
	DB_TXN *txn;
	DB_KEY_RANGE range;
	DBT key;
	DBTYPE type;
	Tcl_Obj *myobjv[3], *retlist;
	void *ktmp;
	db_recno_t recno;
	u_int32_t flag;
	int freekey, i, myobjc, optindex, result, ret;
	char *arg, msg[MSG_SIZE];

	result = TCL_OK;
	flag = 0;
	freekey = 0;
	if (objc < 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "?-txn id? key");
		return (TCL_ERROR);
	}

	txn = NULL;
	i = 2;
	while (i < objc) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbkeyropts, "option",
		    TCL_EXACT, &optindex) != TCL_OK) {
			result = IS_HELP(objv[i]);
			if (result == TCL_OK)
				return (result);
			result = TCL_OK;
			Tcl_ResetResult(interp);
			break;
		}
		i++;
		switch ((enum dbkeyropts)optindex) {
		case DBKEYR_TXN:
			if (i == objc) {
				Tcl_WrongNumArgs(interp, 2, objv, "?-txn id?");
				result = TCL_ERROR;
				break;
			}
			arg = Tcl_GetStringFromObj(objv[i++], NULL);
			txn = NAME_TO_TXN(arg);
			if (txn == NULL) {
				snprintf(msg, MSG_SIZE,
				    "KeyRange: Invalid txn: %s\n", arg);
				Tcl_SetResult(interp, msg, TCL_VOLATILE);
				result = TCL_ERROR;
			}
			break;
		}
	}
	if (result != TCL_OK)
		return (result);
	(void)dbp->get_type(dbp, &type);
	ret = 0;
	/*
	 * Make sure we have a key.
	 */
	if (i != (objc - 1)) {
		Tcl_WrongNumArgs(interp, 2, objv, "?args? key");
		result = TCL_ERROR;
		goto out;
	}
	memset(&key, 0, sizeof(key));
	if (type == DB_RECNO || type == DB_QUEUE) {
		result = _GetUInt32(interp, objv[i], &recno);
		if (result == TCL_OK) {
			key.data = &recno;
			key.size = sizeof(db_recno_t);
		} else
			return (result);
	} else {
		ret = _CopyObjBytes(interp, objv[i++], &ktmp,
		    &key.size, &freekey);
		if (ret != 0) {
			result = _ReturnSetup(interp, ret,
			    DB_RETOK_STD(ret), "db keyrange");
			return (result);
		}
		key.data = ktmp;
	}
	_debug_check();
	ret = dbp->key_range(dbp, txn, &key, &range, flag);
	result = _ReturnSetup(interp, ret, DB_RETOK_STD(ret), "db keyrange");
	if (result == TCL_ERROR)
		goto out;

	/*
	 * If we succeeded, set up return list.
	 */
	myobjc = 3;
	myobjv[0] = Tcl_NewDoubleObj(range.less);
	myobjv[1] = Tcl_NewDoubleObj(range.equal);
	myobjv[2] = Tcl_NewDoubleObj(range.greater);
	retlist = Tcl_NewListObj(myobjc, myobjv);
	if (result == TCL_OK)
		Tcl_SetObjResult(interp, retlist);
out:
	if (freekey)
		(void)__os_free(dbp->dbenv, ktmp);
	return (result);
}
#endif

/*
 * tcl_DbTruncate --
 */
static int
tcl_DbTruncate(interp, objc, objv, dbp)
	Tcl_Interp *interp;		/* Interpreter */
	int objc;			/* How many arguments? */
	Tcl_Obj *CONST objv[];		/* The argument objects */
	DB *dbp;			/* Database pointer */
{
	static char *dbcuropts[] = {
		"-auto_commit",
		"-txn",
		NULL
	};
	enum dbcuropts {
		DBTRUNC_AUTO_COMMIT,
		DBTRUNC_TXN
	};
	DB_TXN *txn;
	Tcl_Obj *res;
	u_int32_t count, flag;
	int i, optindex, result, ret;
	char *arg, msg[MSG_SIZE];

	txn = NULL;
	flag = 0;
	result = TCL_OK;

	i = 2;
	while (i < objc) {
		if (Tcl_GetIndexFromObj(interp, objv[i], dbcuropts, "option",
		    TCL_EXACT, &optindex) != TCL_OK) {
			result = IS_HELP(objv[i]);
			goto out;
		}
		i++;
		switch ((enum dbcuropts)optindex) {
		case DBTRUNC_AUTO_COMMIT:
			flag |= DB_AUTO_COMMIT;
			break;
		case DBTRUNC_TXN:
			if (i == objc) {
				Tcl_WrongNumArgs(interp, 2, objv, "?-txn id?");
				result = TCL_ERROR;
				break;
			}
			arg = Tcl_GetStringFromObj(objv[i++], NULL);
			txn = NAME_TO_TXN(arg);
			if (txn == NULL) {
				snprintf(msg, MSG_SIZE,
				    "Truncate: Invalid txn: %s\n", arg);
				Tcl_SetResult(interp, msg, TCL_VOLATILE);
				result = TCL_ERROR;
			}
			break;
		}
		if (result != TCL_OK)
			break;
	}
	if (result != TCL_OK)
		goto out;

	_debug_check();
	ret = dbp->truncate(dbp, txn, &count, flag);
	if (ret != 0)
		result = _ErrorSetup(interp, ret, "db truncate");

	else {
		res = Tcl_NewLongObj((long)count);
		Tcl_SetObjResult(interp, res);
	}
out:
	return (result);
}
