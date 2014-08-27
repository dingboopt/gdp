/* vim: set ai sw=4 sts=4 ts=4 : */

#ifndef _GDP_PKT_H_
#define _GDP_PKT_H_

#include <stdio.h>
#include <netinet/in.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include "gdp_priv.h"

#define GDP_PORT_DEFAULT		2468	// default IP port

#define GDP_PROTO_CUR_VERSION	1		// current protocol version
#define GDP_PROTO_MIN_VERSION	1		// min version we can accept



/*
**	Header for a GDP Protocol Data Unit.
**
**		This is not the "on the wire" format, which has to be put into
**		network byte order and packed.	However, this does show the
**		order in which fields are written.
**
**		Commands are eight bits with the top three bits encoding
**		additional semantics.  Those bits are:
**
**		00x		"Blind" (unacknowledged) command
**		01x		Acknowledged command
**		10x		Positive acknowledgement
**		110		Negative acknowledgement, client side problem
**		111		Negative acknowledgement, server side problem
**
**		These roughly correspond to the "Type" and "Code" class
**		field in the CoAP header.
**
**		XXX We may still want to play with these allocations,
**			depending on how dense the various spaces become.  I
**			suspect that "acknowledged command" will have the
**			most values and the ack/naks won't have very many.
**			Remember in particular that the commands have to include
**			the commands between gdpds for things like migration,
**			resource negotiation, etc.
**
**		XXX CoAP has two "sequence numbers": a message-id which
**			relates ack/naks to commands and a "token" which is
**			a higher level construct relating (for example)
**			subscribe requests to results.	The "rid" represents
**			a shorter version of the "token".  We don't include
**			"seq" since this is a lower-level concept that is
**			subsumed by TCP.
**
**		The structure of an on-the-wire packet is:
**			1	protocol version
**			1	command or ack/nak
**			1	flags (indicate presence/lack of additional fields)
**			1	reserved (must be zero on send, ignored on receive
**			4	length of data portion
**			[4	request id (optional)]
**			[32	GCL name (optional)]
**			[4	record number (optional)]
**			[16	commit timestamp (optional)]
**			N	data (length indicated above)
**		The structure shown below is the in-memory version
*/

typedef struct gdp_pkt
{
	// metadata
	TAILQ_ENTRY(gdp_pkt)	list;		// work list
//	gdp_chan_t				*chan;		// I/O channel for this packet entry
	bool					inuse:1;	// indicates that this is allocated

	// packet data
	uint8_t				ver;		// protocol version
	uint8_t				cmd;		// command or ack/nak
	uint8_t				flags;		// see below
	uint8_t				reserved1;	// must be zero on send, ignored on receive
	gdp_rid_t			rid;		// sequence number (GDP_PKT_NO_RID => none)
	gcl_name_t			gcl_name;	// name of the GCL of interest (0 => none)
	gdp_datum_t			*datum;		// pointer to data record
} gdp_pkt_t;

#define _GDP_MAX_PKT_HDR		128		// max size of on-wire packet header

/***** values for gdp_pkg_hdr cmd field *****/
/*	 note that the ack/nak values (128-254) also have STAT codes		*/
//		0-63			Blind commands
#define GDP_CMD_KEEPALIVE		0		// used for keepalives
//		64-127			Acknowledged commands
#define GDP_CMD_PING			64		// test connection
#define GDP_CMD_HELLO			65		// initial startup/handshake
#define GDP_CMD_CREATE			66		// create a GCL
#define GDP_CMD_OPEN_AO			67		// open a GCL for append-only
#define GDP_CMD_OPEN_RO			68		// open a GCL for read-only
#define GDP_CMD_CLOSE			69		// close a GCL
#define GDP_CMD_READ			70		// read a given record by index
#define GDP_CMD_PUBLISH			71		// append a record
#define GDP_CMD_SUBSCRIBE		72		// subscribe to a GCL
//		128-191			Positive acks (with CoAP, HTTP equivalents)
#define GDP_ACK_MIN			128				// minimum ack code
#define GDP_ACK_SUCCESS			128		// general success response (none, 200)
#define GDP_ACK_DATA_CREATED	129		// datum created (2.01, 201)
#define GDP_ACK_DATA_DEL		130		// datam deleted (2.02, 202)
#define GDP_ACK_DATA_VALID		131		// data valid (2.03, none)
#define GDP_ACK_DATA_CHANGED	132		// data changed (2.04, ~204)
#define GDP_ACK_DATA_CONTENT	133		// content (2.05, ~200)
#define GDP_ACK_MAX			191				// maximum ack code
//		192-223			Negative acks, client side (CoAP, HTTP)
#define GDP_NAK_MIN			192				// minimum nak code
#define GDP_NAK_C_BADREQ		192		// bad request (4.00, 400)
#define GDP_NAK_C_UNAUTH		193		// unauthorized (4.01, 401)
#define GDP_NAK_C_BADOPT		194		// bad option (4.02, none)
#define GDP_NAK_C_FORBIDDEN		195		// forbidden (4.03, 403)
#define GDP_NAK_C_NOTFOUND		196		// not found (4.04, 404)
#define GDP_NAK_C_METHNOTALLOWED 197	// method not allowed (4.05, 405)
#define GDP_NAK_C_NOTACCEPTABLE 198		// not acceptable (4.06, 406)
#define GDP_NAK_C_PRECONFAILED	204		// precondition failed (4.12, 412)
#define GDP_NAK_C_TOOLARGE		205		// request entity too large (4.13, 413)
#define GDP_NAK_C_UNSUPMEDIA	207		// unsupported media type (4.15, 415)
//		224-254			Negative acks, server side (CoAP, HTTP)
#define GDP_NAK_S_INTERNAL		224		// internal server error (5.00, 500)
#define GDP_NAK_S_NOTIMPL		225		// not implemented (5.01, 501)
#define GDP_NAK_S_BADGATEWAY	226		// bad gateway (5.02, 502)
#define GDP_NAK_S_SVCUNAVAIL	227		// service unavailable (5.03, 503)
#define GDP_NAK_S_GWTIMEOUT		228		// gateway timeout (5.04, 504)
#define GDP_NAK_S_PROXYNOTSUP	229		// proxying not supported (5.05, none)
#define GDP_NAK_MAX			254				// maximum nak code
//		255				Reserved


/***** values for gdp_pkg_hdr flags field *****/
#define GDP_PKT_HAS_RID		0x01		// has a rid field
#define GDP_PKT_HAS_ID		0x02		// has a gcl_name field
#define GDP_PKT_HAS_RECNO	0x04		// has a recno field
#define GDP_PKT_HAS_TS		0x08		// has a timestamp field

/***** dummy values for other fields *****/
#define GDP_PKT_NO_RID		0L			// no request id
#define GDP_PKT_NO_RECNO	UINT64_MAX	// no record number


gdp_pkt_t	*_gdp_pkt_new(void);		// allocate a new packet

void		_gdp_pkt_free(gdp_pkt_t *);	// free a packet

EP_STAT		_gdp_pkt_out(				// send a packet to a network buffer
				gdp_pkt_t *,			// the packet information
				gdp_buf_t *);			// the network buffer

void		_gdp_pkt_out_hard(			// send a packet to a network buffer
				gdp_pkt_t *,			// the packet information
				gdp_buf_t *);			// the network buffer

EP_STAT		_gdp_pkt_in(				// read a packet from a network buffer
				gdp_pkt_t *,			// the buffer to store the result
				gdp_buf_t *);			// the network buffer

void		_gdp_pkt_dump(
				gdp_pkt_t *pkt,
				FILE *fp);

// generic sockaddr union	XXX does this belong in this header file?
union sockaddr_xx
{
	struct sockaddr		sa;
	struct sockaddr_in	sin;
	struct sockaddr_in6 sin6;
};

#endif // _GDP_PKT_H_
