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

#include "access/reloptions.h"
#include "access/table.h"
#include "access/tupdesc.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
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

static bool
sql_ident(const char *s)
{
	size_t		n;
	size_t		i;

	if (s == NULL)
		return false;
	n = strlen(s);
	if (n == 0 || n > 256)
		return false;
	if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
		return false;
	for (i = 1; i < n; i++)
	{
		char		c = s[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			  (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return true;
}

static bool
sql_relation(const char *s)
{
	const char *dot;

	if (sql_ident(s))
		return true;
	dot = strchr(s, '.');
	if (dot == NULL || strchr(dot + 1, '.') != NULL)
		return false;
	{
		char		buf[512];
		size_t		n = (size_t) (dot - s);

		if (n == 0 || n >= sizeof(buf) || strlen(dot + 1) >= sizeof(buf))
			return false;
		memcpy(buf, s, n);
		buf[n] = '\0';
		return sql_ident(buf) && sql_ident(dot + 1);
	}
}

static char *
option_value(List *options, const char *name)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, name) == 0)
			return defGetString(def);
	}
	return NULL;
}

static void
reject_unknown_options(List *options, bool table_options)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "path") == 0)
		{
			if (defGetString(def)[0] == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("glacier path option is empty")));
		}
		else if (table_options && strcmp(def->defname, "relation") == 0)
		{
			if (!sql_relation(defGetString(def)))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("glacier relation option must be name or name.name")));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unknown glacier option \"%s\"", def->defname)));
	}
}

static bool
append_pg_const(StringInfo buf, Const *c)
{
	Oid			typoutput;
	bool		varlena;
	char	   *s;
	char	   *p;

	if (c->constisnull)
	{
		appendStringInfoString(buf, "NULL");
		return true;
	}
	switch (c->consttype)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			getTypeOutputInfo(c->consttype, &typoutput, &varlena);
			s = OidOutputFunctionCall(typoutput, c->constvalue);
			appendStringInfoString(buf, s);
			pfree(s);
			return true;
		case BOOLOID:
			appendStringInfoString(buf, DatumGetBool(c->constvalue) ? "1" : "0");
			return true;
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case NAMEOID:
			getTypeOutputInfo(c->consttype, &typoutput, &varlena);
			s = OidOutputFunctionCall(typoutput, c->constvalue);
			appendStringInfoChar(buf, '\'');
			for (p = s; *p != '\0'; p++)
			{
				if (*p == '\'')
					appendStringInfoString(buf, "''");
				else
					appendStringInfoChar(buf, *p);
			}
			appendStringInfoChar(buf, '\'');
			pfree(s);
			return true;
		default:
			return false;
	}
}

static bool
deparse_pg(StringInfo buf, Node *node, Index relid, TupleDesc desc)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Form_pg_attribute attr;

		if (var->varno != relid || var->varlevelsup != 0 || var->varattno <= 0)
			return false;
		if (var->varattno > desc->natts)
			return false;
		attr = TupleDescAttr(desc, var->varattno - 1);
		if (attr->attisdropped || !sql_ident(NameStr(attr->attname)))
			return false;
		appendStringInfoString(buf, NameStr(attr->attname));
		return true;
	}
	if (IsA(node, Const))
		return append_pg_const(buf, (Const *) node);
	if (IsA(node, NullTest))
	{
		NullTest   *test = (NullTest *) node;

		if (test->argisrow)
			return false;
		if (!deparse_pg(buf, (Node *) test->arg, relid, desc))
			return false;
		appendStringInfoString(buf, test->nulltesttype == IS_NULL ? " IS NULL" : " IS NOT NULL");
		return true;
	}
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		ListCell   *lc;
		const char *op;
		bool		first = true;

		if (b->boolop == NOT_EXPR)
			return false;
		op = b->boolop == AND_EXPR ? " AND " : " OR ";
		foreach(lc, b->args)
		{
			if (!first)
				appendStringInfoString(buf, op);
			appendStringInfoChar(buf, '(');
			if (!deparse_pg(buf, (Node *) lfirst(lc), relid, desc))
				return false;
			appendStringInfoChar(buf, ')');
			first = false;
		}
		return !first;
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		char	   *name;
		Node	   *left;
		Node	   *right;
		StringInfoData lbuf;
		StringInfoData rbuf;
		const char *sqlop;
		bool		ok;

		if (list_length(op->args) != 2)
			return false;
		name = get_opname(op->opno);
		if (name == NULL)
			return false;
		if (strcmp(name, "=") == 0)
			sqlop = " = ";
		else if (strcmp(name, "<>") == 0 || strcmp(name, "!=") == 0)
			sqlop = " <> ";
		else if (strcmp(name, "<") == 0)
			sqlop = " < ";
		else if (strcmp(name, "<=") == 0)
			sqlop = " <= ";
		else if (strcmp(name, ">") == 0)
			sqlop = " > ";
		else if (strcmp(name, ">=") == 0)
			sqlop = " >= ";
		else
		{
			pfree(name);
			return false;
		}
		pfree(name);
		left = (Node *) linitial(op->args);
		right = (Node *) lsecond(op->args);
		initStringInfo(&lbuf);
		initStringInfo(&rbuf);
		ok = deparse_pg(&lbuf, left, relid, desc) && deparse_pg(&rbuf, right, relid, desc);
		if (ok)
		{
			appendStringInfoString(buf, lbuf.data);
			appendStringInfoString(buf, sqlop);
			appendStringInfoString(buf, rbuf.data);
		}
		pfree(lbuf.data);
		pfree(rbuf.data);
		return ok;
	}
	return false;
}

static char *
glacier_table_sql(Oid relid, Relation rel, Index varno, List *scan_clauses)
{
	ForeignTable *ft = GetForeignTable(relid);
	ForeignServer *server = GetForeignServer(ft->serverid);
	char	   *path = option_value(ft->options, "path");
	char	   *relation = option_value(ft->options, "relation");
	TupleDesc	desc = RelationGetDescr(rel);
	StringInfoData sql;
	StringInfoData where;
	ListCell   *lc;
	bool		first = true;
	int			i;

	if (path == NULL)
		path = option_value(server->options, "path");
	if (path == NULL || path[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("glacier foreign table needs a path option")));
	if (relation != NULL && !sql_relation(relation))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("glacier relation option must be name or name.name")));

	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT ");
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (attr->attisdropped)
			continue;
		if (!sql_ident(NameStr(attr->attname)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("glacier column names must be letters, digits, and underscores")));
		if (!first)
			appendStringInfoString(&sql, ", ");
		appendStringInfoString(&sql, NameStr(attr->attname));
		first = false;
	}
	if (first)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("glacier foreign table has no columns")));
	if (relation != NULL)
		appendStringInfo(&sql, " FROM %s", relation);

	initStringInfo(&where);
	foreach(lc, scan_clauses)
	{
		Node	   *expr = (Node *) lfirst(lc);
		StringInfoData clause;

		if (IsA(expr, RestrictInfo))
			expr = (Node *) ((RestrictInfo *) expr)->clause;
		initStringInfo(&clause);
		if (deparse_pg(&clause, expr, varno, desc))
		{
			if (where.len > 0)
				appendStringInfoString(&where, " AND ");
			appendStringInfoString(&where, "(");
			appendStringInfoString(&where, clause.data);
			appendStringInfoString(&where, ")");
		}
		pfree(clause.data);
	}
	if (where.len > 0)
		appendStringInfo(&sql, " WHERE %s", where.data);
	pfree(where.data);
	return sql.data;
}

static void
glacierGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	(void) root;
	(void) foreigntableid;
	baserel->rows = 1000;
}

static void
glacierGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	(void) foreigntableid;
	add_path(baserel, (Path *) create_foreignscan_path(root, baserel, NULL,
													   baserel->rows, 1, baserel->rows,
													   NIL, NULL, NULL, NIL, NIL));
}

static ForeignScan *
glacierGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
					  ForeignPath *best_path, List *tlist, List *scan_clauses,
					  Plan *outer_plan)
{
	Relation	rel;
	char	   *sql;
	char	   *path;
	ForeignTable *ft;
	ForeignServer *server;
	List	   *quals;

	(void) root;
	(void) best_path;
	rel = table_open(foreigntableid, NoLock);
	sql = glacier_table_sql(foreigntableid, rel, baserel->relid, scan_clauses);
	ft = GetForeignTable(foreigntableid);
	server = GetForeignServer(ft->serverid);
	path = option_value(ft->options, "path");
	if (path == NULL)
		path = option_value(server->options, "path");
	path = pstrdup(path);
	table_close(rel, NoLock);
	quals = extract_actual_clauses(scan_clauses, false);
	return make_foreignscan(tlist, quals, baserel->relid, NIL,
							list_make2(makeString(path), makeString(sql)),
							NIL, NIL, outer_plan);
}

typedef struct GlacierScan
{
	char	   *path;
	char	   *sql;
	GlacierDatabase *db;
	GlacierConn *conn;
	GlacierResult *result;
	struct ArrowArray batch;
	struct ArrowSchema schema;
	int64_t		pos;
	int64_t		len;
}			GlacierScan;

static void
glacier_scan_stop(GlacierScan *st)
{
	if (st->batch.release != NULL)
		st->batch.release(&st->batch);
	if (st->schema.release != NULL)
		st->schema.release(&st->schema);
	memset(&st->batch, 0, sizeof(st->batch));
	memset(&st->schema, 0, sizeof(st->schema));
	st->pos = 0;
	st->len = 0;
	if (st->result != NULL)
		glacier_result_destroy(st->result);
	st->result = NULL;
	if (st->conn != NULL)
		glacier_disconnect(st->conn);
	st->conn = NULL;
	if (st->db != NULL)
		glacier_close(st->db);
	st->db = NULL;
}

static void
glacier_scan_start(GlacierScan *st)
{
	char	   *err = NULL;

	glacier_scan_stop(st);
	if (glacier_api_version() != GLACIER_API_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("libglacier API is %d, pg_glacier was built for %d",
						glacier_api_version(), GLACIER_API_VERSION)));
	st->db = glacier_open(st->path, &err);
	if (st->db == NULL)
		fail_owned(err);
	st->conn = glacier_connect(st->db, &err);
	if (st->conn == NULL)
		fail_owned(err);
	st->result = glacier_query(st->conn, st->sql, &err);
	if (st->result == NULL)
		fail_owned(err);
	if (glacier_result_error(st->result) != NULL)
		fail_result(st->result);
}

static void
glacierBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;
	GlacierScan *st;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	st = (GlacierScan *) palloc0(sizeof(GlacierScan));
	st->path = pstrdup(strVal(linitial(fs->fdw_private)));
	st->sql = pstrdup(strVal(lsecond(fs->fdw_private)));
	node->fdw_state = st;
	glacier_scan_start(st);
}

static TupleTableSlot *
glacierIterateForeignScan(ForeignScanState *node)
{
	GlacierScan *st = (GlacierScan *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	int			rc;

	if (st == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("glacier scan was not started")));
	ExecClearTuple(slot);
	while (st->pos >= st->len)
	{
		if (st->batch.release != NULL)
			st->batch.release(&st->batch);
		if (st->schema.release != NULL)
			st->schema.release(&st->schema);
		memset(&st->batch, 0, sizeof(st->batch));
		memset(&st->schema, 0, sizeof(st->schema));
		rc = glacier_result_next_arrow(st->result, &st->batch, &st->schema);
		if (rc == 0)
			return slot;
		if (rc != 1)
		{
			if (glacier_result_error(st->result) != NULL)
				fail_result(st->result);
			glacier_fail("reading the next batch failed");
		}
		st->len = st->batch.length;
		st->pos = 0;
	}
	{
		int			natts = desc->natts;
		int			arrow_col = 0;
		int			att;

		if (arrow_is_null(&st->batch, st->pos))
		{
			for (att = 0; att < natts; att++)
			{
				slot->tts_values[att] = (Datum) 0;
				slot->tts_isnull[att] = true;
			}
		}
		else
		{
			for (att = 0; att < natts; att++)
			{
				if (TupleDescAttr(desc, att)->attisdropped)
				{
					slot->tts_values[att] = (Datum) 0;
					slot->tts_isnull[att] = true;
					continue;
				}
				if (st->schema.children == NULL || st->batch.children == NULL ||
					arrow_col >= st->schema.n_children ||
					st->schema.children[arrow_col] == NULL ||
					st->batch.children[arrow_col] == NULL)
					glacier_fail("Arrow batch is missing a column");
				slot->tts_values[att] = read_value(st->schema.children[arrow_col],
												   st->batch.children[arrow_col],
												   st->pos,
												   TupleDescAttr(desc, att)->atttypid,
												   &slot->tts_isnull[att]);
				arrow_col++;
			}
		}
		ExecStoreVirtualTuple(slot);
		st->pos++;
	}
	return slot;
}

static void
glacierReScanForeignScan(ForeignScanState *node)
{
	GlacierScan *st = (GlacierScan *) node->fdw_state;

	if (st != NULL)
		glacier_scan_start(st);
}

static void
glacierEndForeignScan(ForeignScanState *node)
{
	GlacierScan *st = (GlacierScan *) node->fdw_state;

	if (st == NULL)
		return;
	glacier_scan_stop(st);
	node->fdw_state = NULL;
}

PG_FUNCTION_INFO_V1(glacier_fdw_handler);

Datum
glacier_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	(void) fcinfo;
	routine->GetForeignRelSize = glacierGetForeignRelSize;
	routine->GetForeignPaths = glacierGetForeignPaths;
	routine->GetForeignPlan = glacierGetForeignPlan;
	routine->BeginForeignScan = glacierBeginForeignScan;
	routine->IterateForeignScan = glacierIterateForeignScan;
	routine->ReScanForeignScan = glacierReScanForeignScan;
	routine->EndForeignScan = glacierEndForeignScan;
	PG_RETURN_POINTER(routine);
}

PG_FUNCTION_INFO_V1(glacier_fdw_validator);

Datum
glacier_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	reject_unknown_options(options, catalog == ForeignTableRelationId);
	if (catalog != ForeignTableRelationId && catalog != ForeignServerRelationId &&
		catalog != ForeignDataWrapperRelationId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("glacier options are for a server or a foreign table")));
	PG_RETURN_VOID();
}
