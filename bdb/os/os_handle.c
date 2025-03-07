/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1998-2002
 *	Sleepycat Software.  All rights reserved.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: os_handle.c,v 1.1 2006/01/28 00:09:28 kurt Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#endif

#include "db_int.h"

/*
 * __os_openhandle --
 *	Open a file, using POSIX 1003.1 open flags.
 *
 * PUBLIC: int __os_openhandle __P((DB_ENV *, const char *, int, int, DB_FH *));
 */
int
__os_openhandle(dbenv, name, flags, mode, fhp)
	DB_ENV *dbenv;
	const char *name;
	int flags, mode;
	DB_FH *fhp;
{
	int ret, nrepeat;
#ifdef HAVE_VXWORKS
	int newflags;
#endif

	memset(fhp, 0, sizeof(*fhp));

	/* If the application specified an interface, use it. */
	if (DB_GLOBAL(j_open) != NULL) {
		if ((fhp->fd = DB_GLOBAL(j_open)(name, flags, mode)) == -1)
			return (__os_get_errno());
		F_SET(fhp, DB_FH_VALID);
		return (0);
	}

	for (nrepeat = 1; nrepeat < 4; ++nrepeat) {
		ret = 0;
#ifdef	HAVE_VXWORKS
		/*
		 * VxWorks does not support O_CREAT on open, you have to use
		 * creat() instead.  (It does not support O_EXCL or O_TRUNC
		 * either, even though they are defined "for future support".)
		 * We really want the POSIX behavior that if O_CREAT is set,
		 * we open if it exists, or create it if it doesn't exist.
		 * If O_CREAT is specified, single thread and try to open the
		 * file.  If successful, and O_EXCL return EEXIST.  If
		 * unsuccessful call creat and then end single threading.
		 */
		if (LF_ISSET(O_CREAT)) {
			DB_BEGIN_SINGLE_THREAD;
			newflags = flags & ~(O_CREAT | O_EXCL);
			if ((fhp->fd =
			    open(name, newflags, mode)) != -1) {
				if (LF_ISSET(O_EXCL)) {
					/*
					 * If we get here, we want O_EXCL
					 * create, and it exists.  Close and
					 * return EEXISTS.
					 */
					(void)close(fhp->fd);
					DB_END_SINGLE_THREAD;
					return (EEXIST);
				}
				/*
				 * XXX
				 * Assume any error means non-existence.
				 * Unfortunately return values (even for
				 * non-existence) are driver specific so
				 * there is no single error we can use to
				 * verify we truly got the equivalent of
				 * ENOENT.
				 */
			} else
				fhp->fd = creat(name, newflags);
			DB_END_SINGLE_THREAD;
		} else

		/* FALLTHROUGH */
#endif
#ifdef __VMS
		/*
		 * !!!
		 * Open with full sharing on VMS.
		 *
		 * We use these flags because they are the ones set by the VMS
		 * CRTL mmap() call when it opens a file, and we have to be
		 * able to open files that mmap() has previously opened, e.g.,
		 * when we're joining already existing DB regions.
		 */
		fhp->fd = open(name, flags, mode, "shr=get,put,upd,del,upi");
#else
		fhp->fd = open(name, flags, mode);
#endif

		if (fhp->fd == -1) {
			/*
			 * If it's a "temporary" error, we retry up to 3 times,
			 * waiting up to 12 seconds.  While it's not a problem
			 * if we can't open a database, an inability to open a
			 * log file is cause for serious dismay.
			 */
			ret = __os_get_errno();
			if (ret == ENFILE || ret == EMFILE || ret == ENOSPC) {
				(void)__os_sleep(dbenv, nrepeat * 2, 0);
				continue;
			}

			/*
			 * If it was an EINTR it's reasonable to retry
			 * immediately, and arbitrarily often.
			 */
			if (ret == EINTR) {
				--nrepeat;
				continue;
			}
		} else {
#if defined(HAVE_FCNTL_F_SETFD)
			/* Deny file descriptor access to any child process. */
			if (fcntl(fhp->fd, F_SETFD, FD_CLOEXEC) == -1) {
				ret = __os_get_errno();
				__db_err(dbenv, "fcntl(F_SETFD): %s",
				    strerror(ret));
				(void)__os_closehandle(dbenv, fhp);
			} else
#endif
				F_SET(fhp, DB_FH_VALID);
		}
		break;
	}

	return (ret);
}

/*
 * __os_closehandle --
 *	Close a file.
 *
 * PUBLIC: int __os_closehandle __P((DB_ENV *, DB_FH *));
 */
int
__os_closehandle(dbenv, fhp)
	DB_ENV *dbenv;
	DB_FH *fhp;
{
	int ret;

	/* Don't close file descriptors that were never opened. */
	DB_ASSERT(F_ISSET(fhp, DB_FH_VALID) && fhp->fd != -1);

	do {
		ret = DB_GLOBAL(j_close) != NULL ?
		    DB_GLOBAL(j_close)(fhp->fd) : close(fhp->fd);
	} while (ret != 0 && (ret = __os_get_errno()) == EINTR);

	/* Unlink the file if we haven't already done so. */
	if (F_ISSET(fhp, DB_FH_UNLINK)) {
		(void)__os_unlink(dbenv, fhp->name);
		(void)__os_free(dbenv, fhp->name);
	}

	/*
	 * Smash the POSIX file descriptor -- it's never tested, but we want
	 * to catch any mistakes.
	 */
	fhp->fd = -1;
	F_CLR(fhp, DB_FH_VALID);

	return (ret);
}
