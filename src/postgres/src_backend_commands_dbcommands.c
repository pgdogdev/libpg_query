/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - get_database_name
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		Database management commands (create/drop database).
 *
 * Note: database creation/destruction commands use exclusive locks on
 * the database objects (as expressed by LockSharedObject()) to avoid
 * stepping on each others' toes.  Formerly we used table-level locks
 * on pg_database, but that's too coarse-grained.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dbcommands.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/dbcommands_xlog.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "replication/slot.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/md.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_locale.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Create database strategy.
 *
 * CREATEDB_WAL_LOG will copy the database at the block level and WAL log each
 * copied block.
 *
 * CREATEDB_FILE_COPY will simply perform a file system level copy of the
 * database and log a single record for each tablespace copied. To make this
 * safe, it also triggers checkpoints before and after the operation.
 */
typedef enum CreateDBStrategy
{
	CREATEDB_WAL_LOG,
	CREATEDB_FILE_COPY,
} CreateDBStrategy;

typedef struct
{
	Oid			src_dboid;		/* source (template) DB */
	Oid			dest_dboid;		/* DB we are trying to create */
	CreateDBStrategy strategy;	/* create db strategy */
} createdb_failure_params;

typedef struct
{
	Oid			dest_dboid;		/* DB we are trying to move */
	Oid			dest_tsoid;		/* tablespace we are trying to move to */
} movedb_failure_params;

/*
 * Information about a relation to be copied when creating a database.
 */
typedef struct CreateDBRelInfo
{
	RelFileLocator rlocator;	/* physical relation identifier */
	Oid			reloid;			/* relation oid */
	bool		permanent;		/* relation is permanent or unlogged */
} CreateDBRelInfo;


/* non-export function prototypes */
static void createdb_failure_callback(int code, Datum arg);
static void movedb(const char *dbname, const char *tblspcname);
static void movedb_failure_callback(int code, Datum arg);
static bool get_db_info(const char *name, LOCKMODE lockmode,
						Oid *dbIdP, Oid *ownerIdP,
						int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP, bool *dbHasLoginEvtP,
						TransactionId *dbFrozenXidP, MultiXactId *dbMinMultiP,
						Oid *dbTablespace, char **dbCollate, char **dbCtype, char **dbLocale,
						char **dbIcurules,
						char *dbLocProvider,
						char **dbCollversion);
static void remove_dbtablespaces(Oid db_id);
static bool check_db_file_conflict(Oid db_id);
static int	errdetail_busy_db(int notherbackends, int npreparedxacts);
static void CreateDatabaseUsingWalLog(Oid src_dboid, Oid dst_dboid, Oid src_tsid,
									  Oid dst_tsid);
static List *ScanSourceDatabasePgClass(Oid tbid, Oid dbid, char *srcpath);
static List *ScanSourceDatabasePgClassPage(Page page, Buffer buf, Oid tbid,
										   Oid dbid, char *srcpath,
										   List *rlocatorlist, Snapshot snapshot);
static CreateDBRelInfo *ScanSourceDatabasePgClassTuple(HeapTupleData *tuple,
													   Oid tbid, Oid dbid,
													   char *srcpath);
static void CreateDirAndVersionFile(char *dbpath, Oid dbid, Oid tsid,
									bool isRedo);
static void CreateDatabaseUsingFileCopy(Oid src_dboid, Oid dst_dboid,
										Oid src_tsid, Oid dst_tsid);
static void recovery_create_dbdir(char *path, bool only_tblspc);

/*
 * Create a new database using the WAL_LOG strategy.
 *
 * Each copied block is separately written to the write-ahead log.
 */


/*
 * Scan the pg_class table in the source database to identify the relations
 * that need to be copied to the destination database.
 *
 * This is an exception to the usual rule that cross-database access is
 * not possible. We can make it work here because we know that there are no
 * connections to the source database and (since there can't be prepared
 * transactions touching that database) no in-doubt tuples either. This
 * means that we don't need to worry about pruning removing anything from
 * under us, and we don't need to be too picky about our snapshot either.
 * As long as it sees all previously-committed XIDs as committed and all
 * aborted XIDs as aborted, we should be fine: nothing else is possible
 * here.
 *
 * We can't rely on the relcache for anything here, because that only knows
 * about the database to which we are connected, and can't handle access to
 * other databases. That also means we can't rely on the heap scan
 * infrastructure, which would be a bad idea anyway since it might try
 * to do things like HOT pruning which we definitely can't do safely in
 * a database to which we're not even connected.
 */


/*
 * Scan one page of the source database's pg_class relation and add relevant
 * entries to rlocatorlist. The return value is the updated list.
 */


/*
 * Decide whether a certain pg_class tuple represents something that
 * needs to be copied from the source database to the destination database,
 * and if so, construct a CreateDBRelInfo for it.
 *
 * Visibility checks are handled by the caller, so our job here is just
 * to assess the data stored in the tuple.
 */


/*
 * Create database directory and write out the PG_VERSION file in the database
 * path.  If isRedo is true, it's okay for the database directory to exist
 * already.
 */


/*
 * Create a new database using the FILE_COPY strategy.
 *
 * Copy each tablespace at the filesystem level, and log a single WAL record
 * for each tablespace copied.  This requires a checkpoint before and after the
 * copy, which may be expensive, but it does greatly reduce WAL generation
 * if the copied database is large.
 */


/*
 * CREATE DATABASE
 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
#endif

/*
 * Check whether chosen encoding matches chosen locale settings.  This
 * restriction is necessary because libc's locale-specific code usually
 * fails when presented with data in an encoding it's not expecting. We
 * allow mismatch in four cases:
 *
 * 1. locale encoding = SQL_ASCII, which means that the locale is C/POSIX
 * which works with any encoding.
 *
 * 2. locale encoding = -1, which means that we couldn't determine the
 * locale's encoding and have to trust the user to get it right.
 *
 * 3. selected encoding is UTF8 and platform is win32. This is because
 * UTF8 is a pseudo codepage that is supported in all locales since it's
 * converted to UTF16 before being used.
 *
 * 4. selected encoding is SQL_ASCII, but only if you're a superuser. This
 * is risky but we have historically allowed it --- notably, the
 * regression tests require it.
 *
 * Note: if you change this policy, fix initdb to match.
 */
#ifdef WIN32
#endif
#ifdef WIN32
#endif

/* Error cleanup callback for createdb */



/*
 * DROP DATABASE
 */



/*
 * Rename database
 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
#endif


/*
 * ALTER DATABASE SET TABLESPACE
 */


/* Error cleanup callback for movedb */


/*
 * Process options and call dropdb function.
 */


/*
 * ALTER DATABASE name ...
 */



/*
 * ALTER DATABASE name REFRESH COLLATION VERSION
 */



/*
 * ALTER DATABASE name SET ...
 */



/*
 * ALTER DATABASE name OWNER TO newowner
 */






/*
 * Helper functions
 */

/*
 * Look up info about the database named "name".  If the database exists,
 * obtain the specified lock type on it, fill in any of the remaining
 * parameters that aren't NULL, and return true.  If no such database,
 * return false.
 */


/* Check if current user has createdb privileges */


/*
 * Remove tablespace directories
 *
 * We don't know what tablespaces db_id is using, so iterate through all
 * tablespaces removing <tablespace>/db_id
 */


/*
 * Check for existing files that conflict with a proposed new DB OID;
 * return true if there are any
 *
 * If there were a subdirectory in any tablespace matching the proposed new
 * OID, we'd get a create failure due to the duplicate name ... and then we'd
 * try to remove that already-existing subdirectory during the cleanup in
 * remove_dbtablespaces.  Nuking existing files seems like a bad idea, so
 * instead we make this extra check before settling on the OID of the new
 * database.  This exactly parallels what GetNewRelFileNumber() does for table
 * relfilenumber values.
 */


/*
 * Issue a suitable errdetail message for a busy database
 */


/*
 * get_database_oid - given a database name, look up the OID
 *
 * If missing_ok is false, throw an error if database name not found.  If
 * true, just return InvalidOid.
 */



/*
 * get_database_name - given a database OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such database.
 */
char *
get_database_name(Oid dbid)
{
	HeapTuple	dbtuple;
	char	   *result;

	dbtuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (HeapTupleIsValid(dbtuple))
	{
		result = pstrdup(NameStr(((Form_pg_database) GETSTRUCT(dbtuple))->datname));
		ReleaseSysCache(dbtuple);
	}
	else
		result = NULL;

	return result;
}


/*
 * While dropping a database the pg_database row is marked invalid, but the
 * catalog contents still exist. Connections to such a database are not
 * allowed.
 */



/*
 * Convenience wrapper around database_is_invalid_form()
 */



/*
 * recovery_create_dbdir()
 *
 * During recovery, there's a case where we validly need to recover a missing
 * tablespace directory so that recovery can continue.  This happens when
 * recovery wants to create a database but the holding tablespace has been
 * removed before the server stopped.  Since we expect that the directory will
 * be gone before reaching recovery consistency, and we have no knowledge about
 * the tablespace other than its OID here, we create a real directory under
 * pg_tblspc here instead of restoring the symlink.
 *
 * If only_tblspc is true, then the requested directory must be in pg_tblspc/
 */



/*
 * DATABASE resource manager's routines
 */

