/* Limited catalog information for certain built-in types */
#include <catalog/pg_type.h>
HeapTuple
SearchSysCache1(int cacheId,
				Datum key1)
{
	/*Assert(cacheId >= 0 && cacheId < SysCacheSize &&
		   PointerIsValid(SysCache[cacheId]));
	Assert(SysCache[cacheId]->cc_nkeys == 1);

	return SearchCatCache1(SysCache[cacheId], key1);*/

	HeapTuple tuple;
	HeapTupleHeader td;
	Form_pg_type t = palloc0(sizeof(FormData_pg_type));
	Size		len,
				data_len;
	int			hoff;

    if (cacheId != TYPEOID)
        elog(ERROR, "Not implemented (SearchSysCache1 only supports TYPEOID cache (%d), got cache %d)", TYPEOID, cacheId);

    switch (DatumGetObjectId(key1))
    {
        case BOOLOID:
        {
            strlcpy(NameStr(t->typname), "bool", NAMEDATALEN);
            t->typlen = 1;
            t->typbyval = true;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_BOOLEAN;
            t->typalign = TYPALIGN_CHAR;
            t->typarray = BOOLARRAYOID;
            break;
        }
        case NAMEOID:
        {
            strlcpy(NameStr(t->typname), "name", NAMEDATALEN);
            t->typlen = NAMEDATALEN;
            t->typbyval = false;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_STRING;
            t->typalign = TYPALIGN_CHAR;
            t->typarray = NAMEARRAYOID;
            break;
        }
        case INT2OID:
        {
            strlcpy(NameStr(t->typname), "int2", NAMEDATALEN);
            t->typlen = 2;
            t->typbyval = true;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_NUMERIC;
            t->typalign = TYPALIGN_SHORT;
            t->typarray = INT2ARRAYOID;
            break;
        }
        case INT4OID:
        {
            strlcpy(NameStr(t->typname), "int4", NAMEDATALEN);
            t->typlen = 4;
            t->typbyval = true;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_NUMERIC;
            t->typalign = TYPALIGN_INT;
            t->typarray = INT4ARRAYOID;
            break;
        }
        case INT8OID:
        {
            strlcpy(NameStr(t->typname), "int8", NAMEDATALEN);
            t->typlen = 8;
            t->typbyval = FLOAT8PASSBYVAL;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_NUMERIC;
            t->typalign = TYPALIGN_DOUBLE;
            t->typarray = INT8ARRAYOID;
            break;
        }
        case TEXTOID:
        {
            strlcpy(NameStr(t->typname), "text", NAMEDATALEN);
            t->typlen = -1;
            t->typbyval = false;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_STRING;
            t->typalign = TYPALIGN_INT;
            t->typarray = TEXTARRAYOID;
            break;
        }
        case TEXTARRAYOID:
        {
            strlcpy(NameStr(t->typname), "_text", NAMEDATALEN);
            t->typlen = -1;
            t->typbyval = false;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_ARRAY;
            t->typalign = TYPALIGN_INT;
            t->typarray = 0;
            break;
        }
        case OIDOID:
        {
            strlcpy(NameStr(t->typname), "oid", NAMEDATALEN);
            t->typlen = 4;
            t->typbyval = true;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_NUMERIC;
            t->typalign = TYPALIGN_INT;
            t->typarray = OIDARRAYOID;
            break;
        }
        case VOIDOID:
        {
            strlcpy(NameStr(t->typname), "void", NAMEDATALEN);
            t->typlen = 4;
            t->typbyval = true;
            t->typtype = TYPTYPE_PSEUDO;
            t->typcategory = TYPCATEGORY_PSEUDOTYPE;
            t->typalign = TYPALIGN_INT;
            t->typarray = 0;
            break;
        }
        case VARCHAROID:
        {
            strlcpy(NameStr(t->typname), "varchar", NAMEDATALEN);
            t->typlen = -1;
            t->typbyval = false;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_STRING;
            t->typalign = TYPALIGN_INT;
            t->typarray = VARCHARARRAYOID;
            break;
        }
        case FLOAT4OID:
        {
            strlcpy(NameStr(t->typname), "float4", NAMEDATALEN);
            t->typlen = 4;
            t->typbyval = true;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_NUMERIC;
            t->typalign = TYPALIGN_INT;
            t->typarray = FLOAT4ARRAYOID;
            break;
        }
        case FLOAT8OID:
        {
            strlcpy(NameStr(t->typname), "float8", NAMEDATALEN);
            t->typlen = 8;
            t->typbyval = FLOAT8PASSBYVAL;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_NUMERIC;
            t->typalign = TYPALIGN_DOUBLE;
            t->typarray = FLOAT8ARRAYOID;
            break;
        }
        case DATEOID:
        {
            strlcpy(NameStr(t->typname), "date", NAMEDATALEN);
            t->typlen = 4;
            t->typbyval = true;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_DATETIME;
            t->typalign = TYPALIGN_INT;
            t->typarray = DATEARRAYOID;
            break;
        }
        case UNKNOWNOID:
        {
            strlcpy(NameStr(t->typname), "unknown", NAMEDATALEN);
            t->typlen = -2;
            t->typbyval = false;
            t->typtype = TYPTYPE_PSEUDO;
            t->typcategory = TYPCATEGORY_INTERNAL;
            t->typalign = TYPALIGN_CHAR;
            t->typarray = 0;
            break;
        }
        case INTERVALOID:
        {
            strlcpy(NameStr(t->typname), "interval", NAMEDATALEN);
            t->typlen = 16;
            t->typbyval = false;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_TIMESPAN;
            t->typalign = TYPALIGN_DOUBLE;
            t->typarray = INTERVALARRAYOID;
            break;
        }
        case TIMESTAMPTZOID:
        {
            strlcpy(NameStr(t->typname), "timestamptz", NAMEDATALEN);
            t->typlen = 8;
            t->typbyval = FLOAT8PASSBYVAL;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_DATETIME;
            t->typalign = TYPALIGN_DOUBLE;
            t->typarray = TIMESTAMPTZARRAYOID;
            break;
        }
        case RECORDOID:
        {
            strlcpy(NameStr(t->typname), "record", NAMEDATALEN);
            t->typlen = -1;
            t->typbyval = false;
            t->typtype = TYPTYPE_PSEUDO;
            t->typcategory = TYPCATEGORY_PSEUDOTYPE;
            t->typalign = TYPALIGN_DOUBLE;
            t->typarray = RECORDARRAYOID;
            break;
        }
        case UUIDOID:
        {
            strlcpy(NameStr(t->typname), "uuid", NAMEDATALEN);
            t->typlen = 16;
            t->typbyval = false;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_USER;
            t->typalign = TYPALIGN_CHAR;
            t->typarray = UUIDARRAYOID;
            break;
        }
        case UUIDARRAYOID:
        {
            strlcpy(NameStr(t->typname), "_uuid", NAMEDATALEN);
            t->typlen = -1;
            t->typbyval = false;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_ARRAY;
            t->typalign = TYPALIGN_INT;
            t->typarray = 0;
            break;
        }
        case REFCURSOROID:
        {
            strlcpy(NameStr(t->typname), "refcursor", NAMEDATALEN);
            t->typlen = -1;
            t->typbyval = false;
            t->typtype = TYPTYPE_BASE;
            t->typcategory = TYPCATEGORY_USER;
            t->typalign = TYPALIGN_INT;
            t->typarray = REFCURSORARRAYOID;
            break;
        }
        default:
            elog(ERROR, "Not implemented (SearchSysCache1 got TYPEOID cache request for type OID %d)", DatumGetObjectId(key1));
    }

    t->oid = DatumGetObjectId(key1);
    t->typisdefined = true;

	// The following logic is copied from heap_form_tuple, but pretends there are no nulls, and copies t_data directly

	/*
	 * Determine total space needed
	 */
	len = offsetof(HeapTupleHeaderData, t_bits);

	//if (hasnull)
	//	len += BITMAPLEN(numberOfAttributes);

	hoff = len = MAXALIGN(len); /* align user data safely */

	//data_len = heap_compute_data_size(tupleDescriptor, values, isnull);
	data_len = MAXALIGN(sizeof(FormData_pg_type));

	len += data_len;

	/*
	 * Allocate and zero the space needed.  Note that the tuple body and
	 * HeapTupleData management structure are allocated in one chunk.
	 */
	tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
	tuple->t_data = td = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	/*
	 * And fill in the information.  Note we fill the Datum fields even though
	 * this tuple may never become a Datum.  This lets HeapTupleHeaderGetDatum
	 * identify the tuple type if needed.
	 */
	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;

	HeapTupleHeaderSetDatumLength(td, len);
	//HeapTupleHeaderSetTypeId(td, tupleDescriptor->tdtypeid);
	//HeapTupleHeaderSetTypMod(td, tupleDescriptor->tdtypmod);
	/* We also make sure that t_ctid is invalid unless explicitly set */
	ItemPointerSetInvalid(&(td->t_ctid));

	HeapTupleHeaderSetNatts(td, Natts_pg_type);
	td->t_hoff = hoff;

	/*heap_fill_tuple(tupleDescriptor,
					values,
					isnull,
					(char *) td + hoff,
					data_len,
					&td->t_infomask,
					(hasnull ? td->t_bits : NULL));*/
	memcpy((char *) td + hoff, t, sizeof(FormData_pg_type));

	return tuple;
}
