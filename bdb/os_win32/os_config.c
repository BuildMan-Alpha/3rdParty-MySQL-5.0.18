/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1999-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: os_config.c,v 1.1 2006/01/28 00:09:28 kurt Exp $";
#endif /* not lint */

#include "db_int.h"

/*
 * __os_fs_notzero --
 *	Return 1 if allocated filesystem blocks are not zeroed.
 */
int
__os_fs_notzero()
{
	/*
	 * Windows/NT zero-fills pages that were never explicitly written to
	 * the file.  Windows 95/98 gives you random garbage, and that breaks
	 * Berkeley DB.
	 */
	return (__os_is_winnt() ? 0 : 1);
}
