/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1997-2002
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: cxx_int.h,v 1.1 2006/01/28 00:09:27 kurt Exp $
 */

#ifndef _CXX_INT_H_
#define	_CXX_INT_H_

// private data structures known to the implementation only

//
// Using FooImp classes will allow the implementation to change in the
// future without any modification to user code or even to header files
// that the user includes. FooImp * is just like void * except that it
// provides a little extra protection, since you cannot randomly assign
// any old pointer to a FooImp* as you can with void *.  Currently, a
// pointer to such an opaque class is always just a pointer to the
// appropriate underlying implementation struct.  These are converted
// back and forth using the various overloaded wrap()/unwrap() methods.
// This is essentially a use of the "Bridge" Design Pattern.
//
// WRAPPED_CLASS implements the appropriate wrap() and unwrap() methods
// for a wrapper class that has an underlying pointer representation.
//
#define	WRAPPED_CLASS(_WRAPPER_CLASS, _IMP_CLASS, _WRAPPED_TYPE)           \
									   \
	class _IMP_CLASS {};                                               \
									   \
	inline _WRAPPED_TYPE unwrap(_WRAPPER_CLASS *val)                   \
	{                                                                  \
		if (!val) return (0);                                      \
		return ((_WRAPPED_TYPE)((void *)(val->imp())));            \
	}                                                                  \
									   \
	inline const _WRAPPED_TYPE unwrapConst(const _WRAPPER_CLASS *val)  \
	{                                                                  \
		if (!val) return (0);                                      \
		return ((const _WRAPPED_TYPE)((void *)(val->constimp()))); \
	}                                                                  \
									   \
	inline _IMP_CLASS *wrap(_WRAPPED_TYPE val)                         \
	{                                                                  \
		return ((_IMP_CLASS*)((void *)val));                       \
	}

WRAPPED_CLASS(DbMpoolFile, DbMpoolFileImp, DB_MPOOLFILE*)
WRAPPED_CLASS(Db, DbImp, DB*)
WRAPPED_CLASS(DbEnv, DbEnvImp, DB_ENV*)
WRAPPED_CLASS(DbTxn, DbTxnImp, DB_TXN*)

// A tristate integer value used by the DB_ERROR macro below.
// We chose not to make this an enumerated type so it can
// be kept private, even though methods that return the
// tristate int can be declared in db_cxx.h .
//
#define	ON_ERROR_THROW     1
#define	ON_ERROR_RETURN    0
#define	ON_ERROR_UNKNOWN   (-1)

// Macros that handle detected errors, in case we want to
// change the default behavior.  The 'policy' is one of
// the tristate values given above.  If UNKNOWN is specified,
// the behavior is taken from the last initialized DbEnv.
//
#define	DB_ERROR(caller, ecode, policy) \
    DbEnv::runtime_error(caller, ecode, policy)

#define	DB_ERROR_DBT(caller, dbt, policy) \
    DbEnv::runtime_error_dbt(caller, dbt, policy)

#define	DB_OVERFLOWED_DBT(dbt) \
	(F_ISSET(dbt, DB_DBT_USERMEM) && dbt->size > dbt->ulen)

/* values for Db::flags_ */
#define	DB_CXX_PRIVATE_ENV      0x00000001

#endif /* !_CXX_INT_H_ */
