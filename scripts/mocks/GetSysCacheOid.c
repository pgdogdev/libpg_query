Oid
GetSysCacheOid(int cacheId,
			   AttrNumber oidcol,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	if (cacheId != TYPENAMENSP)
        elog(ERROR, "Not implemented (GetSysCacheOid only supports TYPENAMENSP cache (%d), got cache %d)", TYPENAMENSP, cacheId);

    if (oidcol != Anum_pg_type_oid)
        elog(ERROR, "Not implemented (GetSysCacheOid oidcol not as expected)");

    if (key3 != 0 || key4 != 0)
        elog(ERROR, "Not implemented (GetSysCacheOid key 3 and key 4 must be zero)");

    if (IsCatalogNamespace(DatumGetObjectId(key2)))
    {
        char *t = DatumGetPointer(key1);
        if (strcmp(t, "bool") == 0)
            return BOOLOID;
        else if (strcmp(t, "varchar") == 0)
            return VARCHAROID;
        else if (strcmp(t, "int2") == 0)
            return INT2OID;
        else if (strcmp(t, "int4") == 0)
            return INT4OID;
        else if (strcmp(t, "int8") == 0)
            return INT8OID;
        else if (strcmp(t, "float4") == 0)
            return FLOAT4OID;
        else if (strcmp(t, "float8") == 0)
            return FLOAT8OID;
        else if (strcmp(t, "interval") == 0)
            return INTERVALOID;
        else if (strcmp(t, "record") == 0)
            return RECORDOID;
        else
            elog(ERROR, "Not implemented (unexpected built-in type %s)", t);
    }
    else if (DatumGetObjectId(key2) == PG_PUBLIC_NAMESPACE)
    {
        return RECORDOID;
    }
    else
    {
        elog(ERROR, "Not implemented (GetSysCacheOid only supported for built-in catalog types or custom pseudotypes in public namespace)");
    }
}
