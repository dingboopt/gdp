/* vim: set ai sw=4 sts=4 ts=4 :*/

#include "gdp.h"
#include "gdp_event.h"
#include "gdp_stat.h"
#include "gdp_priv.h"

#include <ep/ep.h>
#include <ep/ep_app.h>
#include <ep/ep_b64.h>
#include <ep/ep_dbg.h>
#include <ep/ep_hash.h>
#include <ep/ep_log.h>
#include <ep/ep_prflags.h>
#include <ep/ep_string.h>
#include <ep/ep_thr.h>

#include <event2/event.h>
#include <event2/thread.h>

#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/*
**	This implements the GDP Protocol.
**
**	In the future this may need to be extended to have knowledge of
**	TSN/AVB, but for now we don't worry about that.
*/

/************************  PRIVATE	************************/

static EP_DBG	Dbg = EP_DBG_INIT("gdp.proto", "GDP protocol processing");


/*
**   GDP_REQ_SEND --- send a request to the GDP daemon
**
**		This makes no attempt to read results.
*/

EP_STAT
_gdp_req_send(gdp_req_t *req)
{
	EP_STAT estat;
	gdp_gcl_t *gcl = req->gcl;

	ep_dbg_cprintf(Dbg, 45, "gdp_req_send: cmd=%d (%s), req=%p\n",
			req->pdu->cmd, _gdp_proto_cmd_name(req->pdu->cmd), req);
	EP_ASSERT(gcl != NULL);

	// link the request to the GCL
	ep_thr_mutex_lock(&gcl->mutex);
	EP_ASSERT(!req->ongcllist);
	LIST_INSERT_HEAD(&gcl->reqs, req, gcllist);
	req->ongcllist = true;
	ep_thr_mutex_unlock(&gcl->mutex);

	// register this handle so we can process the results
	//		(it's likely that it's already in the cache)
	_gdp_gcl_cache_add(gcl, 0);

	// write the message out
	estat = _gdp_pdu_out(req->pdu, req->chan);

	// done
	return estat;
}



/*
**	GDP_INVOKE --- do a remote invocation to the GDP daemon
**
**		This acts as an remote procedure call.  In particular it
**		waits to get a result, which makes it inappropriate for
**		instances where multiple commands are in flight, or
**		where a command can return multiple values (e.g., subscribe).
*/

EP_STAT
_gdp_invoke(gdp_req_t *req)
{
	EP_STAT estat;

	EP_ASSERT_POINTER_VALID(req);
	if (ep_dbg_test(Dbg, 22))
	{
		ep_dbg_printf("gdp_invoke(%p): cmd=%d (%s)\n    gcl@%p: ",
				req,
				req->pdu->cmd,
				_gdp_proto_cmd_name(req->pdu->cmd),
				req->gcl);
		gdp_datum_print(req->pdu->datum, ep_dbg_getfile());
	}

	/*
	**  Top Half: sending the command
	*/

	estat = _gdp_req_send(req);
	EP_STAT_CHECK(estat, goto fail0);

	/*
	**  Bottom Half: read the response
	*/

	// wait until we receive a result
	//		XXX need a timeout here
	ep_dbg_cprintf(Dbg, 37, "gdp_invoke: waiting on %p\n", &req->cond);
	ep_thr_mutex_lock(&req->mutex);
	while (!EP_UT_BITSET(GDP_REQ_DONE, req->flags))
	{
		ep_thr_cond_wait(&req->cond, &req->mutex, NULL);
	}
	estat = req->stat;
	// mutex is released below

	//XXX what status will/should we return?
	if (event_base_got_exit(GdpIoEventBase))
	{
		ep_dbg_cprintf(Dbg, 1, "gdp_invoke: exiting on loopexit\n");
		estat = GDP_STAT_INTERNAL_ERROR;
	}

	// ok, done!
	ep_thr_mutex_unlock(&req->mutex);

fail0:
	if (ep_dbg_test(Dbg, 22))
	{
		char ebuf[200];

		ep_dbg_printf("gdp_invoke: returning %s\n  ",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
		_gdp_req_dump(req, ep_dbg_getfile());
	}
	return estat;
}



/***********************************************************************
**
**	Protocol processing (CMD/ACK/NAK)
**
**		All of these take as parameters:
**			req --- the request information (including packet header)
**
**		They can return GDP_STAT_KEEP_READING to tell the upper
**		layer that the whole packet hasn't been read yet.
**
***********************************************************************/


/*
**  ACKs (success)
*/

static EP_STAT
ack_success(gdp_req_t *req)
{
	EP_STAT estat;
	gdp_gcl_t *gcl;

	// we require a request
	estat = GDP_STAT_PROTOCOL_FAIL;
	if (req == NULL)
		ep_log(estat, "ack_success: null request");
	else if (req->pdu->datum == NULL)
		ep_log(estat, "ack_success: null datum");
	else
		estat = GDP_STAT_FROM_ACK(req->pdu->cmd);
	EP_STAT_CHECK(estat, goto fail0);

	gcl = req->gcl;

	//	If we started with no gcl id, adopt from incoming packet.
	//	This can happen when creating a GCL.
	if (gcl != NULL && !gdp_name_is_valid(gcl->name))
	{
		memcpy(gcl->name, req->pdu->src, sizeof gcl->name);
		gdp_printable_name(gcl->name, gcl->pname);
	}

fail0:
	return estat;
}


/*
**  NAK_CLIENT, NAK_SERVER --- handle NAKs (negative acknowlegements)
*/

static EP_STAT
nak_client(gdp_req_t *req)
{
	ep_dbg_cprintf(Dbg, 2, "nak_client: received %d\n", req->pdu->cmd);
	return GDP_STAT_FROM_C_NAK(req->pdu->cmd);
}


static EP_STAT
nak_server(gdp_req_t *req)
{
	ep_dbg_cprintf(Dbg, 2, "nak_server: received %d\n", req->pdu->cmd);
	return GDP_STAT_FROM_S_NAK(req->pdu->cmd);
}



/*
**	Command/Ack/Nak Dispatch Table
*/

typedef struct
{
	cmdfunc_t	*func;		// function to call
	const char	*name;		// name of command (for debugging)
} dispatch_ent_t;

#define NOENT		{ NULL, NULL }

static dispatch_ent_t	DispatchTable[256] =
{
	{ NULL,				"CMD_KEEPALIVE" },			// 0
	NOENT,				// 1
	NOENT,				// 2
	NOENT,				// 3
	NOENT,				// 4
	NOENT,				// 5
	NOENT,				// 6
	NOENT,				// 7
	NOENT,				// 8
	NOENT,				// 9
	NOENT,				// 10
	NOENT,				// 11
	NOENT,				// 12
	NOENT,				// 13
	NOENT,				// 14
	NOENT,				// 15
	NOENT,				// 16
	NOENT,				// 17
	NOENT,				// 18
	NOENT,				// 19
	NOENT,				// 20
	NOENT,				// 21
	NOENT,				// 22
	NOENT,				// 23
	NOENT,				// 24
	NOENT,				// 25
	NOENT,				// 26
	NOENT,				// 27
	NOENT,				// 28
	NOENT,				// 29
	NOENT,				// 30
	NOENT,				// 31
	NOENT,				// 32
	NOENT,				// 33
	NOENT,				// 34
	NOENT,				// 35
	NOENT,				// 36
	NOENT,				// 37
	NOENT,				// 38
	NOENT,				// 39
	NOENT,				// 40
	NOENT,				// 41
	NOENT,				// 42
	NOENT,				// 43
	NOENT,				// 44
	NOENT,				// 45
	NOENT,				// 46
	NOENT,				// 47
	NOENT,				// 48
	NOENT,				// 49
	NOENT,				// 50
	NOENT,				// 51
	NOENT,				// 52
	NOENT,				// 53
	NOENT,				// 54
	NOENT,				// 55
	NOENT,				// 56
	NOENT,				// 57
	NOENT,				// 58
	NOENT,				// 59
	NOENT,				// 60
	NOENT,				// 61
	NOENT,				// 62
	NOENT,				// 63
	{ NULL,				"CMD_PING"				},			// 64
	{ NULL,				"CMD_HELLO"				},			// 65
	{ NULL,				"CMD_CREATE"			},			// 66
	{ NULL,				"CMD_OPEN_AO"			},			// 67
	{ NULL,				"CMD_OPEN_RO"			},			// 68
	{ NULL,				"CMD_CLOSE"				},			// 69
	{ NULL,				"CMD_READ"				},			// 70
	{ NULL,				"CMD_PUBLISH"			},			// 71
	{ NULL,				"CMD_SUBSCRIBE"			},			// 72
	{ NULL,				"CMD_MULTIREAD",		},			// 73
	{ NULL,				"CMD_GETMETADATA",		},			// 74
	NOENT,				// 75
	NOENT,				// 76
	NOENT,				// 77
	NOENT,				// 78
	NOENT,				// 79
	NOENT,				// 80
	NOENT,				// 81
	NOENT,				// 82
	NOENT,				// 83
	NOENT,				// 84
	NOENT,				// 85
	NOENT,				// 86
	NOENT,				// 87
	NOENT,				// 88
	NOENT,				// 89
	NOENT,				// 90
	NOENT,				// 91
	NOENT,				// 92
	NOENT,				// 93
	NOENT,				// 94
	NOENT,				// 95
	NOENT,				// 96
	NOENT,				// 97
	NOENT,				// 98
	NOENT,				// 99
	NOENT,				// 100
	NOENT,				// 101
	NOENT,				// 102
	NOENT,				// 103
	NOENT,				// 104
	NOENT,				// 105
	NOENT,				// 106
	NOENT,				// 107
	NOENT,				// 108
	NOENT,				// 109
	NOENT,				// 110
	NOENT,				// 111
	NOENT,				// 112
	NOENT,				// 113
	NOENT,				// 114
	NOENT,				// 115
	NOENT,				// 116
	NOENT,				// 117
	NOENT,				// 118
	NOENT,				// 119
	NOENT,				// 120
	NOENT,				// 121
	NOENT,				// 122
	NOENT,				// 123
	NOENT,				// 124
	NOENT,				// 125
	NOENT,				// 126
	NOENT,				// 127
	{ ack_success,		"ACK_SUCCESS"			},			// 128
	{ ack_success,		"ACK_DATA_CREATED"		},			// 129
	{ ack_success,		"ACK_DATA_DEL"			},			// 130
	{ ack_success,		"ACK_DATA_VALID"		},			// 131
	{ ack_success,		"ACK_DATA_CHANGED"		},			// 132
	{ ack_success,		"ACK_DATA_CONTENT"		},			// 133
	NOENT,				// 134
	NOENT,				// 135
	NOENT,				// 136
	NOENT,				// 137
	NOENT,				// 138
	NOENT,				// 139
	NOENT,				// 140
	NOENT,				// 141
	NOENT,				// 142
	NOENT,				// 143
	NOENT,				// 144
	NOENT,				// 145
	NOENT,				// 146
	NOENT,				// 147
	NOENT,				// 148
	NOENT,				// 149
	NOENT,				// 150
	NOENT,				// 151
	NOENT,				// 152
	NOENT,				// 153
	NOENT,				// 154
	NOENT,				// 155
	NOENT,				// 156
	NOENT,				// 157
	NOENT,				// 158
	NOENT,				// 159
	NOENT,				// 160
	NOENT,				// 161
	NOENT,				// 162
	NOENT,				// 163
	NOENT,				// 164
	NOENT,				// 165
	NOENT,				// 166
	NOENT,				// 167
	NOENT,				// 168
	NOENT,				// 169
	NOENT,				// 170
	NOENT,				// 171
	NOENT,				// 172
	NOENT,				// 173
	NOENT,				// 174
	NOENT,				// 175
	NOENT,				// 176
	NOENT,				// 177
	NOENT,				// 178
	NOENT,				// 179
	NOENT,				// 180
	NOENT,				// 181
	NOENT,				// 182
	NOENT,				// 183
	NOENT,				// 184
	NOENT,				// 185
	NOENT,				// 186
	NOENT,				// 187
	NOENT,				// 188
	NOENT,				// 189
	NOENT,				// 190
	NOENT,				// 191
	{ nak_client,		"NAK_C_BADREQ"			},			// 192
	{ nak_client,		"NAK_C_UNAUTH"			},			// 193
	{ nak_client,		"NAK_C_BADOPT"			},			// 194
	{ nak_client,		"NAK_C_FORBIDDEN"		},			// 195
	{ nak_client,		"NAK_C_NOTFOUND"		},			// 196
	{ nak_client,		"NAK_C_METHNOTALLOWED"	},			// 197
	{ nak_client,		"NAK_C_NOTACCEPTABLE"	},			// 198
	NOENT,				// 199
	NOENT,				// 200
	{ nak_client,		"NAK_C_CONFLICT"		},			// 201
	NOENT,				// 202
	NOENT,				// 203
	{ nak_client,		"NAK_C_PRECONFAILED"	},			// 204
	{ nak_client,		"NAK_C_TOOLARGE"		},			// 205
	NOENT,				// 206
	{ nak_client,		"NAK_C_UNSUPMEDIA"		},			// 207
	NOENT,				// 208
	NOENT,				// 209
	NOENT,				// 210
	NOENT,				// 211
	NOENT,				// 212
	NOENT,				// 213
	NOENT,				// 214
	NOENT,				// 215
	NOENT,				// 216
	NOENT,				// 217
	NOENT,				// 218
	NOENT,				// 219
	NOENT,				// 220
	NOENT,				// 221
	NOENT,				// 222
	NOENT,				// 223
	{ nak_server,		"NAK_S_INTERNAL"		},			// 224
	{ nak_server,		"NAK_S_NOTIMPL"			},			// 225
	{ nak_server,		"NAK_S_BADGATEWAY"		},			// 226
	{ nak_server,		"NAK_S_SVCUNAVAIL"		},			// 227
	{ nak_server,		"NAK_S_GWTIMEOUT"		},			// 228
	{ nak_server,		"NAK_S_PROXYNOTSUP"		},			// 229
	NOENT,				// 230
	NOENT,				// 231
	NOENT,				// 232
	NOENT,				// 233
	NOENT,				// 234
	NOENT,				// 235
	NOENT,				// 236
	NOENT,				// 237
	NOENT,				// 238
	NOENT,				// 239
	NOENT,				// 240
	NOENT,				// 241
	NOENT,				// 242
	NOENT,				// 243
	NOENT,				// 244
	NOENT,				// 245
	NOENT,				// 246
	NOENT,				// 247
	NOENT,				// 248
	NOENT,				// 249
	NOENT,				// 250
	NOENT,				// 251
	NOENT,				// 252
	NOENT,				// 253
	NOENT,				// 254
	NOENT,				// 255
};


/*
**	_GDP_CMD_NAME --- return name of command
*/

const char *
_gdp_proto_cmd_name(uint8_t cmd)
{
	dispatch_ent_t *d = &DispatchTable[cmd];

	if (d->name == NULL)
		return "???";
	else
		return d->name;
}


void
_gdp_register_cmdfuncs(struct cmdfuncs *cf)
{
	for (; cf->func != NULL; cf++)
	{
		DispatchTable[cf->cmd].func = cf->func;
	}
}


static EP_STAT
cmd_not_implemented(gdp_req_t *req)
{
	// just ignore unknown commands
	if (ep_dbg_test(Dbg, 1))
	{
		ep_dbg_printf("gdp_req_dispatch: Unknown cmd, req:\n");
		_gdp_req_dump(req, ep_dbg_getfile());
	}

	return GDP_STAT_NOT_IMPLEMENTED;
}


EP_STAT
_gdp_req_dispatch(gdp_req_t *req)
{
	EP_STAT estat;
	dispatch_ent_t *d;

	d = &DispatchTable[req->pdu->cmd];
	if (d->func == NULL)
		estat = cmd_not_implemented(req);
	else
		estat = (*d->func)(req);
	return estat;
}


/*
**  PROCESS_MESSAGE --- execute the command in the packet
*/

static EP_STAT
process_message(gdp_pdu_t *pdu, gdp_chan_t *chan)
{
	EP_STAT estat;
	gdp_gcl_t *gcl = NULL;
	gdp_req_t *req = NULL;

	// find the handle for the GCL
	// (since this is an app, the dst is us, and the src is the GCL)
	gcl = _gdp_gcl_cache_get(pdu->src, 0);
	if (gcl != NULL)
	{
		// find the request
		req = _gdp_req_find(gcl, pdu->rid);
	}
	else if (ep_dbg_test(Dbg, 1))
	{
		gdp_pname_t pbuf;

		gdp_printable_name(pdu->dst, pbuf);
		ep_dbg_printf("process_message: GCL %s has no handle\n", pbuf);
	}

	if (req == NULL)
	{
		ep_dbg_cprintf(Dbg, 43,
				"process_message: allocating new req for gcl %p\n", gcl);
		estat = _gdp_req_new(pdu->cmd, gcl, chan, 0, &req);
		if (!EP_STAT_ISOK(estat))
		{
			ep_log(estat, "process_message: cannot allocate request; dropping packet");

			// not much to do here other than ignore the input
			_gdp_pdu_free(pdu);
			return estat;
		}
		
		//XXX link request into GCL list??
	}
	else if (ep_dbg_test(Dbg, 43))
	{
		ep_dbg_printf("process_message: using existing ");
		_gdp_req_dump(req, ep_dbg_getfile());
	}

	if (req->pdu->datum != NULL)
	{
		ep_dbg_cprintf(Dbg, 43,
				"process_message: reusing old datum for req %p\n", req);

		// don't need the old dbuf
		gdp_buf_free(req->pdu->datum->dbuf);

		// copy the contents of the new message over the old
		memcpy(req->pdu->datum, pdu->datum, sizeof *req->pdu->datum);

		// we no longer need the new message
		pdu->datum->dbuf = NULL;
		gdp_datum_free(pdu->datum);

		// point the new packet at the old datum
		pdu->datum = req->pdu->datum;
		EP_ASSERT(pdu->datum->inuse);
	}

	// can now drop the old pdu and switch to the new one
	req->pdu->datum = NULL;
	_gdp_pdu_free(req->pdu);
	req->pdu = pdu;
	pdu = NULL;

	// invoke the command-specific (or ack-specific) function
	estat = _gdp_req_dispatch(req);

	// ASSERT(all data from chan has been consumed);

	ep_thr_mutex_lock(&req->mutex);

	if (EP_UT_BITSET(GDP_REQ_SUBSCRIPTION, req->flags))
	{
		// link the request onto the event queue
		gdp_event_t *gev;
		int evtype;

		// for the moment we only understand data responses (for subscribe)
		switch (req->pdu->cmd)
		{
		  case GDP_ACK_CONTENT:
			evtype = GDP_EVENT_DATA;
			break;

		  case GDP_ACK_DELETED:
			// end of subscription
			evtype = GDP_EVENT_EOS;
			break;

		  default:
			ep_dbg_cprintf(Dbg, 3, "Got unexpected ack %d\n", req->pdu->cmd);
			estat = GDP_STAT_PROTOCOL_FAIL;
			goto fail1;
		}

		estat = gdp_event_new(&gev);
		EP_STAT_CHECK(estat, goto fail1);

		gev->type = evtype;
		gev->gcl = req->gcl;
		gev->datum = req->pdu->datum;
		gev->udata = req->udata;
		req->pdu->datum = NULL;			// avoid use after free

		if (req->cb.generic != NULL)
		{
			// caller wanted a callback
#ifdef RUN_CALLBACKS_IN_THREAD
			// ... to run in a separate thread ...
			ep_thr_pool_run(req->cb.generic, gev);
#else
			// ... to run in I/O event thread ...
			(*req->cb.generic)(gev);
#endif
		}
		else
		{
			// caller wanted events
			gdp_event_trigger(gev);
		}

		// the callback must call gdp_event_free(gev)
	}
	else
	{
		// return our status via the request
		req->stat = estat;
		req->flags |= GDP_REQ_DONE;
		if (ep_dbg_test(Dbg, 44))
		{
			ep_dbg_printf("process_message: on return, ");
			_gdp_req_dump(req, ep_dbg_getfile());
		}
		ep_thr_cond_signal(&req->cond);
	}
fail1:
	ep_thr_mutex_unlock(&req->mutex);
	return estat;
}


/*
**	GDP_READ_CB --- data is available for reading from gdpd socket
*/

static void
gdp_read_cb(struct bufferevent *bev, void *ctx)
{
	EP_STAT estat;
	gdp_pdu_t *pdu;
	gdp_buf_t *ievb = bufferevent_get_input(bev);
	struct gdp_event_info *gei = ctx;

	ep_dbg_cprintf(Dbg, 50, "gdp_read_cb: fd %d, %zd bytes\n",
			bufferevent_getfd(bev), evbuffer_get_length(ievb));

	while (evbuffer_get_length(ievb) > 0)
	{
		pdu = _gdp_pdu_new();
		estat = _gdp_pdu_in(pdu, gei->chan);
		if (EP_STAT_IS_SAME(estat, GDP_STAT_KEEP_READING))
		{
			_gdp_pdu_free(pdu);
			return;
		}

#ifdef GDP_PACKET_QUEUE
		_gdp_pdu_add_to_queue(pdu, gei->chan);
#else
		estat = process_message(pdu, gei->chan);
#endif
	}
}


/*
**	GDP_EVENT_CB --- events or errors occur on gdpd socket
*/

static EP_PRFLAGS_DESC	EventWhatFlags[] =
{
	{ BEV_EVENT_READING,	BEV_EVENT_READING,		"BEV_EVENT_READING"		},
	{ BEV_EVENT_WRITING,	BEV_EVENT_WRITING,		"BEV_EVENT_WRITING"		},
	{ BEV_EVENT_EOF,		BEV_EVENT_EOF,			"BEV_EVENT_EOF"			},
	{ BEV_EVENT_ERROR,		BEV_EVENT_ERROR,		"BEV_EVENT_ERROR"		},
	{ BEV_EVENT_TIMEOUT,	BEV_EVENT_TIMEOUT,		"BEV_EVENT_TIMEOUT"		},
	{ BEV_EVENT_CONNECTED,	BEV_EVENT_CONNECTED,	"BEV_EVENT_CONNECTED"	},
	{ 0, 0, NULL }
};

void
_gdp_event_cb(struct bufferevent *bev, short events, void *ctx)
{
	bool exitloop = false;
	struct gdp_event_info *gei = ctx;

	if (ep_dbg_test(Dbg, 25))
	{
		ep_dbg_printf("_gdp_event_cb: fd %d: ", bufferevent_getfd(bev));
		ep_prflags(events, EventWhatFlags, ep_dbg_getfile());
		ep_dbg_printf("\n");
	}
	if (EP_UT_BITSET(BEV_EVENT_EOF, events))
	{
		gdp_buf_t *ievb = bufferevent_get_input(bev);
		size_t l = gdp_buf_getlength(ievb);

		ep_dbg_cprintf(Dbg, 1, "_gdp_event_cb: got EOF, %zu bytes left\n", l);
		exitloop = true;
	}
	if (EP_UT_BITSET(BEV_EVENT_ERROR, events))
	{
		ep_dbg_cprintf(Dbg, 1, "_gdp_event_cb: error: %s\n",
				evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
		exitloop = true;
	}
	if (exitloop)
	{
		if (gei->exit_cb != NULL)
			(*gei->exit_cb)(bufferevent_get_base(bev), gei);
		else
			event_base_loopexit(bufferevent_get_base(bev), NULL);
	}
}


/*
**	INITIALIZATION ROUTINES
*/

static EP_STAT
init_error(const char *datum, const char *where)
{
	EP_STAT estat = ep_stat_from_errno(errno);
	char nbuf[40];

	strerror_r(errno, nbuf, sizeof nbuf);
	ep_log(estat, "gdp_init: %s: %s", where, datum);
	ep_app_error("gdp_init: %s: %s: %s", where, datum, nbuf);
	return estat;
}


/*
**	Base loop to be called for event-driven systems.
**	Their events should have already been added to the event base.
**
**		GdpIoEventLoopThread is also used by gdpd, hence non-static.
*/

pthread_t		_GdpIoEventLoopThread;

static void
event_loop_timeout(int fd, short what, void *ctx)
{
	struct event_loop_info *eli = ctx;

	ep_dbg_cprintf(Dbg, 79, "%s event loop timeout\n", eli->where);
}

static void *
run_event_loop(void *ctx)
{
	struct event_loop_info *eli = ctx;
	struct event_base *evb = eli->evb;
	long evdelay = ep_adm_getlongparam("swarm.gdp.event.loopdelay", 100000L);
	
	// keep the loop alive if EVLOOP_NO_EXIT_ON_EMPTY isn't available
	long ev_timeout = ep_adm_getlongparam("swarm.gdp.event.looptimeout", 30L);
	struct timeval tv = { ev_timeout, 0 };
	struct event *evtimer = event_new(evb, -1, EV_PERSIST,
			&event_loop_timeout, eli);
	event_add(evtimer, &tv);

	for (;;)
	{
		if (ep_dbg_test(Dbg, 20))
		{
			ep_dbg_printf("gdp_event_loop: starting up %s base loop\n",
					eli->where);
			event_base_dump_events(evb, ep_dbg_getfile());
		}

#ifdef EVLOOP_NO_EXIT_ON_EMPTY
		event_base_loop(evb, EVLOOP_NO_EXIT_ON_EMPTY);
#else
		event_base_loop(evb, 0);
#endif

		if (ep_dbg_test(Dbg, 1))
		{
			ep_dbg_printf("gdp_event_loop: %s event_base_loop returned\n",
					eli->where);
			if (event_base_got_break(evb))
				ep_dbg_printf(" ... as a result of loopbreak\n");
			if (event_base_got_exit(evb))
				ep_dbg_printf(" ... as a result of loopexit\n");
		}
		if (event_base_got_exit(evb))
		{
			// the GDP daemon went away
			break;
		}

		if (evdelay > 0)
			ep_time_nanosleep(evdelay * 1000LL);		// avoid CPU hogging
	}

	ep_log(GDP_STAT_DEAD_DAEMON, "lost connection to gdp daemon");
	ep_app_abort("lost connection to gdp daemon");
}

EP_STAT
_gdp_start_event_loop_thread(pthread_t *thr,
		struct event_base *evb,
		const char *where)
{
	struct event_loop_info *eli = ep_mem_malloc(sizeof *eli);

	eli->evb = evb;
	eli->where = where;
	if (pthread_create(thr, NULL, run_event_loop, eli) != 0)
		return init_error("cannot create event loop thread", where);
	else
		return EP_STAT_OK;
}


/*
**	_GDP_DO_INIT --- initialize this library
*/

static EP_DBG	EvlibDbg = EP_DBG_INIT("gdp.evlib", "GDP Eventlib");

static void
evlib_log_cb(int severity, const char *msg)
{
	char *sev;
	char *sevstrings[] = { "debug", "msg", "warn", "error" };

	if (severity < 0 || severity > 3)
		sev = "?";
	else
		sev = sevstrings[severity];
	ep_dbg_cprintf(EvlibDbg, ((4 - severity) * 20) + 2, "[%s] %s\n", sev, msg);
}


/*
**  _GDP_OPEN_CONNECTION --- open connection to a gdp daemon
*/

EP_STAT
_gdp_open_connection(const char *gdpd_addr, gdp_chan_t **pchan)
{
	EP_STAT estat = EP_STAT_OK;
	gdp_chan_t *chan;

	// allocate a new channel structure
	chan = ep_mem_zalloc(sizeof *chan);
	LIST_INIT(&chan->reqs);

	// set up the bufferevent
	chan->bev = bufferevent_socket_new(GdpIoEventBase,
						-1,
						BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE |
						BEV_OPT_DEFER_CALLBACKS);
	if (chan->bev == NULL)
	{
		estat = init_error("could not allocate bufferevent", "gdp_init");
		goto fail0;
	}

	struct gdp_event_info *gei = ep_mem_zalloc(sizeof *gei);
	gei->chan = chan;

	bufferevent_setcb(chan->bev, gdp_read_cb, NULL, _gdp_event_cb, gei);
	bufferevent_enable(chan->bev, EV_READ | EV_WRITE);

	// attach it to a socket
	char abuf[100];
	char pbuf[10];
	struct addrinfo *res;
	struct addrinfo hints;
	int r;
	char *port;
	char *host;

	// get the host:port info into abuf
	if (gdpd_addr == NULL)
		gdpd_addr = ep_adm_getstrparam("swarm.gdp.gdpd.addr", NULL);
	if (gdpd_addr == NULL)
		gdpd_addr = "127.0.0.1";
	strlcpy(abuf, gdpd_addr, sizeof abuf);

	// have to handle IPv6 literals, which have [...] before the port
	// ... note that square brackets are stripped off to satisfy getaddrinfo
	port = host = abuf;
	if (*host == '[')
	{
		host++;
		port = strchr(host, ']');
		if (port != NULL)
			*port++ = '\0';
	}

	// see if we have a port number
	if (port != NULL)
	{
		port = strchr(port, ':');
		if (port != NULL)
			*port++ = '\0';
	}
	if (port == NULL || *port == '\0')
	{
		port = pbuf;
		snprintf(pbuf, sizeof pbuf, "%d", GDP_PORT_DEFAULT);
	}
	ep_dbg_cprintf(Dbg, 20, "gdp_init: contacting host %s port %s\n",
			host, port);

	// parsing done....  let's try the lookup
	memset(&hints, '\0', sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	r = getaddrinfo(host, port, &hints, &res);
	if (r == 0)
	{
		struct addrinfo *a = res;
		while (a != NULL)
		{
			if (bufferevent_socket_connect(chan->bev, a->ai_addr,
						a->ai_addrlen) >= 0)
				break;
			a = a->ai_next;
		}
		if (a == NULL)
		{
			estat = init_error("could not connect to GDP socket",
						"gdp_init");
			goto fail1;
		}
		freeaddrinfo(res);
	}
	else
	{
		fprintf(stderr, "gdp_init: %s\n", gai_strerror(r));
		estat = init_error("could not find GDP socket", "gdp_init");
		goto fail1;
	}

	// success
	*pchan = chan;

	// error cleanup and return
	if (false)
	{
fail1:
		ep_mem_free(gei);
		bufferevent_free(chan->bev);
		chan->bev = NULL;
fail0:
		ep_mem_free(chan);
	}
	{
		char ebuf[80];

		ep_dbg_cprintf(Dbg, 20, "_gdp_open_connection => %s\n",
				ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}


/*
**  Initialization, Part 1:
**		Initialize the various external libraries.
**		Set up the I/O event loop base.
**		Initialize the GCL cache.
*/

EP_STAT
_gdp_do_init_1(void)
{
	EP_STAT estat = EP_STAT_OK;
	const char *progname;
	const char *myname = NULL;

	ep_dbg_cprintf(Dbg, 4, "gdp_init: initializing\n");

	if (ep_dbg_test(EvlibDbg, 80))
	{
		// according to the book...
		//event_enable_debug_logging(EVENT_DBG_ALL);
		// according to the code...
		event_enable_debug_mode();
	}

	// initialize the EP library
	ep_lib_init(EP_LIB_USEPTHREADS);
	ep_adm_readparams("gdp");
	progname = ep_app_getprogname();
	if (progname != NULL)
		ep_adm_readparams(progname);

	// register status strings
	_gdp_stat_init();

	// figure out or generate our name
	if (progname != NULL)
	{
		char argname[100];

		snprintf(argname, sizeof argname, "swarm.%s.gname", progname);
		myname = ep_adm_getstrparam(argname, NULL);
		if (myname != NULL)
		{
			estat = gdp_parse_name(myname, _GdpRoutingName);
			EP_STAT_CHECK(estat, myname = NULL);
		}
	}
	if (myname == NULL)
	{
		// no name found in configuration
		_gdp_newname(_GdpRoutingName);
	}

	if (ep_dbg_test(Dbg, 1))
	{
		gdp_pname_t pname;

		ep_dbg_printf("Our name = %s\n",
				gdp_printable_name(_GdpRoutingName, pname));
	}

	// tell the event library that we're using pthreads
	if (evthread_use_pthreads() < 0)
		return init_error("cannot use pthreads", "gdp_init");
	if (ep_dbg_test(Dbg, 90))
	{
		evthread_enable_lock_debuging();
	}

	// use our debugging printer
	event_set_log_callback(evlib_log_cb);

	// set up the event base
	if (GdpIoEventBase == NULL)
	{
		// Initialize for I/O events
		{
			struct event_config *ev_cfg = event_config_new();

			event_config_require_features(ev_cfg, 0);
			GdpIoEventBase = event_base_new_with_config(ev_cfg);
			if (GdpIoEventBase == NULL)
				estat = init_error("could not create event base", "gdp_init");
			event_config_free(ev_cfg);
			EP_STAT_CHECK(estat, goto fail0);
		}
	}

	estat = _gdp_gcl_cache_init();

fail0:
	return estat;
}

/*
**	Initialization, Part 2:
**		Open the connection to gdpd.
**		Start the event loop to service that connection.
**
**	This code is not called by gdpd.
*/

EP_STAT
_gdp_do_init_2(const char *gdpd_addr)
{
	gdp_chan_t *chan;
	EP_STAT estat;

	estat = _gdp_open_connection(gdpd_addr, &chan);
	EP_STAT_CHECK(estat, goto fail0);

	_GdpChannel = chan;
	ep_dbg_cprintf(Dbg, 10, "gdp_init: talking on fd %d\n",
			bufferevent_getfd(chan->bev));

	// create a thread to run the event loop
	estat = _gdp_start_event_loop_thread(&_GdpIoEventLoopThread,
										GdpIoEventBase, "I/O");
	EP_STAT_CHECK(estat, goto fail1);

	ep_dbg_cprintf(Dbg, 4, "gdp_init: success\n");
	return estat;

fail1:
	bufferevent_free(chan->bev);
	chan->bev = NULL;
	ep_mem_free(chan);

fail0:
	{
		char ebuf[200];

		ep_dbg_cprintf(Dbg, 4, "gdp_init: failure: %s\n",
					ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}
	return estat;
}
