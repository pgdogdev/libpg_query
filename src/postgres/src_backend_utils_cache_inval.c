/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - AcceptInvalidationMessages
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * inval.c
 *	  POSTGRES cache invalidation dispatcher code.
 *
 *	This is subtle stuff, so pay attention:
 *
 *	When a tuple is updated or deleted, our standard visibility rules
 *	consider that it is *still valid* so long as we are in the same command,
 *	ie, until the next CommandCounterIncrement() or transaction commit.
 *	(See access/heap/heapam_visibility.c, and note that system catalogs are
 *  generally scanned under the most current snapshot available, rather than
 *  the transaction snapshot.)	At the command boundary, the old tuple stops
 *	being valid and the new version, if any, becomes valid.  Therefore,
 *	we cannot simply flush a tuple from the system caches during heap_update()
 *	or heap_delete().  The tuple is still good at that point; what's more,
 *	even if we did flush it, it might be reloaded into the caches by a later
 *	request in the same command.  So the correct behavior is to keep a list
 *	of outdated (updated/deleted) tuples and then do the required cache
 *	flushes at the next command boundary.  We must also keep track of
 *	inserted tuples so that we can flush "negative" cache entries that match
 *	the new tuples; again, that mustn't happen until end of command.
 *
 *	Once we have finished the command, we still need to remember inserted
 *	tuples (including new versions of updated tuples), so that we can flush
 *	them from the caches if we abort the transaction.  Similarly, we'd better
 *	be able to flush "negative" cache entries that may have been loaded in
 *	place of deleted tuples, so we still need the deleted ones too.
 *
 *	If we successfully complete the transaction, we have to broadcast all
 *	these invalidation events to other backends (via the SI message queue)
 *	so that they can flush obsolete entries from their caches.  Note we have
 *	to record the transaction commit before sending SI messages, otherwise
 *	the other backends won't see our updated tuples as good.
 *
 *	When a subtransaction aborts, we can process and discard any events
 *	it has queued.  When a subtransaction commits, we just add its events
 *	to the pending lists of the parent transaction.
 *
 *	In short, we need to remember until xact end every insert or delete
 *	of a tuple that might be in the system caches.  Updates are treated as
 *	two events, delete + insert, for simplicity.  (If the update doesn't
 *	change the tuple hash value, catcache.c optimizes this into one event.)
 *
 *	We do not need to register EVERY tuple operation in this way, just those
 *	on tuples in relations that have associated catcaches.  We do, however,
 *	have to register every operation on every tuple that *could* be in a
 *	catcache, whether or not it currently is in our cache.  Also, if the
 *	tuple is in a relation that has multiple catcaches, we need to register
 *	an invalidation message for each such catcache.  catcache.c's
 *	PrepareToInvalidateCacheTuple() routine provides the knowledge of which
 *	catcaches may need invalidation for a given tuple.
 *
 *	Also, whenever we see an operation on a pg_class, pg_attribute, or
 *	pg_index tuple, we register a relcache flush operation for the relation
 *	described by that tuple (as specified in CacheInvalidateHeapTuple()).
 *	Likewise for pg_constraint tuples for foreign keys on relations.
 *
 *	We keep the relcache flush requests in lists separate from the catcache
 *	tuple flush requests.  This allows us to issue all the pending catcache
 *	flushes before we issue relcache flushes, which saves us from loading
 *	a catcache tuple during relcache load only to flush it again right away.
 *	Also, we avoid queuing multiple relcache flush requests for the same
 *	relation, since a relcache flush is relatively expensive to do.
 *	(XXX is it worth testing likewise for duplicate catcache flush entries?
 *	Probably not.)
 *
 *	Many subsystems own higher-level caches that depend on relcache and/or
 *	catcache, and they register callbacks here to invalidate their caches.
 *	While building a higher-level cache entry, a backend may receive a
 *	callback for the being-built entry or one of its dependencies.  This
 *	implies the new higher-level entry would be born stale, and it might
 *	remain stale for the life of the backend.  Many caches do not prevent
 *	that.  They rely on DDL for can't-miss catalog changes taking
 *	AccessExclusiveLock on suitable objects.  (For a change made with less
 *	locking, backends might never read the change.)  The relation cache,
 *	however, needs to reflect changes from CREATE INDEX CONCURRENTLY no later
 *	than the beginning of the next transaction.  Hence, when a relevant
 *	invalidation callback arrives during a build, relcache.c reattempts that
 *	build.  Caches with similar needs could do likewise.
 *
 *	If a relcache flush is issued for a system relation that we preload
 *	from the relcache init file, we must also delete the init file so that
 *	it will be rebuilt during the next backend restart.  The actual work of
 *	manipulating the init file is in relcache.c, but we keep track of the
 *	need for it here.
 *
 *	Currently, inval messages are sent without regard for the possibility
 *	that the object described by the catalog tuple might be a session-local
 *	object such as a temporary table.  This is because (1) this code has
 *	no practical way to tell the difference, and (2) it is not certain that
 *	other backends don't have catalog cache or even relcache entries for
 *	such tables, anyway; there is nothing that prevents that.  It might be
 *	worth trying to avoid sending such inval traffic in the future, if those
 *	problems can be overcome cheaply.
 *
 *	When making a nontransactional change to a cacheable object, we must
 *	likewise send the invalidation immediately, before ending the change's
 *	critical section.  This includes inplace heap updates, relmap, and smgr.
 *
 *	When wal_level=logical, write invalidations into WAL at each command end to
 *	support the decoding of the in-progress transactions.  See
 *	CommandEndInvalidationMessages.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/inval.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/pg_constraint.h"
#include "miscadmin.h"
#include "storage/procnumber.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/catcache.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * Pending requests are stored as ready-to-send SharedInvalidationMessages.
 * We keep the messages themselves in arrays in TopTransactionContext (there
 * are separate arrays for catcache and relcache messages).  For transactional
 * messages, control information is kept in a chain of TransInvalidationInfo
 * structs, also allocated in TopTransactionContext.  (We could keep a
 * subtransaction's TransInvalidationInfo in its CurTransactionContext; but
 * that's more wasteful not less so, since in very many scenarios it'd be the
 * only allocation in the subtransaction's CurTransactionContext.)  For
 * inplace update messages, control information appears in an
 * InvalidationInfo, allocated in CurrentMemoryContext.
 *
 * We can store the message arrays densely, and yet avoid moving data around
 * within an array, because within any one subtransaction we need only
 * distinguish between messages emitted by prior commands and those emitted
 * by the current command.  Once a command completes and we've done local
 * processing on its messages, we can fold those into the prior-commands
 * messages just by changing array indexes in the TransInvalidationInfo
 * struct.  Similarly, we need distinguish messages of prior subtransactions
 * from those of the current subtransaction only until the subtransaction
 * completes, after which we adjust the array indexes in the parent's
 * TransInvalidationInfo to include the subtransaction's messages.  Inplace
 * invalidations don't need a concept of command or subtransaction boundaries,
 * since we send them during the WAL insertion critical section.
 *
 * The ordering of the individual messages within a command's or
 * subtransaction's output is not considered significant, although this
 * implementation happens to preserve the order in which they were queued.
 * (Previous versions of this code did not preserve it.)
 *
 * For notational convenience, control information is kept in two-element
 * arrays, the first for catcache messages and the second for relcache
 * messages.
 */
#define CatCacheMsgs 0
#define RelCacheMsgs 1

/* Pointers to main arrays in TopTransactionContext */
typedef struct InvalMessageArray
{
	SharedInvalidationMessage *msgs;	/* palloc'd array (can be expanded) */
	int			maxmsgs;		/* current allocated size of array */
} InvalMessageArray;



/* Control information for one logical group of messages */
typedef struct InvalidationMsgsGroup
{
	int			firstmsg[2];	/* first index in relevant array */
	int			nextmsg[2];		/* last+1 index */
} InvalidationMsgsGroup;

/* Macros to help preserve InvalidationMsgsGroup abstraction */
#define SetSubGroupToFollow(targetgroup, priorgroup, subgroup) \
	do { \
		(targetgroup)->firstmsg[subgroup] = \
			(targetgroup)->nextmsg[subgroup] = \
			(priorgroup)->nextmsg[subgroup]; \
	} while (0)

#define SetGroupToFollow(targetgroup, priorgroup) \
	do { \
		SetSubGroupToFollow(targetgroup, priorgroup, CatCacheMsgs); \
		SetSubGroupToFollow(targetgroup, priorgroup, RelCacheMsgs); \
	} while (0)

#define NumMessagesInSubGroup(group, subgroup) \
	((group)->nextmsg[subgroup] - (group)->firstmsg[subgroup])

#define NumMessagesInGroup(group) \
	(NumMessagesInSubGroup(group, CatCacheMsgs) + \
	 NumMessagesInSubGroup(group, RelCacheMsgs))


/*----------------
 * Transactional invalidation messages are divided into two groups:
 *	1) events so far in current command, not yet reflected to caches.
 *	2) events in previous commands of current transaction; these have
 *	   been reflected to local caches, and must be either broadcast to
 *	   other backends or rolled back from local cache when we commit
 *	   or abort the transaction.
 * Actually, we need such groups for each level of nested transaction,
 * so that we can discard events from an aborted subtransaction.  When
 * a subtransaction commits, we append its events to the parent's groups.
 *
 * The relcache-file-invalidated flag can just be a simple boolean,
 * since we only act on it at transaction commit; we don't care which
 * command of the transaction set it.
 *----------------
 */

/* fields common to both transactional and inplace invalidation */
typedef struct InvalidationInfo
{
	/* Events emitted by current command */
	InvalidationMsgsGroup CurrentCmdInvalidMsgs;

	/* init file must be invalidated? */
	bool		RelcacheInitFileInval;
} InvalidationInfo;

/* subclass adding fields specific to transactional invalidation */
typedef struct TransInvalidationInfo
{
	/* Base class */
	struct InvalidationInfo ii;

	/* Events emitted by previous commands of this (sub)transaction */
	InvalidationMsgsGroup PriorCmdInvalidMsgs;

	/* Back link to parent transaction's info */
	struct TransInvalidationInfo *parent;

	/* Subtransaction nesting depth */
	int			my_level;
} TransInvalidationInfo;





/* GUC storage */


/*
 * Dynamically-registered callback functions.  Current implementation
 * assumes there won't be enough of these to justify a dynamically resizable
 * array; it'd be easy to improve that if needed.
 *
 * To avoid searching in CallSyscacheCallbacks, all callbacks for a given
 * syscache are linked into a list pointed to by syscache_callback_links[id].
 * The link values are syscache_callback_list[] index plus 1, or 0 for none.
 */

#define MAX_SYSCACHE_CALLBACKS 64
#define MAX_RELCACHE_CALLBACKS 10
#define MAX_RELSYNC_CALLBACKS 10
















/* ----------------------------------------------------------------
 *				Invalidation subgroup support functions
 * ----------------------------------------------------------------
 */

/*
 * AddInvalidationMessage
 *		Add an invalidation message to a (sub)group.
 *
 * The group must be the last active one, since we assume we can add to the
 * end of the relevant InvalMessageArray.
 *
 * subgroup must be CatCacheMsgs or RelCacheMsgs.
 */


/*
 * Append one subgroup of invalidation messages to another, resetting
 * the source subgroup to empty.
 */


/*
 * Process a subgroup of invalidation messages.
 *
 * This is a macro that executes the given code fragment for each message in
 * a message subgroup.  The fragment should refer to the message as *msg.
 */
#define ProcessMessageSubGroup(group, subgroup, codeFragment) \
	do { \
		int		_msgindex = (group)->firstmsg[subgroup]; \
		int		_endmsg = (group)->nextmsg[subgroup]; \
		for (; _msgindex < _endmsg; _msgindex++) \
		{ \
			SharedInvalidationMessage *msg = \
				&InvalMessageArrays[subgroup].msgs[_msgindex]; \
			codeFragment; \
		} \
	} while (0)

/*
 * Process a subgroup of invalidation messages as an array.
 *
 * As above, but the code fragment can handle an array of messages.
 * The fragment should refer to the messages as msgs[], with n entries.
 */
#define ProcessMessageSubGroupMulti(group, subgroup, codeFragment) \
	do { \
		int		n = NumMessagesInSubGroup(group, subgroup); \
		if (n > 0) { \
			SharedInvalidationMessage *msgs = \
				&InvalMessageArrays[subgroup].msgs[(group)->firstmsg[subgroup]]; \
			codeFragment; \
		} \
	} while (0)


/* ----------------------------------------------------------------
 *				Invalidation group support functions
 *
 * These routines understand about the division of a logical invalidation
 * group into separate physical arrays for catcache and relcache entries.
 * ----------------------------------------------------------------
 */

/*
 * Add a catcache inval entry
 */


/*
 * Add a whole-catalog inval entry
 */


/*
 * Add a relcache inval entry
 */


/*
 * Add a relsync inval entry
 *
 * We put these into the relcache subgroup for simplicity. This message is the
 * same as AddRelcacheInvalidationMessage() except that it is for
 * RelationSyncCache maintained by decoding plugin pgoutput.
 */


/*
 * Add a snapshot inval entry
 *
 * We put these into the relcache subgroup for simplicity.
 */


/*
 * Append one group of invalidation messages to another, resetting
 * the source group to empty.
 */


/*
 * Execute the given function for all the messages in an invalidation group.
 * The group is not altered.
 *
 * catcache entries are processed first, for reasons mentioned above.
 */


/*
 * As above, but the function is able to process an array of messages
 * rather than just one at a time.
 */


/* ----------------------------------------------------------------
 *					  private support functions
 * ----------------------------------------------------------------
 */

/*
 * RegisterCatcacheInvalidation
 *
 * Register an invalidation event for a catcache tuple entry.
 */


/*
 * RegisterCatalogInvalidation
 *
 * Register an invalidation event for all catcache entries from a catalog.
 */


/*
 * RegisterRelcacheInvalidation
 *
 * As above, but register a relcache invalidation event.
 */


/*
 * RegisterRelsyncInvalidation
 *
 * As above, but register a relsynccache invalidation event.
 */


/*
 * RegisterSnapshotInvalidation
 *
 * Register an invalidation event for MVCC scans against a given catalog.
 * Only needed for catalogs that don't have catcaches.
 */


/*
 * PrepareInvalidationState
 *		Initialize inval data for the current (sub)transaction.
 */


/*
 * PrepareInplaceInvalidationState
 *		Initialize inval data for an inplace update.
 *
 * See previous function for more background.
 */


/* ----------------------------------------------------------------
 *					  public functions
 * ----------------------------------------------------------------
 */



/*
 * LocalExecuteInvalidationMessage
 *
 * Process a single invalidation message (which could be of any type).
 * Only the local caches are flushed; this does not transmit the message
 * to other backends.
 */


/*
 *		InvalidateSystemCaches
 *
 *		This blows away all tuples in the system catalog caches and
 *		all the cached relation descriptors and smgr cache entries.
 *		Relation descriptors that have positive refcounts are then rebuilt.
 *
 *		We call this when we see a shared-inval-queue overflow signal,
 *		since that tells us we've lost some shared-inval messages and hence
 *		don't know what needs to be invalidated.
 */


/*
 * AcceptInvalidationMessages
 *		Read and process invalidation messages from the shared invalidation
 *		message queue.
 *
 * Note:
 *		This should be called as the first step in processing a transaction.
 */

void
AcceptInvalidationMessages(void)
{
	/* Do nothing */
}

/*
 * PostPrepare_Inval
 *		Clean up after successful PREPARE.
 *
 * Here, we want to act as though the transaction aborted, so that we will
 * undo any syscache changes it made, thereby bringing us into sync with the
 * outside world, which doesn't believe the transaction committed yet.
 *
 * If the prepared transaction is later aborted, there is nothing more to
 * do; if it commits, we will receive the consequent inval messages just
 * like everyone else.
 */


/*
 * xactGetCommittedInvalidationMessages() is called by
 * RecordTransactionCommit() to collect invalidation messages to add to the
 * commit record. This applies only to commit message types, never to
 * abort records. Must always run before AtEOXact_Inval(), since that
 * removes the data we need to see.
 *
 * Remember that this runs before we have officially committed, so we
 * must not do anything here to change what might occur *if* we should
 * fail between here and the actual commit.
 *
 * see also xact_redo_commit() and xact_desc_commit()
 */


/*
 * inplaceGetInvalidationMessages() is called by the inplace update to collect
 * invalidation messages to add to its WAL record.  Like the previous
 * function, we might still fail.
 */


/*
 * ProcessCommittedInvalidationMessages is executed by xact_redo_commit() or
 * standby_redo() to process invalidation messages. Currently that happens
 * only at end-of-xact.
 *
 * Relcache init file invalidation requires processing both
 * before and after we send the SI messages. See AtEOXact_Inval()
 */


/*
 * AtEOXact_Inval
 *		Process queued-up invalidation messages at end of main transaction.
 *
 * If isCommit, we must send out the messages in our PriorCmdInvalidMsgs list
 * to the shared invalidation message queue.  Note that these will be read
 * not only by other backends, but also by our own backend at the next
 * transaction start (via AcceptInvalidationMessages).  This means that
 * we can skip immediate local processing of anything that's still in
 * CurrentCmdInvalidMsgs, and just send that list out too.
 *
 * If not isCommit, we are aborting, and must locally process the messages
 * in PriorCmdInvalidMsgs.  No messages need be sent to other backends,
 * since they'll not have seen our changed tuples anyway.  We can forget
 * about CurrentCmdInvalidMsgs too, since those changes haven't touched
 * the caches yet.
 *
 * In any case, reset our state to empty.  We need not physically
 * free memory here, since TopTransactionContext is about to be emptied
 * anyway.
 *
 * Note:
 *		This should be called as the last step in processing a transaction.
 */


/*
 * PreInplace_Inval
 *		Process queued-up invalidation before inplace update critical section.
 *
 * Tasks belong here if they are safe even if the inplace update does not
 * complete.  Currently, this just unlinks a cache file, which can fail.  The
 * sum of this and AtInplace_Inval() mirrors AtEOXact_Inval(isCommit=true).
 */


/*
 * AtInplace_Inval
 *		Process queued-up invalidations after inplace update buffer mutation.
 */


/*
 * ForgetInplace_Inval
 *		Alternative to PreInplace_Inval()+AtInplace_Inval(): discard queued-up
 *		invalidations.  This lets inplace update enumerate invalidations
 *		optimistically, before locking the buffer.
 */


/*
 * AtEOSubXact_Inval
 *		Process queued-up invalidation messages at end of subtransaction.
 *
 * If isCommit, process CurrentCmdInvalidMsgs if any (there probably aren't),
 * and then attach both CurrentCmdInvalidMsgs and PriorCmdInvalidMsgs to the
 * parent's PriorCmdInvalidMsgs list.
 *
 * If not isCommit, we are aborting, and must locally process the messages
 * in PriorCmdInvalidMsgs.  No messages need be sent to other backends.
 * We can forget about CurrentCmdInvalidMsgs too, since those changes haven't
 * touched the caches yet.
 *
 * In any case, pop the transaction stack.  We need not physically free memory
 * here, since CurTransactionContext is about to be emptied anyway
 * (if aborting).  Beware of the possibility of aborting the same nesting
 * level twice, though.
 */


/*
 * CommandEndInvalidationMessages
 *		Process queued-up invalidation messages at end of one command
 *		in a transaction.
 *
 * Here, we send no messages to the shared queue, since we don't know yet if
 * we will commit.  We do need to locally process the CurrentCmdInvalidMsgs
 * list, so as to flush our caches of any entries we have outdated in the
 * current command.  We then move the current-cmd list over to become part
 * of the prior-cmds list.
 *
 * Note:
 *		This should be called during CommandCounterIncrement(),
 *		after we have advanced the command ID.
 */



/*
 * CacheInvalidateHeapTupleCommon
 *		Common logic for end-of-command and inplace variants.
 */


/*
 * CacheInvalidateHeapTuple
 *		Register the given tuple for invalidation at end of command
 *		(ie, current command is creating or outdating this tuple) and end of
 *		transaction.  Also, detect whether a relcache invalidation is implied.
 *
 * For an insert or delete, tuple is the target tuple and newtuple is NULL.
 * For an update, we are called just once, with tuple being the old tuple
 * version and newtuple the new version.  This allows avoidance of duplicate
 * effort during an update.
 */


/*
 * CacheInvalidateHeapTupleInplace
 *		Register the given tuple for nontransactional invalidation pertaining
 *		to an inplace update.  Also, detect whether a relcache invalidation is
 *		implied.
 *
 * Like CacheInvalidateHeapTuple(), but for inplace updates.
 */


/*
 * CacheInvalidateCatalog
 *		Register invalidation of the whole content of a system catalog.
 *
 * This is normally used in VACUUM FULL/CLUSTER, where we haven't so much
 * changed any tuples as moved them around.  Some uses of catcache entries
 * expect their TIDs to be correct, so we have to blow away the entries.
 *
 * Note: we expect caller to verify that the rel actually is a system
 * catalog.  If it isn't, no great harm is done, just a wasted sinval message.
 */


/*
 * CacheInvalidateRelcache
 *		Register invalidation of the specified relation's relcache entry
 *		at end of command.
 *
 * This is used in places that need to force relcache rebuild but aren't
 * changing any of the tuples recognized as contributors to the relcache
 * entry by CacheInvalidateHeapTuple.  (An example is dropping an index.)
 */


/*
 * CacheInvalidateRelcacheAll
 *		Register invalidation of the whole relcache at the end of command.
 *
 * This is used by alter publication as changes in publications may affect
 * large number of tables.
 */


/*
 * CacheInvalidateRelcacheByTuple
 *		As above, but relation is identified by passing its pg_class tuple.
 */


/*
 * CacheInvalidateRelcacheByRelid
 *		As above, but relation is identified by passing its OID.
 *		This is the least efficient of the three options; use one of
 *		the above routines if you have a Relation or pg_class tuple.
 */


/*
 * CacheInvalidateRelSync
 *		Register invalidation of the cache in logical decoding output plugin
 *		for a database.
 *
 * This type of invalidation message is used for the specific purpose of output
 * plugins. Processes which do not decode WALs would do nothing even when it
 * receives the message.
 */


/*
 * CacheInvalidateRelSyncAll
 *		Register invalidation of the whole cache in logical decoding output
 *		plugin.
 */


/*
 * CacheInvalidateSmgr
 *		Register invalidation of smgr references to a physical relation.
 *
 * Sending this type of invalidation msg forces other backends to close open
 * smgr entries for the rel.  This should be done to flush dangling open-file
 * references when the physical rel is being dropped or truncated.  Because
 * these are nontransactional (i.e., not-rollback-able) operations, we just
 * send the inval message immediately without any queuing.
 *
 * Note: in most cases there will have been a relcache flush issued against
 * the rel at the logical level.  We need a separate smgr-level flush because
 * it is possible for backends to have open smgr entries for rels they don't
 * have a relcache entry for, e.g. because the only thing they ever did with
 * the rel is write out dirty shared buffers.
 *
 * Note: because these messages are nontransactional, they won't be captured
 * in commit/abort WAL entries.  Instead, calls to CacheInvalidateSmgr()
 * should happen in low-level smgr.c routines, which are executed while
 * replaying WAL as well as when creating it.
 *
 * Note: In order to avoid bloating SharedInvalidationMessage, we store only
 * three bytes of the ProcNumber using what would otherwise be padding space.
 * Thus, the maximum possible ProcNumber is 2^23-1.
 */


/*
 * CacheInvalidateRelmap
 *		Register invalidation of the relation mapping for a database,
 *		or for the shared catalogs if databaseId is zero.
 *
 * Sending this type of invalidation msg forces other backends to re-read
 * the indicated relation mapping file.  It is also necessary to send a
 * relcache inval for the specific relations whose mapping has been altered,
 * else the relcache won't get updated with the new filenode data.
 *
 * Note: because these messages are nontransactional, they won't be captured
 * in commit/abort WAL entries.  Instead, calls to CacheInvalidateRelmap()
 * should happen in low-level relmapper.c routines, which are executed while
 * replaying WAL as well as when creating it.
 */



/*
 * CacheRegisterSyscacheCallback
 *		Register the specified function to be called for all future
 *		invalidation events in the specified cache.  The cache ID and the
 *		hash value of the tuple being invalidated will be passed to the
 *		function.
 *
 * NOTE: Hash value zero will be passed if a cache reset request is received.
 * In this case the called routines should flush all cached state.
 * Yes, there's a possibility of a false match to zero, but it doesn't seem
 * worth troubling over, especially since most of the current callees just
 * flush all cached state anyway.
 */


/*
 * CacheRegisterRelcacheCallback
 *		Register the specified function to be called for all future
 *		relcache invalidation events.  The OID of the relation being
 *		invalidated will be passed to the function.
 *
 * NOTE: InvalidOid will be passed if a cache reset request is received.
 * In this case the called routines should flush all cached state.
 */


/*
 * CacheRegisterRelSyncCallback
 *		Register the specified function to be called for all future
 *		relsynccache invalidation events.
 *
 * This function is intended to be call from the logical decoding output
 * plugins.
 */


/*
 * CallSyscacheCallbacks
 *
 * This is exported so that CatalogCacheFlushCatalog can call it, saving
 * this module from knowing which catcache IDs correspond to which catalogs.
 */


/*
 * CallSyscacheCallbacks
 */


/*
 * LogLogicalInvalidations
 *
 * Emit WAL for invalidations caused by the current command.
 *
 * This is currently only used for logging invalidations at the command end
 * or at commit time if any invalidations are pending.
 */

