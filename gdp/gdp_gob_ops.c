/* vim: set ai sw=4 sts=4 ts=4 :*/

/*
**	This implements GDP Connection Log (GOB) utilities.
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
#include <ep/ep_prflags.h>
#include <ep/ep_uuid.h>

#include "gdp.h"
#include "gdp_chan.h"		// for PUT64
#include "gdp_event.h"
#include "gdp_md.h"
#include "gdp_priv.h"

#include <event2/event.h>

#include <string.h>
#include <sys/errno.h>

static EP_DBG	Dbg = EP_DBG_INIT("gdp.gob.ops", "GOB operations for GDP");
#if GDP_DEBUG_EXTENDED_TESTS
static EP_DBG	TestDbg = EP_DBG_INIT("test.gdp.gob.ops", "extended testing");
#endif


/*
**	CREATE_GOB_NAME -- create a name for a new GOB
*/

EP_STAT
_gdp_gob_newname(gdp_gob_t *gob)
{
	if (!GDP_GOB_ISGOOD(gob))
		return GDP_STAT_LOG_NOT_OPEN;
	_gdp_newname(gob->name, gob->gob_md);
	gdp_printable_name(gob->name, gob->pname);
	return EP_STAT_OK;
}


/*
**  Get the name of the default creation service.
**		Right now this is compiled in, but in the future it should
**		probably try zeroconf first.
*/

static const char *
get_default_creation_service_name(void)
{
	return GDP_DEFAULT_CREATION_SERVICE;
}


/*
**	_GDP_GOB_CREATE --- create a new GOB
**
**		Creation is a bit tricky, since we don't start with an existing
**		GOB, and we address the message to the desired daemon instead
**		of to the GOB itself.  Some magic needs to occur.
*/

EP_STAT
_gdp_gob_create(
				gdp_md_t *gmd,
				gdp_name_t service_name,
				gdp_gob_t **pgob)
{
	EP_STAT estat = EP_STAT_OK;
	gdp_name_t gobname;
	uint8_t *serialized_md;
	size_t serialized_len;
	uint32_t reqflags = 0;
	gdp_req_t *req = NULL;
	gdp_gob_t *gob = NULL;
	gdp_msg_t *msg;
	gdp_chan_t *chan = _GdpChannel;

	errno = 0;				// avoid spurious messages

	if (!gdp_name_is_valid(service_name))
	{
		const char *p;
		p = ep_adm_getstrparam("swarm.gdp.create.service", NULL);
		if (p == NULL)
			p = get_default_creation_service_name();
		if (p == NULL)
		{
			ep_dbg_cprintf(Dbg, 1, "_gdp_gob_create: no service name\n");
			return GDP_STAT_SVC_NAME_REQ;
		}
		estat = gdp_name_parse(p, service_name, NULL);
		if (!EP_STAT_ISOK(estat))
		{
			ep_dbg_cprintf(Dbg, 1,
					"_gdp_gob_create: cannot parse service name %s\n",
					p);
			return GDP_STAT_SVC_NAME_REQ;
		}
	}

	// compute the name
	estat = _gdp_md_to_gdpname(gmd, &gobname, &serialized_md);
	if (!EP_STAT_ISOK(estat))
	{
		char ebuf[100];
		ep_dbg_cprintf(Dbg, 1, "_gdp_gob_create: _gdp_md_to_gdpname => %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
		return estat;
	}
	serialized_len = EP_STAT_TO_INT(estat);

	if (ep_dbg_test(Dbg, 17))
	{
		gdp_pname_t gxname, dxname;

		ep_dbg_printf("_gdp_gob_create: gob=%s\n\tservice=%s\n",
				gdp_printable_name(gobname, gxname),
				gdp_printable_name(service_name, dxname));
	}

	// create a new pseudo-GOB for the daemon so we can correlate the results
	estat = _gdp_gob_new(service_name, &gob);
	EP_STAT_CHECK(estat, goto fail0);

	// create the request
	_gdp_gob_lock(gob);
	reqflags |= GDP_REQ_ROUTEFAIL;		// don't retry on route failure
	estat = _gdp_req_new(GDP_CMD_CREATE, gob, chan, NULL, reqflags, &req);
	EP_STAT_CHECK(estat, goto fail0);

	msg = req->cpdu->msg;
	EP_ASSERT_ELSE(msg != NULL, return EP_STAT_ASSERT_ABORT);
	{
		GdpMessage__CmdCreate *payload = msg->cmd_create;
		EP_ASSERT_ELSE(payload != NULL, return EP_STAT_ASSERT_ABORT);

		// send the name of the log to be created in the payload
		payload->has_logname = true;
		payload->logname.len = sizeof (gdp_name_t);
		payload->logname.data = (uint8_t *) ep_mem_zalloc(sizeof (gdp_name_t));
		memcpy(payload->logname.data, gobname, sizeof (gdp_name_t));

		// add the metadata to the output stream
		payload->metadata->data.len = serialized_len;
		payload->metadata->data.data = serialized_md;

		// send command and wait for results
		estat = _gdp_invoke(req);
		GDP_GOB_ASSERT_ISLOCKED(gob);
	}
	EP_STAT_CHECK(estat, goto fail0);

	// change GOB name
	(void) memcpy(gob->name, gobname, sizeof (gdp_name_t));

	// add new GOB to cache
	EP_ASSERT(req->gob == gob);
	_gdp_req_unlock(req);		// lock ordering
	_gdp_gob_cache_add(gob);
	_gdp_req_lock(req);			// must be locked for _gdp_req_free

	// free resources and return results
	*pgob = gob;

fail0:
	if (req != NULL)
		_gdp_req_free(&req);
	if (gob != NULL)
	{
		if (!EP_STAT_ISOK(estat))
			_gdp_gob_decref(&gob, false);
		else
			_gdp_gob_unlock(gob);
	}

	if (!EP_STAT_ISOK(estat))
		*pgob = NULL;

	{
		char ebuf[100];

		ep_dbg_cprintf(Dbg, 8, "_gdp_gob_create <<< %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}


static EP_STAT
find_secret_key(gdp_gob_t *gob,
			gdp_open_info_t *open_info)
{
	// We will write the log, and it does have a public key.  We need
	// to find the secret to match it.
	size_t pkbuflen;
	const uint8_t *pkbuf;
	int md_alg_id;
	EP_CRYPTO_KEY *secretkey = NULL;
	bool my_secretkey = false;
	EP_STAT estat;

	// see if we have a public key; if not we're done
	estat = gdp_md_find(gob->gob_md, GDP_MD_WRITERPUBKEY,
				&pkbuflen, (const void **) &pkbuf);
	if (!EP_STAT_ISOK(estat))
	{
		estat = gdp_md_find(gob->gob_md, GDP_MD_OWNERPUBKEY,
					&pkbuflen, (const void **) &pkbuf);
	}
#if GDP_COMPAT_OLD_PUBKEYS
	if (!EP_STAT_ISOK(estat))
	{
		estat = gdp_md_find(gob->gob_md, GDP_MD_PUBKEY,
					&pkbuflen, (const void **) &pkbuf);
	}
#endif
	if (!EP_STAT_ISOK(estat))
	{
		ep_dbg_cprintf(Dbg, 30, "_gdp_gob_open: no public key\n");
		return EP_STAT_OK;
	}

	md_alg_id = pkbuf[0];

	// get the secret key if needed
	if (open_info != NULL)
	{
		secretkey = open_info->signkey;
		if (secretkey == NULL && open_info->signkey_cb != NULL)
		{
			estat = (*open_info->signkey_cb)(gob->name,
							open_info->signkey_udata, &secretkey);
			EP_STAT_CHECK(estat, return estat);
			my_secretkey = true;				// we must deallocate
		}
	}

	// nothing from user; let's try a standard search
	if (secretkey == NULL)
	{
		secretkey = _gdp_crypto_skey_read(gob->pname, "pem");

		if (secretkey == NULL)
		{
			// OK, now we have a problem --- we can't sign
			ep_dbg_cprintf(Dbg, 30, "_gdp_gob_open: no secret key\n");
			return GDP_STAT_SKEY_REQUIRED;
		}

		my_secretkey = true;			// we must deallocate
	}

	// validate the compatibility of the public and secret keys
	{
		EP_CRYPTO_KEY *pubkey = ep_crypto_key_read_mem(pkbuf + 4, pkbuflen - 4,
				EP_CRYPTO_KEYFORM_DER, EP_CRYPTO_F_PUBLIC);

		if (ep_dbg_test(Dbg, 40))
		{
			ep_crypto_key_print(pubkey, ep_dbg_getfile(), EP_CRYPTO_F_PUBLIC);
		}
		estat = ep_crypto_key_compat(secretkey, pubkey);
		ep_crypto_key_free(pubkey);
		if (!EP_STAT_ISOK(estat))
		{
			// XXX: cheat: use internal interface
			(void) _ep_crypto_error(estat,
							"public & secret keys are not compatible");
			return estat;
		}
	}

	// set up the signing context (doubles as a message digest)
	gob->sign_ctx = ep_crypto_sign_new(secretkey, md_alg_id);

	// we can release the key now
	if (my_secretkey)
		ep_crypto_key_free(secretkey);

	if (gob->sign_ctx == NULL)
		return EP_STAT_CRYPTO_DIGEST;

	// add the GOB name to the hashed message digest
	ep_crypto_sign_update(gob->sign_ctx, gob->name, sizeof gob->name);

	// re-serialize the metadata and include it
	{
		uint8_t *mdbuf;
		size_t mdlen;
		mdlen = _gdp_md_serialize(gob->gob_md, &mdbuf);
		ep_crypto_sign_update(gob->sign_ctx, mdbuf, mdlen);
		ep_mem_free(mdbuf);
	}

	// the GOB hash structure now has the fixed part of the hash
	gob->flags |= GOBF_SIGNING;
	return estat;
}


/*
**	_GDP_GOB_OPEN --- open a GOB for reading or further appending
*/

EP_STAT
_gdp_gob_open(gdp_gob_t *gob,
			gdp_cmd_t cmd,
			gdp_open_info_t *open_info,
			gdp_chan_t *chan,
			uint32_t reqflags)
{
	EP_STAT estat = EP_STAT_OK;
	gdp_req_t *req = NULL;
	gdp_msg_t *msg;

	EP_ASSERT_ELSE(GDP_GOB_ISGOOD(gob),
					return EP_STAT_ASSERT_ABORT);
	GDP_GOB_ASSERT_ISLOCKED(gob);

	// send the request across to the log daemon
	errno = 0;				// avoid spurious messages
	reqflags |= GDP_REQ_ALLOC_RID;			// always use a new request id
	reqflags |= GDP_REQ_ROUTEFAIL;			// don't retry on router errors
	estat = _gdp_req_new(cmd, gob, chan, NULL, reqflags, &req);
	EP_STAT_CHECK(estat, goto fail0);

	msg = req->cpdu->msg;
	EP_ASSERT_ELSE(msg != NULL, return EP_STAT_ASSERT_ABORT);

	// nothing actually sent in the payload

	estat = _gdp_invoke(req);
	EP_STAT_CHECK(estat, goto fail0);
	msg = req->rpdu->msg;
	EP_ASSERT(msg != NULL);
	if (msg->body_case != GDP_MESSAGE__BODY_ACK_SUCCESS)
	{
		ep_dbg_cprintf(Dbg, 1,
				"_gdp_gob_open: unexpected response type %d (expected %d)\n",
				msg->body_case,
				GDP_MESSAGE__BODY_ACK_SUCCESS);
		//XXX: estat = ???;
	}
	else
	{
		// success
		GdpMessage__AckSuccess *payload = msg->ack_success;

		// save the number of records
		gob->nrecs = payload->recno;

		// read in the metadata to internal format
		gob->gob_md = _gdp_md_deserialize(payload->metadata.data,
										payload->metadata.len);
	}

	// if we're not going to write, we don't need a secret key
	if (cmd == GDP_CMD_OPEN_AO || cmd == GDP_CMD_OPEN_RA)
		estat = find_secret_key(gob, open_info);

	if (gob->vrfy_ctx == NULL)
	{
		// set up the verification context (or at least the hash digest)
		estat = _gdp_gob_init_vrfy_ctx(gob);
		if (!EP_UT_BITSET(GDP_REQ_VRFY_CONTENT, reqflags))
		{
			// since we aren't verifying, just ignore setup errors
			estat = EP_STAT_OK;
		}
	}

fail0:
	if (req != NULL)
		_gdp_req_free(&req);

	// log failure
	if (EP_STAT_ISOK(estat))
	{
		// success!
		if (ep_dbg_test(Dbg, 30))
		{
			ep_dbg_printf("Opened ");
			_gdp_gob_dump(gob, ep_dbg_getfile(), GDP_PR_DETAILED, 0);
		}
		else
		{
			ep_dbg_cprintf(Dbg, 10, "Opened GOB %s\n", gob->pname);
		}
	}
	else
	{
		char ebuf[100];

		ep_dbg_cprintf(Dbg, 9,
				"Couldn't open GOB %s:\n\t%s\n",
				gob->pname, ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}


/*
**	_GDP_GOB_CLOSE --- share operation for closing a GOB handle
*/

EP_STAT
_gdp_gob_close(gdp_gob_t *gob,
			gdp_chan_t *chan,
			uint32_t reqflags)
{
	EP_STAT estat = EP_STAT_OK;
	gdp_req_t *req;

	if (ep_dbg_test(Dbg, 38))
	{
		ep_dbg_printf("_gdp_gob_close: ");
		_gdp_gob_dump(gob, ep_dbg_getfile(), GDP_PR_DETAILED, 0);
	}

	errno = 0;				// avoid spurious messages
	estat = _gdp_req_new(GDP_CMD_CLOSE, gob, chan, NULL, reqflags, &req);
	EP_STAT_CHECK(estat, return estat);

	// tell the daemon to close it
	estat = _gdp_invoke(req);

	_gdp_req_free(&req);
	return estat;
}


/*
**	_GDP_GOB_DELETE --- close and delete a GOB
*/

EP_STAT
_gdp_gob_delete(gdp_gob_t *gob,
			gdp_chan_t *chan,
			uint32_t reqflags)
{
	EP_STAT estat = EP_STAT_OK;
	gdp_req_t *req;

	if (ep_dbg_test(Dbg, 38))
	{
		ep_dbg_printf("_gdp_gob_delete: ");
		_gdp_gob_dump(gob, ep_dbg_getfile(), GDP_PR_DETAILED, 0);
	}

	errno = 0;				// avoid spurious messages
	estat = _gdp_req_new(GDP_CMD_DELETE, gob, chan, NULL, reqflags, &req);
	EP_STAT_CHECK(estat, return estat);

	// tell the daemon to delete it
	estat = _gdp_invoke(req);

	if (EP_STAT_ISOK(estat))
	{
		// invalidate gob regardless of reference count (but leave memory)
		gob->flags |= GOBF_DROPPING;
	}

	_gdp_req_free(&req);
	return estat;
}


/*
**  APPEND_COMMON --- common code for sync and async appends
**
**		This produces a filled-in APPEND request.  Can append
**		multiple records on each call.
**
**		datums should be locked when called.
**		req will be locked upon return.
**
**		The caller must provide the hash of the previous record in
**		the chain.  The individual records will be linked into the
**		hash chain.  The final record in this call will be signed.
*/

static EP_STAT
append_common(
		gdp_gob_t *gob,				// the receiving GOB
		gdp_gin_t *gin,				// associated GIN, may be NULL
		int n_datums,				// number of datums in this chain
		gdp_datum_t **datums,		// the actual datums
		gdp_hash_t *prevhash,		// hash of record -1
		gdp_chan_t *chan,			// the I/O channel on which to send
		uint32_t reqflags,			// special flags to tweak operations
		gdp_req_t **reqp)			// O: the new request
{
	EP_STAT estat = GDP_STAT_BAD_IOMODE;
	gdp_req_t *req;
	gdp_msg_t *msg;

	errno = 0;				// avoid spurious messages

	if (!GDP_GOB_ISGOOD(gob))
		return GDP_STAT_LOG_NOT_OPEN;
	if (!EP_ASSERT(n_datums > 0))
	{
		ep_dbg_cprintf(Dbg, 1, "append_common: need at least one datum\n");
		return GDP_STAT_DATUM_REQUIRED;
	}
	EP_ASSERT_POINTER_VALID(datums);

	// set up datums with appropriate back links
	int dno;
	gdp_recno_t recno = gob->nrecs + 1;
	gdp_datum_t *datum;
	for (dno = 0; dno < n_datums; dno++)
	{
		datum = datums[dno];
		if (!GDP_DATUM_ISGOOD(datum))
		{
			ep_dbg_cprintf(Dbg, 1, "append_common: datum %d invalid\n", dno);
			return GDP_STAT_DATUM_REQUIRED;
		}

		// fill in datum information
		datum->recno = recno++;		// assumes records written in order with no gaps
		if (!EP_TIME_IS_VALID(&datum->ts))
			ep_time_now(&datum->ts);
		if (datum->prevhash != NULL)
			gdp_hash_free(datum->prevhash);
		datum->prevhash = prevhash;
		prevhash = _gdp_datum_hash(datum, gob);
	}

	// compute the signature on the final record in the chain
	// (secret key and initial digest is already in the gob)
	if (EP_UT_BITSET(GOBF_SIGNING, gob->flags))
	{
		estat = _gdp_datum_sign(datum, gob);
		if (!EP_STAT_ISOK(estat) && ep_dbg_test(Dbg, 1))
		{
			char ebuf[100];
			ep_dbg_printf("append_common: _gdp_datum_sign => %s\n",
					ep_stat_tostr(estat, ebuf, sizeof ebuf));
		}
#if GDP_DEBUG_EXTENDED_TESTS
		if (ep_dbg_test(TestDbg, 101))
		{
			// Change the datum after computing signature.
			// Should cause the append to be rejected.
			gdp_buf_getptr(datum->dbuf, 1)[0]++;	//TEST
		}
#endif
	}

	// create a new request structure
	estat = _gdp_req_new(GDP_CMD_APPEND, gob, chan, NULL, reqflags, reqp);
	EP_STAT_CHECK(estat, goto fail0);
	req = *reqp;
	req->gin = gin;

	// set up the message content
	msg = req->cpdu->msg;
	EP_ASSERT_ELSE(msg != NULL, return EP_STAT_ASSERT_ABORT);
	{
		GdpMessage__CmdAppend *payload = msg->cmd_append;
		EP_ASSERT_ELSE(payload != NULL, return EP_STAT_ASSERT_ABORT);

		payload->dl->n_d = n_datums;
		payload->dl->d = (GdpDatum **) ep_mem_zalloc(n_datums * sizeof *datums);
		for (dno = 0; dno < n_datums; dno++)
		{
			datum = datums[dno];
			if (ep_dbg_test(Dbg, 44))
			{
				ep_dbg_printf("append_common: _gdp_datum_to_pb: ");
				_gdp_datum_dump(datum, NULL);
			}
			payload->dl->d[dno] = (GdpDatum *) ep_mem_zalloc(sizeof (GdpDatum));
			gdp_datum__init(payload->dl->d[dno]);
			_gdp_datum_to_pb(datum, msg, payload->dl->d[dno]);
		}
	}

	// Note that this is just a guess: the append may still fail,
	// but we need to do this if there are multiple threads appending
	// at the same time.
	// If the append fails, we'll be out of sync and all hell breaks loose.
	gob->nrecs += n_datums;

fail0:
	return estat;
}


/*
**  _GDP_GOB_APPEND_SYNC --- shared operation for synchronous append to a GOB
*/

EP_STAT
_gdp_gob_append_sync(
			gdp_gob_t *gob,
			int n_datums,
			gdp_datum_t **datums,
			gdp_hash_t *prevhash,
			gdp_chan_t *chan,
			uint32_t reqflags)
{
	EP_STAT estat = GDP_STAT_BAD_IOMODE;
	gdp_req_t *req = NULL;

	estat = append_common(gob, NULL, n_datums, datums, prevhash,
						chan, reqflags, &req);
	EP_STAT_CHECK(estat, goto fail0);

	// send the request to the log server and wait for response
	estat = _gdp_invoke(req);
	EP_STAT_CHECK(estat, goto fail1);

	// collect results (currently no results of interest)

fail1:
	_gdp_req_free(&req);
fail0:
	if (ep_dbg_test(Dbg, 42))
	{
		ep_dbg_printf("_gdp_gob_append: returning ");
		int dno;
		for (dno = 0; dno < n_datums; dno++)
			gdp_datum_print(datums[dno], ep_dbg_getfile(), GDP_DATUM_PRDEBUG);
	}
	return estat;
}


/*
**  _GDP_GOB_APPEND_ASYNC --- asynchronous append
*/

#ifndef SIZE_MAX
# define SIZE_MAX ((size_t) -1)
#endif

EP_STAT
_gdp_gob_append_async(
			gdp_gob_t *gob,
			gdp_gin_t *gin,
			int n_datums,
			gdp_datum_t **datums,
			gdp_hash_t *prevhash,
			gdp_event_cbfunc_t cbfunc,
			void *cbarg,
			gdp_chan_t *chan,
			uint32_t reqflags)
{
	EP_STAT estat;
	gdp_req_t *req = NULL;

	// deliver results asynchronously
	reqflags |= GDP_REQ_ASYNCIO;
	estat = append_common(gob, gin, n_datums, datums, prevhash,
						chan, reqflags, &req);
	EP_STAT_CHECK(estat, goto fail0);

	// arrange for responses to appear as events or callbacks
	_gdp_event_setcb(req, cbfunc, cbarg);

	estat = _gdp_req_send(req);

	int dno;
	for (dno = 0; dno < n_datums; dno++)
	{
		gdp_buf_reset(datums[dno]->dbuf);
		if (datums[dno]->sig != NULL)
			gdp_sig_reset(datums[dno]->sig);
	}

	// cleanup and return
	if (!EP_STAT_ISOK(estat))
	{
		_gdp_req_free(&req);
	}
	else
	{
		req->state = GDP_REQ_IDLE;
		ep_thr_cond_signal(&req->cond);
		_gdp_req_unlock(req);
	}
fail0:
	if (ep_dbg_test(Dbg, 11))
	{
		char ebuf[100];
		ep_dbg_printf("_gdp_gob_append_async => %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}


/*
**  _GDP_GOB_READ_BY_RECNO --- read a record from a GOB by record number
**
**  Only returns a single record; if multiple records match, the choice
**  is random.  Use the async version to get all records that match.
**
**		Parameters:
**			gob --- the gob from which to read
**			recno --- the starting record number
**			chan --- the data channel used to contact the remote
**			reqflags --- flags for the request
**			datum --- the data buffer (to avoid dynamic memory)
**
**		This might be read by recno or read by timestamp based on
**		the command.  In any case the cmd is the defining factor.
*/

EP_STAT
_gdp_gob_read_by_recno(gdp_gob_t *gob,
			gdp_recno_t recno,
			gdp_chan_t *chan,
			uint32_t reqflags,
			gdp_datum_t *datum)
{
	EP_STAT estat = GDP_STAT_BAD_IOMODE;
	gdp_req_t *req;

	errno = 0;				// avoid spurious messages

	// sanity checks
	if (!GDP_GOB_ISGOOD(gob))
		return GDP_STAT_LOG_NOT_OPEN;
	if (!GDP_DATUM_ISGOOD(datum))
		return GDP_STAT_DATUM_REQUIRED;
	EP_ASSERT_ELSE(datum->inuse, return EP_STAT_ASSERT_ABORT);

	// create and send a new request
	estat = _gdp_req_new(GDP_CMD_READ_BY_RECNO, gob, chan, NULL, reqflags, &req);
	EP_STAT_CHECK(estat, goto fail0);

	// create the command payload
	{
		gdp_msg_t *msg = req->cpdu->msg;
		EP_ASSERT_ELSE(msg != NULL, return EP_STAT_ASSERT_ABORT);
		GdpMessage__CmdReadByRecno *payload = msg->cmd_read_by_recno;
		EP_ASSERT_ELSE(payload != NULL, return EP_STAT_ASSERT_ABORT);
		payload->recno = recno;
		payload->nrecs = 0;
		payload->has_nrecs = true;
	}

	estat = _gdp_invoke(req);
	EP_STAT_CHECK(estat, goto fail1);

	// parse the response payload
	gdp_msg_t *msg = req->rpdu->msg;
	EP_ASSERT_ELSE(msg != NULL, return EP_STAT_ASSERT_ABORT);
	GdpMessage__AckContent *payload = msg->ack_content;
	EP_ASSERT_ELSE(payload != NULL, return EP_STAT_ASSERT_ABORT);

	// make sure there really is a record there
	if (payload->dl->n_d < 1)
	{
		ep_dbg_cprintf(Dbg, 1, "_gdp_gob_read_by_recno: no data\n");
		estat = GDP_STAT_RECORD_MISSING;
	}
	else
	{
		// ok, done!  pass the datum contents to the caller and free the request
		_gdp_datum_from_pb(datum, payload->dl->d[0], msg->sig);
	}

fail1:
	_gdp_req_free(&req);
fail0:
	return estat;
}


/*
**  _GDP_GOB_READ_BY_RECNO_ASYNC --- asynchronously read records from a GOB
**
**		Parameters:
**			gob --- the gob from which to read
**			recno --- the record number to read
**			nrecs --- the number of records to read
**			cbfunc --- the callback function (NULL => deliver as events)
**			cbarg --- user argument to cbfunc
**			chan --- the data channel used to contact the remote
**
**		This might be read by recno or read by timestamp based on
**		the command.  In any case the cmd is the defining factor.
*/

EP_STAT
_gdp_gob_read_by_recno_async(
			gdp_gob_t *gob,
			gdp_gin_t *gin,
			gdp_recno_t recno,
			uint32_t nrecs,
			gdp_event_cbfunc_t cbfunc,
			void *cbarg,
			gdp_chan_t *chan)
{
	EP_STAT estat;
	gdp_req_t *req;
	gdp_msg_t *msg;
	uint32_t reqflags = GDP_REQ_ASYNCIO | GDP_REQ_PERSIST;

	errno = 0;				// avoid spurious messages

	// sanity checks
	if (!GDP_GOB_ISGOOD(gob))
		return GDP_STAT_LOG_NOT_OPEN;

	// create a new READ request (don't need a special command)
	if (EP_UT_BITSET(GINF_SIG_VRFY, gin->flags))
		reqflags |= GDP_REQ_VRFY_CONTENT;
	estat = _gdp_req_new(GDP_CMD_READ_BY_RECNO, gob, chan,
						NULL, reqflags, &req);
	EP_STAT_CHECK(estat, return estat);
	req->gin = gin;

	msg = req->cpdu->msg;
	EP_ASSERT_ELSE(msg != NULL, return EP_STAT_ASSERT_ABORT);
	GdpMessage__CmdReadByRecno *payload = msg->cmd_read_by_recno;
	EP_ASSERT_ELSE(payload != NULL, return EP_STAT_ASSERT_ABORT);
	payload->recno = recno;
	if (nrecs > 0)
	{
		payload->nrecs = nrecs;
		payload->has_nrecs = true;
	}

	// arrange for responses to appear as events or callbacks
	_gdp_event_setcb(req, cbfunc, cbarg);

	estat = _gdp_req_send(req);

	if (EP_STAT_ISOK(estat))
	{
		req->state = GDP_REQ_IDLE;
		_gdp_req_unlock(req);
	}
	else
	{
		_gdp_req_free(&req);
	}

	// ok, done!
	return estat;
}


/*
**  _GDP_GOB_GETMETADATA --- return metadata for a log
*/

EP_STAT
_gdp_gob_getmetadata(gdp_gob_t *gob,
		gdp_md_t **gmdp,
		gdp_chan_t *chan,
		uint32_t reqflags)
{
	EP_STAT estat;
	gdp_req_t *req;
	gdp_msg_t *msg;

	if (!GDP_GOB_ISGOOD(gob))
		return GDP_STAT_LOG_NOT_OPEN;

	errno = 0;				// avoid spurious messages
	estat = _gdp_req_new(GDP_CMD_GETMETADATA, gob, chan, NULL, reqflags, &req);
	EP_STAT_CHECK(estat, goto fail0);

	estat = _gdp_invoke(req);
	EP_STAT_CHECK(estat, goto fail1);

	msg = req->rpdu->msg;
	EP_ASSERT(msg != NULL);
	if (msg->body_case !=
			GDP_MESSAGE__BODY_ACK_SUCCESS)
	{
		ep_dbg_cprintf(Dbg, 1,
				"_gdp_gob_open: unexpected response type %d (expected %d)\n",
				msg->body_case,
				GDP_MESSAGE__BODY_ACK_SUCCESS);
		//estat = ???;
	}
	else
	{
		// success
		GdpMessage__AckSuccess *payload = msg->ack_success;

		// read in the metadata to internal format
		gob->gob_md = _gdp_md_deserialize(payload->metadata.data,
										payload->metadata.len);
	}

	*gmdp = gob->gob_md;

fail1:
	_gdp_req_free(&req);

fail0:
	return estat;
}


/*
**  Initialize signature verification context.
**
**		Returns GDP_STAT_CRYPTO_NO_PUB_KEY if there is no public key
**		associated with this log.  However, even if no public key is
**		found, the message digest is set up since it is used in
**		other places such as the hash chain.
**
**		If there is a public key, it also sets the GOBF_VERIFYING flag
**		for use down the line.
*/

EP_STAT
_gdp_gob_init_vrfy_ctx(gdp_gob_t *gob)
{
	EP_STAT estat;
	size_t pklen;
	uint8_t *pkbuf;
	int pktype;
	int mdtype = gob->hashalg;
	EP_CRYPTO_KEY *key;

	// assuming we have a public key, set up the message digest context
	if (gob->gob_md == NULL)
		goto nopubkey;
	estat = gdp_md_find(gob->gob_md, GDP_MD_PUBKEY, &pklen,
					(const void **) &pkbuf);
	if (!EP_STAT_ISOK(estat) || pklen < 5)
		goto nopubkey;

	mdtype = pkbuf[0];
	pktype = pkbuf[1];
	ep_dbg_cprintf(Dbg, 40,
				"init_vrfy_ctx: mdtype=%d, pktype=%d, pklen=%zd\n",
				mdtype, pktype, pklen);
	key = ep_crypto_key_read_mem(pkbuf + 4, pklen - 4,
			EP_CRYPTO_KEYFORM_DER, EP_CRYPTO_F_PUBLIC);
	if (key != NULL)
	{
		gob->vrfy_ctx = ep_crypto_vrfy_new(key, mdtype);
		gob->flags |= GOBF_VERIFYING;
	}
	else
	{
nopubkey:
		ep_dbg_cprintf(Dbg, 30,
					"init_vrfy_ctx: no public key for %s\n",
					gob->pname);
		estat = GDP_STAT_CRYPTO_NO_PUB_KEY;

		// still need to compute the digest for the hash chain
		gob->vrfy_ctx = ep_crypto_md_new(mdtype);
	}

	// include the GOB name
	ep_crypto_md_update(gob->vrfy_ctx, gob->name, sizeof gob->name);

	// and the metadata (re-serialized)
	// NOTE:  this will not be needed once the GOB name becomes the
	// NOTE:  hash of the metadata.
	uint8_t *mdbuf;
	size_t mdlen = _gdp_md_serialize(gob->gob_md, &mdbuf);
	ep_crypto_md_update(gob->vrfy_ctx, mdbuf, mdlen);
	ep_mem_free(mdbuf);

	char ebuf[100];
	ep_dbg_cprintf(Dbg, 40,
				"init_vrfy_ctx: %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
	return estat;
}


/***********************************************************************
**  Client side implementations for commands used internally only.
***********************************************************************/

#if 0	//XXX this should probably be done at Layer 4
/*
**  _GDP_GOB_FWD_APPEND --- forward APPEND command
**
**		Forwards an APPEND command to a different server.  This is one
**		of the few commands that is directed directly to a gdplogd instance.
**		However, the response is going to come back from the original
**		GOB, not the gdplogd instance, so we arrange for the request to
**		be linked on that GOB's chain.
**
**		Note: Unlike other calls, the datum is not cleared.  This is
**		because we expect this to be used multiple times on a single
**		datum.  When all copies are sent, the caller must call
**		gdp_buf_drain(datum, gdp_buf_getlength(datum)).
*/

EP_STAT
_gdp_gob_fwd_append(
		gdp_gob_t *gob,
		gdp_datum_t *datum,
		gdp_name_t to_server,
		gdp_event_cbfunc_t cbfunc,
		void *cbarg,
		gdp_chan_t *chan,
		uint32_t reqflags)
{
	EP_STAT estat;
	gdp_req_t *req;

	// sanity checks
	if (!GDP_GOB_ISGOOD(gob))
		return GDP_STAT_LOG_NOT_OPEN;
	if (GDP_NAME_SAME(to_server, _GdpMyRoutingName))
	{
		// forwarding to ourselves: bad idea
		EP_ASSERT_PRINT("_gdp_gob_fwd_append: forwarding to myself");
		return EP_STAT_ASSERT_ABORT;
	}

	// deliver results asynchronously
	reqflags |= GDP_REQ_ASYNCIO | GDP_REQ_PERSIST;

	_gdp_gob_lock(gob);
	estat = _gdp_req_new(GDP_CMD_FWD_APPEND, gob, chan, NULL, reqflags, &req);
	EP_STAT_CHECK(estat, goto fail0);

	// arrange for responses to appear as events or callbacks
	_gdp_event_setcb(req, cbfunc, cbarg);

	// add the actual target GDP name to the data
	gdp_buf_write(req->cpdu->datum->dbuf, req->cpdu->dst, sizeof req->cpdu->dst);

	// change the destination to be the final server, not the GOB
	memcpy(req->cpdu->dst, to_server, sizeof req->cpdu->dst);

	// copy the existing datum, including metadata
	size_t l = gdp_buf_getlength(datum->dbuf);
	gdp_buf_write(req->cpdu->datum->dbuf, gdp_buf_getptr(datum->dbuf, l), l);
	req->cpdu->datum->recno = datum->recno;
	req->cpdu->datum->ts = datum->ts;
	req->cpdu->datum->mdalg = datum->mdalg;
	req->cpdu->datum->siglen = datum->siglen;
	if (req->cpdu->datum->sig != NULL)
		gdp_buf_free(req->cpdu->datum->sig);
	req->cpdu->datum->sig = NULL;
	if (datum->sig != NULL)
	{
		l = gdp_buf_getlength(datum->sig);
		req->cpdu->datum->sig = gdp_buf_new();
		gdp_buf_write(req->cpdu->datum->sig, gdp_buf_getptr(datum->sig, l), l);
	}

	// XXX should we take a callback function?

	estat = _gdp_req_send(req);

	// unlike append_async, we leave the datum intact

	// cleanup
	req->cpdu->datum = NULL;			// owned by caller
	if (!EP_STAT_ISOK(estat))
	{
		_gdp_req_free(&req);
	}
	else
	{
		req->state = GDP_REQ_IDLE;
		ep_thr_cond_signal(&req->cond);
		_gdp_req_unlock(req);
	}

fail0:
	_gdp_gob_unlock(gob);
	if (ep_dbg_test(Dbg, 11))
	{
		char ebuf[100];
		ep_dbg_printf("_gdp_gob_fwd_append => %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}

#endif
