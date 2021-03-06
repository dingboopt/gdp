#!/usr/bin/env python

"""
A log creation service that receives Log Creation commands from an
unmodified client and passes them to a log-server. The purpose of
such a log-creation service is to provide a layer of indirection
between the clients and log-servers (and ensure log-duplication
is handled cleanly, before we can come up with a better solution).


"""

from GDPService import GDPService
import gdp
import gdp_pb2
import random
import argparse
import time
import sqlite3
import threading
import logging
import os
import sys
import struct
from hashlib import sha256
import mysql.connector as mariadb


SERVICE_NAME = gdp.GDP_NAME('logcreationservice').internal_name()
DEFAULT_ROUTER_PORT = 8007
DEFAULT_ROUTER_HOST = "172.30.0.1"

GDPROUTER_ADDRESS = (chr(255) + chr(0)) * 16
### these come from gdp/gdp_pdu.h from the GDP C library

# Acks/Naks
GDP_ACK_MIN = 128
GDP_ACK_MAX = 191
GDP_NAK_C_MIN = 192
GDP_NAK_C_MAX = 223
GDP_NAK_C_MIN = 223
GDP_NAC_C_MAX = 239
GDP_NAK_R_MIN = 240
GDP_NAK_R_MAX = 254
GDP_NAK_C_CONFLICT = 201
GDP_NAK_S_NOTIMPL = 225

# specific commands
GDP_CMD_CREATE = 66
GDP_NAK_C_BADREQ = 192


class logCreationService(GDPService):

    def __init__(self, dbname, router, GDPaddrs, logservers, namedb_info):
        """
        router: a 'host:port' string representing the GDP router
        GDPaddrs: a list of 256-bit addresses of this particular service
        logservers: a list of log-servers on the backend that we use
        dbname: sqlite database location
        namedb_info: A remote database (mariadb) where we populate a
                     human name => internal name mapping.
        """

        ## First call the __init__ of GDPService
        super(logCreationService, self).__init__(SERVICE_NAME,
                                                        router, GDPaddrs)

        ## Setup instance specific constants
        self.GDPaddrs = GDPaddrs
        self.logservers = logservers
        self.dbname = dbname
        self.lock = threading.Lock()

        ## Setup a connection to the backend database
        if os.path.exists(self.dbname):
            logging.info("Loading existing database, %r", self.dbname)
            self.conn = sqlite3.connect(self.dbname, check_same_thread=False)
            self.cur = self.conn.cursor()
        else:
            logging.info("Creating new database, %r", self.dbname)
            self.conn = sqlite3.connect(self.dbname, check_same_thread=False)
            self.cur = self.conn.cursor()

            ## Make table for bookkeeping
            self.cur.execute("""CREATE TABLE logs(
                                    logname TEXT UNIQUE, srvname TEXT,
                                    ack_seen INTEGER DEFAULT 0,
                                    ts DATETIME DEFAULT CURRENT_TIMESTAMP,
                                    creator TEXT, rid INTEGER)""")
            self.cur.execute("""CREATE UNIQUE INDEX logname_ndx
                                                ON logs(logname)""")
            self.cur.execute("CREATE INDEX srvname_ndx ON logs(srvname)")
            self.cur.execute("CREATE INDEX ack_seen_ndx ON logs(ack_seen)")
            self.conn.commit()

        self.namedb_info = namedb_info
        if namedb_info.get("host", None) is not None:
            logging.info("Initiating connection to directory server")
            self.namedb_conn = mariadb.connect(
                        host=namedb_info["host"],
                        user=namedb_info.get("user", "anonymous"),
                        password=namedb_info.get("passwd", ""),
                        database=namedb_info.get("database", "gdp_hongd"))

            ## XXX Not sure if we can share a single cursor, or whether
            ## one ought to acquire a new cursor for each (potentially
            ## concurrent) request. Check this later.
            self.namedb_cur = self.namedb_conn.cursor()
        else:
            self.namedb_conn = None
            self.namedb_cur = None


    def __del__(self):

        ## Close database connections
        logging.info("Closing database connection to %r", self.dbname)
        self.conn.close()


    def request_handler(self, req):
        """
        The routine that gets invoked when a PDU is received. In our case,
        it can be either a new request (from a client), or a response from
        a log-server.
        """

        # first, deserialize the payload from req pdu.
        payload = gdp_pb2.GdpMessage()
        payload.ParseFromString(req['data'])

        # early exit if a router told us something (usually not a good
        # sign)
        if payload.cmd >= GDP_NAK_R_MIN and \
                        payload.cmd <= GDP_NAK_R_MAX:
            logger.warning("Routing error, src: %r", req['src'])
            return

        # check if it's a request from a client or a response from a logd.
        if payload.cmd < GDP_ACK_MIN:      ## it's a command

            ## First check for any error conditions. If any of the
            ## following occur, we ought to send back a NAK
            if req['src'] in self.logservers:
                logging.warning("error: received cmd %d from server", payload.cmd)
                return self.gen_nak(req, gdp_pb2.NAK_C_BADREQ)

            if payload.cmd != gdp_pb2.CMD_CREATE:
                logging.warning("error: recieved unknown request")
                return self.gen_nak(req, gdp_pb2.NAK_S_NOTIMPL)

            ## By now, we know the request is a CREATE request from a client

            ## figure out the data we need to insert in the database
            humanname, logname = self.extract_name(payload.cmd_create)

            if humanname is not None and self.namedb_conn is not None:
                ## Add this to the humanname=>logname mapping directory
                try:
                    # Note that logname is the internal 256-bit name
                    # (and not a printable name)
                    self.add_to_hongd(humanname, logname)
                except mariadb.Error as error:
                    logging.warning("Couldn't add mapping. %s", str(error))
                    ## XXX what is the correct behavior? exit?
                    return self.gen_nak(req, gdp_pb2.NAK_C_CONFLICT)

            srvname = random.choice(self.logservers)
            ## private information that we will need later
            creator = req['src']
            rid = payload.rid

            ## log this to our backend database. Generate printable
            ## names (except the null character, which causes problems)
            __logname = gdp.GDP_NAME(logname,
                                force_internal=True).printable_name()[:-1]
            __srvname = gdp.GDP_NAME(srvname,
                                force_internal=True).printable_name()[:-1]
            __creator = gdp.GDP_NAME(creator,
                                force_internal=True).printable_name()[:-1]

            logging.info("Received Create request for logname %r, "
                                "picking server %r", __logname, __srvname)

            try:

                with self.lock:

                    logging.debug("inserting to database %r, %r, %r, %d",
                                __logname, __srvname, __creator, rid)
                    self.cur.execute("""INSERT INTO logs (logname, srvname,
                                    creator, rid) VALUES(?,?,?,?);""",
                                    (__logname, __srvname, __creator, rid))
                    self.conn.commit()

                    spoofed_req = req.copy()
                    spoofed_req['src'] = req['dst']
                    spoofed_req['dst'] = srvname
                    ## make a copy of payload so that we can change rid
                    __spoofed_payload = gdp_pb2.GdpMessage()
                    __spoofed_payload.ParseFromString(req['data'])
                    __spoofed_payload.rid = self.cur.lastrowid
                    spoofed_req['data'] = __spoofed_payload.SerializeToString()

                    # now return this spoofed request back to transport layer
                    # Since we have overridden the destination, it will go
                    # to a log server instead of the actual client
                    return spoofed_req

            except sqlite3.IntegrityError:

                logging.warning("Log already exists")
                return self.gen_nak(req, gdp_pb2.NAK_C_CONFLICT)

            ## Send a spoofed request to the logserver

        else: ## response.

            ## Sanity checking
            if req['src'] not in self.logservers:
                logging.warning("error: received a response from non-logserver")
                return self.gen_nak(req, gdp_pb2.NAK_C_BADREQ)

            logging.info("Response from log-server, row %d", payload.rid)

            with self.lock:

                ## Fetch the original creator and rid from our database
                self.cur.execute("""SELECT creator, rid, ack_seen FROM logs
                                            WHERE rowid=?""", (payload.rid,))
                dbrows = self.cur.fetchall()

            good_resp = len(dbrows) == 1
            if good_resp:
                (__creator, orig_rid, ack_seen) = dbrows[0]
                creator = gdp.GDP_NAME(__creator).internal_name()
                if ack_seen != 0:
                    good_resp = False

            if not good_resp:

                logging.warning("error: bogus response")
                return self.gen_nak(req, gdp_pb2.NAK_C_BADREQ)

            else:

                with self.lock:
                    logging.debug("Setting ack_seen to 1 for row %d",
                                                              payload.rid)
                    self.cur.execute("""UPDATE logs SET ack_seen=1
                                        WHERE rowid=?""", (payload.rid,))
                    self.conn.commit()

            # create a spoofed reply and send it to the client
            spoofed_reply = req.copy()
            spoofed_reply['src'] = req['dst']
            spoofed_reply['dst'] = creator

            spoofed_payload = gdp_pb2.GdpMessage()
            spoofed_payload.ParseFromString(req['data'])
            spoofed_payload.rid = orig_rid

            spoofed_reply['data'] = spoofed_payload.SerializeToString()

            # return this back to the transport layer
            return spoofed_reply


    def gen_nak(self, req, nak=gdp_pb2.NAK_C_BADREQ):
        resp = dict()
        resp['src'] = req['dst']
        resp['dst'] = req['src']

        resp_payload = gdp_pb2.GdpMessage()
        resp_payload.ParseFromString(req['data'])
        resp_payload.cmd = nak
        resp_payload.nak.ep_stat = 0

        resp['data'] = resp_payload.SerializeToString()

        return resp

    @staticmethod
    def extract_name(create_msg):
        """
        returns a tuple (human name, internal name) from the Protobuf
        CmdCreate message. Any changes to the create message format
        should ideally only need changes here.

        Note that this needs to peek into the serialized metadata..
        """

        def __deserialize_md(data):
            """returns a dictionary"""
            ret = {}

            try:
                nmd = struct.unpack("!H", data[0:2])[0]
                offset = 2
                tmplist = []    ## list of (md_id, md_len) tuples
                for _ in xrange(nmd):
                    md_id = struct.unpack("!I", data[offset:offset+4])[0]
                    md_len = struct.unpack("!I", data[offset+4:offset+8])[0]
                    offset += 8
                    tmplist.append((md_id, md_len))

                for (md_id, md_len) in tmplist:
                    ret[md_id] = data[offset:offset+md_len]
                    offset += md_len

                if offset!=len(data):
                    logging.warning("%d bytes leftover when parsing metadata",
                                                            len(data)-offset)

            except struct.error as e:
                logging.warning("%s", str(e))
                logging.warning("Probably incomplete data when parsing metadata")
                ret = {}

            return ret


        ## Let's see if there's a human name in the metadata. Could be null
        md_dict = __deserialize_md(create_msg.metadata.data)
        humanname = md_dict.get(0x00584944, None)   # XID

        if create_msg.HasField("logname"):
            _logname = create_msg.logname
        else:
            _logname = None

        ## Let's take the hash of the metadata, and see what we have
        smd = create_msg.metadata.SerializeToString()
        logname = sha256(smd).digest()
        if _logname is not None and _logname != logname:
            logging.debug("Overriding hash of metadata with provided name")
            logname = _logname

        ## just for debugging
        __logname = gdp.GDP_NAME(logname,
                    force_internal=True).printable_name()[:-1]
        logging.info("Mapping '%s' => '%s'", humanname, __logname)

        return (humanname, logname) 


    def add_to_hongd(self, humanname, logname):
        """perform the database operation"""
        table = self.namedb_info.get("table", "human_to_gdp")
        query = "insert into "+table+" (hname, gname) values (%s, %s)"
        self.namedb_cur.execute(query, (humanname, logname))
        self.namedb_conn.commit()


if __name__ == "__main__":

    ## argument parsing
    parser = argparse.ArgumentParser(description="Log creation service")

    parser.add_argument("-v", "--verbose", action='store_true',
                                help="Be quite verbose in execution.")
    parser.add_argument("-i", "--host", type=str, default=DEFAULT_ROUTER_HOST,
                                help="host of gdp_router instance. "
                                     "default = %s" % DEFAULT_ROUTER_HOST)
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_ROUTER_PORT,
                                help="port for gdp_router instance. "
                                     "default = %d" % DEFAULT_ROUTER_PORT)
    parser.add_argument("-d", "--dbname", type=str, required=True,
                                help="filename for sqlite database")
    parser.add_argument("-a", "--addr", type=str, nargs='+', required=True,
                                help="Address(es) for this service, typically "
                                     "human readable names.")
    parser.add_argument("-s", "--server", type=str, nargs='+', required=True,
                                help="Log server(s) to be used for actual log "
                                     "creation, typically human readable names")

    ## The following for namedb/hongd
    parser.add_argument("--namedb_host", help="Hostname for namedb")
    parser.add_argument("--namedb_user", help="Username for namedb")
    parser.add_argument("--namedb_passwd", help="Password for namedb")
    parser.add_argument("--namedb_database", help="Database name for namedb")
    parser.add_argument("--namedb_table", help="Table name for namedb")


    args = parser.parse_args()

    ## done argument parsing, instantiate the service
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    else:
        logging.basicConfig(level=logging.INFO)

    ## parse arguments
    router = "%s:%d" % (args.host, args.port)
    addrs = [gdp.GDP_NAME(x).internal_name() for x in args.addr]
    servers = [gdp.GDP_NAME(x).internal_name() for x in args.server]

    namedb_info = {}
    if args.namedb_host is not None:
        namedb_info["host"] = args.namedb_host
    if args.namedb_user is not None:
        namedb_info["user"] = args.namedb_user
    if args.namedb_passwd is not None:
        namedb_info["passwd"] = args.namedb_passwd
    if args.namedb_database is not None:
        namedb_info["database"] = args.namedb_database
    if args.namedb_table is not None:
        namedb_info["table"] = args.namedb_table


    logging.info("Starting a log-creation service...")
    logging.info(">> Connecting to %s", router)
    logging.info(">> Servicing names %r", args.addr)
    logging.info(">> Using log servers %r", args.server)
    logging.info(">> Human name directory at %r", args.namedb_host)

    ## instantiate the service
    service = logCreationService(args.dbname, router, addrs, servers,
                                                            namedb_info)

    ## all done, start the service (and sleep indefinitely)
    logging.info("Starting logcreationservice")
    service.start()

    try:
        while True:
            time.sleep(1)
    except (KeyboardInterrupt, SystemExit):
        service.stop()
