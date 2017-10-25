/* vim: set ai sw=4 sts=4 ts=4 :*/

/*
**	This implements GDP resource management (primarily locking and memory).
**
**	----- BEGIN LICENSE BLOCK -----
**	GDP: Global Data Plane Support Library
**	From the Ubiquitous Swarm Lab, 490 Cory Hall, U.C. Berkeley.
**
**	Copyright (c) 2015-2017, Regents of the University of California.
**	All rights reserved.
**
**	Permission is hereby granted, without written agreement and without
**	license or royalty fees, to use, copy, modify, and distribute this
**	software and its documentation for any purpose, provided that the above
**	copyright notice and the following two paragraphs appear in all copies
**	of this software.
**
**	IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
**	SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST
**	PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION,
**	EVEN IF REGENTS HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
**	REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
**	LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
**	FOR A PARTICULAR PURPOSE. THE SOFTWARE AND ACCOMPANYING DOCUMENTATION,
**	IF ANY, PROVIDED HEREUNDER IS PROVIDED "AS IS". REGENTS HAS NO
**	OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS,
**	OR MODIFICATIONS.
**	----- END LICENSE BLOCK -----
*/

#include <ep/ep.h>
#include <ep/ep_app.h>
#include <ep/ep_dbg.h>
#include <ep/ep_hash.h>
#include <ep/ep_log.h>
#include <ep/ep_prflags.h>
#include <ep/ep_thr.h>

#include "gdp.h"
#include "gdp_priv.h"

#include <event2/event.h>

#include <errno.h>
#include <string.h>

static EP_DBG	Dbg = EP_DBG_INIT("gdp.gob.mgmt", "GOB resource management");



/***********************************************************************
**
**	GOB Management
**
***********************************************************************/

static EP_THR_MUTEX		_GobFreeListMutex	EP_THR_MUTEX_INITIALIZER;
static LIST_HEAD(gob_free_head, gdp_gob)
						GobFreeList = LIST_HEAD_INITIALIZER(GobFreeList);

static int				NGobsAllocated = 0;


/*
**	_GDP_GOB_NEW --- create a new gob & initialize
**
**		Only initialization done is the mutex and the name.
**
**	Parameters:
**		gob_name --- internal (256-bit) name of the GOB
**		pgob --- location to store the resulting GOB handle
**
**		This does not add the new GOB handle to the cache.
**
**		gob is returned unlocked.
*/

EP_STAT
_gdp_gob_new(gdp_name_t gob_name, gdp_gob_t **pgob)
{
	EP_STAT estat = EP_STAT_OK;
	gdp_gob_t *gob = NULL;

	ep_thr_mutex_lock(&_GobFreeListMutex);
	if (!LIST_EMPTY(&GobFreeList))
	{
		gob = LIST_FIRST(&GobFreeList);
		LIST_REMOVE(gob, ulist);
	}
	ep_thr_mutex_unlock(&_GobFreeListMutex);

	if (gob != NULL && !EP_ASSERT(!EP_UT_BITSET(GCLF_INUSE, gob->flags)))
		gob = NULL;

	if (gob == NULL)
	{
		// allocate the memory to hold the gob handle
		gob = ep_mem_zalloc(sizeof *gob);
		if (gob == NULL)
			goto fail1;

		if (ep_thr_mutex_init(&gob->mutex, EP_THR_MUTEX_DEFAULT) != 0)
			goto fail1;
		ep_thr_mutex_setorder(&gob->mutex, GDP_MUTEX_LORDER_GOB);
	}
	if (!EP_ASSERT(gob->flags == 0))
		_gdp_gob_dump(gob, NULL, GDP_PR_DETAILED, 0);
	VALGRIND_HG_CLEAN_MEMORY(gob, sizeof *gob);

	LIST_INIT(&gob->reqs);
	gob->refcnt = 1;
	gob->nrecs = 0;
	NGobsAllocated++;

	// create a name if we don't have one passed in
	if (gob_name == NULL || !gdp_name_is_valid(gob_name))
		_gdp_newname(gob->name, gob->gclmd);	//XXX bogus: gob->gclmd isn't set yet
	else
		memcpy(gob->name, gob_name, sizeof gob->name);
	gdp_printable_name(gob->name, gob->pname);

	// success
	gob->flags = GCLF_INUSE | GCLF_PENDING;
	*pgob = gob;
	ep_dbg_cprintf(Dbg, 28, "_gdp_gob_new => %p (%s)\n",
			gob, gob->pname);
	return estat;

fail1:
	estat = ep_stat_from_errno(errno);
	ep_mem_free(gob);

	char ebuf[100];
	ep_dbg_cprintf(Dbg, 4, "_gdp_gob_new failed: %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
	return estat;
}


/*
**  _GDP_GOB_FREE --- drop an existing gob
*/

void
_gdp_gob_free(gdp_gob_t **pgob)
{
	gdp_gob_t *gob = *pgob;

	ep_dbg_cprintf(Dbg, 28, "_gdp_gob_free(%p)\n", gob);
	if (gob == NULL)
		return;
	*pgob = NULL;
	if (!EP_ASSERT(EP_UT_BITSET(GCLF_INUSE, gob->flags)))
		return;
	GDP_GOB_ASSERT_ISLOCKED(gob);

	// this is a forced free, so ignore existing refcnts, etc.
	gob->flags |= GCLF_DROPPING;
	gob->refcnt = 0;

	if (EP_UT_BITSET(GCLF_INCACHE, gob->flags))
	{
		// drop it from the name -> handle cache and the LRU list
		_gdp_gob_cache_drop(gob, false);
		EP_ASSERT(!EP_UT_BITSET(GCLF_INCACHE, gob->flags));
	}

	// release any remaining requests
	_gdp_req_freeall(gob, NULL, NULL);

	// free any additional per-GOB resources
	if (gob->freefunc != NULL)
		(*gob->freefunc)(gob);
	gob->freefunc = NULL;
	if (gob->gclmd != NULL)
		gdp_gclmd_free(gob->gclmd);
	gob->gclmd = NULL;
	if (gob->digest != NULL)
		ep_crypto_md_free(gob->digest);
	gob->digest = NULL;

	// if there is any "extra" data, drop that
	//		(redundant; should be done by the freefunc)
	if (gob->x != NULL)
	{
		ep_mem_free(gob->x);
		gob->x = NULL;
	}

	// gob should be inaccessible now, so this should be safe
	_gdp_gob_unlock(gob);
	gob->flags = 0;

	// drop this (now empty) GOB handle on the free list
	ep_thr_mutex_lock(&_GobFreeListMutex);
#if GDP_DEBUG_NO_FREE_LISTS		// avoid helgrind complaints
	ep_thr_mutex_destroy(&gob->mutex);
	ep_mem_free(gob);
#else
	LIST_INSERT_HEAD(&GobFreeList, gob, ulist);
#endif
	ep_thr_mutex_unlock(&_GobFreeListMutex);
	NGobsAllocated--;
}


/*
**  _GDP_GOB_DUMP --- print a GOB (for debugging)
*/

EP_PRFLAGS_DESC	_GdpGobFlags[] =
{
	{ GCLF_DROPPING,		GCLF_DROPPING,			"DROPPING"			},
	{ GCLF_INCACHE,			GCLF_INCACHE,			"INCACHE"			},
	{ GCLF_ISLOCKED,		GCLF_ISLOCKED,			"ISLOCKED"			},
	{ GCLF_INUSE,			GCLF_INUSE,				"INUSE"				},
	{ GCLF_DEFER_FREE,		GCLF_DEFER_FREE,		"DEFER_FREE"		},
	{ GCLF_KEEPLOCKED,		GCLF_KEEPLOCKED,		"KEEPLOCKED"		},
	{ GCLF_PENDING,			GCLF_PENDING,			"PENDING"			},
	{ 0, 0, NULL }
};

void
_gdp_gob_dump(
		const gdp_gob_t *gob,
		FILE *fp,
		int detail,
		int indent)
{
	if (fp == NULL)
		fp = ep_dbg_getfile();
	if (detail >= GDP_PR_BASIC)
		fprintf(fp, "GOB@%p: ", gob);
	VALGRIND_HG_DISABLE_CHECKING(gob, sizeof *gob);
	if (gob == NULL)
	{
		fprintf(fp, "NULL\n");
	}
	else
	{
		if (!gdp_name_is_valid(gob->name))
		{
			fprintf(fp, "no name\n");
		}
		else
		{
			fprintf(fp, "%s\n", gob->pname);
		}

		if (detail >= GDP_PR_BASIC)
		{
			fprintf(fp, "\trefcnt = %d, reqs = %p, nrecs = %" PRIgdp_recno
					"\n\tflags = ",
					gob->refcnt, LIST_FIRST(&gob->reqs),
					gob->nrecs);
			ep_prflags(gob->flags, _GdpGobFlags, fp);
			fprintf(fp, "\n");
			if (detail >= GDP_PR_DETAILED)
			{
				char tbuf[40];
				struct tm tm;

				fprintf(fp, "\tfreefunc = %p, gclmd = %p, digest = %p\n",
						gob->freefunc, gob->gclmd, gob->digest);
				gmtime_r(&gob->utime, &tm);
				strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);
				fprintf(fp, "\tutime = %s, x = %p\n", tbuf, gob->x);
			}
		}
	}
	VALGRIND_HG_ENABLE_CHECKING(gob, sizeof *gob);
}


/*
**  _GDP_GOB_LOCK --- lock a GOB
*/

void
_gdp_gob_lock_trace(
		gdp_gob_t *gob,
		const char *file,
		int line,
		const char *id)
{
	//XXX cheat: _ep_thr_mutex_lock is a libep-private interface
	if (_ep_thr_mutex_lock(&gob->mutex, file, line, id) == 0)
	{
		EP_ASSERT(!EP_UT_BITSET(GCLF_ISLOCKED, gob->flags));
		gob->flags |= GCLF_ISLOCKED;
	}
}


/*
**  _GDP_GOB_UNLOCK --- unlock a GOB
*/

void
_gdp_gob_unlock_trace(
		gdp_gob_t *gob,
		const char *file,
		int line,
		const char *id)
{
	EP_ASSERT(EP_UT_BITSET(GCLF_ISLOCKED, gob->flags));
	gob->flags &= ~GCLF_ISLOCKED;

	//XXX cheat: _ep_thr_mutex_unlock is a libep-private interface
	_ep_thr_mutex_unlock(&gob->mutex, file, line, id);
}


/*
**  Check to make sure a mutex is locked / unlocked.
*/

static int
get_lock_type(void)
{
	static int locktype;
	static bool initialized = false;

	if (initialized)
		return locktype;

	const char *p = ep_adm_getstrparam("libep.thr.mutex.type", "default");
	if (strcasecmp(p, "normal") == 0)
		locktype = EP_THR_MUTEX_NORMAL;
	else if (strcasecmp(p, "errorcheck") == 0)
		locktype = EP_THR_MUTEX_ERRORCHECK;
	else if (strcasecmp(p, "recursive") == 0)
		locktype = EP_THR_MUTEX_RECURSIVE;
	else
		locktype = EP_THR_MUTEX_DEFAULT;

	initialized = true;
	return locktype;
}

bool
_gdp_mutex_check_islocked(
		EP_THR_MUTEX *m,
		const char *mstr,
		const char *file,
		int line)
{
	int istat;

	// if we are using recursive locks, this won't tell much
	if (get_lock_type() == EP_THR_MUTEX_RECURSIVE)
		return true;

	// trylock should fail if the mutex is already locked
	istat = ep_thr_mutex_trylock(m);
	if (istat != 0)
		return true;

	// oops, must have been unlocked
	ep_thr_mutex_unlock(m);
	ep_assert_print(file, line, "mutex %s is not locked", mstr);
	return false;
}


bool
_gdp_mutex_check_isunlocked(
		EP_THR_MUTEX *m,
		const char *mstr,
		const char *file,
		int line)
{
	int istat;

	// anything but error checking locks?  tryunlock doesn't work
	if (get_lock_type() != EP_THR_MUTEX_ERRORCHECK)
		return true;

	// tryunlock should fail if the mutex is not already locked
	istat = ep_thr_mutex_tryunlock(m);
	if (istat == EPERM)
		return true;

	// oops, must have been locked
	ep_thr_mutex_lock(m);
	ep_assert_print(file, line, "mutex %s is already locked", mstr);
	return false;
}


/*
**  _GDP_GOB_INCREF --- increment the reference count on a GOB
**
**		Must be called with GOB locked.
*/

void
_gdp_gob_incref(gdp_gob_t *gob)
{
	EP_ASSERT_ELSE(GDP_GOB_ISGOOD(gob), return);
	GDP_GOB_ASSERT_ISLOCKED(gob);

	gob->refcnt++;
	ep_dbg_cprintf(Dbg, 51, "_gdp_gob_incref(%p): %d\n", gob, gob->refcnt);
}


/*
**  _GDP_GOB_DECREF --- decrement the reference count on a GOB
**
**		The GOB must be locked on entry.  Upon return it will
**		be unlocked (and possibly deallocated if the refcnt has
**		dropped to zero.
*/

#undef _gdp_gob_decref

void
_gdp_gob_decref(gdp_gob_t **gobp, bool keeplocked)
{
	_gdp_gob_decref_trace(gobp, keeplocked, __FILE__, __LINE__, "gobp");
}

void
_gdp_gob_decref_trace(
		gdp_gob_t **gobp,
		bool keeplocked,
		const char *file,
		int line,
		const char *id)
{
	gdp_gob_t *gob = *gobp;

	if (!EP_ASSERT(GDP_GOB_ISGOOD(gob)))
	{
		_gdp_gob_dump(gob, NULL, GDP_PR_BASIC, 0);
		return;
	}
	(void) ep_thr_mutex_assert_islocked(&gob->mutex, id, file, line);

	if (gob->refcnt > 0)
		gob->refcnt--;
	else
		ep_assert_print(file, line, "gob->refcnt = %d (%s)", gob->refcnt, id);
	*gobp = NULL;

	ep_dbg_cprintf(Dbg, 51, "_gdp_gob_decref(%p): %d\n",
			gob, gob->refcnt);
	if (gob->refcnt == 0 && !EP_UT_BITSET(GCLF_DEFER_FREE, gob->flags))
	{
		EP_ASSERT(!keeplocked && !EP_UT_BITSET(GCLF_KEEPLOCKED, gob->flags));
		_gdp_gob_free(&gob);
	}
	else if (!EP_UT_BITSET(GCLF_ISLOCKED, gob->flags))
	{
		ep_dbg_cprintf(Dbg, 1,
				"_gdp_gob_decref(%p, %s) not locked at %s:%d\n",
				gob, id, file, line);
	} else if (!keeplocked && !EP_UT_BITSET(GCLF_KEEPLOCKED, gob->flags))
		_gdp_gob_unlock_trace(gob, file, line, id);
}


/*
**  Print statistics (for debugging)
*/

void
_gdp_gob_pr_stats(FILE *fp)
{
	fprintf(fp, "GOBs Allocated: %d\n", NGobsAllocated);
}