/* Copyright (c) 2026 Marcelo Tesla
 *
 * PostgreSQL License. See LICENSE in this directory.
 *
 * pg_glacier is a PostgreSQL extension. _PG_init does not open a Glacier
 * session: that would run in the postmaster when the library is preloaded.
 * glacier_open runs only when glacier_query is called, in the backend.
 * One connection is one call. It is not shared across backends.
 */

#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "glacier.h"

#include <limits.h>
#include <string.h>

PG_MODULE_MAGIC;

void _PG_init(void);

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_glacier must be listed in shared_preload_libraries")));
}

PG_FUNCTION_INFO_V1(pg_glacier_query);

/* Microseconds from 1970-01-01 to the PostgreSQL epoch, 2000-01-01. */
#define PG_EPOCH_DELTA_US 946684800000000LL

static void
glacier_fail(const char *msg)
{
	ereport(ERROR,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg("glacier: %s", (msg != NULL && msg[0] != '\0') ? msg : "failed")));
}

static void
fail_owned(char *err)
{
	char		buf[768];

	snprintf(buf, sizeof(buf), "%s", (err != NULL && err[0] != '\0') ? err : "failed");
	glacier_free(err);
	glacier_fail(buf);
}

static void
fail_result(GlacierResult *result)
{
	const char *msg = glacier_result_error(result);
	char		buf[768];

	snprintf(buf, sizeof(buf), "%s", (msg != NULL && msg[0] != '\0') ? msg : "query failed");
	glacier_fail(buf);
}

static bool
arrow_is_null(const struct ArrowArray *col, int64_t row)
{
	const uint8_t *bits;
	int64_t		i;

	if (col->buffers == NULL || col->n_buffers < 1 || col->buffers[0] == NULL)
		return false;
	if (col->null_count == 0)
		return false;
	bits = col->buffers[0];
	i = col->offset + row;
	return ((bits[i >> 3] >> (i & 7)) & 1) == 0;
}

static const void *
arrow_buf(const struct ArrowArray *col, int64_t which)
{
	if (col->buffers == NULL || col->n_buffers <= which || col->buffers[which] == NULL)
		return NULL;
	return col->buffers[which];
}

static int64_t
arrow_index(const struct ArrowArray *col, int64_t row)
{
	if (row < 0 || row >= col->length)
		glacier_fail("Arrow batch row is out of range");
	return col->offset + row;
}

static bool
type_is_text(Oid typid)
{
	return typid == TEXTOID || typid == VARCHAROID || typid == BPCHAROID;
}

static bool
type_is_int(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID;
}

static Datum
datum_int(int64_t v, Oid typid)
{
	if (typid == INT8OID)
		return Int64GetDatum(v);
	if (typid == NUMERICOID)
		return DirectFunctionCall1(int8_numeric, Int64GetDatum(v));
	if (typid == INT4OID)
	{
		if (v < (int64_t) INT_MIN || v > (int64_t) INT_MAX)
			glacier_fail("integer does not fit int4");
		return Int32GetDatum((int32) v);
	}
	if (typid == INT2OID)
	{
		if (v < (int64_t) SHRT_MIN || v > (int64_t) SHRT_MAX)
			glacier_fail("integer does not fit int2");
		return Int16GetDatum((int16) v);
	}
	if (typid == FLOAT8OID)
		return Float8GetDatum((float8) v);
	if (typid == FLOAT4OID)
		return Float4GetDatum((float4) v);
	glacier_fail("integer column does not match the AS type");
	pg_unreachable();
}

static Datum
datum_float(double v, Oid typid)
{
	if (typid == FLOAT8OID)
		return Float8GetDatum(v);
	if (typid == FLOAT4OID)
		return Float4GetDatum((float4) v);
	if (typid == NUMERICOID)
		return DirectFunctionCall1(float8_numeric, Float8GetDatum(v));
	glacier_fail("float column does not match the AS type");
	pg_unreachable();
}

static Datum
datum_text(const char *data, int64_t len, Oid typid)
{
	text	   *t;

	if (!type_is_text(typid))
		glacier_fail("string column does not match the AS type");
	if (len < 0 || len > (int64_t) INT_MAX)
		glacier_fail("string value is too long for PostgreSQL");
	t = cstring_to_text_with_len(data != NULL ? data : "", (int) len);
	return PointerGetDatum(t);
}

static int
i128_text(const uint8_t *le, int scale, char *out, size_t cap)
{
	unsigned __int128 mag;
	int			neg;
	int			nd = 0;
	int			pos = 0;
	int			i;
	char		digs[48];

	if (cap < 4)
		return -1;
	memcpy(&mag, le, 16);
	neg = (le[15] & 0x80) != 0;
	if (neg)
		mag = ~mag + 1;
	if (mag == 0)
		digs[nd++] = '0';
	while (mag != 0 && nd < (int) sizeof(digs))
	{
		digs[nd++] = (char) ('0' + (int) (mag % 10));
		mag /= 10;
	}
	if (mag != 0)
		return -1;
	if (neg && !(nd == 1 && digs[0] == '0'))
		out[pos++] = '-';
	if (scale <= 0)
	{
		for (i = nd - 1; i >= 0; i--)
		{
			if (pos + 1 >= (int) cap)
				return -1;
			out[pos++] = digs[i];
		}
		for (i = 0; i < -scale; i++)
		{
			if (pos + 1 >= (int) cap)
				return -1;
			out[pos++] = '0';
		}
		out[pos] = '\0';
		return pos;
	}
	if (nd > scale)
	{
		for (i = nd - 1; i >= scale; i--)
		{
			if (pos + 1 >= (int) cap)
				return -1;
			out[pos++] = digs[i];
		}
	}
	else
	{
		if (pos + 1 >= (int) cap)
			return -1;
		out[pos++] = '0';
	}
	if (pos + 1 >= (int) cap)
		return -1;
	out[pos++] = '.';
	for (i = scale - 1; i >= 0; i--)
	{
		if (pos + 1 >= (int) cap)
			return -1;
		out[pos++] = (i < nd) ? digs[i] : '0';
	}
	out[pos] = '\0';
	return pos;
}

static bool
timestamp_us(const char *fmt, int64_t raw, int64_t *us)
{
	if (fmt[0] != 't' || fmt[1] != 's' || fmt[2] == '\0')
		return false;
	switch (fmt[2])
	{
		case 's':
			if (raw > INT64_MAX / 1000000LL || raw < INT64_MIN / 1000000LL)
				return false;
			*us = raw * 1000000LL;
			return true;
		case 'm':
			if (raw > INT64_MAX / 1000LL || raw < INT64_MIN / 1000LL)
				return false;
			*us = raw * 1000LL;
			return true;
		case 'u':
			*us = raw;
			return true;
		case 'n':
			*us = raw / 1000LL;
			return true;
		default:
			return false;
	}
}

static Datum
read_value(const struct ArrowSchema *field, const struct ArrowArray *col,
		   int64_t row, Oid typid, bool *isnull)
{
	const char *fmt;
	int64_t		ix;

	if (field->format == NULL)
		glacier_fail("Arrow column has no format");
	fmt = field->format;
	if (field->dictionary != NULL || col->dictionary != NULL || field->n_children != 0)
		glacier_fail("nested or dictionary-encoded Arrow values are not supported");
	if (arrow_is_null(col, row))
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;
	ix = arrow_index(col, row);

	if (strcmp(fmt, "b") == 0)
	{
		const uint8_t *bits = arrow_buf(col, 1);
		int			v;

		if (bits == NULL)
			glacier_fail("boolean column is missing its value buffer");
		v = (bits[ix >> 3] >> (ix & 7)) & 1;
		if (typid == BOOLOID)
			return BoolGetDatum(v != 0);
		if (type_is_int(typid))
			return datum_int(v, typid);
		glacier_fail("boolean column does not match the AS type");
	}
	if (strcmp(fmt, "i") == 0)
	{
		const int32_t *v = arrow_buf(col, 1);

		if (v == NULL)
			glacier_fail("int32 column is missing its value buffer");
		if (type_is_int(typid) || typid == NUMERICOID || typid == FLOAT4OID || typid == FLOAT8OID)
			return datum_int(v[ix], typid);
		glacier_fail("int32 column does not match the AS type");
	}
	if (strcmp(fmt, "l") == 0)
	{
		const int64_t *v = arrow_buf(col, 1);

		if (v == NULL)
			glacier_fail("int64 column is missing its value buffer");
		if (type_is_int(typid) || typid == NUMERICOID || typid == FLOAT4OID || typid == FLOAT8OID)
			return datum_int(v[ix], typid);
		glacier_fail("int64 column does not match the AS type");
	}
	if (strcmp(fmt, "f") == 0)
	{
		const float *v = arrow_buf(col, 1);

		if (v == NULL)
			glacier_fail("float column is missing its value buffer");
		if (typid == FLOAT4OID || typid == FLOAT8OID || typid == NUMERICOID)
			return datum_float((double) v[ix], typid);
		glacier_fail("float column does not match the AS type");
	}
	if (strcmp(fmt, "g") == 0)
	{
		const double *v = arrow_buf(col, 1);

		if (v == NULL)
			glacier_fail("double column is missing its value buffer");
		if (typid == FLOAT4OID || typid == FLOAT8OID || typid == NUMERICOID)
			return datum_float(v[ix], typid);
		glacier_fail("double column does not match the AS type");
	}
	if (strcmp(fmt, "u") == 0 || strcmp(fmt, "U") == 0)
	{
		const char *bytes = arrow_buf(col, 2);
		int64_t		start;
		int64_t		end;

		if (strcmp(fmt, "u") == 0)
		{
			const int32_t *off = arrow_buf(col, 1);

			if (off == NULL)
				glacier_fail("string column is missing its offset buffer");
			start = off[ix];
			end = off[ix + 1];
		}
		else
		{
			const int64_t *off = arrow_buf(col, 1);

			if (off == NULL)
				glacier_fail("string column is missing its offset buffer");
			start = off[ix];
			end = off[ix + 1];
		}
		if (end < start)
			glacier_fail("string column has a bad offset");
		if (end == start)
			return datum_text("", 0, typid);
		if (bytes == NULL)
			glacier_fail("string column is missing its byte buffer");
		return datum_text(bytes + start, end - start, typid);
	}
	if (strncmp(fmt, "ts", 2) == 0)
	{
		const int64_t *v = arrow_buf(col, 1);
		int64_t		us;

		if (v == NULL)
			glacier_fail("timestamp column is missing its value buffer");
		if (!timestamp_us(fmt, v[ix], &us))
			glacier_fail("timestamp unit is not supported");
		if (typid == TIMESTAMPOID || typid == TIMESTAMPTZOID)
			return TimestampGetDatum((Timestamp) (us - PG_EPOCH_DELTA_US));
		if (typid == INT8OID)
			return Int64GetDatum(us);
		glacier_fail("timestamp column does not match the AS type");
	}
	if (strncmp(fmt, "d:", 2) == 0)
	{
		const uint8_t *raw = arrow_buf(col, 1);
		int			scale = 0;
		char		textv[80];
		int			n;

		if (raw == NULL)
			glacier_fail("decimal column is missing its value buffer");
		sscanf(fmt + 2, "%*d,%d", &scale);
		n = i128_text(raw + (size_t) ix * 16, scale, textv, sizeof(textv));
		if (n < 0)
			glacier_fail("decimal value does not fit a PostgreSQL numeric");
		if (typid == NUMERICOID)
			return DirectFunctionCall3(numeric_in,
									   CStringGetDatum(textv),
									   ObjectIdGetDatum(InvalidOid),
									   Int32GetDatum(-1));
		if (type_is_text(typid))
			return datum_text(textv, n, typid);
		glacier_fail("decimal column does not match the AS type");
	}
	if (strncmp(fmt, "w:16", 4) == 0)
	{
		const uint8_t *raw = arrow_buf(col, 1);
		const uint8_t *b;
		char		hex[37];

		if (raw == NULL)
			glacier_fail("fixed-width column is missing its value buffer");
		b = raw + (size_t) ix * 16;
		if (typid == BYTEAOID)
		{
			bytea	   *out = (bytea *) palloc(VARHDRSZ + 16);

			SET_VARSIZE(out, VARHDRSZ + 16);
			memcpy(VARDATA(out), b, 16);
			return PointerGetDatum(out);
		}
		snprintf(hex, sizeof(hex),
				 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
				 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
				 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
		if (typid == UUIDOID)
			return DirectFunctionCall1(uuid_in, CStringGetDatum(hex));
		if (type_is_text(typid))
			return datum_text(hex, 36, typid);
		glacier_fail("uuid column does not match the AS type");
	}
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("glacier: Arrow format \"%s\" is not supported", fmt)));
	pg_unreachable();
}

Datum
pg_glacier_query(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	text	   *path_t = PG_GETARG_TEXT_PP(0);
	text	   *sql_t = PG_GETARG_TEXT_PP(1);
	char	   *path = text_to_cstring(path_t);
	char	   *sql = text_to_cstring(sql_t);
	GlacierDatabase *volatile db = NULL;
	GlacierConn *volatile conn = NULL;
	GlacierResult *volatile result = NULL;
	struct ArrowArray *batch;
	struct ArrowSchema *schema;
	void		(*volatile release_array) (struct ArrowArray *) = NULL;
	void		(*volatile release_schema) (struct ArrowSchema *) = NULL;

	if (glacier_api_version() != GLACIER_API_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("libglacier API is %d, pg_glacier was built for %d",
						glacier_api_version(), GLACIER_API_VERSION)));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("glacier_query must be called where a set is expected")));
	if ((rsinfo->allowedModes & SFRM_Materialize) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("glacier_query requires materialize mode")));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("call glacier_query with a column list: AS t(col type, ...)")));

	batch = (struct ArrowArray *) palloc0(sizeof(struct ArrowArray));
	schema = (struct ArrowSchema *) palloc0(sizeof(struct ArrowSchema));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	MemoryContextSwitchTo(oldcontext);
	tupdesc = BlessTupleDesc(tupdesc);

	PG_TRY();
	{
		char	   *err = NULL;
		Datum	   *values;
		bool	   *nulls;
		int			natts = tupdesc->natts;
		int			rc;

		db = glacier_open(path, &err);
		if (db == NULL)
			fail_owned(err);
		conn = glacier_connect(db, &err);
		if (conn == NULL)
			fail_owned(err);
		result = glacier_query(conn, sql, &err);
		if (result == NULL)
			fail_owned(err);
		if (glacier_result_error(result) != NULL)
			fail_result(result);

		values = (Datum *) palloc(sizeof(Datum) * (natts > 0 ? natts : 1));
		nulls = (bool *) palloc(sizeof(bool) * (natts > 0 ? natts : 1));

		for (;;)
		{
			int64_t		r;
			int			c;

			if (release_array != NULL)
			{
				release_array(batch);
				release_array = NULL;
			}
			if (release_schema != NULL)
			{
				release_schema(schema);
				release_schema = NULL;
			}
			memset(batch, 0, sizeof(*batch));
			memset(schema, 0, sizeof(*schema));
			rc = glacier_result_next_arrow(result, batch, schema);
			release_array = batch->release;
			release_schema = schema->release;
			if (rc == 0)
				break;
			if (rc != 1)
			{
				if (glacier_result_error(result) != NULL)
					fail_result(result);
				glacier_fail("reading the next batch failed");
			}
			if (schema->n_children != natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("glacier returned %ld columns, the AS clause has %d",
								(long) schema->n_children, natts)));
			for (r = 0; r < batch->length; r++)
			{
				if (arrow_is_null(batch, r))
				{
					for (c = 0; c < natts; c++)
					{
						values[c] = (Datum) 0;
						nulls[c] = true;
					}
				}
				else
				{
					for (c = 0; c < natts; c++)
					{
						if (schema->children == NULL || schema->children[c] == NULL ||
							batch->children == NULL || batch->children[c] == NULL)
							glacier_fail("Arrow batch is missing a column");
						values[c] = read_value(schema->children[c], batch->children[c],
											   r, TupleDescAttr(tupdesc, c)->atttypid,
											   &nulls[c]);
					}
				}
				tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			}
		}
	}
	PG_CATCH();
	{
		if (release_array != NULL)
			release_array(batch);
		if (release_schema != NULL)
			release_schema(schema);
		if (result != NULL)
			glacier_result_destroy(result);
		if (conn != NULL)
			glacier_disconnect(conn);
		if (db != NULL)
			glacier_close(db);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (release_array != NULL)
		release_array(batch);
	if (release_schema != NULL)
		release_schema(schema);
	glacier_result_destroy(result);
	glacier_disconnect(conn);
	glacier_close(db);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	PG_RETURN_NULL();
}
