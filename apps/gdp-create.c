/* vim: set ai sw=4 sts=4 ts=4 : */

/*
**  LOG-CREATE --- create a new log
**
**		This is a temporary interface.  Ultimately we need a log
**		creation service that does reasonable log placement rather
**		than having to name a specific log server.
**
**	----- BEGIN LICENSE BLOCK -----
**	Applications for the Global Data Plane
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
//#include <ep/ep_crypto.h>
#include <ep/ep_dbg.h>
//#include <ep/ep_string.h>
#include <gdp/gdp.h>

//#include <openssl/rsa.h>
//#include <openssl/engine.h>
//#include <openssl/pem.h>

//#include <unistd.h>
//#include <fcntl.h>
//#include <getopt.h>
//#include <netdb.h>
#include <string.h>
#include <sysexits.h>
//#include <unistd.h>
//#include <sys/stat.h>


// minimum secure key length
#ifndef GDP_MIN_KEY_LEN
//# define GDP_MIN_KEY_LEN		1024
#endif // GDP_MIN_KEY_LEN

static EP_DBG	Dbg = EP_DBG_INIT("gdp-create", "Create new log");


// command line options
#define OPTIONS		"b:c:C:D:e:G:h:k:K:qs:SwW:"

void
usage(void)
{
	fprintf(stderr, "Usage: %s [-C creator] [-D dbgspec] [-e key_enc_alg]\n"
			"\t[-G gdpd_addr] [-q] [-s logd_name] [-S]\n"
			"\t[-h] [-k keytype] [-K owner_keyfile] [-w] [-W writer_keyfile]\n"
			"\t[-b keybits] [-c curve] [<mdid>=<metadata>...] [log_name]\n"
			"    -C  set name of log creator (owner; for metadata)\n"
			"    -D  set debugging flags\n"
			"    -e  set secret key encryption algorithm\n"
			"    -G  IP host to contact for GDP router\n"
			"    -q  don't give error if log already exists\n"
			"    -s  set creation service name\n"
			"    -S  skip the test to see if log already exists (for debugging)\n"
			"    -h  set the hash (message digest) algorithm (defaults to sha256)\n"
			"    -k  type of key; valid key types are \"rsa\", \"dsa\", and \"ec\"\n"
			"\t(defaults to ec); \"none\" turns off key generation\n"
			"    -K  use indicated key/place to write secret owner key\n"
			"    -w  create separate writer key\n"
			"    -W  use indicated key/place to write secret writer key\n"
			"\tIf -K or -W specify a directory, a .pem file is written there\n"
			"\twith the name of the GCL (defaults to \"KEYS\" or \".\")\n"
			"    -b  set size of key in bits (RSA and DSA only)\n"
			"    -c  set curve name (EC only)\n"
			"    creator is the id of the creator, formatted as an email address\n"
			"    logd_name is the name of the log server to host this log\n"
			"    metadata ids are (by convention) four letters or digits\n"
			"    log_name is the name of the log to create\n",
			ep_app_getprogname());
	exit(EX_USAGE);
}


int
main(int argc, char **argv)
{
	const char *gobxname = NULL;	// external name of GCL
	const char *logdxname = NULL;	// external name of log daemon
	const char *cname = NULL;		// creator/owner name (for metadata)
	gdp_gin_t *gin = NULL;
	gdp_create_info_t *gci = NULL;
	int opt;
	EP_STAT estat;
	char *gdpd_addr = NULL;
	bool show_usage = false;
	bool quiet = false;
	bool skip_existence_test = false;
	const char *dig_alg_name = NULL;
	const char *key_alg_name = NULL;
	int key_bits = 0;
	const char *key_curve = NULL;
	const char *owner_keyfile = NULL;
	const char *writer_keyfile = NULL;
	const char *key_enc_alg_name = NULL;
	bool separate_writer_key = false;
	const char *phase = NULL;

	// quick pass so debugging is on for initialization
	while ((opt = getopt(argc, argv, OPTIONS)) > 0)
	{
		if (opt == 'D')
			ep_dbg_set(optarg);
	}
	optind = 1;
#if EP_OSCF_NEED_OPTRESET
	optreset = 1;
#endif

	// preinit library (must be early due to crypto code in arg processing)
	ep_crypto_init(0);

	// collect command-line arguments
	while ((opt = getopt(argc, argv, OPTIONS)) > 0)
	{
		switch (opt)
		{
		 case 'b':
			key_bits = atoi(optarg);
			break;

		 case 'c':
			key_curve = optarg;
			break;

		 case 'C':
			cname = optarg;
			break;

		 case 'D':
			// already done
			break;

		 case 'e':
			if (ep_crypto_keyenc_byname(optarg) < 0)
			{
				ep_app_error("unknown key encryption algorithm %s", optarg);
				show_usage = true;
			}
			key_enc_alg_name = optarg;
			break;

		 case 'G':
			gdpd_addr = optarg;
			break;

		 case 'h':
			if (ep_crypto_md_alg_byname(optarg) < 0)
			{
				ep_app_error("unknown digest hash algorithm %s", optarg);
				show_usage = true;
			}
			dig_alg_name = optarg;
			break;

		 case 'k':
			if (ep_crypto_keytype_byname(optarg) == EP_CRYPTO_KEYTYPE_UNKNOWN)
			{
				ep_app_error("unknown key type %s", optarg);
				show_usage = true;
			}
			key_alg_name = optarg;
			break;

		 case 'K':
			owner_keyfile = optarg;
			break;

		 case 'q':
			quiet = true;
			break;

		 case 's':
			logdxname = optarg;
			break;

		 case 'S':
			skip_existence_test = true;
			break;

		 case 'w':
			separate_writer_key = true;
			break;

		 case 'W':
			writer_keyfile = optarg;
			separate_writer_key = true;
			break;

		 default:
			show_usage = true;
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (show_usage || argc < 0)
		usage();

	// initialize the GDP library
	phase = "initializing GDP";
	estat = gdp_init(gdpd_addr);
	if (!EP_STAT_ISOK(estat))
		goto fail0;

	// allow thread to settle to avoid interspersed debug output
	ep_time_nanosleep(INT64_C(100000000));

	gci = gdp_create_info_new();

	// collect any metadata and external name
	while (argc > 0)
	{
		char *p;
		if ((p = strchr(argv[0], '=')) != NULL)
		{
			gdp_md_id_t mdid = 0;
			int i;

			p++;
			for (i = 0; i < 4; i++)
			{
				if (argv[0][i] == '=')
					break;
				mdid = (mdid << 8) | (unsigned) argv[0][i];
			}

			gdp_create_info_add_metadata(gci, mdid, strlen(p), p);
		}
		else if (gobxname == NULL)
		{
			gobxname = *argv;
		}
		else
		{
			usage();
		}

		argc--;
		argv++;
	}

	// name is optional ; if omitted one will be created
	if (argc-- > 0)
		gobxname = *argv++;

	if (show_usage || argc > 0)
		usage();

	if (gobxname != NULL)
	{
		if (!skip_existence_test)
		{
			// make sure it doesn't already exist
			gdp_name_t gobiname;
			gin = NULL;
			if (EP_STAT_ISOK(estat = gdp_parse_name(gobxname, gobiname)) &&
				EP_STAT_ISOK(estat = gdp_gin_open(gobiname, GDP_MODE_RO,
												NULL, &gin)))
			{
				// oops, we shouldn't be able to open it
				if (gin != NULL)
					(void) gdp_gin_close(gin);
				if (!quiet)
					ep_app_error("Cannot create %s: already exists", gobxname);
				exit(EX_CANTCREAT);
			}
		}
	}

	/**************************************************************
	**  Set up other automatic metadata
	*/

	if (cname != NULL)
	{
		phase = "setting creator";
		estat = gdp_create_info_set_creator(gci, cname, NULL);
		EP_STAT_CHECK(estat, goto fail1);
	}

	if (logdxname != NULL)
	{
		phase = "setting creation service";
		estat = gdp_create_info_set_creation_service(gci, logdxname);
		EP_STAT_CHECK(estat, goto fail1);
	}

	if (owner_keyfile != NULL)
	{
		phase = "setting owner key";
		EP_CRYPTO_KEY *key = ep_crypto_key_read_file(owner_keyfile, 0, 0);
		estat = gdp_create_info_set_owner_key(gci, key, dig_alg_name);
	}
	else if (dig_alg_name != NULL || key_alg_name != NULL ||
			key_bits > 0 || key_curve != NULL || key_enc_alg_name != NULL)
	{
		phase = "creating owner key";
		estat = gdp_create_info_new_owner_key(gci, dig_alg_name, key_alg_name,
						key_bits, key_curve, key_enc_alg_name);
	}
	EP_STAT_CHECK(estat, goto fail1);

	if (writer_keyfile != NULL)
	{
		phase = "setting writer key";
		EP_CRYPTO_KEY *key = ep_crypto_key_read_file(writer_keyfile, 0, 0);
		estat = gdp_create_info_set_writer_key(gci, key, dig_alg_name);
	}
	else if (separate_writer_key)
	{
		phase = "creating writer key";
		estat = gdp_create_info_new_writer_key(gci, dig_alg_name, key_alg_name,
						key_bits, key_curve, key_enc_alg_name);
	}
	EP_STAT_CHECK(estat, goto fail1);

	/*
	**  Hello sailor, this is where the actual creation happens
	*/

	phase = "creating GDP object";
	estat = gdp_gin_create(gci, gobxname, &gin);
	EP_STAT_CHECK(estat, goto fail1);

	// just for a lark, let the user know the (internal) name
	if (!quiet)
	{
		gdp_pname_t pname;

		// this output gets parsed by gdp-rest --- don't change it!
		printf("Created new GCL %s\n",
				gdp_printable_name(*gdp_gin_getname(gin), pname));
	}

	gdp_gin_close(gin);

fail1:
	// free creation information
	gdp_create_info_free(&gci);

fail0:
	if (!EP_STAT_ISOK(estat))
	{
		char ebuf[100];
		ep_app_error("Creation failed while %s: %s",
				phase, ep_stat_tostr(estat, ebuf, sizeof ebuf));
	}

	int exitstat;
	if (EP_STAT_ISOK(estat))
		exitstat = EX_OK;
	else if (EP_STAT_IS_SAME(estat, GDP_STAT_NAK_NOROUTE))
		exitstat = EX_NOHOST;
	else if (EP_STAT_IS_SAME(estat, GDP_STAT_NAK_CONFLICT))
		exitstat = EX_CANTCREAT;
	else if (EP_STAT_ISABORT(estat))
		exitstat = EX_SOFTWARE;
	else
		exitstat = EX_UNAVAILABLE;

	// OK status can have values; hide that from the user
	if (EP_STAT_ISOK(estat))
		estat = EP_STAT_OK;

	if (!EP_STAT_ISOK(estat))
		ep_app_message(estat, "exiting with status");
	else if (!quiet)
		fprintf(stderr, "Exiting with status OK\n");

	return exitstat;
}
