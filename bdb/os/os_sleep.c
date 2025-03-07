/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1997-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: os_sleep.c,v 1.1 2006/01/28 00:09:28 kurt Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_VXWORKS
#include <sys/times.h>
#include <time.h>
#include <selectLib.h>
#else
#if TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else
#if HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <time.h>
#endif /* HAVE_SYS_TIME_H */
#endif /* TIME_WITH SYS_TIME */
#endif /* HAVE_VXWORKS */

#include <string.h>
#include <unistd.h>
#endif

#include "db_int.h"

/*
 * __os_sleep --
 *	Yield the processor for a period of time.
 *
 * PUBLIC: int __os_sleep __P((DB_ENV *, u_long, u_long));
 */
int
__os_sleep(dbenv, secs, usecs)
	DB_ENV *dbenv;
	u_long secs, usecs;		/* Seconds and microseconds. */
{
	struct timeval t;
	int ret;

	/* Don't require that the values be normalized. */
	for (; usecs >= 1000000; usecs -= 1000000)
		++secs;

	if (DB_GLOBAL(j_sleep) != NULL)
		return (DB_GLOBAL(j_sleep)(secs, usecs));

	/*
	 * It's important that we yield the processor here so that other
	 * processes or threads are permitted to run.
	 */
	t.tv_sec = secs;
	t.tv_usec = usecs;
	do {
		ret = select(0, NULL, NULL, NULL, &t) == -1 ?
		    __os_get_errno() : 0;
	} while (ret == EINTR);

	if (ret != 0)
		__db_err(dbenv, "select: %s", strerror(ret));

	return (ret);
}
