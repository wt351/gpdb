/*-------------------------------------------------------------------------
 *
 * gphdfsformatter.c
 *
 * This is the GPDB side for serializing and deserializing a tuple to a
 * common format which can be parsed/generated by Hadoop's GPDBWritable.
 *
 * The serialized form produced by gphdfsformatter_export is identical to
 * GPDBWritable.write(DataOutput out).
 *
 * The deserialization gphdfsformatter_import can deserialize the
 * bytes produced by gphdfsformatter_export and GPDBWritable.write.
 *
 *
 * Copyright (c) 2011, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

#include "access/formatter.h"
#include "catalog/pg_proc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/syscache.h"

#include "utils/lsyscache.h"
#include <unistd.h>

/* Do the module magic dance */
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(gphdfsformatter_export);
PG_FUNCTION_INFO_V1(gphdfsformatter_import);

Datum gphdfsformatter_export(PG_FUNCTION_ARGS);
Datum gphdfsformatter_import(PG_FUNCTION_ARGS);

typedef struct {
	/* The Datum/null of the tuple */
	Datum     *values;
	bool      *nulls;

	/* The export formatted value/len */
	char*     *outval;
	int       *outlen;
	int       *outpadlen; /* padding length for alignment */

	/* Buffer to hold the export formatted tuple */
	StringInfo export_format_tuple;

	/* (Binary) In/Out functions
	 * For import, it's input function; for export, it's output function.
	 * If the type is using binary format, the function will the be binary
	 * conversion function.
	 */
	FmgrInfo *io_functions;
	Oid      *typioparams;
} format_t;


/**
 * Serialize the object using the following format:
 * Total Length | Version | #columns | Col type | Col type |... | Null Bit array   | Col val...
 * 4 byte		| 2 byte	2 byte	   2 byte     2 byte	      ceil(#col/8) byte	 fixed length or var len
 *
 * For fixed length type, we know the length.
 * In the col val, we align pad according to the alignemnt requirement of the type.
 * For var length type, the alignment is always 4 byte.
 * For var legnth type, col val is <4 byte length><payload val>
 */
#define GPDBWRITABLE_VERSION 1

/* Bit flag */
#define GPDBWRITABLE_BITFLAG_ISNULL 1 /* Column is null */

/*
 * appendStringInfoFill
 *
 * Append a single byte, repeated 0 or more times, to str.
 */
static void
appendStringInfoFill(StringInfo str, int occurrences, char ch)
{
    /* Length must not overflow. */
    if (str->len + occurrences <= str->len)
        return;

    /* Make more room if needed */
    if (str->len + occurrences >= str->maxlen)
	    enlargeStringInfo(str, occurrences);

    /* Fill specified number of bytes with the character. */
    memset(str->data + str->len, ch, occurrences);
    str->len += occurrences;
    str->data[str->len] = '\0';
}

/**
 * Write a int4 to the buffer
 */
void appendIntToBuffer(StringInfo buf, int val);
void appendIntToBuffer(StringInfo buf, int val)
{
	uint32 n32 = htonl((uint32) val);
	appendBinaryStringInfo(buf, (char *) &n32, 4);
}

/**
 * Read a int to the buffer, given the offset;
 * it will return the int value and increase the offset
 */
static int readIntFromBuffer(char* buffer, int *offset)
{
	uint32 n32;
	memcpy(&n32, &buffer[*offset], sizeof(int));
	*offset += 4;
	return ntohl(n32);
}

/**
 * Write a int2 to the buffer
 */
void appendInt2ToBuffer(StringInfo buf, uint16 val);
void appendInt2ToBuffer(StringInfo buf, uint16 val)
{
	uint16 n16 = htons((uint16) val);
	appendBinaryStringInfo(buf, (char *) &n16, 2);
}

/**
 * Read a int2 to the buffer, given the offset;
 * it will return the int value and increase the offset
 */
static uint16 readInt2FromBuffer(char* buffer, int *offset)
{
	uint16 n16;
	memcpy(&n16, &buffer[*offset], sizeof(uint16));
	*offset += 2;
	return ntohs(n16);
}

/**
 * Write a int1 to the buffer
 */
void appendInt1ToBuffer(StringInfo buf, uint8 val);
void appendInt1ToBuffer(StringInfo buf, uint8 val)
{
	unsigned char n8;
	n8 = (unsigned char)val;
	appendBinaryStringInfo(buf, (char *) &n8, 1);
}

/**
 * Read a int1 to the buffer, given the offset;
 * it will return the int value and increase the offset
 */
static uint8 readInt1FromBuffer(char* buffer, int *offset)
{
	uint8 n8;
	memcpy(&n8, &buffer[*offset], sizeof(uint8));
	*offset += 1;
	return n8;
}

/**
 * Tells whether the given type will be formatted as binary or text
 */
static inline bool isBinaryFormatType(Oid typeid)
{
	/* For version 1, we support binary format for these type */
	if (typeid == BOOLOID   ||
		typeid == BYTEAOID  ||
		typeid == FLOAT4OID ||
		typeid == FLOAT8OID ||
		typeid == INT2OID   ||
		typeid == INT4OID   ||
		typeid == INT8OID)
		return true;
	return false;
}

/**
 * Tells whether the given type is variable length
 */
static inline bool isVariableLength(Oid typeid)
{
	if (typeid == BYTEAOID || !isBinaryFormatType(typeid))
		return true;
	return false;
}

/**
 * Convert typeOID to java enum DBType.ordinal()
 */
static inline int8 getJavaEnumOrdinal(Oid typeid)
{
	/* For version 1, we support binary format for these type */
	switch (typeid)
	{
	case INT8OID:   return 0;
	case BOOLOID:   return 1;
	case FLOAT8OID: return 2;
	case INT4OID:   return 3;
	case FLOAT4OID: return 4;
	case INT2OID:   return 5;
	case BYTEAOID:  return 6;
	}
	return 7;
}

/**
 * Convert java enum DBType.ordinal() to typeOID
 */
static inline Oid getTypeOidFromJavaEnumOrdinal(int8 enumType)
{
	switch(enumType)
	{
	case 0: return INT8OID;
	case 1: return BOOLOID;
	case 2: return FLOAT8OID;
	case 3: return INT4OID;
	case 4: return FLOAT4OID;
	case 5: return INT2OID;
	case 6: return BYTEAOID;
	case 7: return TEXTOID;
	default: ereport(ERROR,
					 (errcode(ERRCODE_DATA_EXCEPTION),
					  errmsg("Ill-formatted record: unknown Java Enum Ordinal (%d)", enumType)));

	}
	return 0;
}

/**
 * Helper to determine the size of the null byte array
 */
static int getNullByteArraySize(int colCnt)
{
	return (colCnt/8) + (colCnt%8 != 0 ? 1:0);
}

/**
 * Helper routine to convert boolean array to byte array
 */
static bits8* boolArrayToByteArray(bool* data, int len, int* outlen)
{
	int i, j, k;
	bits8* result;
	*outlen = getNullByteArraySize(len);
	result  = palloc0(*outlen * sizeof(bits8));

	for (i = 0, j = 0, k = 7; i < len; i++) {
        result[j] |= (data[i] ? 1 : 0) << k--;
        if (k < 0) { j++; k = 7; }
    }
    return result;
}

/**
 * Helper routine to convert byte array to boolean array
 * It'll write the output to booldata.
 */
static void byteArrayToBoolArray(bits8* data, int len, bool** booldata, int boollen)
{
	int i, j, k;
	for(i=0, j=0, k=7; i<boollen; i++)
	{
		(*booldata)[i] =  ((data[j] >> k--) & 0x01) == 1;
        if (k < 0) { j++; k = 7; }
	}
}

Datum
gphdfsformatter_export(PG_FUNCTION_ARGS)
{
	HeapTupleHeader		rec	= PG_GETARG_HEAPTUPLEHEADER(0);
	TupleDesc           tupdesc;
	HeapTupleData		tuple;
	format_t           *myData;
	int                 datlen;
	AttrNumber          ncolumns = 0;
	AttrNumber          i;
	MemoryContext 		per_row_ctx, oldcontext;
	bits8               *nullBit;
	int                  nullBitLen;
	int                  endpadding;

	/* Must be called via the external table format manager */
	if (!CALLED_AS_FORMATTER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("cannot execute gphdfsformatter_export outside format manager")));

	tupdesc = FORMATTER_GET_TUPDESC(fcinfo);

	/* Get our internal description of the formatter */
	ncolumns = tupdesc->natts;
	myData = (format_t *) FORMATTER_GET_USER_CTX(fcinfo);

	/**
	 * Initialize the context structure
	 */
	if (myData == NULL)
	{

		myData          = palloc(sizeof(format_t));
		myData->values  = palloc(sizeof(Datum) * ncolumns);
		myData->outval  = palloc(sizeof(char*) * ncolumns);
		myData->nulls   = palloc(sizeof(bool) * ncolumns);
		myData->outlen  = palloc(sizeof(int) * ncolumns);
		myData->outpadlen  = palloc(sizeof(int) * ncolumns);
		myData->io_functions  = palloc(sizeof(FmgrInfo) * ncolumns);
		myData->export_format_tuple = makeStringInfo();

		/* setup the text/binary input function */
		for(i=0; i<ncolumns; i++)
		{
			Oid type = tupdesc->attrs[i]->atttypid;
			bool isvarlena;
			Oid functionId;

			/* External table do not support dropped columns; error out now */
			if (tupdesc->attrs[i]->attisdropped)
				ereport(ERROR,
						(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
						 errmsg("cannot handle external table with dropped columns")));

			/* Get the text/binary "send" function */
			if (isBinaryFormatType(type))
				getTypeBinaryOutputInfo(type, &(functionId), &isvarlena);
			else
				getTypeOutputInfo(type, &(functionId), &isvarlena);
			fmgr_info(functionId, &(myData->io_functions[i]));
		}

		FORMATTER_SET_USER_CTX(fcinfo, myData);
	}

	per_row_ctx = FORMATTER_GET_PER_ROW_MEM_CTX(fcinfo);
	oldcontext  = MemoryContextSwitchTo(per_row_ctx);

	/* break the input tuple into fields */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_data = rec;
	heap_deform_tuple(&tuple, tupdesc, myData->values, myData->nulls);

	/**
	 * Starting from here. The conversion to bytes is  exactly the
	 * same as GPDBWritable.toBytes()
	 */

	/**
	 * Now, compute the total payload and header length
	 * header = total length (4 byte), Version (2 byte), #col (2 byte)
	 * col type array = #col * 1 byte
	 * null bit array = ceil(#col/8)
	 */
	datlen = sizeof(int32)+sizeof(int16)+sizeof(int16);
	datlen += ncolumns;
	datlen += getNullByteArraySize(ncolumns);

	/**
	 * We need to know the total length of the tuple. So, we've to
	 * transformed each column so that we know the transformed size
	 * and the alignment padding.
	 *
	 * Since we're computing the conversion function, we use
	 * per-row memory context inside the loop.
	 */
	for (i = 0; i < ncolumns; i++)
	{
		Oid   type       = tupdesc->attrs[i]->atttypid;
		Datum val        = myData->values[i];
		bool  nul        = myData->nulls[i];
		FmgrInfo *iofunc = &(myData->io_functions[i]);
		int alignpadlen   = 0;

		if (nul)
			myData->outlen[i]=0;
		else
		{
			if (isBinaryFormatType(type))
			{
				bytea* tmpval = SendFunctionCall(iofunc, val);;

				/* NOTE: exclude the header length */
				myData->outval[i] = VARDATA(tmpval);
				myData->outlen[i] = VARSIZE_ANY_EXHDR(tmpval);
			}
			else
			{
				/* NOTE: include the "\0" in the length for text format */
				myData->outval[i] = OutputFunctionCall(iofunc, val);
				myData->outlen[i] = strlen(myData->outval[i])+1;
			}

			/* For variable length type, we added a 4 byte length header.
			 * So, it'll be aligned int4.
			 * For fixed length type, we'll use the type aligment.
			 */
			if (isVariableLength(type))
			{
				alignpadlen = INTALIGN(datlen) - datlen;
				datlen += sizeof(int32);
			}
			else
				alignpadlen = att_align_nominal(datlen, tupdesc->attrs[i]->attalign) - datlen;
			myData->outpadlen[i] = alignpadlen;
			datlen += alignpadlen;
		}
		datlen += myData->outlen[i];
	}

	/*
	 * Add the final alignment padding for the next record
	 */
	endpadding = DOUBLEALIGN(datlen) - datlen;
	datlen += endpadding;

	/* Now, we're done with per-row computation. Switch back to the
	 * old memory context
	 */
	MemoryContextSwitchTo(oldcontext);

	/* Resize buffer, if needed
	 * The new size includes the 4 byte VARHDSZ, the entire payload
	 * and 1 more byte for '\0' that StringInfo always ends with.
	 */
	if (myData->export_format_tuple->maxlen < VARHDRSZ+datlen+1)
	{
		pfree(myData->export_format_tuple->data);
		initStringInfoOfSize(myData->export_format_tuple, VARHDRSZ+datlen+1);
	}

	/* Reset the export format buffer */
	resetStringInfo(myData->export_format_tuple);

	/* Reserve VARHDRSZ bytes for the bytea length word */
	appendStringInfoFill(myData->export_format_tuple, VARHDRSZ, '\0');

	/* Construct the packet header */
	appendIntToBuffer(myData->export_format_tuple, datlen);
	appendInt2ToBuffer(myData->export_format_tuple, GPDBWRITABLE_VERSION);
	appendInt2ToBuffer(myData->export_format_tuple, ncolumns);

	/* Write col type */
	for(i=0; i<ncolumns; i++)
		appendInt1ToBuffer(myData->export_format_tuple,
				getJavaEnumOrdinal(tupdesc->attrs[i]->atttypid));

	/* Write Nullness */
	nullBit = boolArrayToByteArray(myData->nulls, ncolumns, &nullBitLen);
	appendBinaryStringInfo(myData->export_format_tuple, nullBit, nullBitLen);

	/* Column Value */
	for(i = 0; i < ncolumns; i++)
	{
		if (!myData->nulls[i])
		{
			/* Pad the alignment byte first */
			appendStringInfoFill(myData->export_format_tuple, myData->outpadlen[i], '\0');

			/* For variable length type, we added a 4 byte length header */
			if (isVariableLength(tupdesc->attrs[i]->atttypid))
				appendIntToBuffer(myData->export_format_tuple, myData->outlen[i]);

			/* Now, write the actual column value */
			appendBinaryStringInfo(myData->export_format_tuple,
					myData->outval[i], myData->outlen[i]);
		}
	}

	/* End padding */
	appendStringInfoFill(myData->export_format_tuple, endpadding, '\0');

	Insist(myData->export_format_tuple->len == datlen + VARHDRSZ);
	SET_VARSIZE(myData->export_format_tuple->data, datlen + VARHDRSZ);
	PG_RETURN_BYTEA_P(myData->export_format_tuple->data);
}

Datum
gphdfsformatter_import(PG_FUNCTION_ARGS)
{
	HeapTuple			tuple;
	TupleDesc           tupdesc;
	MemoryContext 		per_row_ctx, oldcontext;
	format_t           *myData;
	char               *data_buf;
	AttrNumber          ncolumns = 0;
	AttrNumber          i;
	int			  		data_cur;
	int                 data_len;
	int                 tuplelen = 0;
	int                 bufidx = 0;
	int16               version = 0;
	int16               colcnt = 0;
	int                 remaining = 0;

	/* Must be called via the external table format manager */
	if (!CALLED_AS_FORMATTER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("cannot execute gphdfsformatter_import outside format manager")));

	tupdesc = FORMATTER_GET_TUPDESC(fcinfo);

	/* Get our internal description of the formatter */
	ncolumns = tupdesc->natts;
	myData = (format_t *) FORMATTER_GET_USER_CTX(fcinfo);

	/**
	 * Initialize the context structure
	 */
	if (myData == NULL)
	{

		myData          = palloc(sizeof(format_t));
		myData->values  = palloc(sizeof(Datum) * ncolumns);
		myData->nulls   = palloc(sizeof(bool) * ncolumns);
		myData->outlen  = palloc(sizeof(int) * ncolumns);
		myData->typioparams = (Oid *) palloc(ncolumns * sizeof(Oid));
		myData->io_functions = palloc(sizeof(FmgrInfo) * ncolumns);

		for (i = 0; i < ncolumns; i++)
		{
			Oid type = tupdesc->attrs[i]->atttypid;
			Oid functionId;

			/* External table do not support dropped columns; error out now */
			if (tupdesc->attrs[i]->attisdropped)
				ereport(ERROR,
						(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
						 errmsg("cannot handle external table with dropped columns")));

			/* Get the text/binary "receive" function */
			if (isBinaryFormatType(type))
				getTypeBinaryInputInfo(type, &(functionId), &myData->typioparams[i]);
			else
				getTypeInputInfo(type, &(functionId), &myData->typioparams[i]);
			fmgr_info(functionId, &(myData->io_functions[i]));
		}

		FORMATTER_SET_USER_CTX(fcinfo, myData);
	}

	/* get our input data buf and number of valid bytes in it */
	data_buf = FORMATTER_GET_DATABUF(fcinfo);
	data_len = FORMATTER_GET_DATALEN(fcinfo);
	data_cur = FORMATTER_GET_DATACURSOR(fcinfo);

	/* =======================================================================
	 *                            MAIN FORMATTING CODE
	 *
	 *
	 * ======================================================================= */


	/* Get the first 4 byte; That's the length of the entire packet */
	remaining = data_len - data_cur;
	bufidx    = data_cur;

	/* NOTE: Unexpected EOF Error Handling
	 * The first time we noticed an unexpected EOF, we'll set the datacursor
	 * forward and then raise the error. But then, the framework will still
	 * call the formatter the function again. Now, the formatter function will
	 * be provided with a zero length data buffer. In this case, we should not
	 * raise an error again, but simply returns "NEED MORE DATA". This is how
	 * the formatter framework works.
	 */
	if (remaining == 0 && FORMATTER_GET_SAW_EOF(fcinfo))
		FORMATTER_RETURN_NOTIFICATION(fcinfo, FMT_NEED_MORE_DATA);

	if (remaining < 4)
	{
		if (FORMATTER_GET_SAW_EOF(fcinfo))
		{
			FORMATTER_SET_BAD_ROW_DATA(fcinfo, data_buf+data_cur, remaining);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("unexpected end of file")));
		}
		FORMATTER_RETURN_NOTIFICATION(fcinfo, FMT_NEED_MORE_DATA);
	}

	tuplelen = readIntFromBuffer(data_buf, &bufidx);

	/* Now, make sure we've received the entire tuple */
	if (remaining < tuplelen)
	{
		if (FORMATTER_GET_SAW_EOF(fcinfo))
		{
			FORMATTER_SET_BAD_ROW_DATA(fcinfo, data_buf+data_cur, remaining);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("unexpected end of file")));
		}
		FORMATTER_RETURN_NOTIFICATION(fcinfo, FMT_NEED_MORE_DATA);
	}

	/* We got here. So, we've the ENTIRE tuple in the buffer */
	FORMATTER_SET_BAD_ROW_DATA(fcinfo, data_buf+data_cur, tuplelen);

	/* start clean */
	MemSet(myData->values, 0,    ncolumns * sizeof(Datum));
	MemSet(myData->nulls,  true, ncolumns * sizeof(bool));

	per_row_ctx = FORMATTER_GET_PER_ROW_MEM_CTX(fcinfo);
	oldcontext  = MemoryContextSwitchTo(per_row_ctx);

	/* extract the version and column count */
	version = readInt2FromBuffer(data_buf, &bufidx);
	colcnt  = readInt2FromBuffer(data_buf, &bufidx);

	if (version != GPDBWRITABLE_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("cannot import data version %d", version)));

	if (colcnt != ncolumns)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("input data column count (%d) did not match the external table definition",
						colcnt)));

	/* Extract Column Type and check against External Table definition */
	for(i=0; i< ncolumns; i++)
	{
		Oid   input_type = 0;
		Oid   defined_type = tupdesc->attrs[i]->atttypid;
		int8  enumType   = readInt1FromBuffer(data_buf, &bufidx);

		/* Convert enumType to type oid */
		input_type = getTypeOidFromJavaEnumOrdinal(enumType);
		if ((isBinaryFormatType(defined_type) || isBinaryFormatType(input_type)) &&
			input_type != defined_type)
		{
			char	   *intype;

			intype = get_type_name(input_type);
			Insist(intype);

			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("input data column %d of type \"%s\" did not match the external table definition",
							i+1, intype)));
		}
	}
	/* Extract null bit array */
	{
		int nullByteLen = getNullByteArraySize(ncolumns);
		bits8* nullByteArray = (bits8*)(data_buf+bufidx);
		bufidx += nullByteLen;
		byteArrayToBoolArray(nullByteArray, nullByteLen, &myData->nulls, ncolumns);
	}

	/* extract column value */
	for(i=0; i< ncolumns; i++)
	{
		if (!myData->nulls[i])
		{
			FmgrInfo *iofunc = &(myData->io_functions[i]);

			/* Skip the aligment padding
			 * For fixed length type, use the type aligment.
			 * For var length type, always align int4 because we'reading a length header.
			 */
			if (isVariableLength(tupdesc->attrs[i]->atttypid))
				bufidx = INTALIGN(bufidx);
			else
				bufidx = att_align_nominal(bufidx, tupdesc->attrs[i]->attalign);

			/* For fixed length type, we can use the type length attribute.
			 * For variable length type, we'll get the payload length from the first 4 byte.
			 */
			if (isVariableLength(tupdesc->attrs[i]->atttypid))
				myData->outlen[i] = readIntFromBuffer(data_buf, &bufidx);
			else
				myData->outlen[i] = tupdesc->attrs[i]->attlen;

			if (isBinaryFormatType(tupdesc->attrs[i]->atttypid))
			{
				StringInfoData tmpbuf;
				tmpbuf.data   = data_buf+bufidx;
				tmpbuf.maxlen = myData->outlen[i];
				tmpbuf.len    = myData->outlen[i];
				tmpbuf.cursor = 0;

				myData->values[i] = ReceiveFunctionCall(
						iofunc,
						&tmpbuf,
						myData->typioparams[i],
						tupdesc->attrs[i]->atttypmod);
			}
			else
			{
				myData->values[i] = InputFunctionCall(
						iofunc,
						data_buf+bufidx,
						myData->typioparams[i],
						tupdesc->attrs[i]->atttypmod);
			}
			bufidx += myData->outlen[i];
		}
	}
	bufidx = DOUBLEALIGN(bufidx);

	if(data_cur + tuplelen != bufidx)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Tuplelen != bufidx: %d:%d:%d", tuplelen, bufidx, data_cur)));

	data_cur += tuplelen;

	MemoryContextSwitchTo(oldcontext);

	FORMATTER_SET_DATACURSOR(fcinfo, data_cur);

	tuple = heap_form_tuple(tupdesc, myData->values, myData->nulls);

	FORMATTER_SET_TUPLE(fcinfo, tuple);
	FORMATTER_RETURN_TUPLE(tuple);
}
