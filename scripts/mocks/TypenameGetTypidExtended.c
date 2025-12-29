Oid
TypenameGetTypidExtended(const char *typname, bool temp_ok)
{
    if (strcmp(typname, "timestamptz") == 0)
        return TIMESTAMPTZOID;
    else if (strcmp(typname, "uuid") == 0)
        return UUIDOID;
    else if (strcmp(typname, "bool") == 0)
        return BOOLOID;
    else if (strcmp(typname, "text") == 0)
        return TEXTOID;
    else if (strcmp(typname, "date") == 0)
        return DATEOID;
    else
        return UNKNOWNOID;
}
