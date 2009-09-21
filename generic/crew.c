/*
 * crew.c --
 *
 * CREW: Concurrent Read Exclusive Write
 *
 * WARNING: Callers can create a deadlock by calling the routines of this
 * module.  Each routine that calls a CREW_*Hold() routine on a CREW_Lock,
 * must also call CREW_Release on the same lock before returning.  Also,
 * a CREW_WriteHold() call cannot be nested within any section of code where
 * a read hold on the same CREW_Lock is held.  More complex deadlock scenarios
 * are also possible.  Use care with these routines.
 */

#include <tcl.h>
#include "crew.h"

/*
 * The following data structure is the internal form of a
 * CREW_Lock.  Each instance of this data structure controls
 * concurrent read, exclusive write access to some resource.
 * A CREW_Lock is really a (Lock *).
 */
typedef struct Lock {
    Tcl_Mutex		mutex;		/* Mutex used to serialize condition
					 * variable operations */
    Tcl_Condition	wakeReaders;	/* Condition variable that is notified
					 * when threads waiting to read should
					 * get a chance */
    Tcl_Condition	wakeWriters;	/* Condition variable that is notified
					 * when threads waiting to write should
					 * get a chance */
    int			readWaiters;	/* Number of threads waiting for
					 * a chance to read */
    int			writeWaiters;	/* Number of threads waiting for
					 * a chance to write */
    int			holders;	/* When > 0, the number of read holds
					 * on this lock.  Each thread can have
					 * multiple read holds at once, and
					 * multiple threads can also have read
					 * holds simultaneously.
					 * When == -1, one thread has a write
					 * hold on the resource.
					 * When == 0, there are no users
					 * of the resource. */
} Lock;

/*
 *  A mutex used to serialize some internal operations
 */
TCL_DECLARE_MUTEX(crewMasterMutex)

/*
 * Procedure used only in this module
 */
static Lock *		GetLock(CREW_Lock *clPtr);


/*
 *---------------------------------------------------------------------------
 *
 * GetLock --
 *
 *      Gets the internal Lock structure corresponding to a CREW_Lock passed
 *      in.  Automatically initializes a CREW_Lock on first use.
 *
 * Results:
 *      Returns a pointer to the Lock.
 *
 * Side effects:
 *      May initialize a Lock/CREW_Lock on first use.  This includes
 *      allocation of memory that will be freed only by CREW_Finalize().
 *
 *---------------------------------------------------------------------------
 */

static Lock *
GetLock(CREW_Lock *clPtr) {
    if (*clPtr == NULL) {			/* first use */
	Tcl_MutexLock(&crewMasterMutex);	/* settle any race between
						 * multiple threads both
						 * trying to use this
						 * CREW_Lock first */
	if (*clPtr == NULL) {
	    Lock *lockPtr = (Lock *)Tcl_Alloc((int) sizeof(Lock));
	    lockPtr->mutex = NULL;
	    lockPtr->wakeReaders = NULL;
	    lockPtr->wakeWriters = NULL;
	    lockPtr->readWaiters = 0;
	    lockPtr->writeWaiters = 0;
	    lockPtr->holders = 0;
	    *clPtr = (CREW_Lock) lockPtr;
	}
	Tcl_MutexUnlock(&crewMasterMutex);
    }
    return (Lock *) *clPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * CREW_Finalize --
 *
 *      Frees up resources of a CREW_Lock when it will not be used anymore.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees memory.
 *
 *---------------------------------------------------------------------------
 */

void
CREW_Finalize(CREW_Lock *clPtr) {
    Lock *lockPtr = (Lock *) *clPtr; /* GetLock() call might be safer */
    if (lockPtr != NULL) {
	/* Do we need to clear waiters? */
    	Tcl_ConditionFinalize(&lockPtr->wakeReaders);
    	Tcl_ConditionFinalize(&lockPtr->wakeWriters);
    	Tcl_MutexFinalize(&lockPtr->mutex);
    	Tcl_Free((char *)lockPtr);
    	*clPtr = NULL;
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * CREW_ReadHold --
 *
 *      Called when read access to the locked resource is desired.  Will
 *      block until read access is granted according to CREW rules.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------------
 */

void
CREW_ReadHold(CREW_Lock *clPtr) {
    Lock *lockPtr = GetLock(clPtr);
    Tcl_MutexLock(&lockPtr->mutex);
    while (lockPtr->holders < 0 		/* something is writing */
	    || lockPtr->writeWaiters > 0	/* or waiting to write */
    ) {
	lockPtr->readWaiters++;
	Tcl_ConditionWait(&lockPtr->wakeReaders, &lockPtr->mutex, NULL);
	lockPtr->readWaiters--;
    }
    lockPtr->holders++;	/* no longer writers in the way; claim a read hold */
    Tcl_MutexUnlock(&lockPtr->mutex);
}

/*
 *---------------------------------------------------------------------------
 *
 * CREW_WriteHold --
 *
 *      Called when write access to the locked resource is desired.  Will
 *      block until write access is granted according to CREW rules.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------------
 */

void
CREW_WriteHold(CREW_Lock *clPtr) {
    Lock *lockPtr = GetLock(clPtr);
    Tcl_MutexLock(&lockPtr->mutex);
    while (lockPtr->holders != 0) {	/* something is reading */
	lockPtr->writeWaiters++;
	Tcl_ConditionWait(&lockPtr->wakeWriters, &lockPtr->mutex, NULL);
	lockPtr->writeWaiters--;
    }
    lockPtr->holders = -1;	/* no more readers; claim a write hold */
    Tcl_MutexUnlock(&lockPtr->mutex);
}


/*
 *---------------------------------------------------------------------------
 *
 * CREW_Release --
 *
 *      Release a hold on the lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May wake a blocked thread waiting for the lock.
 *
 *---------------------------------------------------------------------------
 */

void
CREW_Release(CREW_Lock *clPtr) {
    Lock *lockPtr = (Lock *) *clPtr;	/* GetLock() call might be safer */
    if (NULL == lockPtr) {
	/* Release a never-used lock -> no-op */
	return;
    }
    Tcl_MutexLock(&lockPtr->mutex);
    if (--lockPtr->holders < 0) {	
	lockPtr->holders = 0;		/* a write hold was in effect;
					 * release it */
    }

    /* writers waiting and holders == 0 -> let a writer in */
    if (lockPtr->writeWaiters) {
	if (lockPtr->holders == 0) {
	    Tcl_ConditionNotify(&lockPtr->wakeWriters);
	}

    /* no writers waiting and readers waiting -> let a reader in */
    } else if (lockPtr->readWaiters) {
	Tcl_ConditionNotify(&lockPtr->wakeReaders);
    }
    Tcl_MutexUnlock(&lockPtr->mutex);
}
