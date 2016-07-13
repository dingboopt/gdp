/* vim: set ai sw=4 sts=4 ts=4 : */

/*
**	----- BEGIN LICENSE BLOCK -----
**	GDPLOGD: Log Daemon for the Global Data Plane
**	From the Ubiquitous Swarm Lab, 490 Cory Hall, U.C. Berkeley.
**
**	Copyright (c) 2015, Regents of the University of California.
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


/*
**  Implement on-disk version of logs.
*/

#include "logd.h"
#include "logd_disklog.h"

#include <gdp/gdp_buf.h>
#include <gdp/gdp_gclmd.h>

#include <ep/ep_hash.h>
#include <ep/ep_hexdump.h>
#include <ep/ep_log.h>
#include <ep/ep_mem.h>
#include <ep/ep_net.h>
#include <ep/ep_thr.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/file.h>
#include <stdint.h>

#define SEGMENT_SUPPORT				1	// allow multiple segments
#define PRE_SEGMENT_BACK_COMPAT		1	// handle pre-segment on disk format


static EP_DBG	Dbg = EP_DBG_INIT("gdplogd.physlog", "GDP Log Daemon Physical Log");

#define GCL_PATH_MAX		200		// max length of pathname

static const char	*GCLDir;		// the gcl data directory

#define GETPHYS(gcl)	((gcl)->x->physinfo)



/*
**  FSIZEOF --- return the size of a file
*/

static off_t
fsizeof(FILE *fp)
{
	struct stat st;

	if (fstat(fileno(fp), &st) < 0)
	{
		char errnobuf[200];

		strerror_r(errno, errnobuf, sizeof errnobuf);
		ep_dbg_cprintf(Dbg, 1, "fsizeof: fstat failure: %s\n", errnobuf);
		return -1;
	}

	return st.st_size;
}


/*
**  POSIX_ERROR --- flag error caused by a Posix (Unix) syscall
*/

static EP_STAT EP_TYPE_PRINTFLIKE(2, 3)
posix_error(int _errno, const char *fmt, ...)
{
	va_list ap;
	EP_STAT estat = ep_stat_from_errno(_errno);

	va_start(ap, fmt);
	ep_logv(estat, fmt, ap);
	va_end(ap);

	return estat;
}


/*
**  Initialize the physical I/O module
*/

static EP_STAT
disk_init()
{
	EP_STAT estat = EP_STAT_OK;

	// find physical location of GCL directory
	GCLDir = ep_adm_getstrparam("swarm.gdplogd.gcl.dir", GCL_DIR);
	ep_dbg_cprintf(Dbg, 8, "disk_init: log dir = %s\n", GCLDir);

	return estat;
}



/*
**	GET_GCL_PATH --- get the pathname to an on-disk version of the gcl
*/

static EP_STAT
get_gcl_path(gdp_gcl_t *gcl,
		int segment,
		const char *sfx,
		char *pbuf,
		int pbufsiz)
{
	EP_STAT estat = EP_STAT_OK;
	gdp_pname_t pname;
	int i;
	struct stat st;
	char segment_str[20] = "";

	EP_ASSERT_POINTER_VALID(gcl);

	errno = 0;
	gdp_printable_name(gcl->name, pname);

#if SEGMENT_SUPPORT
	if (segment >= 0)
	{
		snprintf(segment_str, sizeof segment_str, "-%06d", segment);
	}

#if PRE_SEGMENT_BACK_COMPAT
	if (segment == 0)
	{
		// try file without segment number
		i = snprintf(pbuf, pbufsiz, "%s/_%02x/%s%s",
				GCLDir, gcl->name[0], pname, sfx);
		if (i >= pbufsiz)
			goto fail1;
		if (stat(pbuf, &st) >= 0)
		{
			// OK, old style name exists
			segment = -1;
			strlcpy(segment_str, "", sizeof segment_str);
		}
	}
#endif // PRE_SEGMENT_BACK_COMPAT
#endif // SEGMENT_SUPPORT

	// find the subdirectory based on the first part of the name
	i = snprintf(pbuf, pbufsiz, "%s/_%02x", GCLDir, gcl->name[0]);
	if (i >= pbufsiz)
		goto fail1;
	if (stat(pbuf, &st) < 0)
	{
		// doesn't exist; we need to create it
		ep_dbg_cprintf(Dbg, 10, "get_gcl_path: creating %s\n", pbuf);
		i = mkdir(pbuf, 0775);
		if (i < 0)
			goto fail0;
	}
	else if ((st.st_mode & S_IFMT) != S_IFDIR)
	{
		errno = ENOTDIR;
		goto fail0;
	}

	// now return the final complete name
	i = snprintf(pbuf, pbufsiz, "%s/_%02x/%s%s%s",
				GCLDir, gcl->name[0], pname, segment_str, sfx);
	if (i < pbufsiz)
		return EP_STAT_OK;

fail1:
	estat = EP_STAT_BUF_OVERFLOW;

fail0:
	{
		char ebuf[100];

		if (EP_STAT_ISOK(estat))
		{
			if (errno == 0)
				estat = EP_STAT_ERROR;
			else
				estat = ep_stat_from_errno(errno);
		}

		ep_dbg_cprintf(Dbg, 1, "get_gcl_path(%s):\n\t%s\n",
				pbuf, ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}


/*
**  Allocate and free a new in-memory segment.  Does not touch disk.
*/

static segment_t *
segment_alloc(uint32_t segno)
{
	segment_t *seg = ep_mem_zalloc(sizeof *seg);

	seg->segno = segno;

	return seg;
}

static void
segment_free(segment_t *seg)
{
	if (seg == NULL)
		return;
	ep_dbg_cprintf(Dbg, 41, "segment_free: closing fp @ %p (segment %d)\n",
			seg->fp, seg->segno);
	if (seg->fp != NULL && fclose(seg->fp) < 0)
		(void) posix_error(errno, "segment_free: fclose (segment %d)",
						seg->segno);
	seg->fp = NULL;
	ep_mem_free(seg);
}


/*
**  Print a segment for debugging.
*/

static void
segment_dump(segment_t *seg, FILE *fp)
{
	fprintf(fp, "Segment %d @ %p:\n", seg->segno, seg);
	fprintf(fp, "\tfp %p, ver %d, hsize %zd\n",
			seg->fp, seg->ver, seg->header_size);
	fprintf(fp, "\trecno_offset %" PRIgdp_recno ", max_offset %jd\n",
			seg->recno_offset, (intmax_t) seg->max_offset);
}


/*
**  SEGMENT_OPEN --- physically open a segment
**
**		The caller allocates and passes in the new segment.
*/

static EP_STAT
segment_open(gdp_gcl_t *gcl, segment_t *seg)
{
	EP_STAT estat;
	FILE *data_fp;
	char data_pbuf[GCL_PATH_MAX];

	ep_dbg_cprintf(Dbg, 20, "segment_open(seg %d, fp %p)\n",
			seg->segno, seg->fp);

	// if already open, this is a no-op
	if (seg->fp != NULL)
		return EP_STAT_OK;

	// figure out where the segment lives on disk
	//XXX for the moment assume that it's on our local disk
	estat = get_gcl_path(gcl, seg->segno, GCL_LDF_SUFFIX,
					data_pbuf, sizeof data_pbuf);
	EP_STAT_CHECK(estat, goto fail0);

	// do the physical open
	{
		//XXX should open read only if this segment is now frozen
		int fd = open(data_pbuf, O_RDWR | O_APPEND);

		if (fd < 0 || flock(fd, LOCK_SH) < 0 ||
				(data_fp = fdopen(fd, "a+")) == NULL)
		{
			estat = ep_stat_from_errno(errno);
			ep_dbg_cprintf(Dbg, 6, "segment_open(%s): %s\n",
					data_pbuf, strerror(errno));
			if (fd >= 0)
				close(fd);
			goto fail0;
		}
		ep_dbg_cprintf(Dbg, 20, "segment_open(%s) OK\n", data_pbuf);
	}

	// read in the segment header
	segment_header_t seg_hdr;
	rewind(data_fp);
	if (fread(&seg_hdr, sizeof seg_hdr, 1, data_fp) < 1)
	{
		estat = ep_stat_from_errno(errno);
		ep_log(estat, "segment_open(%s): header read failure", data_pbuf);
		goto fail1;
	}

	// convert on-disk format from network to host byte order
	seg_hdr.magic = ep_net_ntoh32(seg_hdr.magic);
	seg_hdr.version = ep_net_ntoh32(seg_hdr.version);
	seg_hdr.header_size = ep_net_ntoh32(seg_hdr.header_size);
	seg_hdr.n_md_entries = ep_net_ntoh16(seg_hdr.n_md_entries);
	seg_hdr.log_type = ep_net_ntoh16(seg_hdr.log_type);
	seg_hdr.recno_offset = ep_net_ntoh64(seg_hdr.recno_offset);

	// validate the segment header
	if (seg_hdr.magic != GCL_LDF_MAGIC)
	{
		estat = GDP_STAT_CORRUPT_GCL;
		ep_log(estat, "segment_open(%s): bad magic: found: 0x%" PRIx32
				", expected: 0x%" PRIx32 "\n",
				data_pbuf,
				seg_hdr.magic, GCL_LDF_MAGIC);
		goto fail1;
	}

	if (seg_hdr.version < GCL_LDF_MINVERS ||
			seg_hdr.version > GCL_LDF_MAXVERS)
	{
		estat = GDP_STAT_GCL_VERSION_MISMATCH;
		ep_log(estat, "segment_open(%s): bad version: found: %" PRId32
				", expected: %" PRIx32 "-%" PRId32 "\n",
				data_pbuf,
				seg_hdr.version, GCL_LDF_MINVERS, GCL_LDF_MAXVERS);
		goto fail1;
	}

	// now we can interpret the data (for the segment)
	seg->header_size = seg_hdr.header_size;
	seg->recno_offset = seg_hdr.recno_offset;
	seg->fp = data_fp;
	seg->ver = seg_hdr.version;
	seg->max_offset = fsizeof(data_fp);

	// interpret data (for the entire log)
	gcl->x->log_type = seg_hdr.log_type;
	if (gcl->gclmd != NULL)
	{
		// we've already read the metadata; no need to do it again
		goto success;
	}

	gcl->x->n_md_entries = seg_hdr.n_md_entries;

	// read metadata entries
	if (seg_hdr.n_md_entries > 0)
	{
		int mdtotal = 0;
		void *md_data;
		int i;

		gcl->gclmd = gdp_gclmd_new(seg_hdr.n_md_entries);
		for (i = 0; i < seg_hdr.n_md_entries; i++)
		{
			uint32_t md_id;
			uint32_t md_len;

			if (fread(&md_id, sizeof md_id, 1, data_fp) != 1 ||
				fread(&md_len, sizeof md_len, 1, data_fp) != 1)
			{
				estat = GDP_STAT_GCL_READ_ERROR;
				goto fail1;
			}

			md_id = ep_net_ntoh32(md_id);
			md_len = ep_net_ntoh32(md_len);

			gdp_gclmd_add(gcl->gclmd, md_id, md_len, NULL);
			mdtotal += md_len;
		}
		md_data = ep_mem_malloc(mdtotal);
		if (fread(md_data, mdtotal, 1, data_fp) != 1)
		{
			estat = GDP_STAT_GCL_READ_ERROR;
			goto fail1;
		}
		_gdp_gclmd_adddata(gcl->gclmd, md_data);
	}

success:
	if (ep_dbg_test(Dbg, 20))
	{
		ep_dbg_printf("segment_open: ");
		segment_dump(seg, ep_dbg_getfile());
	}
	return estat;

fail1:
	ep_dbg_cprintf(Dbg, 10, "segment_open: closing fp %p (error)\n", data_fp);
	fclose(data_fp);
fail0:
	EP_ASSERT_ENSURE(!EP_STAT_ISOK(estat));
	return estat;
}


void
segment_close(gdp_gcl_t *gcl, uint32_t segno)
{
	gcl_physinfo_t *phys = GETPHYS(gcl);
	segment_t *seg;

	if (phys->segments == NULL ||
			phys->nsegments < segno ||
			(seg = phys->segments[segno]) == NULL)
	{
		// nothing to do
		return;
	}
	if (seg->fp != NULL)
	{
		ep_dbg_cprintf(Dbg, 39, "segment_close: closing segment fp %p\n",
				seg->fp);
		if (fclose(seg->fp) != 0)
			(void) posix_error(errno, "segment_close: cannot fclose");
		seg->fp = NULL;
	}
	ep_mem_free(seg);
	phys->segments[segno] = NULL;
}


/*
**  Get the in-memory representation of a numbered segment.
**
**		This does not open it if it's new, but it does allocate the
**		memory and makes sure that the physinfo is allocated.
**
**		If segno is negative, get any segment (we just need the
**		header information and/or metadata).
*/

static segment_t *
segment_get(gdp_gcl_t *gcl, int segno)
{
	segment_t *seg;
	gcl_physinfo_t *phys = GETPHYS(gcl);

	if (segno < 0)
	{
		int i;

		for (i = 0; i < phys->nsegments; i++)
		{
			if (phys->segments[i] != NULL)
				return phys->segments[i];
		}

		// nothing in memory yet?  OK, go for segment zero
		//XXX really we should do a wildcard to find anything we have
		segno = 0;
	}

	// see if we need to expand segment list
	if (phys->segments == NULL || segno >= phys->nsegments)
	{
		// not enough space for segment pointers allocated
		EP_ASSERT(phys->segments != NULL || phys->nsegments == 0);
		phys->segments = ep_mem_realloc(phys->segments, 
							(segno + 1) * sizeof phys->segments[0]);
		memset(&phys->segments[phys->nsegments], 0,
				(segno + 1 - phys->nsegments) * sizeof phys->segments[0]);
		phys->nsegments = segno + 1;
	}

	seg = phys->segments[segno];
	if (seg == NULL)
	{
		// allocate space for new segment
		phys->segments[segno] = seg = segment_alloc(segno);
	}

	EP_ASSERT_ENSURE(seg != NULL);
	return seg;
}


/*
**  SEGMENT_CREATE --- create a new segment on disk
*/

static EP_STAT
segment_create(gdp_gcl_t *gcl,
		gdp_gclmd_t *gmd,
		uint32_t segno,
		gdp_recno_t recno_offset)
{
	EP_STAT estat = EP_STAT_OK;
	FILE *data_fp = NULL;
	segment_t *seg;

	ep_dbg_cprintf(Dbg, 10, "segment_create(%s, %d)\n",
			gcl->pname, segno);

	// this will allocate memory, but leave the disk untouched
	seg = segment_get(gcl, segno);

	// create a file node representing the gcl
	{
		int data_fd;
		char data_pbuf[GCL_PATH_MAX];

		estat = get_gcl_path(gcl, segno, GCL_LDF_SUFFIX,
						data_pbuf, sizeof data_pbuf);
		EP_STAT_CHECK(estat, goto fail1);

		ep_dbg_cprintf(Dbg, 20, "segment_create: creating %s\n", data_pbuf);
		data_fd = open(data_pbuf, O_RDWR | O_CREAT | O_APPEND | O_EXCL, 0644);
		if (data_fd < 0 || (flock(data_fd, LOCK_EX) < 0))
		{
			char nbuf[40];

			estat = ep_stat_from_errno(errno);
			strerror_r(errno, nbuf, sizeof nbuf);
			ep_log(estat, "segment_create: cannot create %s: %s",
					data_pbuf, nbuf);
			if (data_fd >= 0)
				close(data_fd);
			goto fail1;
		}
		data_fp = fdopen(data_fd, "a+");
		if (data_fp == NULL)
		{
			char nbuf[40];

			estat = ep_stat_from_errno(errno);
			strerror_r(errno, nbuf, sizeof nbuf);
			ep_log(estat, "segment_create: cannot fdopen %s: %s",
					data_pbuf, nbuf);
			(void) close(data_fd);
			goto fail1;
		}
	}

	// write the segment header
	{
		segment_header_t seg_hdr;
		size_t metadata_size = 0;

		if (gmd == NULL)
		{
			seg_hdr.n_md_entries = 0;
		}
		else
		{
			// allow space for id and length fields
			metadata_size = gmd->nused * 2 * sizeof (uint32_t);
			gcl->x->n_md_entries = gmd->nused;
			seg_hdr.n_md_entries = ep_net_hton16(gmd->nused);

			// compute the space needed for the data fields
			int i;
			for (i = 0; i < gmd->nused; i++)
				metadata_size += gmd->mds[i].md_len;
		}

		EP_ASSERT_POINTER_VALID(seg);

		seg->ver = GCL_LDF_VERSION;
		seg->segno = segno;
		seg->header_size = seg->max_offset = sizeof seg_hdr + metadata_size;
		seg->recno_offset = 0;

		seg_hdr.magic = ep_net_hton32(GCL_LDF_MAGIC);
		seg_hdr.version = ep_net_hton32(GCL_LDF_VERSION);
		seg_hdr.header_size = ep_net_ntoh32(seg->max_offset);
		seg_hdr.reserved1 = 0;
		seg_hdr.log_type = ep_net_hton16(0);		// unused for now
		seg_hdr.segment = ep_net_hton32(segno);
		seg_hdr.reserved2 = 0;
		memcpy(seg_hdr.gname, gcl->name, sizeof seg_hdr.gname);
		seg_hdr.recno_offset = ep_net_hton64(recno_offset);

		fwrite(&seg_hdr, sizeof seg_hdr, 1, data_fp);
	}

	// write metadata
	if (gmd != NULL)
	{
		int i;

		// first the id and length fields
		for (i = 0; i < gmd->nused; i++)
		{
			uint32_t t32;

			t32 = ep_net_hton32(gmd->mds[i].md_id);
			fwrite(&t32, sizeof t32, 1, data_fp);
			t32 = ep_net_hton32(gmd->mds[i].md_len);
			fwrite(&t32, sizeof t32, 1, data_fp);
		}

		// ... then the actual data
		for (i = 0; i < gmd->nused; i++)
		{
			fwrite(gmd->mds[i].md_data, gmd->mds[i].md_len, 1, data_fp);
		}
	}

	// make sure all header information is written on disk
	// XXX arguably this should use fsync(), but that's expensive
	if (fflush(data_fp) != 0 || ferror(data_fp))
		goto fail2;

	// success!
	fflush(data_fp);
	seg->fp = data_fp;
	flock(fileno(data_fp), LOCK_UN);
	ep_dbg_cprintf(Dbg, 10, "Created GCL Segment %s-%06d\n",
			gcl->pname, segno);
	return estat;

fail2:
	estat = ep_stat_from_errno(errno);
	if (data_fp != NULL)
	{
		ep_dbg_cprintf(Dbg, 20, "segment_create: closing data_fp @ %p\n",
				data_fp);
		fclose(data_fp);
	}
fail1:
	// turn OK into an errno-based code
	if (EP_STAT_ISOK(estat))
		estat = ep_stat_from_errno(errno);

	// turn "file exists" into a meaningful response code
	if (EP_STAT_IS_SAME(estat, ep_stat_from_errno(EEXIST)))
			estat = GDP_STAT_NAK_CONFLICT;

	if (ep_dbg_test(Dbg, 1))
	{
		char ebuf[100];

		ep_dbg_printf("Could not create GCL Handle: %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}


/*
**  Allocate/Free the in-memory version of the physical representation
**		of a GCL.
**
**		XXX Currently allocates space for the first segment.
**		XXX That should probably be deferred until it is actually
**			read off of disk.
*/

static gcl_physinfo_t *
physinfo_alloc(gdp_gcl_t *gcl)
{
	gcl_physinfo_t *phys = ep_mem_zalloc(sizeof *phys);

	if (ep_thr_rwlock_init(&phys->lock) != 0)
		goto fail1;

	//XXX Need to figure out how many segments exist
	//XXX This is just for transition.
	phys->nsegments = 0;

	return phys;

fail1:
	ep_mem_free(phys);
	return NULL;
}


static void
physinfo_free(gcl_physinfo_t *phys)
{
	uint32_t segno;

	if (phys == NULL)
		return;

	if (phys->index.fp != NULL)
	{
		ep_dbg_cprintf(Dbg, 41, "physinfo_free: closing index fp @ %p\n",
				phys->index.fp);
		if (fclose(phys->index.fp) != 0)
			(void) posix_error(errno, "physinfo_free: cannot close index fp");
		phys->index.fp = NULL;
	}

	for (segno = 0; segno < phys->nsegments; segno++)
	{
		if (phys->segments[segno] != NULL)
			segment_free(phys->segments[segno]);
		phys->segments[segno] = NULL;
	}
	ep_mem_free(phys->segments);
	phys->segments = NULL;

	if (ep_thr_rwlock_destroy(&phys->lock) != 0)
		(void) posix_error(errno, "physinfo_free: cannot destroy rwlock");

	ep_mem_free(phys);
	return;
}


static void
physinfo_dump(gcl_physinfo_t *phys, FILE *fp)
{
	int segno;

	fprintf(fp, "physinfo @ %p: min_recno %" PRIgdp_recno
			", max_recno %" PRIgdp_recno "\n",
			phys, phys->min_recno, phys->max_recno);
	fprintf(fp, "\tnsegments %d, last_segment %d\n",
			phys->nsegments, phys->last_segment);
	fprintf(fp, "\tindex: fp %p, min_recno %" PRIgdp_recno
			", max_offset %jd, header_size %zd\n",
			phys->index.fp, phys->index.min_recno,
			(intmax_t) phys->index.max_offset,
			phys->index.header_size);

	for (segno = 0; segno < phys->nsegments; segno++)
	{
		segment_t *seg = phys->segments[segno];
		fprintf(fp, "    ");
		if (seg == NULL)
			fprintf(fp, "Segment %d: NULL\n", segno);
		else
			segment_dump(seg, fp);
	}
}


EP_STAT
xcache_create(gcl_physinfo_t *phys)
{
	return EP_STAT_OK;
}


index_entry_t *
xcache_get(gcl_physinfo_t *phys, gdp_recno_t recno)
{
	return NULL;
}

void
xcache_put(gcl_physinfo_t *phys, gdp_recno_t recno, off_t off)
{
	return;
}

void
xcache_free(gcl_physinfo_t *phys)
{
	return;
}


/*
**  GCL_PHYSCREATE --- create a brand new GCL on disk
*/

static EP_STAT
disk_create(gdp_gcl_t *gcl, gdp_gclmd_t *gmd)
{
	EP_STAT estat = EP_STAT_OK;
	FILE *index_fp;
	gcl_physinfo_t *phys;

	EP_ASSERT_POINTER_VALID(gcl);

	// allocate space for the physical information
	phys = physinfo_alloc(gcl);
	if (phys == NULL)
		goto fail1;
	phys->last_segment = 0;
	gcl->x->physinfo = phys;

	// allocate a name
	if (!gdp_name_is_valid(gcl->name))
	{
		_gdp_gcl_newname(gcl);
	}

	// create an initial segment for the GCL
	estat = segment_create(gcl, gmd, 0, 0);

	// create an offset index for that gcl
	{
		int index_fd;
		char index_pbuf[GCL_PATH_MAX];

		estat = get_gcl_path(gcl, -1, GCL_LXF_SUFFIX,
						index_pbuf, sizeof index_pbuf);
		EP_STAT_CHECK(estat, goto fail2);

		ep_dbg_cprintf(Dbg, 20, "disk_create: creating %s\n", index_pbuf);
		index_fd = open(index_pbuf, O_RDWR | O_CREAT | O_APPEND | O_EXCL, 0644);
		if (index_fd < 0)
		{
			char nbuf[40];

			estat = ep_stat_from_errno(errno);
			strerror_r(errno, nbuf, sizeof nbuf);
			ep_log(estat, "disk_create: cannot create %s: %s",
				index_pbuf, nbuf);
			goto fail2;
		}
		index_fp = fdopen(index_fd, "a+");
		if (index_fp == NULL)
		{
			char nbuf[40];

			estat = ep_stat_from_errno(errno);
			strerror_r(errno, nbuf, sizeof nbuf);
			ep_log(estat, "disk_create: cannot fdopen %s: %s",
				index_pbuf, nbuf);
			(void) close(index_fd);
			goto fail2;
		}
	}

	// write the index header
	{
		index_header_t index_header;

		index_header.magic = ep_net_hton32(GCL_LXF_MAGIC);
		index_header.version = ep_net_hton32(GCL_LXF_VERSION);
		index_header.header_size = ep_net_hton32(SIZEOF_INDEX_HEADER);
		index_header.reserved1 = 0;
		index_header.min_recno = ep_net_hton64(1);

		if (fwrite(&index_header, sizeof index_header, 1, index_fp) != 1)
		{
			estat = posix_error(errno, "disk_create: cannot write header");
			goto fail3;
		}
	}

	// create a cache for that index
	estat = xcache_create(phys);
	EP_STAT_CHECK(estat, goto fail2);

	// success!
	phys->index.fp = index_fp;
	phys->index.max_offset = phys->index.header_size = SIZEOF_INDEX_HEADER;
	phys->index.min_recno = phys->min_recno = 1;
	phys->max_recno = 0;
	ep_dbg_cprintf(Dbg, 10, "Created new GCL %s\n", gcl->pname);
	return estat;

fail3:
	ep_dbg_cprintf(Dbg, 20, "disk_create: closing index_fp @ %p\n",
			index_fp);
	fclose(index_fp);
fail2:
fail1:
	// turn OK into an errno-based code
	if (EP_STAT_ISOK(estat))
		estat = ep_stat_from_errno(errno);

	// turn "file exists" into a meaningful response code
	if (EP_STAT_IS_SAME(estat, ep_stat_from_errno(EEXIST)))
			estat = GDP_STAT_NAK_CONFLICT;

	if (ep_dbg_test(Dbg, 1))
	{
		char ebuf[100];

		ep_dbg_printf("Could not create GCL: %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}


/*
**	DISK_OPEN --- do physical open of a GCL
**
**		XXX: Should really specify whether we want to start reading:
**		(a) At the beginning of the log (easy).	 This includes random
**			access.
**		(b) Anything new that comes into the log after it is opened.
**			To do this we need to read the existing index to find the end.
**		There is a possibility that there exists a new segment that is
**		one greater than the last segment mentioned in the index.  We
**		have to check for that file to be sure.
*/

static EP_STAT
disk_open(gdp_gcl_t *gcl)
{
	EP_STAT estat = EP_STAT_OK;
	int fd;
	FILE *index_fp;
	gcl_physinfo_t *phys;
	char index_pbuf[GCL_PATH_MAX];
	const char *phase;

	// allocate space for physical data
	EP_ASSERT_REQUIRE(gcl->x->physinfo == NULL);
	phase = "physinfo_alloc";
	gcl->x->physinfo = phys = physinfo_alloc(gcl);
	if (phys == NULL)
	{
		estat = ep_stat_from_errno(errno);
		goto fail0;
	}

	// open the index file
	phase = "get_gcl_path(index)";
	estat = get_gcl_path(gcl, -1, GCL_LXF_SUFFIX,
					index_pbuf, sizeof index_pbuf);
	EP_STAT_CHECK(estat, goto fail0);
	ep_dbg_cprintf(Dbg, 39, "disk_open: opening %s\n", index_pbuf);
	phase = "open index";
	fd = open(index_pbuf, O_RDWR | O_APPEND);
	if (fd < 0 || flock(fd, LOCK_SH) < 0 ||
			(index_fp = fdopen(fd, "a+")) == NULL)
	{
		estat = ep_stat_from_errno(errno);
		if (EP_STAT_IS_SAME(estat, ep_stat_from_errno(ENOENT)))
			estat = GDP_STAT_NAK_NOTFOUND;
		ep_log(estat, "disk_open(%s): index open failure", index_pbuf);
		if (fd >= 0)
			close(fd);
		goto fail0;
	}

	// check for valid index header (distinguish old and new format)
	phase = "index header read";
	index_header_t index_header;

	index_header.magic = 0;
	if (fsizeof(index_fp) < sizeof index_header)
	{
		// must be old style
	}
	else if (fread(&index_header, sizeof index_header, 1, index_fp) != 1)
	{
		estat = posix_error(errno,
					"disk_open(%s): index header read failure",
					index_pbuf);
		goto fail0;
	}
	else if (index_header.magic == 0)
	{
		// must be old style
	}
	else if (ep_net_ntoh32(index_header.magic) != GCL_LXF_MAGIC)
	{
		estat = GDP_STAT_CORRUPT_INDEX;
		ep_log(estat, "disk_open(%s): bad index magic", index_pbuf);
		goto fail0;
	}
	else if (ep_net_ntoh32(index_header.version) < GCL_LXF_MINVERS ||
			 ep_net_ntoh32(index_header.version) > GCL_LXF_MAXVERS)
	{
		estat = GDP_STAT_CORRUPT_INDEX;
		ep_log(estat, "disk_open(%s): bad index version", index_pbuf);
		goto fail0;
	}

	if (index_header.magic == 0)
	{
		// old-style index; fake the header
		index_header.min_recno = 1;
		index_header.header_size = 0;
	}
	else
	{
		index_header.min_recno = ep_net_ntoh64(index_header.min_recno);
		index_header.header_size = ep_net_ntoh32(index_header.header_size);
	}

	// create a cache for the index information
	//XXX should do data too, but that's harder because it's variable size
	phase = "xcache_create";
	estat = xcache_create(phys);
	EP_STAT_CHECK(estat, goto fail0);

	phys->index.fp = index_fp;
	phys->index.max_offset = fsizeof(index_fp);
	phys->index.header_size = index_header.header_size;
	phys->index.min_recno = index_header.min_recno;
	phys->min_recno = index_header.min_recno;
	phys->max_recno = ((phys->index.max_offset - index_header.header_size)
							/ SIZEOF_INDEX_RECORD) - index_header.min_recno + 1;
	gcl->nrecs = phys->max_recno;

	/*
	**  Index header has been read.
	**  Find the last segment mentioned in that index.
	*/

	phase = "initial index read";
	if (phys->index.max_offset == 0)
	{
		phys->last_segment = 0;
	}
	else
	{
		index_entry_t xent;
		if (fseek(phys->index.fp, phys->index.max_offset - SIZEOF_INDEX_RECORD,
					SEEK_SET) < 0 ||
				fread(&xent, SIZEOF_INDEX_RECORD, 1, phys->index.fp) != 1)
		{
			goto fail0;
		}
		phys->last_segment = ep_net_ntoh32(xent.segment);
	}

#if SEGMENT_SUPPORT
	/*
	**  Now we have to see if there is another (empty) segment
	*/

	{
		char data_pbuf[GCL_PATH_MAX];
		struct stat stbuf;

		phase = "get_gcl_path(data)";
		estat = get_gcl_path(gcl, phys->last_segment + 1, GCL_LDF_SUFFIX,
						data_pbuf, sizeof data_pbuf);
		EP_STAT_CHECK(estat, goto fail0);
		if (stat(data_pbuf, &stbuf) >= 0)
			phys->last_segment++;
	}
#endif // SEGMENT_SUPPORT

	/*
	**  We need to physically read at least one segment if only
	**  to initialize the metadata.
	**  We choose the last segment on the assumption that new data
	**  is "hotter" than old data.
	*/

	phase = "segment_get";
	{
		segment_t *seg = segment_get(gcl, phys->last_segment);
		estat = segment_open(gcl, seg);
		EP_STAT_CHECK(estat, goto fail0);
	}

	if (ep_dbg_test(Dbg, 20))
	{
		ep_dbg_printf("disk_open => ");
		physinfo_dump(phys, ep_dbg_getfile());
	}
	return estat;

fail0:
	if (EP_STAT_ISOK(estat))
		estat = ep_stat_from_errno(errno);

	if (ep_dbg_test(Dbg, 10))
	{
		char ebuf[100];

		ep_dbg_printf("disk_open(%s): couldn't open gcl %s:\n\t%s\n",
				phase, gcl->pname, ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}

/*
**	GCL_PHYSCLOSE --- physically close an open gcl
*/

static EP_STAT
disk_close(gdp_gcl_t *gcl)
{
	EP_ASSERT_POINTER_VALID(gcl);
	EP_ASSERT_POINTER_VALID(gcl->x);
	EP_ASSERT_POINTER_VALID(gcl->x->physinfo);

	physinfo_free(gcl->x->physinfo);
	gcl->x->physinfo = NULL;
	return EP_STAT_OK;
}


/*
**	GCL_PHYSREAD --- read a message from a gcl
**
**		Reads in a message indicated by datum->recno into datum.
*/

static EP_STAT
fseek_to_index_recno(
		struct physinfo *phys,
		gdp_recno_t recno,
		gdp_pname_t gclpname)
{
	off_t xoff;

	xoff = (recno - phys->index.min_recno) * SIZEOF_INDEX_RECORD +
			phys->index.header_size;
	ep_dbg_cprintf(Dbg, 14,
			"recno=%" PRIgdp_recno ", min_recno=%" PRIgdp_recno
			", index_hdrsize=%zd, xoff=%jd\n",
			recno, phys->min_recno,
			phys->index.header_size, (intmax_t) xoff);
	if (xoff < phys->index.header_size || xoff > phys->index.max_offset)
	{
		// computed offset is out of range
		ep_log(GDP_STAT_CORRUPT_INDEX,
				"fseek_to_index_recno(%s): computed offset %jd"
				" out of range (%jd - %jd)",
				gclpname,
				(intmax_t) xoff,
				(intmax_t) phys->index.header_size,
				(intmax_t) phys->index.max_offset);
		return GDP_STAT_CORRUPT_INDEX;
	}

	if (fseek(phys->index.fp, xoff, SEEK_SET) < 0)
	{
		return posix_error(errno, "fseek_to_index_recno: fseek failed");
	}

	return EP_STAT_OK;
}

static EP_STAT
disk_read(gdp_gcl_t *gcl,
		gdp_datum_t *datum)
{
	gcl_physinfo_t *phys = GETPHYS(gcl);
	EP_STAT estat = EP_STAT_OK;
	index_entry_t index_entry;
	index_entry_t *xent;

	EP_ASSERT_POINTER_VALID(gcl);
	gdp_buf_reset(datum->dbuf);
	if (datum->sig != NULL)
		gdp_buf_reset(datum->sig);

	ep_dbg_cprintf(Dbg, 14, "disk_read(%" PRIgdp_recno "): ", datum->recno);

	ep_thr_rwlock_rdlock(&phys->lock);

	// verify that the recno is in range
	if (datum->recno > phys->max_recno)
	{
		// record does not yet exist
		estat = GDP_STAT_NAK_NOTFOUND;
		ep_dbg_cprintf(Dbg, 14, "EOF\n");
		goto fail0;
	}
	if (datum->recno < phys->min_recno)
	{
		// record is no longer available
		estat = GDP_STAT_RECORD_EXPIRED;
		ep_dbg_cprintf(Dbg, 14, "expired\n");
		goto fail0;
	}

	// check if recno offset is in the index cache
	xent = xcache_get(phys, datum->recno);
	if (xent != NULL)
	{
		ep_dbg_cprintf(Dbg, 14, "cached\n");
	}
	else
	{
		xent = &index_entry;

		// recno is not in the index cache: read it from disk
		flockfile(phys->index.fp);

		estat = fseek_to_index_recno(phys, datum->recno, gcl->pname);
		EP_STAT_CHECK(estat, goto fail3);
		if (fread(xent, SIZEOF_INDEX_RECORD, 1, phys->index.fp) < 1)
		{
			estat = posix_error(errno, "gcl_diskread: fread failed");
			goto fail3;
		}
		xent->recno = ep_net_ntoh64(xent->recno);
		xent->offset = ep_net_ntoh64(xent->offset);
		xent->segment = ep_net_ntoh32(xent->segment);
		xent->reserved = ep_net_ntoh32(xent->reserved);

		ep_dbg_cprintf(Dbg, 14,
				"got index entry: recno %" PRIgdp_recno ", segment %" PRIu32
				", offset=%jd, rsvd=%" PRIu32 "\n",
				xent->recno, xent->segment,
				(intmax_t) xent->offset, xent->reserved); 

		if (xent->offset == 0)
		{
			// we don't have this record available
			estat = GDP_STAT_RECORD_MISSING;
		}

fail3:
		funlockfile(phys->index.fp);
	}

	EP_STAT_CHECK(estat, goto fail0);

	// xent now points to the index entry for this record

	// get the open segment
	segment_t *seg = segment_get(gcl, xent->segment);
	estat = segment_open(gcl, seg);
	if (!EP_STAT_ISOK(estat))
	{
		// if this an ENOENT, it might be because the data is expired
		if (EP_STAT_IS_SAME(estat, ep_stat_from_errno(ENOENT)) &&
				datum->recno < phys->max_recno)
			estat = GDP_STAT_RECORD_EXPIRED;
		goto fail0;
	}

	// read record header
	segment_record_t log_record;
	flockfile(seg->fp);
	if (fseek(seg->fp, xent->offset, SEEK_SET) < 0 ||
			fread(&log_record, sizeof log_record, 1, seg->fp) < 1)
	{
		ep_dbg_cprintf(Dbg, 1, "gcl_diskread: header fread failed: %s\n",
				strerror(errno));
		estat = ep_stat_from_errno(errno);
		goto fail1;
	}

	log_record.recno = ep_net_ntoh64(log_record.recno);
	ep_net_ntoh_timespec(&log_record.timestamp);
	log_record.sigmeta = ep_net_ntoh16(log_record.sigmeta);
	log_record.flags = ep_net_ntoh16(log_record.flags);
	log_record.data_length = ep_net_ntoh32(log_record.data_length);

	ep_dbg_cprintf(Dbg, 29, "gcl_diskread: recno %" PRIgdp_recno
				", sigmeta 0x%x, dlen %" PRId32 ", offset %" PRId64 "\n",
				log_record.recno, log_record.sigmeta, log_record.data_length,
				xent->offset);

	if (log_record.recno != datum->recno)
		EP_ASSERT_FAILURE("gcl_diskread: recno mismatch: wanted %" PRIgdp_recno
				", got %" PRIgdp_recno,
				datum->recno, log_record.recno);

	datum->recno = log_record.recno;
	memcpy(&datum->ts, &log_record.timestamp, sizeof datum->ts);
	datum->sigmdalg = (log_record.sigmeta >> 12) & 0x000f;
	datum->siglen = log_record.sigmeta & 0x0fff;


	// read data in chunks and add it to the evbuffer
	char read_buffer[GCL_READ_BUFFER_SIZE];
	int64_t data_length = log_record.data_length;

	char *phase = "data";
	while (data_length >= sizeof read_buffer)
	{
		if (fread(read_buffer, sizeof read_buffer, 1, seg->fp) < 1)
			goto fail2;
		gdp_buf_write(datum->dbuf, read_buffer, sizeof read_buffer);
		data_length -= sizeof read_buffer;
	}
	if (data_length > 0)
	{
		if (fread(read_buffer, data_length, 1, seg->fp) < 1)
			goto fail2;
		gdp_buf_write(datum->dbuf, read_buffer, data_length);
	}

	// read signature
	if (datum->siglen > 0)
	{
		phase = "signature";
		if (datum->siglen > sizeof read_buffer)
		{
			fprintf(stderr, "datum->siglen = %d, sizeof read_buffer = %zd\n",
					datum->siglen, sizeof read_buffer);
			EP_ASSERT_INSIST(datum->siglen <= sizeof read_buffer);
		}
		if (datum->sig == NULL)
			datum->sig = gdp_buf_new();
		else
			gdp_buf_reset(datum->sig);
		if (fread(read_buffer, datum->siglen, 1, seg->fp) < 1)
			goto fail2;
		gdp_buf_write(datum->sig, read_buffer, datum->siglen);
	}

	// done

	if (false)
	{
fail2:
		ep_dbg_cprintf(Dbg, 1, "gcl_diskread: %s fread failed: %s\n",
				phase, strerror(errno));
		estat = ep_stat_from_errno(errno);
	}
fail1:
	funlockfile(seg->fp);
fail0:
	ep_thr_rwlock_unlock(&phys->lock);

	return estat;
}

/*
**	GCL_PHYSAPPEND --- append a message to a writable gcl
*/

static EP_STAT
disk_append(gdp_gcl_t *gcl,
			gdp_datum_t *datum)
{
	segment_record_t log_record;
	int64_t record_size = sizeof log_record;
	index_entry_t index_entry;
	size_t dlen;
	gcl_physinfo_t *phys;
	segment_t *seg;
	EP_STAT estat = EP_STAT_OK;

	if (ep_dbg_test(Dbg, 14))
	{
		ep_dbg_printf("disk_append ");
		_gdp_datum_dump(datum, ep_dbg_getfile());
	}

	phys = GETPHYS(gcl);
	EP_ASSERT_POINTER_VALID(phys);
	EP_ASSERT_POINTER_VALID(datum);
	dlen = evbuffer_get_length(datum->dbuf);

	ep_thr_rwlock_wrlock(&phys->lock);

	seg = segment_get(gcl, phys->last_segment);
	estat = segment_open(gcl, seg);
	EP_STAT_CHECK(estat, return estat);

	memset(&log_record, 0, sizeof log_record);
	log_record.recno = ep_net_hton64(datum->recno);
	log_record.timestamp = datum->ts;
	ep_net_hton_timespec(&log_record.timestamp);
	log_record.data_length = ep_net_hton32(dlen);
	log_record.sigmeta = (datum->siglen & 0x0fff) |
				((datum->sigmdalg & 0x000f) << 12);
	log_record.sigmeta = ep_net_hton16(log_record.sigmeta);
	log_record.flags = ep_net_hton16(log_record.flags);

	// write log record header
	fwrite(&log_record, sizeof log_record, 1, seg->fp);

	// write log record data
	if (dlen > 0)
	{
		unsigned char *p = evbuffer_pullup(datum->dbuf, dlen);
		if (p != NULL)
			fwrite(p, dlen, 1, seg->fp);
		record_size += dlen;
	}

	// write signature
	if (datum->sig != NULL)
	{
		size_t slen = evbuffer_get_length(datum->sig);
		unsigned char *p = evbuffer_pullup(datum->sig, slen);

		if (datum->siglen != slen && ep_dbg_test(Dbg, 1))
		{
			ep_dbg_printf("disk_append: datum->siglen = %d, slen = %zd\n",
					datum->siglen, slen);
			if (slen > 0)
				ep_hexdump(p, slen, ep_dbg_getfile(), EP_HEXDUMP_ASCII, 0);
		}
		EP_ASSERT_INSIST(datum->siglen == slen);
		if (slen > 0 && p != NULL)
			fwrite(p, slen, 1, seg->fp);
		record_size += slen;
	}
	else if (datum->siglen > 0)
	{
		// "can't happen"
		ep_app_abort("disk_append: siglen = %d but no signature",
				datum->siglen);
	}

	index_entry.recno = log_record.recno;	// already in net byte order
	index_entry.offset = ep_net_hton64(seg->max_offset);
	index_entry.segment = ep_net_hton32(phys->last_segment);
	index_entry.reserved = 0;

	// write index record
	estat = fseek_to_index_recno(phys, datum->recno, gcl->pname);
	EP_STAT_CHECK(estat, goto fail0);
	fwrite(&index_entry, sizeof index_entry, 1, phys->index.fp);

	// commit
	if (fflush(seg->fp) < 0 || ferror(seg->fp))
		estat = posix_error(errno, "disk_append: cannot flush data");
	else if (fflush(phys->index.fp) < 0 || ferror(seg->fp))
		estat = posix_error(errno, "disk_append: cannot flush index");
	else
	{
		xcache_put(phys, index_entry.recno, index_entry.offset);
		++phys->max_recno;
		phys->index.max_offset += sizeof index_entry;
		seg->max_offset += record_size;
	}

fail0:
	ep_thr_rwlock_unlock(&phys->lock);

	return estat;
}


/*
**  GCL_PHYSGETMETADATA --- read metadata from disk
**
**		This is depressingly similar to _gdp_gclmd_deserialize.
*/

#define STDIOCHECK(tag, targ, f)	\
			do	\
			{	\
				int t = f;	\
				if (t != targ)	\
				{	\
					ep_dbg_cprintf(Dbg, 1,	\
							"%s: stdio failure; expected %d got %d (errno=%d)\n"	\
							"\t%s\n",	\
							tag, targ, t, errno, #f)	\
					goto fail_stdio;	\
				}	\
			} while (0);

static EP_STAT
disk_getmetadata(gdp_gcl_t *gcl,
		gdp_gclmd_t **gmdp)
{
	gdp_gclmd_t *gmd;
	int i;
	size_t tlen;
	gcl_physinfo_t *phys = GETPHYS(gcl);
	segment_t *seg = segment_get(gcl, -1);
	EP_STAT estat = EP_STAT_OK;

	ep_dbg_cprintf(Dbg, 29, "gcl_physgetmetadata: n_md_entries %d\n",
			gcl->x->n_md_entries);

	// allocate and populate the header
	gmd = ep_mem_zalloc(sizeof *gmd);
	gmd->flags = GCLMDF_READONLY;
	gmd->nalloc = gmd->nused = gcl->x->n_md_entries;
	gmd->mds = ep_mem_zalloc(gmd->nalloc * sizeof *gmd->mds);

	// lock the GCL so that no one else seeks around on us
	ep_thr_rwlock_rdlock(&phys->lock);

	// seek to the metadata area
	STDIOCHECK("gcl_physgetmetadata: fseek#0", 0,
			fseek(seg->fp, sizeof (segment_header_t), SEEK_SET));

	// read in the individual metadata headers
	tlen = 0;
	for (i = 0; i < gmd->nused; i++)
	{
		uint32_t t32;

		STDIOCHECK("gcl_physgetmetadata: fread#0", 1,
				fread(&t32, sizeof t32, 1, seg->fp));
		gmd->mds[i].md_id = ep_net_ntoh32(t32);
		STDIOCHECK("gcl_physgetmetadata: fread#1", 1,
				fread(&t32, sizeof t32, 1, seg->fp));
		gmd->mds[i].md_len = ep_net_ntoh32(t32);
		tlen += ep_net_ntoh32(t32);
		ep_dbg_cprintf(Dbg, 34, "\tid = %08x, len = %zd\n",
				gmd->mds[i].md_id, gmd->mds[i].md_len);
	}

	ep_dbg_cprintf(Dbg, 24, "gcl_physgetmetadata: nused = %d, tlen = %zd\n",
			gmd->nused, tlen);

	// now the data
	gmd->databuf = ep_mem_malloc(tlen);
	STDIOCHECK("gcl_physgetmetadata: fread#2", 1,
			fread(gmd->databuf, tlen, 1, seg->fp));

	// now map the pointers to the data
	void *dbuf = gmd->databuf;
	for (i = 0; i < gmd->nused; i++)
	{
		gmd->mds[i].md_data = dbuf;
		dbuf += gmd->mds[i].md_len;
	}

	*gmdp = gmd;

	if (false)
	{
fail_stdio:
		// well that's not very good...
		if (gmd->databuf != NULL)
			ep_mem_free(gmd->databuf);
		ep_mem_free(gmd->mds);
		ep_mem_free(gmd);
		estat = GDP_STAT_CORRUPT_GCL;
	}

	ep_thr_rwlock_unlock(&phys->lock);
	return estat;
}


/*
**  Create a new segment.
*/

#if SEGMENT_SUPPORT
static EP_STAT
disk_newsegment(gdp_gcl_t *gcl)
{
	EP_STAT estat;
	gcl_physinfo_t *phys = GETPHYS(gcl);
	int newsegno = phys->last_segment + 1;
	gdp_gclmd_t *gmd;

	// get the metadata
	estat = disk_getmetadata(gcl, &gmd);
	EP_STAT_CHECK(estat, return estat);

	ep_thr_rwlock_wrlock(&phys->lock);
	estat = segment_create(gcl, gmd, newsegno, phys->max_recno);
	if (EP_STAT_ISOK(estat))
		phys->last_segment = newsegno;
	ep_thr_rwlock_unlock(&phys->lock);

	return estat;
}
#endif // SEGMENT_SUPPORT


/*
**  GCL_PHYSFOREACH --- call function for each GCL in directory
*/

static void
disk_foreach(void (*func)(gdp_name_t, void *), void *ctx)
{
	int subdir;

	for (subdir = 0; subdir < 0x100; subdir++)
	{
		DIR *dir;
		char dbuf[400];

		snprintf(dbuf, sizeof dbuf, "%s/_%02x", GCLDir, subdir);
		dir = opendir(dbuf);
		if (dir == NULL)
			continue;

		for (;;)
		{
			struct dirent dentbuf;
			struct dirent *dent;

			// read the next directory entry
			int i = readdir_r(dir, &dentbuf, &dent);
			if (i != 0)
			{
				ep_log(ep_stat_from_errno(i),
						"gcl_physforeach: readdir_r(%s) failed", dbuf);
				break;
			}
			if (dent == NULL)
				break;

			// we're only interested in .gdpndx files
			char *p = strrchr(dent->d_name, '.');
			if (p == NULL || strcmp(p, GCL_LXF_SUFFIX) != 0)
				continue;

			// strip off the file extension
			*p = '\0';

			// convert the base64-encoded name to internal form
			gdp_name_t gname;
			EP_STAT estat = gdp_internal_name(dent->d_name, gname);
			EP_STAT_CHECK(estat, continue);

			// now call the function
			(*func)((uint8_t *) gname, ctx);
		}
		closedir(dir);
	}
}


/*
**  Deliver statistics for management visualization
*/

static void
disk_getstats(
		gdp_gcl_t *gcl,
		struct gcl_phys_stats *st)
{
	int segno;
	gcl_physinfo_t *phys = gcl->x->physinfo;

	st->nrecs = gcl->nrecs;
	st->size = 0;
	for (segno = 0; segno < phys->last_segment; segno++)
	{
		segment_t *seg = phys->segments[segno];
		if (seg != NULL && seg->fp != NULL)
			st->size += fsizeof(seg->fp);
	}
}


struct gcl_phys_impl	GdpDiskImpl =
{
	.init =			disk_init,
	.read =			disk_read,
	.create =		disk_create,
	.open =			disk_open,
	.close =		disk_close,
	.append =		disk_append,
	.getmetadata =	disk_getmetadata,
#if SEGMENT_SUPPORT
	.newsegment =	disk_newsegment,
#endif
	.foreach =		disk_foreach,
	.getstats =		disk_getstats,
};
