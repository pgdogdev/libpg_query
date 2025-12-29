#include "catalog/pg_collation_d.h"
static PLpgSQL_type * parse_datatype(const char *string, int location, yyscan_t yyscanner) {
	PLpgSQL_type *typ;

	/* Ignore trailing spaces */
	size_t len = strlen(string);
	while (len > 0 && scanner_isspace(string[len - 1])) --len;

	typ = (PLpgSQL_type *) palloc0(sizeof(PLpgSQL_type));
	typ->typname = pstrdup(string);
	typ->ttype = pg_strncasecmp(string, "RECORD", len) == 0 ? PLPGSQL_TTYPE_REC : PLPGSQL_TTYPE_SCALAR;
	if (pg_strncasecmp(string, "REFCURSOR", len) == 0 || pg_strncasecmp(string, "CURSOR", len) == 0)
	{
		typ->typoid = REFCURSOROID;
	}
	else if (pg_strncasecmp(string, "TEXT", len) == 0)
	{
		typ->typoid = TEXTOID;
		typ->collation = DEFAULT_COLLATION_OID;
	}
	return typ;
}
