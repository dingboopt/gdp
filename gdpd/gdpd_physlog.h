/* vim: set ai sw=4 sts=4 ts=4 : */

#include "gdpd.h"

/*
**	Headers for the physical log implementation.
**		This is how bytes are actually laid out on the disk.
*/

EP_STAT			gcl_physlog_init();

EP_STAT			gcl_offset_cache_init(
						gdp_gcl_t *gclh);

EP_STAT			gcl_read(
						gdp_gcl_t *gclh,
						gdp_datum_t *datum);

EP_STAT			gcl_create(
						gcl_name_t gcl_name,
						gdp_gcl_t **pgclh);

EP_STAT			gcl_open(
						gcl_name_t gcl_name,
						gdp_iomode_t mode,
						gdp_gcl_t **pgclh);

EP_STAT			gcl_close(
						gdp_gcl_t *gclh);

EP_STAT			gcl_append(
						gdp_gcl_t *gclh,
						gdp_datum_t *datum);

#define GCL_DIR				"/var/tmp/gcl"
#define GCL_VERSION			0

#define GCL_LOG_MAGIC		UINT64_C(0x8F4E39104A803299)

#define GCL_DATA_SUFFIX		".data"
#define GCL_INDEX_SUFFIX	".index"

#define GCL_READ_BUFFER_SIZE 4096

typedef struct gcl_log_record
{
	gdp_recno_t		recno;
	EP_TIME_SPEC	timestamp;
	int64_t			data_length;
	char			data[];
} gcl_log_record;

/*
 * The GCL metadata consists of num_metadata_entires (N), followed by
 * N lengths (l_1, l_2, ... , l_N), followed by N **non-null-terminated**
 * strings of lengths l_1, ... , l_N. It is up to the user application to
 * interpret the metadata strings.
 *
 * num_metadata_entries is a int16_t
 * l_1, ... , l_N are int16_t
 */

typedef struct gcl_log_header
{
	int64_t magic;
	int64_t version;
	int16_t header_size; 	// the total size of the header such that
							// the data records begin at offset header_size
	int16_t log_type;
	int16_t num_metadata_entries;
	int16_t pad;			// the header is padded anyway by compiler
} gcl_log_header;

typedef struct gcl_index_record
{
	gdp_recno_t recno;
	int64_t offset;
} gcl_index_record;
