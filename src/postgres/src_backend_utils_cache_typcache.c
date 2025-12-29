/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - lookup_type_cache
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * typcache.c
 *	  POSTGRES type cache code
 *
 * The type cache exists to speed lookup of certain information about data
 * types that is not directly available from a type's pg_type row.  For
 * example, we use a type's default btree opclass, or the default hash
 * opclass if no btree opclass exists, to determine which operators should
 * be used for grouping and sorting the type (GROUP BY, ORDER BY ASC/DESC).
 *
 * Several seemingly-odd choices have been made to support use of the type
 * cache by generic array and record handling routines, such as array_eq(),
 * record_cmp(), and hash_array().  Because those routines are used as index
 * support operations, they cannot leak memory.  To allow them to execute
 * efficiently, all information that they would like to re-use across calls
 * is kept in the type cache.
 *
 * Once created, a type cache entry lives as long as the backend does, so
 * there is no need for a call to release a cache entry.  If the type is
 * dropped, the cache entry simply becomes wasted storage.  This is not
 * expected to happen often, and assuming that typcache entries are good
 * permanently allows caching pointers to them in long-lived places.
 *
 * We have some provisions for updating cache entries if the stored data
 * becomes obsolete.  Core data extracted from the pg_type row is updated
 * when we detect updates to pg_type.  Information dependent on opclasses is
 * cleared if we detect updates to pg_opclass.  We also support clearing the
 * tuple descriptor and operator/function parts of a rowtype's cache entry,
 * since those may need to change as a consequence of ALTER TABLE.  Domain
 * constraint changes are also tracked properly.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/typcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/session.h"
#include "access/table.h"
#include "catalog/pg_am.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "common/int.h"
#include "executor/executor.h"
#include "lib/dshash.h"
#include "optimizer/optimizer.h"
#include "port/pg_bitutils.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* The main type cache hashtable searched by lookup_type_cache */


/*
 * The mapping of relation's OID to the corresponding composite type OID.
 * We're keeping the map entry when the corresponding typentry has something
 * to clear i.e it has either TCFLAGS_HAVE_PG_TYPE_DATA, or
 * TCFLAGS_OPERATOR_FLAGS, or tupdesc.
 */


typedef struct RelIdToTypeIdCacheEntry
{
	Oid			relid;			/* OID of the relation */
	Oid			composite_typid;	/* OID of the relation's composite type */
} RelIdToTypeIdCacheEntry;

/* List of type cache entries for domain types */


/* Private flag bits in the TypeCacheEntry.flags field */
#define TCFLAGS_HAVE_PG_TYPE_DATA			0x000001
#define TCFLAGS_CHECKED_BTREE_OPCLASS		0x000002
#define TCFLAGS_CHECKED_HASH_OPCLASS		0x000004
#define TCFLAGS_CHECKED_EQ_OPR				0x000008
#define TCFLAGS_CHECKED_LT_OPR				0x000010
#define TCFLAGS_CHECKED_GT_OPR				0x000020
#define TCFLAGS_CHECKED_CMP_PROC			0x000040
#define TCFLAGS_CHECKED_HASH_PROC			0x000080
#define TCFLAGS_CHECKED_HASH_EXTENDED_PROC	0x000100
#define TCFLAGS_CHECKED_ELEM_PROPERTIES		0x000200
#define TCFLAGS_HAVE_ELEM_EQUALITY			0x000400
#define TCFLAGS_HAVE_ELEM_COMPARE			0x000800
#define TCFLAGS_HAVE_ELEM_HASHING			0x001000
#define TCFLAGS_HAVE_ELEM_EXTENDED_HASHING	0x002000
#define TCFLAGS_CHECKED_FIELD_PROPERTIES	0x004000
#define TCFLAGS_HAVE_FIELD_EQUALITY			0x008000
#define TCFLAGS_HAVE_FIELD_COMPARE			0x010000
#define TCFLAGS_HAVE_FIELD_HASHING			0x020000
#define TCFLAGS_HAVE_FIELD_EXTENDED_HASHING	0x040000
#define TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS	0x080000
#define TCFLAGS_DOMAIN_BASE_IS_COMPOSITE	0x100000

/* The flags associated with equality/comparison/hashing are all but these: */
#define TCFLAGS_OPERATOR_FLAGS \
	(~(TCFLAGS_HAVE_PG_TYPE_DATA | \
	   TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS | \
	   TCFLAGS_DOMAIN_BASE_IS_COMPOSITE))

/*
 * Data stored about a domain type's constraints.  Note that we do not create
 * this struct for the common case of a constraint-less domain; we just set
 * domainData to NULL to indicate that.
 *
 * Within a DomainConstraintCache, we store expression plan trees, but the
 * check_exprstate fields of the DomainConstraintState nodes are just NULL.
 * When needed, expression evaluation nodes are built by flat-copying the
 * DomainConstraintState nodes and applying ExecInitExpr to check_expr.
 * Such a node tree is not part of the DomainConstraintCache, but is
 * considered to belong to a DomainConstraintRef.
 */
struct DomainConstraintCache
{
	List	   *constraints;	/* list of DomainConstraintState nodes */
	MemoryContext dccContext;	/* memory context holding all associated data */
	long		dccRefCount;	/* number of references to this struct */
};

/* Private information to support comparisons of enum values */
typedef struct
{
	Oid			enum_oid;		/* OID of one enum value */
	float4		sort_order;		/* its sort position */
} EnumItem;

typedef struct TypeCacheEnumData
{
	Oid			bitmap_base;	/* OID corresponding to bit 0 of bitmapset */
	Bitmapset  *sorted_values;	/* Set of OIDs known to be in order */
	int			num_values;		/* total number of values in enum */
	EnumItem	enum_values[FLEXIBLE_ARRAY_MEMBER];
} TypeCacheEnumData;

/*
 * We use a separate table for storing the definitions of non-anonymous
 * record types.  Once defined, a record type will be remembered for the
 * life of the backend.  Subsequent uses of the "same" record type (where
 * sameness means equalRowTypes) will refer to the existing table entry.
 *
 * Stored record types are remembered in a linear array of TupleDescs,
 * which can be indexed quickly with the assigned typmod.  There is also
 * a hash table to speed searches for matching TupleDescs.
 */

typedef struct RecordCacheEntry
{
	TupleDesc	tupdesc;
} RecordCacheEntry;

/*
 * To deal with non-anonymous record types that are exchanged by backends
 * involved in a parallel query, we also need a shared version of the above.
 */
struct SharedRecordTypmodRegistry
{
	/* A hash table for finding a matching TupleDesc. */
	dshash_table_handle record_table_handle;
	/* A hash table for finding a TupleDesc by typmod. */
	dshash_table_handle typmod_table_handle;
	/* A source of new record typmod numbers. */
	pg_atomic_uint32 next_typmod;
};

/*
 * When using shared tuple descriptors as hash table keys we need a way to be
 * able to search for an equal shared TupleDesc using a backend-local
 * TupleDesc.  So we use this type which can hold either, and hash and compare
 * functions that know how to handle both.
 */
typedef struct SharedRecordTableKey
{
	union
	{
		TupleDesc	local_tupdesc;
		dsa_pointer shared_tupdesc;
	}			u;
	bool		shared;
} SharedRecordTableKey;

/*
 * The shared version of RecordCacheEntry.  This lets us look up a typmod
 * using a TupleDesc which may be in local or shared memory.
 */
typedef struct SharedRecordTableEntry
{
	SharedRecordTableKey key;
} SharedRecordTableEntry;

/*
 * An entry in SharedRecordTypmodRegistry's typmod table.  This lets us look
 * up a TupleDesc in shared memory using a typmod.
 */
typedef struct SharedTypmodTableEntry
{
	uint32		typmod;
	dsa_pointer shared_tupdesc;
} SharedTypmodTableEntry;





/*
 * A comparator function for SharedRecordTableKey.
 */


/*
 * A hash function for SharedRecordTableKey.
 */


/* Parameters for SharedRecordTypmodRegistry's TupleDesc table. */


/* Parameters for SharedRecordTypmodRegistry's typmod hash table. */


/* hashtable for recognizing registered record types */


typedef struct RecordCacheArrayEntry
{
	uint64		id;
	TupleDesc	tupdesc;
} RecordCacheArrayEntry;

/* array of info about registered record types, indexed by assigned typmod */

	/* allocated length of above array */
	/* number of entries used */

/*
 * Process-wide counter for generating unique tupledesc identifiers.
 * Zero and one (INVALID_TUPLEDESC_IDENTIFIER) aren't allowed to be chosen
 * as identifiers, so we start the counter at INVALID_TUPLEDESC_IDENTIFIER.
 */


static void load_typcache_tupdesc(TypeCacheEntry *typentry);
static void load_rangetype_info(TypeCacheEntry *typentry);
static void load_multirangetype_info(TypeCacheEntry *typentry);
static void load_domaintype_info(TypeCacheEntry *typentry);
static int	dcs_cmp(const void *a, const void *b);
static void decr_dcc_refcount(DomainConstraintCache *dcc);
static void dccref_deletion_callback(void *arg);
static List *prep_domain_constraints(List *constraints, MemoryContext execctx);
static bool array_element_has_equality(TypeCacheEntry *typentry);
static bool array_element_has_compare(TypeCacheEntry *typentry);
static bool array_element_has_hashing(TypeCacheEntry *typentry);
static bool array_element_has_extended_hashing(TypeCacheEntry *typentry);
static void cache_array_element_properties(TypeCacheEntry *typentry);
static bool record_fields_have_equality(TypeCacheEntry *typentry);
static bool record_fields_have_compare(TypeCacheEntry *typentry);
static bool record_fields_have_hashing(TypeCacheEntry *typentry);
static bool record_fields_have_extended_hashing(TypeCacheEntry *typentry);
static void cache_record_field_properties(TypeCacheEntry *typentry);
static bool range_element_has_hashing(TypeCacheEntry *typentry);
static bool range_element_has_extended_hashing(TypeCacheEntry *typentry);
static void cache_range_element_properties(TypeCacheEntry *typentry);
static bool multirange_element_has_hashing(TypeCacheEntry *typentry);
static bool multirange_element_has_extended_hashing(TypeCacheEntry *typentry);
static void cache_multirange_element_properties(TypeCacheEntry *typentry);
static void TypeCacheRelCallback(Datum arg, Oid relid);
static void TypeCacheTypCallback(Datum arg, int cacheid, uint32 hashvalue);
static void TypeCacheOpcCallback(Datum arg, int cacheid, uint32 hashvalue);
static void TypeCacheConstrCallback(Datum arg, int cacheid, uint32 hashvalue);
static void load_enum_cache_data(TypeCacheEntry *tcache);
static EnumItem *find_enumitem(TypeCacheEnumData *enumdata, Oid arg);
static int	enum_oid_cmp(const void *left, const void *right);
static void shared_record_typmod_registry_detach(dsm_segment *segment,
												 Datum datum);
static TupleDesc find_or_make_matching_shared_tupledesc(TupleDesc tupdesc);
static dsa_pointer share_tupledesc(dsa_area *area, TupleDesc tupdesc,
								   uint32 typmod);
static void insert_rel_type_cache_if_needed(TypeCacheEntry *typentry);
static void delete_rel_type_cache_if_needed(TypeCacheEntry *typentry);


/*
 * Hash function compatible with one-arg system cache hash function.
 */


/*
 * lookup_type_cache
 *
 * Fetch the type cache entry for the specified datatype, and make sure that
 * all the fields requested by bits in 'flags' are valid.
 *
 * The result is never NULL --- we will ereport() if the passed type OID is
 * invalid.  Note however that we may fail to find one or more of the
 * values requested by 'flags'; the caller needs to check whether the fields
 * are InvalidOid or not.
 *
 * Note that while filling TypeCacheEntry we might process concurrent
 * invalidation messages, causing our not-yet-filled TypeCacheEntry to be
 * invalidated.  In this case, we typically only clear flags while values are
 * still available for the caller.  It's expected that the caller holds
 * enough locks on type-depending objects that the values are still relevant.
 * It's also important that the tupdesc is filled after all other
 * TypeCacheEntry items for TYPTYPE_COMPOSITE.  So, tupdesc can't get
 * invalidated during the lookup_type_cache() call.
 */

TypeCacheEntry *
lookup_type_cache(Oid type_id, int flags)
{
	Assert(false); elog(ERROR, "Not implemented");
}

/*
 * load_typcache_tupdesc --- helper routine to set up composite type's tupDesc
 */


/*
 * load_rangetype_info --- helper routine to set up range type information
 */


/*
 * load_multirangetype_info --- helper routine to set up multirange type
 * information
 */


/*
 * load_domaintype_info --- helper routine to set up domain constraint info
 *
 * Note: we assume we're called in a relatively short-lived context, so it's
 * okay to leak data into the current context while scanning pg_constraint.
 * We build the new DomainConstraintCache data in a context underneath
 * CurrentMemoryContext, and reparent it under CacheMemoryContext when
 * complete.
 */


/*
 * qsort comparator to sort DomainConstraintState pointers by name
 */


/*
 * decr_dcc_refcount --- decrement a DomainConstraintCache's refcount,
 * and free it if no references remain
 */


/*
 * Context reset/delete callback for a DomainConstraintRef
 */


/*
 * prep_domain_constraints --- prepare domain constraints for execution
 *
 * The expression trees stored in the DomainConstraintCache's list are
 * converted to executable expression state trees stored in execctx.
 */


/*
 * InitDomainConstraintRef --- initialize a DomainConstraintRef struct
 *
 * Caller must tell us the MemoryContext in which the DomainConstraintRef
 * lives.  The ref will be cleaned up when that context is reset/deleted.
 *
 * Caller must also tell us whether it wants check_exprstate fields to be
 * computed in the DomainConstraintState nodes attached to this ref.
 * If it doesn't, we need not make a copy of the DomainConstraintState list.
 */


/*
 * UpdateDomainConstraintRef --- recheck validity of domain constraint info
 *
 * If the domain's constraint set changed, ref->constraints is updated to
 * point at a new list of cached constraints.
 *
 * In the normal case where nothing happened to the domain, this is cheap
 * enough that it's reasonable (and expected) to check before *each* use
 * of the constraint info.
 */


/*
 * DomainHasConstraints --- utility routine to check if a domain has constraints
 *
 * This is defined to return false, not fail, if type is not a domain.
 */



/*
 * array_element_has_equality and friends are helper routines to check
 * whether we should believe that array_eq and related functions will work
 * on the given array type or composite type.
 *
 * The logic above may call these repeatedly on the same type entry, so we
 * make use of the typentry->flags field to cache the results once known.
 * Also, we assume that we'll probably want all these facts about the type
 * if we want any, so we cache them all using only one lookup of the
 * component datatype(s).
 */











/*
 * Likewise, some helper functions for composite types.
 */











/*
 * Likewise, some helper functions for range and multirange types.
 *
 * We can borrow the flag bits for array element properties to use for range
 * element properties, since those flag bits otherwise have no use in a
 * range or multirange type's typcache entry.
 */













/*
 * Make sure that RecordCacheArray and RecordIdentifierArray are large enough
 * to store 'typmod'.
 */


/*
 * lookup_rowtype_tupdesc_internal --- internal routine to lookup a rowtype
 *
 * Same API as lookup_rowtype_tupdesc_noerror, but the returned tupdesc
 * hasn't had its refcount bumped.
 */


/*
 * lookup_rowtype_tupdesc
 *
 * Given a typeid/typmod that should describe a known composite type,
 * return the tuple descriptor for the type.  Will ereport on failure.
 * (Use ereport because this is reachable with user-specified OIDs,
 * for example from record_in().)
 *
 * Note: on success, we increment the refcount of the returned TupleDesc,
 * and log the reference in CurrentResourceOwner.  Caller must call
 * ReleaseTupleDesc when done using the tupdesc.  (There are some
 * cases in which the returned tupdesc is not refcounted, in which
 * case PinTupleDesc/ReleaseTupleDesc are no-ops; but in these cases
 * the tupdesc is guaranteed to live till process exit.)
 */


/*
 * lookup_rowtype_tupdesc_noerror
 *
 * As above, but if the type is not a known composite type and noError
 * is true, returns NULL instead of ereport'ing.  (Note that if a bogus
 * type_id is passed, you'll get an ereport anyway.)
 */


/*
 * lookup_rowtype_tupdesc_copy
 *
 * Like lookup_rowtype_tupdesc(), but the returned TupleDesc has been
 * copied into the CurrentMemoryContext and is not reference-counted.
 */


/*
 * lookup_rowtype_tupdesc_domain
 *
 * Same as lookup_rowtype_tupdesc_noerror(), except that the type can also be
 * a domain over a named composite type; so this is effectively equivalent to
 * lookup_rowtype_tupdesc_noerror(getBaseType(type_id), typmod, noError)
 * except for being a tad faster.
 *
 * Note: the reason we don't fold the look-through-domain behavior into plain
 * lookup_rowtype_tupdesc() is that we want callers to know they might be
 * dealing with a domain.  Otherwise they might construct a tuple that should
 * be of the domain type, but not apply domain constraints.
 */


/*
 * Hash function for the hash table of RecordCacheEntry.
 */


/*
 * Match function for the hash table of RecordCacheEntry.
 */


/*
 * assign_record_type_typmod
 *
 * Given a tuple descriptor for a RECORD type, find or create a cache entry
 * for the type, and set the tupdesc's tdtypmod field to a value that will
 * identify this cache entry to lookup_rowtype_tupdesc.
 */


/*
 * assign_record_type_identifier
 *
 * Get an identifier, which will be unique over the lifespan of this backend
 * process, for the current tuple descriptor of the specified composite type.
 * For named composite types, the value is guaranteed to change if the type's
 * definition does.  For registered RECORD types, the value will not change
 * once assigned, since the registered type won't either.  If an anonymous
 * RECORD type is specified, we return a new identifier on each call.
 */


/*
 * Return the amount of shmem required to hold a SharedRecordTypmodRegistry.
 * This exists only to avoid exposing private innards of
 * SharedRecordTypmodRegistry in a header.
 */


/*
 * Initialize 'registry' in a pre-existing shared memory region, which must be
 * maximally aligned and have space for SharedRecordTypmodRegistryEstimate()
 * bytes.
 *
 * 'area' will be used to allocate shared memory space as required for the
 * typemod registration.  The current process, expected to be a leader process
 * in a parallel query, will be attached automatically and its current record
 * types will be loaded into *registry.  While attached, all calls to
 * assign_record_type_typmod will use the shared registry.  Worker backends
 * will need to attach explicitly.
 *
 * Note that this function takes 'area' and 'segment' as arguments rather than
 * accessing them via CurrentSession, because they aren't installed there
 * until after this function runs.
 */


/*
 * Attach to 'registry', which must have been initialized already by another
 * backend.  Future calls to assign_record_type_typmod and
 * lookup_rowtype_tupdesc_internal will use the shared registry until the
 * current session is detached.
 */


/*
 * InvalidateCompositeTypeCacheEntry
 *		Invalidate particular TypeCacheEntry on Relcache inval callback
 *
 * Delete the cached tuple descriptor (if any) for the given composite
 * type, and reset whatever info we have cached about the composite type's
 * comparability.
 */


/*
 * TypeCacheRelCallback
 *		Relcache inval callback function
 *
 * Delete the cached tuple descriptor (if any) for the given rel's composite
 * type, or for all composite types if relid == InvalidOid.  Also reset
 * whatever info we have cached about the composite type's comparability.
 *
 * This is called when a relcache invalidation event occurs for the given
 * relid.  We can't use syscache to find a type corresponding to the given
 * relation because the code can be called outside of transaction. Thus, we
 * use the RelIdToTypeIdCacheHash map to locate appropriate typcache entry.
 */


/*
 * TypeCacheTypCallback
 *		Syscache inval callback function
 *
 * This is called when a syscache invalidation event occurs for any
 * pg_type row.  If we have information cached about that type, mark
 * it as needing to be reloaded.
 */


/*
 * TypeCacheOpcCallback
 *		Syscache inval callback function
 *
 * This is called when a syscache invalidation event occurs for any pg_opclass
 * row.  In principle we could probably just invalidate data dependent on the
 * particular opclass, but since updates on pg_opclass are rare in production
 * it doesn't seem worth a lot of complication: we just mark all cached data
 * invalid.
 *
 * Note that we don't bother watching for updates on pg_amop or pg_amproc.
 * This should be safe because ALTER OPERATOR FAMILY ADD/DROP OPERATOR/FUNCTION
 * is not allowed to be used to add/drop the primary operators and functions
 * of an opclass, only cross-type members of a family; and the latter sorts
 * of members are not going to get cached here.
 */


/*
 * TypeCacheConstrCallback
 *		Syscache inval callback function
 *
 * This is called when a syscache invalidation event occurs for any
 * pg_constraint row.  We flush information about domain constraints
 * when this happens.
 *
 * It's slightly annoying that we can't tell whether the inval event was for
 * a domain constraint record or not; there's usually more update traffic
 * for table constraints than domain constraints, so we'll do a lot of
 * useless flushes.  Still, this is better than the old no-caching-at-all
 * approach to domain constraints.
 */



/*
 * Check if given OID is part of the subset that's sortable by comparisons
 */



/*
 * compare_values_of_enum
 *		Compare two members of an enum type.
 *		Return <0, 0, or >0 according as arg1 <, =, or > arg2.
 *
 * Note: currently, the enumData cache is refreshed only if we are asked
 * to compare an enum value that is not already in the cache.  This is okay
 * because there is no support for re-ordering existing values, so comparisons
 * of previously cached values will return the right answer even if other
 * values have been added since we last loaded the cache.
 *
 * Note: the enum logic has a special-case rule about even-numbered versus
 * odd-numbered OIDs, but we take no account of that rule here; this
 * routine shouldn't even get called when that rule applies.
 */


/*
 * Load (or re-load) the enumData member of the typcache entry.
 */


/*
 * Locate the EnumItem with the given OID, if present
 */


/*
 * qsort comparison function for OID-ordered EnumItems
 */


/*
 * Copy 'tupdesc' into newly allocated shared memory in 'area', set its typmod
 * to the given value and return a dsa_pointer.
 */


/*
 * If we are attached to a SharedRecordTypmodRegistry, use it to find or
 * create a shared TupleDesc that matches 'tupdesc'.  Otherwise return NULL.
 * Tuple descriptors returned by this function are not reference counted, and
 * will exist at least as long as the current backend remained attached to the
 * current session.
 */


/*
 * On-DSM-detach hook to forget about the current shared record typmod
 * infrastructure.  This is currently used by both leader and workers.
 */


/*
 * Insert RelIdToTypeIdCacheHash entry if needed.
 */


/*
 * Delete entry RelIdToTypeIdCacheHash if needed after resetting of the
 * TCFLAGS_HAVE_PG_TYPE_DATA flag, or any of TCFLAGS_OPERATOR_FLAGS,
 * or tupDesc.
 */
#ifdef USE_ASSERT_CHECKING
#endif
#ifdef USE_ASSERT_CHECKING
#endif

/*
 * Add possibly missing RelIdToTypeId entries related to TypeCacheHash
 * entries, marked as in-progress by lookup_type_cache().  It may happen
 * in case of an error or interruption during the lookup_type_cache() call.
 */





