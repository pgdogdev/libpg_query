/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - SharedInvalidMessageCounter
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * sinval.c
 *	  POSTGRES shared cache invalidation communication code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/sinval.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/sinvaladt.h"
#include "utils/inval.h"


__thread uint64		SharedInvalidMessageCounter;



/*
 * Because backends sitting idle will not be reading sinval events, we
 * need a way to give an idle backend a swift kick in the rear and make
 * it catch up before the sinval queue overflows and forces it to go
 * through a cache reset exercise.  This is done by sending
 * PROCSIG_CATCHUP_INTERRUPT to any backend that gets too far behind.
 *
 * The signal handler will set an interrupt pending flag and will set the
 * processes latch. Whenever starting to read from the client, or when
 * interrupted while doing so, ProcessClientReadInterrupt() will call
 * ProcessCatchupEvent().
 */



/*
 * SendSharedInvalidMessages
 *	Add shared-cache-invalidation message(s) to the global SI message queue.
 */


/*
 * ReceiveSharedInvalidMessages
 *		Process shared-cache-invalidation messages waiting for this backend
 *
 * We guarantee to process all messages that had been queued before the
 * routine was entered.  It is of course possible for more messages to get
 * queued right after our last SIGetDataEntries call.
 *
 * NOTE: it is entirely possible for this routine to be invoked recursively
 * as a consequence of processing inside the invalFunction or resetFunction.
 * Furthermore, such a recursive call must guarantee that all outstanding
 * inval messages have been processed before it exits.  This is the reason
 * for the strange-looking choice to use a statically allocated buffer array
 * and counters; it's so that a recursive call can process messages already
 * sucked out of sinvaladt.c.
 */
#define MAXINVALMSGS 32


/*
 * HandleCatchupInterrupt
 *
 * This is called when PROCSIG_CATCHUP_INTERRUPT is received.
 *
 * We used to directly call ProcessCatchupEvent directly when idle. These days
 * we just set a flag to do it later and notify the process of that fact by
 * setting the process's latch.
 */


/*
 * ProcessCatchupInterrupt
 *
 * The portion of catchup interrupt handling that runs outside of the signal
 * handler, which allows it to actually process pending invalidations.
 */

