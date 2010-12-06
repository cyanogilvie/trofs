/*
 * trofs.c --
 *
 * Tcl Read-Only File System.
 *
 */

#include "trofs.h"
#include "crew.h"
#if (TCL_MINOR_VERSION == 4)
#include "tclDict.h"
#endif
#include <sys/types.h>	/* mode_t typedef */
/*
 * Fill in the mode_t typedef on Windows, where no system header
 * defines it.
 */
#ifdef WIN32
typedef unsigned short mode_t;
#endif

#include <sys/stat.h>	/* S_I* values and macros for mode_t processing */
/*
 * Fill in some mode_t related macros that we use, in case
 * they're missing from the system header file, sys/stat.h .
 */
#ifndef S_IFLNK
#   define S_IFLNK	0120000  /* Symbolic Link */
#endif
#ifndef S_IRUSR
#   define S_IRUSR	0400	/* User read permission */
#endif
#ifndef S_IRGRP
#   define S_IRGRP	0040	/* Group read permission */
#endif
#ifndef S_IROTH
#   define S_IROTH	0004	/* Other read permission */
#endif
#ifndef S_ISREG
#   define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif /* !S_ISREG */
#ifndef S_ISDIR
#   define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif /* !S_ISDIR */

#include <stdio.h>	/* SEEK_* values for Tcl_*Seek() */
#include <string.h>	/* strchr(), memcmp(), strcmp(), memcpy() */
#include <errno.h>	/* E* values for Tcl_SetErrno() */
/*
 * Fill in some error codes we use, in case they're missing from the
 * system header file, errno.h .
 */
#ifndef ELOOP
#   define ELOOP	90	/* Symbolic link loop */
#endif
#ifndef ESTALE
#   define ESTALE	151	/* Stale NFS file handle */
#endif

#include <fcntl.h>	/* O_* values for Tcl_FSOpenFileChannelProc() */
#ifndef O_ACCMODE
#   define O_ACCMODE	(O_RDONLY | O_WRONLY | O_RDWR)
#endif

#ifndef WIN32
#  include <unistd.h>	/* *_OK values for Tcl_FSAccess() */
#endif
/*
 * Fill in the *_OK values, in case no system header file provided them.
 */
#ifndef F_OK
#    define F_OK 00
#endif
#ifndef X_OK
#    define X_OK 01
#endif
#ifndef W_OK
#    define W_OK 02
#endif
#ifndef R_OK
#    define R_OK 04
#endif

/*
 * The following data structure represents a mount in the active
 * filesystem managed by the "trofs" Tcl_Filesystem.  Each instance
 * of this data structure is a value accessible to all threads in
 * the process, so no (Tcl_Obj *)'s, (Tcl_Interp *)'s, or Tcl_Channel's
 * are stored in it.
 */
typedef struct Mount {
    int			refCount;	/* Number of TrofsPath's, Mount's,
					 * and trofsMountList that hold a
					 * pointer to this structure.  This
					 * count is managed by
					 * MountPreserve() and MountRelease()
					 * with serialization via
					 * trofsMountMutex.  When this count
					 * falls to 0, MountRelease() will
					 * free the structure */
    const char		*archivePath;   /* The normalized path to the
					 * archive file that holds the trofs
					 * archive containing the contents
					 * of this mount */
    int			archiveLen;	/* Number of bytes in archivePath */
    const char		*path;		/* The normalized path of the
					 * mountpoint at which this mount
					 * appears in the active filesystem */
    int			pathLen;	/* Number of bytes in path */
    Tcl_WideInt		mtime;		/* "Last modified" time of the
					 * archive file at time of mounting;
					 * used to detect if the archive
					 * changes after mounting */
    Tcl_WideInt		indexOffset;	/* The seek offset within the
					 * archive file where the index of
					 * the root directory of this mount
					 * is found */
    int			indexSize;	/* Number of bytes in the
					 * root directory index */
    struct Mount	*next;		/* Next mount in the trofsMountList */
} Mount;

/*
 * The following data structure is the "native" representation of a Tcl_Obj
 * of the "path" type that is claimed by the trofs filesystem.  Each instance
 * of this data structure is associated with a Tcl_Obj; thus, with a
 * single thread.
 */
typedef struct TrofsPath {
    int		refCount;	/* # Tcl_Obj's holding this value */
    Mount	*mountPtr;	/* The trofs mount which claims this path */
    Tcl_Obj	*parent;	/* The parent directory of this path.  This
				 * is a directory under the same mount.  If
				 * NULL, then this path is the root directory
				 * of the mount. */
    Tcl_Obj	*tail;		/* If parent != NULL, this is the [file tail]
				 * part of the path.  Join it to the
				 * normalized parent path to produce normalized
				 * form of this path.   If parent == NULL, then
				 * this should also be NULL, and normalized
				 * form of this path is mountPtr->path . */
    mode_t	mode;		/* What type of file does this path
				 * represent.  Either 0 for non-existent
				 * file, or S_IFLNK for a symbolic link,
				 * S_IFREG for a regular file, or S_IFDIR for
				 * a directory */
    Tcl_WideInt	offset;		/* The seek offset within the archive file
				 * where the contents of this path are found */
    Tcl_WideInt	size;		/* Number of bytes within the archive that
				 * are the contents for this path */
    Tcl_Obj	*value;		/* The "value" of the path.  Interpretation
				 * according to mode.  For S_IFLNK, it's
				 * a string holding the value of the link.
				 * For S_IFDIR, it's a dict holding the
				 * contents index of the directory.  */
} TrofsPath;

/*
 * The following data structure is the clientdata for a trofs file channel.
 * Each instance of this data structure is associated with a Tcl_Channel; thus,
 * with a single thread.
 */
typedef struct TrofsChannel {
    Tcl_Obj		*pathPtr;	/* The "path" Tcl_Obj that was opened
					 * to create this channel.  We keep
					 * this so we can detect whether the
					 * path continues to refer to the
					 * same bytes in the same archive as
					 * we do channel operations.  It's
					 * possible that after opening the
					 * channel, subsequent mounts/unmounts
					 * cause this path to refer to some
					 * other file(system).  In that case,
					 * we want channel operations to
					 * fail noisily, rather than mislead. */
    Tcl_WideInt		mtime;		/* The "last modified" tine of the
					 * archive at the time the channel was
					 * opened.  Used to detect changes to
					 * the underlying archive or changes
					 * in the mounts, again to noisily
					 * fail when the path we opened no
					 * longer refers to the same thing
					 * as when we opened it */
    Tcl_Channel		channel;	/* Backreference to the Tcl_Channel
					 * holding this as ClientData - used
					 * to get the stacked channel */
    Tcl_WideInt		startOffset;	/* Seek offset within the archive
					 * where the contents of the opened
					 * file begin */
    Tcl_WideInt		endOffset;	/* Seek offset within the archive
					 * where the contents of the opened
					 * file end */
    Tcl_WideInt		leftToRead;	/* Number of bytes we have yet to
					 * read from the opened file.  This
					 * is how we keep track of the
					 * current access position of the
					 * opened file. */
} TrofsChannel;

/*
 * Command procedure declarations for the trofs package
 */

static Tcl_ObjCmdProc			MountObjCmd;
static Tcl_ObjCmdProc			UnmountObjCmd;

/*
 * Procedure declarations for component functions of the trofs Tcl_Filesystem 
 */

static Tcl_FSPathInFilesystemProc	PathInFilesystem;
static Tcl_FSDupInternalRepProc		DupInternalRep;
static Tcl_FSFreeInternalRepProc	FreeInternalRep;
static Tcl_FSInternalToNormalizedProc	InternalToNormalized;
static Tcl_FSNormalizePathProc		NormalizePath;
static Tcl_FSFilesystemSeparatorProc	FilesystemSeparator;
static Tcl_FSStatProc			Stat;
static Tcl_FSAccessProc			Access;
static Tcl_FSOpenFileChannelProc	OpenFileChannel;
static Tcl_FSMatchInDirectoryProc	MatchInDirectory;
static Tcl_FSUtimeProc			Utime;
static Tcl_FSLinkProc			Link;
static Tcl_FSListVolumesProc		ListVolumes;
static Tcl_FSFileAttrStringsProc	FileAttrStrings;
static Tcl_FSFileAttrsGetProc		FileAttrsGet;
static Tcl_FSFileAttrsSetProc		FileAttrsSet;
static Tcl_FSLstatProc			Lstat;

/*
 * Procedure declarations for component functions of the trofs Tcl_ChannelType
 */

static Tcl_DriverCloseProc		DriverClose;
static Tcl_DriverInputProc		DriverInput;
static Tcl_DriverOutputProc		DriverOutput;	
static Tcl_DriverSeekProc		DriverSeek;
static Tcl_DriverSetOptionProc		DriverSetOption;
static Tcl_DriverGetOptionProc		DriverGetOption;
static Tcl_DriverWatchProc		DriverWatch;
static Tcl_DriverBlockModeProc		DriverBlockMode;
static Tcl_DriverHandlerProc		DriverHandler;
static Tcl_DriverWideSeekProc		DriverWideSeek;

/*
 * Other procedures used only in this file
 */

static TrofsPath *	CacheRootDirOfMountPoint(Mount *mountPtr);
static void		ClearHash(Tcl_HashTable *tablePtr);
static void		ExitHandler(ClientData clientData);
static void		FailMessage(Tcl_Interp *interp, const char *prefix,
				Tcl_Obj *pathPtr, const char *suffix);
static Tcl_Obj *	FetchDirFromCache(Tcl_Obj *pathPtr,
				TrofsPath **tpPtrPtr);
static TrofsPath *	FetchTrofsPath(Tcl_Obj *pathPtr);
static void		FreeThreadHash(ClientData clientData);
static Tcl_Obj *	GetCurrentDirCache();
static void		GetDirIndexFromArchive(TrofsPath *tpPtr);
static Tcl_HashTable *	GetThreadHash(Tcl_ThreadDataKey *keyPtr);
static void		MountPreserve(Mount *mountPtr);
static void		MountRelease(Mount *mountPtr);
static void		StoreDirInCache(Tcl_Obj *pathPtr, TrofsPath *tpPtr);
static void		TranslateOpenMode(int mode,
				Tcl_DString *modeStringPtr);
static int		Unmount(const char *path, int numBytes);
static int		VerifyTrofsArchive(Tcl_Interp *interp,
				Tcl_Obj *archive, Tcl_Channel chan,
				Tcl_WideInt *offsetPtr, int *indexSizePtr);

/*
 * The trofs Tcl_Filesystem
 */

static Tcl_Filesystem trofsFilesystem = {
    PACKAGE_NAME,
    sizeof(Tcl_Filesystem),
    TCL_FILESYSTEM_VERSION_1,
    &PathInFilesystem,
    &DupInternalRep,
    &FreeInternalRep,
    &InternalToNormalized,
    NULL,			/* &CreateInternalRep, */
    &NormalizePath,
    NULL,			/* &FilesystemPathType, */
    &FilesystemSeparator,
    &Stat,
    &Access,
    &OpenFileChannel,
    &MatchInDirectory,
    &Utime,
    &Link,
    &ListVolumes,
    &FileAttrStrings,
    &FileAttrsGet,
    &FileAttrsSet,
    NULL,			/* &CreateDirectory, */
    NULL,			/* &RemoveDirectory, */
    NULL,			/* &DeleteFile, */
    NULL,			/* &CopyFile, */
    NULL,			/* &RenameFile, */
    NULL,			/* &CopyDirectory, */
    &Lstat,
    NULL,			/* &LoadFile, */
    NULL,			/* &GetCwd, */
    NULL			/* &Chdir */
};

/*
 * The trofs Tcl_ChannelType
 */

static Tcl_ChannelType trofsFileChannel = {
    PACKAGE_NAME,
    TCL_CHANNEL_VERSION_3,
    &DriverClose,
    &DriverInput,
    &DriverOutput,
    &DriverSeek,
    &DriverSetOption,
    &DriverGetOption,
    &DriverWatch,
    NULL,		/* &DriverGetHandle, */
    NULL,		/* &DriverClose2, */
    &DriverBlockMode,
    NULL,		/* &DriverFlush, */
    &DriverHandler,
    &DriverWideSeek,
};

/*
 * Mutex to serialize operations on Mount.refCount values by
 * MountPreserve() and MountRelease()
 */
TCL_DECLARE_MUTEX(trofsMountMutex)

/*
 * trofsMountList is the list of trofs mounts in the active filesystem.
 * Each time the contents of the list changes, trofsMountEpoch is
 * incremented, so it can be quickly checked whether the mounts have
 * changed since something was derived from them.  Access to this
 * list is governed by Concurrent Read / Exclusive Write rules,
 * enforced by the trofsMountLock.
 */
static int trofsMountEpoch = 0;
static Mount *trofsMountList = NULL;
CREW_Lock trofsMountLock;

/*
 * trofsGenerator is a serial number used in MountObjCmd()
 * to generate new mountpoints.
 * Access to it is serialized by trofsGenerator Mutex.  
 */
static int trofsGenerator = 0;
TCL_DECLARE_MUTEX(trofsGeneratorMutex)

#if (TCL_MINOR_VERSION >= 5)
/*
 *  Embedded package configuration data to pass to Tcl_RegisterConfig()
 */
static Tcl_Config trofsConfig [] = {
  {"scriptdir,runtime", TROFS_RUNTIME_SCRDIR},
  {"scriptdir,source",  TROFS_SOURCE_SCRDIR},
  {NULL, NULL}
};
#endif

/*
 *----------------------------------------------------------------------
 *
 * MountPreserve --
 *	Called when a mountPtr will be held, and the struct it
 *	refers to should not be allowed to go away.
 *
 * Side effects:
 *	May block waiting for serialized access to the refCount
 *
 *----------------------------------------------------------------------
 */

static void
MountPreserve(Mount *mountPtr) {
    Tcl_MutexLock(&trofsMountMutex);
    mountPtr->refCount++;
    Tcl_MutexUnlock(&trofsMountMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * MountRelease --
 *	Called when a mountPtr will no longer be held, so the caller
 *	no longer cares if the struct it refers to goes away.
 *
 * Side effects:
 *	May block waiting for serialized access to the refCount.
 *	Frees mountPtr when the last interest in it is released.
 *
 *----------------------------------------------------------------------
 */

static void
MountRelease(Mount *mountPtr) {
    int free;
    Tcl_MutexLock(&trofsMountMutex);
    free = (--mountPtr->refCount == 0);
    Tcl_MutexUnlock(&trofsMountMutex);
    if (free) {
	Tcl_Free((char *)mountPtr->archivePath);
	Tcl_Free((char *)mountPtr->path);
	Tcl_Free((char *) mountPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GetThreadHash --
 *
 *      Get a thread-specific (Tcl_HashTable *) associated with a
 *      thread data key.
 *
 * Results:
 *      The Tcl_HashTable * corresponding to *keyPtr.  
 *
 * Side effects:
 *      The first call on a keyPtr in each thread creates a new
 *      Tcl_HashTable, and registers a thread exit handler to
 *      dispose of it.
 *
 *----------------------------------------------------------------------
 */

static Tcl_HashTable *
GetThreadHash(keyPtr)
    Tcl_ThreadDataKey *keyPtr;
{
    Tcl_HashTable **tablePtrPtr = (Tcl_HashTable **)
	    Tcl_GetThreadData(keyPtr, (int)sizeof(Tcl_HashTable *));
    if (NULL == *tablePtrPtr) {
	*tablePtrPtr = (Tcl_HashTable *)ckalloc(sizeof(Tcl_HashTable));
	Tcl_CreateThreadExitHandler(FreeThreadHash, (ClientData)*tablePtrPtr);
	Tcl_InitObjHashTable(*tablePtrPtr);
    }
    return *tablePtrPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeThreadHash --
 *      Thread exit handler used by GetThreadHash to dispose
 *      of a thread hash table.
 *
 * Side effects:
 *      Frees a Tcl_HashTable.
 *
 *----------------------------------------------------------------------
 */

static void
FreeThreadHash(clientData)
    ClientData clientData;
{
    Tcl_HashTable *tablePtr = (Tcl_HashTable *) clientData;
    ClearHash(tablePtr);
    Tcl_DeleteHashTable(tablePtr);
    ckfree((char *) tablePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * ClearHash --
 *      Remove all the entries in the hash table *tablePtr.
 *
 *----------------------------------------------------------------------
 */

static void
ClearHash(tablePtr)
    Tcl_HashTable *tablePtr;
{
    Tcl_HashSearch search;
    Tcl_HashEntry *hPtr;

    for (hPtr = Tcl_FirstHashEntry(tablePtr, &search); hPtr != NULL;
	    hPtr = Tcl_NextHashEntry(&search)) {
	Tcl_Obj *objPtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
	Tcl_DecrRefCount(objPtr);
	Tcl_DeleteHashEntry(hPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * StoreDirInCache --
 *
 * 	To speed the searches performed by PathInFilesystem, once we
 * 	determine that a path is a directory in the trofs filesystem,
 * 	we record that path and its TrofsPath "native" representation
 * 	in a cache for quick subsequent fetching, rather than linear
 * 	mount list searching followed by TrofsPath construction.
 *
 * 	This routine stores directory path *pathPtr and its native
 * 	representation *tpPtr in the current cache.
 *
 * 	The caller should hold a read lock on trofsMountLock when
 * 	calling this routine.
 *
 *----------------------------------------------------------------------
 */
static void
StoreDirInCache(Tcl_Obj *pathPtr, TrofsPath *tpPtr) {
    Tcl_DictObjPut(NULL, GetCurrentDirCache(), pathPtr,
	    Tcl_FSNewNativePath(&trofsFilesystem, 
	    DupInternalRep((ClientData)tpPtr)));
}

/*
 *----------------------------------------------------------------------
 *
 * FetchDirFromCache --
 *
 * 	Check whether pathPtr is in the cache, and if so, record a
 * 	pointer to its native representation at *tpPtrPtr.
 *
 * 	The caller should hold a read lock on trofsMountLock when
 * 	calling this routine.
 *
 * Results:
 *	Returns NULL if pathPtr is not in the cache.  Otherwise,
 *	returns a (Tcl_Obj *) pointing to the cached value associated
 *	with pathPtr, and write the native rep of the cached value
 *	to *tpPtrPtr.  Note that while pathPtr and the returned
 *	(Tcl_Obj *) have the same string rep, pathPtr might be of
 *	any Tcl_ObjType, while the returned value is of the "path"
 *	Tcl_ObjType, with a trofs native representation.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
FetchDirFromCache(Tcl_Obj *pathPtr, TrofsPath **tpPtrPtr) {
    Tcl_Obj *dirPathPtr = NULL;
    Tcl_Obj *dictPtr = GetCurrentDirCache();

    Tcl_DictObjGet(NULL, dictPtr, pathPtr, &dirPathPtr);
    if (NULL != dirPathPtr) {
	TrofsPath *tpPtr = FetchTrofsPath(dirPathPtr);
	if (NULL == tpPtr) {
	    Tcl_DictObjRemove(NULL, dictPtr, pathPtr);
	    return NULL;
	}
	*tpPtrPtr = tpPtr;
    }
    return dirPathPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * GetCurrentDirCache --
 *
 * 	Keeps the directory cache current with changes to the
 * 	mount list.  Called only by StoreDirInCache() and
 * 	FetchDirFromCache().
 *
 * Results:
 *	Returns a "dict" (Tcl_Obj *) that contains the directory
 *	cache appropriate for the current trofsMountEpoch.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
GetCurrentDirCache() {
    static Tcl_ThreadDataKey trofsDirectoryCacheKey;
    Tcl_Obj *currentEpoch, *directoryCache;
    Tcl_HashTable *cacheMap;
    Tcl_HashEntry *hPtr;

    currentEpoch = Tcl_NewIntObj(trofsMountEpoch);
    Tcl_IncrRefCount(currentEpoch);
    cacheMap = GetThreadHash(&trofsDirectoryCacheKey);
    hPtr = Tcl_FindHashEntry(cacheMap, (char *) currentEpoch);
    if (NULL == hPtr) {
	int dummy;
	Mount *mountPtr;

	/* No cache for the current epoch - must be a new one */
	/* First, clear the cacheMap, as anything in it must
	 * refer to some expired epoch.*/
	ClearHash(cacheMap);
	/* Next, create an empty directory cache for this epoch... */
	directoryCache = Tcl_NewDictObj();
	/* ...and store it in the cache map */
	hPtr = Tcl_CreateHashEntry(cacheMap, (char *) currentEpoch, &dummy);
	Tcl_SetHashValue(hPtr, (ClientData) directoryCache);
	Tcl_IncrRefCount(directoryCache);
	/* Then fill it with the mount points */
	for (mountPtr = trofsMountList; mountPtr != NULL;
		mountPtr = mountPtr->next) {
	    CacheRootDirOfMountPoint(mountPtr);
	}
    }
    Tcl_DecrRefCount(currentEpoch);
    return (Tcl_Obj *) Tcl_GetHashValue(hPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * FetchTrofsPath --
 *
 *	Pull the trofs "native" path representation out of
 *	a "path" Tcl_Obj.  pathPtr should already be a "path" Tcl_Obj
 *	that the Tcl VFS core thinks belongs to the trofs filesystem.
 *
 *	Callers call this rather than Tcl_FSGetInternalRep() directly,
 *	because it can detect that pathPtr's internal rep is "stale"
 *	due to changing mounts.
 *
 * Results:
 * 	Returns a pointer to the TrofsPath representation, or NULL
 * 	to indicate a stale pathPtr error.
 *
 * Side effects:
 *	Sets errno to ESTALE on error.
 *
 *----------------------------------------------------------------------
 */

static TrofsPath *
FetchTrofsPath(Tcl_Obj *pathPtr) {
    TrofsPath *tpPtr = (TrofsPath *)
	    Tcl_FSGetInternalRep(pathPtr, &trofsFilesystem);	
    if ((NULL == tpPtr)
	/* Shouldn't happen - apparently we told the core the path is ours,
	 * but now we don't have any internal rep for it. */
	    || (0 == tpPtr->mountPtr->mtime)
	/* The backing archive for this path is invalid. */
	    ) {
	/* In either case, things are not well.  The cached "native"
	 * rep of the path no longer reflects the state of the archive.
	 * Nearest POSIX error appears to be "stale file handle" */
	Tcl_SetErrno(ESTALE);
	return NULL;
    }
    return tpPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * GetDirIndexFromArchive --
 *
 * 	Stores in tpPtr->value the "dict" (Tcl_Obj *) that holds the
 * 	index of the directory whose "native" TrofsPath is *tpPtr.
 * 	The index data is read from the archive file.  If unable to
 * 	get the index data, store NULL in tpPtr->value.
 *
 * 	The caller should hold a read lock on trofsMountLock when
 * 	calling this routine.
 *
 *----------------------------------------------------------------------
 */

static void
GetDirIndexFromArchive(TrofsPath *tpPtr) {
    Tcl_StatBuf stat;
    Tcl_Obj *archivePtr = NULL;
    Tcl_Channel archiveChan = NULL;
    Mount *mountPtr = tpPtr->mountPtr;
    int numBytes, toRead = (int) tpPtr->size;
    const char *bytes;
    Tcl_Encoding encoding;
    Tcl_DString ds;

    /* TODO: devise way to cache the archive channel per-thread */
    tpPtr->mode = 0;
    tpPtr->value = NULL;
    if (0 == mountPtr->mtime) {
	/* Archive already known invalid */
	goto done;
    }
    archivePtr = Tcl_NewStringObj(mountPtr->archivePath, mountPtr->archiveLen);
    Tcl_IncrRefCount(archivePtr);
    if (-1 == Tcl_FSStat(archivePtr, &stat)) {
	goto done;
    }
    if (stat.st_mtime != mountPtr->mtime) {
	goto done;
    }
    archiveChan = Tcl_FSOpenFileChannel(NULL, archivePtr, "r", 0);
    if (NULL == archiveChan) {
	goto done;
    }
    if (-1 == Tcl_Seek(archiveChan, tpPtr->offset, SEEK_SET)) {
	goto done;
    }
    /* read bytes */
    Tcl_SetChannelOption(NULL, archiveChan, "-translation", "binary");
    tpPtr->value = Tcl_NewObj();
    Tcl_IncrRefCount(tpPtr->value);
    /* Assume a blocking channel */
    if (toRead != Tcl_ReadChars(archiveChan, tpPtr->value, toRead, 1)) {
	Tcl_DecrRefCount(tpPtr->value);
	tpPtr->value = NULL;
	goto done;
    }
    /* TODO: Simpler way to convert bytes in a ByteArray to the utf-8
     * encoded string they represent? */
    bytes = (char *)Tcl_GetByteArrayFromObj(tpPtr->value, &numBytes);
    encoding = Tcl_GetEncoding(NULL, "utf-8");
    Tcl_ExternalToUtfDString(encoding, bytes, numBytes, &ds);
    Tcl_DecrRefCount(tpPtr->value);
    tpPtr->value = Tcl_NewStringObj(
	    Tcl_DStringValue(&ds), Tcl_DStringLength(&ds));
    Tcl_IncrRefCount(tpPtr->value);
    if (TCL_ERROR == Tcl_DictObjSize(NULL, tpPtr->value, &numBytes)) {
	Tcl_DecrRefCount(tpPtr->value);
	tpPtr->value = NULL;
    } else {
	tpPtr->mode = S_IFDIR;
    }
done:
    if (tpPtr->mode != S_IFDIR) {
	mountPtr->mtime = 0;	/* Mark archive as invalid */
    }
    if (NULL != archiveChan) {
	Tcl_Close(NULL, archiveChan);
    }
    if (archivePtr) {
        Tcl_DecrRefCount(archivePtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CacheRootDirOfMountPoint --
 *
 * 	Utility routine to make sure the root directory of a mountpoint
 * 	is stored in the cache of directories claimed for the trofs
 * 	filesystem.
 *
 * Results:
 * 	The "native" path representation of the root dir of the
 * 	mountpoint.
 *
 * Side effects:
 * 	May create the "native" path rep, and store it in the
 * 	directory cache.
 *
 *----------------------------------------------------------------------
 */

static TrofsPath *
CacheRootDirOfMountPoint(Mount *mountPtr) {
    Tcl_Obj *rootDir = Tcl_NewStringObj(mountPtr->path, mountPtr->pathLen);
    TrofsPath *newPtr;

    Tcl_IncrRefCount(rootDir);
    if (NULL == FetchDirFromCache(rootDir, &newPtr)) {
	newPtr = (TrofsPath *)Tcl_Alloc((int)sizeof(TrofsPath));
	newPtr->refCount = 0;
	newPtr->mountPtr = mountPtr;
	MountPreserve(newPtr->mountPtr);
	newPtr->parent = NULL;
	newPtr->tail = NULL;
	newPtr->size = mountPtr->indexSize;
	newPtr->offset = mountPtr->indexOffset;
	GetDirIndexFromArchive(newPtr);
	/* TODO: decide whether non-dirs should also be cached */
	if (S_IFDIR == newPtr->mode) {
	    StoreDirInCache(rootDir, newPtr);
	}
    }
    Tcl_DecrRefCount(rootDir);
    return newPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * PathInFilesystem --
 *
 * 	Part of the "trofs" Tcl_Filesystem.
 * 	Determine whether pathPtr is in an active trofs mount.
 *
 * Results:
 * 	Returns TCL_OK and writes the TrofsPath representation of
 * 	pathPtr at *clientDataPtr when pathPtr is a trofs path.
 * 	Returns -1 otherwise.
 *
 * Side effects:
 * 	The TrofsPath representation may be allocated, and stored
 * 	in a cache.
 *
 *----------------------------------------------------------------------
 */

static int
PathInFilesystem(Tcl_Obj *pathPtr, ClientData *clientDataPtr) {
    int elemCount, result = -1;
    int checkMounts = 1;
    Tcl_Obj *normPathPtr, *pathElements, *parentPathPtr = NULL;
    Tcl_Obj *tail = NULL;
    Tcl_Obj *value = NULL;
    TrofsPath *newPtr, *tpPtr = NULL;

    /* No mount changes while we track down this path */
    CREW_ReadHold(&trofsMountLock);
    normPathPtr = Tcl_FSGetNormalizedPath(NULL, pathPtr);
    if (NULL == normPathPtr) {
	/* Should not happen I think */
	goto done;
    }
    Tcl_IncrRefCount(normPathPtr);
    if (FetchDirFromCache(normPathPtr, &tpPtr)) {
	/* Internal rep for the path is already in our cache
	 * of directory paths we own.  Claim it. */
	*clientDataPtr = DupInternalRep((ClientData) tpPtr);
	result = TCL_OK;
	goto done;
    }

    /* Path is not a directory in our cache.  Create its parent path. */
    pathElements = Tcl_FSSplitPath(normPathPtr, &elemCount);
    Tcl_IncrRefCount(pathElements);
    if (elemCount > 1) {
	Tcl_Obj *temp;
	parentPathPtr = Tcl_FSJoinPath(pathElements, elemCount - 1);
	Tcl_IncrRefCount(parentPathPtr);
	Tcl_ListObjIndex(NULL, pathElements, elemCount - 1, &tail);
	Tcl_IncrRefCount(tail);

	if ((temp = FetchDirFromCache(parentPathPtr, &tpPtr))) {
	    /* We have cached data on the parent dir, so no need
	     * to scan the mount points */
	    Tcl_IncrRefCount(temp);
	    Tcl_DecrRefCount(parentPathPtr);
	    parentPathPtr = temp;
	    checkMounts = 0;
	}
    }
    Tcl_DecrRefCount(pathElements);

    /* TODO: Consider rejection caches as well? */

    if (checkMounts) {
	/* Neither path nor parent is known to us (yet) 
	 * Check if the path has a mount as a prefix. */
	Mount *mountPtr;
	int normPathLen;
	const char *normPath = Tcl_GetStringFromObj(normPathPtr, &normPathLen);

	for (mountPtr = trofsMountList; mountPtr != NULL;
		mountPtr = mountPtr->next) {
	    if (normPathLen < mountPtr->pathLen) continue;
	    if (0 == memcmp(mountPtr->path, normPath,
		    (size_t)mountPtr->pathLen)) {
		break;
	    }
	}
	if (NULL == mountPtr) {
	    /* None of our mounts is a prefix.  Reject it. */
	    goto done;
	}
	if (normPathLen == mountPtr->pathLen) {
	    /* The pathPtr is one of our mount points.  Claim it. */
	    newPtr = CacheRootDirOfMountPoint(mountPtr);
	    *clientDataPtr = DupInternalRep((ClientData) newPtr);
	    result = TCL_OK;
	    goto done;
	}
    }

    if (NULL == tpPtr) {
	TrofsPath **tpPtrPtr = &tpPtr;
	
	/* Dispose of weird corner case, if it arises.
	 * This is probably impossible. */
	if (NULL == parentPathPtr) {
	    goto done;
	}
	/* Parent path was not in our cache.  Try a recursive call,
	 * to place it there, or determine the parent path isn't
	 * in this filesystem. */
	if (-1 == PathInFilesystem(parentPathPtr, (ClientData *)tpPtrPtr)) {
	    /* Parent path is not in this filesystem; thus, neither is
	     * the original path.  Reject it.*/
	    goto done;
	}
    }
    if (S_IFDIR != tpPtr->mode) {
	/* Parent path is in this filesystem, but not as a directory!
	 * This means the original path can only be a path inside
	 * an archive, apparently an archive governed by a different
	 * filesystem. Reject it. */
	goto done;
    }

    /* Parent path is directory in this filesystem.  Thus original path
     * is in this filesystem too.  Claim it. */
    newPtr = (TrofsPath *)Tcl_Alloc((int)sizeof(TrofsPath));
    newPtr->refCount = 1;

    /* path has same Mount as parent; ref count it. */
    newPtr->mountPtr = tpPtr->mountPtr;
    MountPreserve(newPtr->mountPtr);

    newPtr->parent = parentPathPtr;
    Tcl_IncrRefCount(newPtr->parent);

    newPtr->tail = tail;
    Tcl_IncrRefCount(newPtr->tail);

    Tcl_DictObjGet(NULL, tpPtr->value, newPtr->tail, &value);
    if (NULL == value) {
	/* Path to non-existent file */
	newPtr->mode = 0;
	newPtr->size = 0;
	newPtr->offset = 0;
	newPtr->value = NULL;
    } else {
	char typeFlag;
	Tcl_Obj *type;

	Tcl_IncrRefCount(value);
	Tcl_ListObjIndex(NULL, value, 0, &type);
	Tcl_IncrRefCount(type);
	typeFlag = Tcl_GetString(type)[0];
	Tcl_DecrRefCount(type);
	switch (typeFlag) {
	    case 'L':
		newPtr->mode = S_IFLNK;
		newPtr->size = 0;
		newPtr->offset = 0;
		Tcl_ListObjIndex(NULL, value, 1, &newPtr->value);
		Tcl_IncrRefCount(newPtr->value);
		break;
	    case 'F':
		newPtr->mode = S_IFREG;

		/* Cheating! Use value field as temp space */
		Tcl_ListObjIndex(NULL, value, 1, &newPtr->value);
		Tcl_IncrRefCount(newPtr->value);
		Tcl_GetWideIntFromObj(NULL, newPtr->value, &newPtr->size);
		Tcl_DecrRefCount(newPtr->value);

		Tcl_ListObjIndex(NULL, value, 2, &newPtr->value);
		Tcl_IncrRefCount(newPtr->value);
		Tcl_GetWideIntFromObj(NULL, newPtr->value, &newPtr->offset);
		Tcl_DecrRefCount(newPtr->value);
		newPtr->offset = tpPtr->offset - newPtr->offset;

		newPtr->value = NULL;
		break;
	    case 'D':

		/* Cheating! Use value field as temp space */
		Tcl_ListObjIndex(NULL, value, 1, &newPtr->value);
		Tcl_IncrRefCount(newPtr->value);
		Tcl_GetWideIntFromObj(NULL, newPtr->value, &newPtr->size);
		Tcl_DecrRefCount(newPtr->value);

		Tcl_ListObjIndex(NULL, value, 2, &newPtr->value);
		Tcl_IncrRefCount(newPtr->value);
		Tcl_GetWideIntFromObj(NULL, newPtr->value, &newPtr->offset);
		Tcl_DecrRefCount(newPtr->value);
		newPtr->offset = tpPtr->offset - newPtr->offset;

		GetDirIndexFromArchive(newPtr);
		if (NULL == newPtr->value) {
		    FreeInternalRep((ClientData) newPtr);
		    goto done;
		}
		StoreDirInCache(normPathPtr, newPtr);
	}
	Tcl_DecrRefCount(value);
    }
    *clientDataPtr = (ClientData) newPtr;
    result = TCL_OK;

done:
    if (NULL != normPathPtr) {
	Tcl_DecrRefCount(normPathPtr);
    }
    if (NULL != parentPathPtr) {
	Tcl_DecrRefCount(parentPathPtr);
    }
    if (NULL != tail) {
	Tcl_DecrRefCount(tail);
    }
    CREW_Release(&trofsMountLock);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DupInternalRep --
 *
 * 	Part of the "trofs" Tcl_Filesystem.
 * 	Duplicate the TrofsPath	"native" rep of a path.
 *
 * Results:
 * 	Returns clientData, with refcount incremented.
 *
 *----------------------------------------------------------------------
 */

static ClientData
DupInternalRep(ClientData clientData) {
    TrofsPath *tpPtr = (TrofsPath *) clientData;
    tpPtr->refCount++;
    return (ClientData)tpPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeInternalRep --
 *
 * 	Part of the "trofs" Tcl_Filesystem.
 * 	Free one reference to the TrofsPath "native" rep of a path.
 * 	When all references gone, free the struct.
 *
 * Side effects:
 * 	May free memory.
 *
 *----------------------------------------------------------------------
 */

static void
FreeInternalRep(ClientData clientData) {
    TrofsPath *tpPtr = (TrofsPath *) clientData;

    if (--tpPtr->refCount == 0) {
	MountRelease(tpPtr->mountPtr);
	if (tpPtr->parent) {
	    Tcl_DecrRefCount(tpPtr->parent);
	}
	if (tpPtr->tail) {
	    Tcl_DecrRefCount(tpPtr->tail);
	}
	if (tpPtr->value) {
	    Tcl_DecrRefCount(tpPtr->value);
	}
	Tcl_Free((char *)tpPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * InternalToNormalized --
 *
 * 	Part of the "trofs" Tcl_Filesystem.
 * 	From a TrofsPath representation, produce the path string rep.
 *
 * Results:
 * 	Returns a Tcl_Obj holding the string rep.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
InternalToNormalized(ClientData clientData) {
    TrofsPath *tpPtr = (TrofsPath *) clientData;
    if (NULL == tpPtr->parent) {
	return Tcl_NewStringObj(
		tpPtr->mountPtr->path, tpPtr->mountPtr->pathLen);
    } else {
	return Tcl_FSJoinToPath(tpPtr->parent, 1, &tpPtr->tail);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NormalizePath --
 *
 * 	Part of the "trofs" Tcl_Filesystem.
 * 	Gets a pathPtr normalized up to byte nextCheckPoint.
 * 	Normalizes further, eliminating symlinks, and returns
 * 	new checkpoint value.
 *	See docs for Tcl_FSNormalizePathProc for details.
 *	NOTE: This can be called for any pathPtr at all; not
 *	limited to those known to be in a trofs filesystem
 *
 *----------------------------------------------------------------------
 */

static int  
NormalizePath(Tcl_Interp *interp, Tcl_Obj *pathPtr, int nextCheckPoint) {
    int pathLen;
    char *path = Tcl_GetStringFromObj(pathPtr, &pathLen);

    CREW_ReadHold(&trofsMountLock);
    if (nextCheckPoint == 0) {
	Mount *mountPtr;

	for (mountPtr = trofsMountList; mountPtr != NULL;
		mountPtr = mountPtr->next) {
	    if (pathLen < mountPtr->pathLen) continue;
	    if (0 == memcmp(mountPtr->path, path, (size_t)mountPtr->pathLen)) {
		nextCheckPoint = mountPtr->pathLen;
		break;
	    }
	}
    }
    while (nextCheckPoint && nextCheckPoint < pathLen) {
	TrofsPath *tpPtr;
	Tcl_Obj *verified = Tcl_NewStringObj(path, nextCheckPoint);
	const char * p= path + nextCheckPoint + 1;
	const char *nextSep = strchr(p, '/');
	Tcl_Obj *element = NULL;
	Tcl_Obj *entry = NULL;
	Tcl_Obj *type = NULL;
	char typeFlag;
	Tcl_Obj *origPathPtr, *linkPathPtr = NULL;
	static Tcl_ThreadDataKey trofsNormalizeKey;
	Tcl_HashTable *linksFollowed = GetThreadHash(&trofsNormalizeKey);
	Tcl_HashEntry *hPtr;
	int new;
       
	Tcl_IncrRefCount(verified);
	tpPtr = FetchTrofsPath(verified);	
	if (NULL == tpPtr) {
	    Tcl_DecrRefCount(verified);
	    /* Verified path is not ours.  Let another filesystem claim it */
	    break;
	}

	/* verified path is ours.
	 * It must be a directory, or something's not right. */
	if (S_IFDIR != tpPtr->mode) {
	    Tcl_DecrRefCount(verified);
	    break;
	}

	/* If verified path is last directory in path,
	 * then whole path is normalized */
	if (NULL == nextSep) {
	    nextCheckPoint = pathLen;
	    Tcl_DecrRefCount(verified);
	    continue;
	}

	/* Get the next path element, and look up to see if it's
	 * in our directory's contents */
	element = Tcl_NewStringObj(p, (int)(nextSep - p));
	Tcl_IncrRefCount(element);
	Tcl_DictObjGet(NULL, tpPtr->value, element, &entry);

	if (NULL == entry) {
	    /* It's not: must allow other filesystems to claim
	     * (might be their mount point). */
	    Tcl_DecrRefCount(verified);
	    Tcl_DecrRefCount(element);
	    break;
	}

	/* It's in the directory.  What is it?  First element of
	 * entry (as a list) indicates the type.*/
	Tcl_ListObjIndex(NULL, entry, 0, &type);
	typeFlag = Tcl_GetString(type)[0];

	if ('D' == typeFlag) {
	    /* It's a directory, as expected.
	     * Advance the checkpoint, and continue the scan. */
	    nextCheckPoint = nextSep - path;
	    Tcl_DecrRefCount(verified);
	    Tcl_DecrRefCount(element);
	    continue;
	}
	if ('F' == typeFlag) {
	    /* It's a regular file in our filesystem, yet the
	     * path is using it like a directory.  Perhaps it's
	     * an archive with a filesystem inside?  To handle that,
	     * advance checkpoint to cover the whole filename
	     * (it's normalized so far) but allow another filesystem
	     * to claim the rest. */
	    nextCheckPoint = nextSep - path;
	    Tcl_DecrRefCount(verified);
	    Tcl_DecrRefCount(element);
	    break;
	}

	if ('L' != typeFlag) {
	    Tcl_Panic("unknown directory entry in NormalizePath");
	}

	/* It's a link. */
	origPathPtr = Tcl_FSJoinToPath(verified, 1, &element);
	Tcl_DecrRefCount(element);
	Tcl_IncrRefCount(origPathPtr);

	hPtr = Tcl_FindHashEntry(linksFollowed, (char *)origPathPtr);
	if (NULL != hPtr) {
	    /* We already followed this link.  There's a loop.
	     * In fact, we've made one trip around the cycle.
	     * Just claim that the original path is completely normalized.
	     * (Any entrypoint to the loop is good as any other) */

	    ClearHash(linksFollowed);
	    nextCheckPoint = pathLen;
	} else {
	    /* Haven't followed the link yet
	     * Replace with its contents and try normalizing the results */
	    Tcl_Obj *normLinkPathPtr, *result;
	    Tcl_Obj *restOfPath = Tcl_NewStringObj(nextSep + 1,
		    (int)(pathLen - (nextSep - path) - 1));
	    Tcl_Obj *linkValue;

	    Tcl_IncrRefCount(restOfPath);
	    Tcl_ListObjIndex(NULL, entry, 1, &linkValue);
	    Tcl_IncrRefCount(linkValue);
	    linkPathPtr = Tcl_FSJoinToPath(verified, 1, &linkValue);

	    Tcl_IncrRefCount(linkPathPtr);
	    Tcl_DecrRefCount(linkValue);

	    hPtr = Tcl_CreateHashEntry(linksFollowed,
		    (char *) origPathPtr, &new);
	    Tcl_IncrRefCount(linkPathPtr);
	    if (!new) {
		Tcl_Obj *oldValuePtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
		Tcl_DecrRefCount(oldValuePtr);
	    }
	    Tcl_SetHashValue(hPtr, linkPathPtr);

	    normLinkPathPtr = Tcl_FSGetNormalizedPath(NULL, linkPathPtr);
	    Tcl_IncrRefCount(normLinkPathPtr);
	    Tcl_DecrRefCount(linkPathPtr);
	    Tcl_GetStringFromObj(normLinkPathPtr, &nextCheckPoint);
	    result = Tcl_FSJoinToPath(normLinkPathPtr, 1, &restOfPath);
	    Tcl_IncrRefCount(result);
	    Tcl_DecrRefCount(restOfPath);
	    Tcl_DecrRefCount(normLinkPathPtr);

	    hPtr = Tcl_FindHashEntry(linksFollowed, (char *)origPathPtr);
	    if (hPtr != NULL) {
		Tcl_Obj *valuePtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
		Tcl_DecrRefCount(valuePtr);
		Tcl_DeleteHashEntry(hPtr);
	    }

	    path = Tcl_GetStringFromObj(result, &pathLen);
	    Tcl_SetStringObj(pathPtr, path, pathLen);
	    Tcl_DecrRefCount(result);
	}
	Tcl_DecrRefCount(origPathPtr);
	Tcl_DecrRefCount(verified);
    }
    CREW_Release(&trofsMountLock);
    return nextCheckPoint;
}

/*
 *----------------------------------------------------------------------
 *
 * Stat, Lstat --
 *
 * 	Parts of the "trofs" Tcl_Filesystem.
 *	Fills *statPtr with file meta data.  Stat follow links
 *	and returns meta data for what they refer to; Lstat returns
 *	meta data for the links themselves.
 *
 * Results:
 * 	Returns 0 when *statPtr is filled, or -1 to indicate an error.
 *
 * Side effects:
 * 	Sets errno on error.
 *
 *----------------------------------------------------------------------
 */

static int
Stat(Tcl_Obj *pathPtr, Tcl_StatBuf *statPtr) {
    int result = -1;
    TrofsPath *tpPtr;

    /* Don't let mounts change while chasing links */
    CREW_ReadHold(&trofsMountLock);
    tpPtr = FetchTrofsPath(pathPtr);	
    if (NULL == tpPtr) {
	goto done;
    }
    if (S_IFLNK == tpPtr->mode) {
	static Tcl_ThreadDataKey trofsStatKey;
	Tcl_Obj *linkPathPtr = NULL;
	Tcl_HashTable *linksFollowed = GetThreadHash(&trofsStatKey);
	Tcl_HashEntry *hPtr;
	int new;

	hPtr = Tcl_FindHashEntry(linksFollowed, (char *)pathPtr);
	if (NULL != hPtr) {
	    /* We already followed this link.  Raise ELOOP */
	    Tcl_SetErrno(ELOOP);
	    ClearHash(linksFollowed);
	    goto done;
	}
	linkPathPtr = Tcl_FSJoinToPath(
		Tcl_FSGetNormalizedPath(NULL, tpPtr->parent), 1, &tpPtr->value);
	Tcl_IncrRefCount(linkPathPtr);

	hPtr = Tcl_CreateHashEntry(linksFollowed, (char *) pathPtr, &new);
	Tcl_IncrRefCount(linkPathPtr);
	if (!new) {
	    Tcl_Obj *oldValuePtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
	    Tcl_DecrRefCount(oldValuePtr);
	}
	Tcl_SetHashValue(hPtr, linkPathPtr);
	result = Tcl_FSStat(linkPathPtr, statPtr);

	hPtr = Tcl_FindHashEntry(linksFollowed, (char *)pathPtr);
	if (hPtr != NULL) {
	    Tcl_Obj *valuePtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
	    Tcl_DecrRefCount(valuePtr);
	    Tcl_DeleteHashEntry(hPtr);
	}
	Tcl_DecrRefCount(linkPathPtr);
    } else {
	result = Lstat(pathPtr, statPtr);
    }
done:
    CREW_Release(&trofsMountLock);
    return result;
}

static int
Lstat(Tcl_Obj *pathPtr, Tcl_StatBuf *statPtr) {
    int result = -1;
    Tcl_Obj *archivePtr;
    TrofsPath *tpPtr;

    CREW_ReadHold(&trofsMountLock);
    tpPtr = FetchTrofsPath(pathPtr);	
    if (NULL == tpPtr) {
	goto done;
    }
    if (0 == tpPtr->mode) {
	TrofsPath *parentPtr = FetchTrofsPath(tpPtr->parent);	
	if ((NULL == parentPtr) || (S_IFDIR != parentPtr->mode)) {
	    Tcl_SetErrno(ENOTDIR);
	} else {
	    Tcl_SetErrno(ENOENT);
	}
	goto done;
    }

    archivePtr = Tcl_NewStringObj(tpPtr->mountPtr->archivePath,
	    tpPtr->mountPtr->archiveLen);
    Tcl_IncrRefCount(archivePtr);
    result = Tcl_FSStat(archivePtr, statPtr);
    if ((-1 == result) || (statPtr->st_mtime != tpPtr->mountPtr->mtime)) {
	tpPtr->mountPtr->mtime = 0;
	Tcl_SetErrno(ESTALE);
	result = -1;
    }
    Tcl_DecrRefCount(archivePtr);
    if (0 != result) {
	goto done;
    }

    statPtr->st_size = tpPtr->size;	/* size of our virtual file */
#ifdef HAVE_ST_BLOCKS
    statPtr->st_blocks = -1;		/* undefined */
#endif
    statPtr->st_nlink = 0;		/* no hard links to virtual files */
    
    /* Update mode to type of our virtual file,
     * and mask to read permissions only */
    statPtr->st_mode = (statPtr->st_mode & (mode_t)(~S_IFMT)) | tpPtr->mode;
    statPtr->st_mode &= (mode_t)(S_IFMT | S_IRUSR | S_IRGRP | S_IROTH);

    /* Not bothering to track the access time of the virtual file separate from
     * the archive that contains it.  This means the access time reported here
     * will generally be later than the correct value for the virtual file */

done:
    CREW_Release(&trofsMountLock);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Access --
 *
 * 	Part of the "trofs" Tcl_Filesystem.  Tests existence and
 * 	permissions on pathPtr.
 *
 * Results:
 * 	Returns 0 or -1 to indicate success or failure for the access
 * 	to pathPtr indicated by mode.
 *
 * Side effects:
 * 	When -1 is returned, errno is set.
 *
 *----------------------------------------------------------------------
 */

static int
Access(Tcl_Obj *pathPtr, int mode) {
    TrofsPath *tpPtr;
    int result = -1;

    if (mode & ~(R_OK | W_OK | X_OK | F_OK)) {
	/* Some bits set other than those that make up a valid mode */
	Tcl_SetErrno(EINVAL);
	return result;
    }

    /* Don't let mounts change while chasing links */
    CREW_ReadHold(&trofsMountLock);
    tpPtr = FetchTrofsPath(pathPtr);
    if (NULL == tpPtr) {
	goto done;
    }
    if (S_IFLNK == tpPtr->mode) {
	static Tcl_ThreadDataKey trofsAccessKey;
	Tcl_Obj *linkPathPtr = NULL;
	Tcl_HashTable *linksFollowed = GetThreadHash(&trofsAccessKey);
	Tcl_HashEntry *hPtr;
	int new;

	hPtr = Tcl_FindHashEntry(linksFollowed, (char *) pathPtr);
	if (NULL != hPtr) {
	    /* We already followed this link.  Raise ELOOP */
	    Tcl_SetErrno(ELOOP);
	    ClearHash(linksFollowed);
	    goto done;
	}
	linkPathPtr = Tcl_FSJoinToPath(
		Tcl_FSGetNormalizedPath(NULL, tpPtr->parent), 1, &tpPtr->value);
	Tcl_IncrRefCount(linkPathPtr);
	hPtr = Tcl_CreateHashEntry(linksFollowed, (char *) pathPtr, &new);
	Tcl_IncrRefCount(linkPathPtr);
	if (!new) {
	    Tcl_Obj *oldValuePtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
	    Tcl_DecrRefCount(oldValuePtr);
	}
	Tcl_SetHashValue(hPtr, linkPathPtr);
	result = Tcl_FSAccess(linkPathPtr, mode);
	hPtr = Tcl_FindHashEntry(linksFollowed, (char *)pathPtr);
	if (hPtr != NULL) {
	    Tcl_Obj *valuePtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
	    Tcl_DecrRefCount(valuePtr);
	    Tcl_DeleteHashEntry(hPtr);
	}
	Tcl_DecrRefCount(linkPathPtr);
    } else {
	if (0 == tpPtr->mode) {
	    TrofsPath *parentPtr = FetchTrofsPath(tpPtr->parent);	
	    if ((NULL == parentPtr) || (S_IFDIR != parentPtr->mode)) {
	    /* Other than stale file handles, seems hard to get here;
	     * if the parent path is not our directory, then the original
	     * path should not have been claimed by us in the first place. */
		Tcl_SetErrno(ENOTDIR);
	    } else {
		Tcl_SetErrno(ENOENT);
	    }
	    goto done;
	}
	if (mode & (W_OK | X_OK)) {
	    Tcl_SetErrno(EROFS);
	    goto done;
	}
	result = 0;
    }
done:
    CREW_Release(&trofsMountLock);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TranslateOpenMode --
 *
 * 	Utility routine to translate between the bits of a mode value
 * 	passed in to a Tcl_FSOpenFileChannelProc and the corresponding
 * 	string value accepted by Tcl_FSOpenFileChannel.
 *
 * Results:
 * 	The string equivalent to mode is written into *modeStringPtr.
 *
 *----------------------------------------------------------------------
 */

static void
TranslateOpenMode(int mode, Tcl_DString *modeStringPtr) {
    Tcl_DStringInit(modeStringPtr);
    /* This is a pain.  We get a bitarray mode, but have to construct
     * an encoded modeString. */
    switch (mode & O_ACCMODE) {
	case O_RDONLY:
	    Tcl_DStringAppendElement(modeStringPtr, "RDONLY");
	    break;
	case O_WRONLY:
	    Tcl_DStringAppendElement(modeStringPtr, "WRONLY");
	    break;
	case O_RDWR:
	    Tcl_DStringAppendElement(modeStringPtr, "RDWR");
	    break;
    }
    if (mode & O_CREAT) {
	Tcl_DStringAppendElement(modeStringPtr, "CREAT");
    }
    if (mode & O_EXCL) {
	Tcl_DStringAppendElement(modeStringPtr, "EXCL");
    }
#ifdef O_NOCTTY
    if (mode & O_NOCTTY) {
	Tcl_DStringAppendElement(modeStringPtr, "NOCTTY");
    }
#endif
    if (mode & O_TRUNC) {
	Tcl_DStringAppendElement(modeStringPtr, "TRUNC");
    }
    if (mode & O_APPEND) {
	Tcl_DStringAppendElement(modeStringPtr, "APPEND");
    }
#if defined(O_NDELAY) || defined(O_NONBLOCK)
#  ifdef O_NONBLOCK
    if (mode & O_NONBLOCK) {
	Tcl_DStringAppendElement(modeStringPtr, "NONBLOCK");
    }
#  else
    if (mode & O_NDELAY) {
	Tcl_DStringAppendElement(modeStringPtr, "NONBLOCK");
    }
#  endif
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * FailMessage --
 *
 * 	Utility routine to write a consistently formatted error message
 * 	into an interp result.
 *
 * Side effects:
 * 	Interp result is set.
 *
 *----------------------------------------------------------------------
 */

static void
FailMessage(	Tcl_Interp *interp, const char *prefix,
		Tcl_Obj *pathPtr, const char *suffix) {
    Tcl_Obj *result = Tcl_NewObj();
    Tcl_AppendStringsToObj(result, prefix, " \"", NULL);
    Tcl_AppendObjToObj(result, pathPtr);
    Tcl_AppendStringsToObj(result, "\": ", suffix, NULL);
    Tcl_SetObjResult(interp, result);
}

/*
 *----------------------------------------------------------------------
 *
 * OpenFileChannel --
 *
 * 	Part of the "trofs" Tcl_Filesystem.  
 * 	Opens the file pathPtr.
 *
 * Results:
 * 	Returns the Tcl_Channel from opening pathPtr, or NULL when
 * 	there is an error.
 *
 * Side effects:
 * 	On error, writes an error message to the interp result
 *
 *----------------------------------------------------------------------
 */

static Tcl_Channel
OpenFileChannel(Tcl_Interp *interp, Tcl_Obj *pathPtr,
		int mode, int permissions) {
    const char *prefix = "cannot open";
    Tcl_Channel result = NULL;
    TrofsPath *tpPtr;

    CREW_ReadHold(&trofsMountLock);
    tpPtr = FetchTrofsPath(pathPtr);	
    if (NULL == tpPtr) {
	FailMessage(interp, prefix, pathPtr, "stale file handle");
	goto done;
    }
    switch (tpPtr->mode) {
      case 0:
	FailMessage(interp, prefix, pathPtr, "no such file");
	Tcl_SetErrno(ENOENT);
	break;
      case S_IFDIR:
	FailMessage(interp, prefix, pathPtr,
		"illegal operation on a directory");
	Tcl_SetErrno(EISDIR);
	break;
      case S_IFLNK: {
	static Tcl_ThreadDataKey trofsOpenKey;
	Tcl_Obj *linkPathPtr, *targetPathPtr;
	Tcl_HashTable *linksFollowed = GetThreadHash(&trofsOpenKey);
	Tcl_HashEntry *hPtr;
	int new;
	Tcl_DString modeString;

	hPtr = Tcl_FindHashEntry(linksFollowed, (char *) pathPtr);
	if (NULL != hPtr) {
	    /* We already followed this link.  Raise ELOOP */
	    FailMessage(interp, prefix, pathPtr,
		    "too many levels of symbolic links");
	    Tcl_SetErrno(ELOOP);
	    ClearHash(linksFollowed);
	    goto done;
	}
	targetPathPtr = Tcl_FSJoinToPath(Tcl_FSGetNormalizedPath(NULL,
		tpPtr->parent), 1, &tpPtr->value);
	Tcl_IncrRefCount(targetPathPtr);
	linkPathPtr = Tcl_FSGetNormalizedPath(NULL, targetPathPtr);
	Tcl_IncrRefCount(linkPathPtr);
	Tcl_DecrRefCount(targetPathPtr);

	hPtr = Tcl_CreateHashEntry(linksFollowed, (char *) pathPtr, &new);
	Tcl_IncrRefCount(linkPathPtr);
	if (!new) {
	    Tcl_Obj *oldValuePtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
	    Tcl_DecrRefCount(oldValuePtr);
	}
	Tcl_SetHashValue(hPtr, linkPathPtr);

	TranslateOpenMode(mode, &modeString);
	result = Tcl_FSOpenFileChannel(interp, linkPathPtr,
		Tcl_DStringValue(&modeString), permissions);
	if (NULL == result) {
	    FailMessage(interp, prefix, pathPtr, Tcl_PosixError(interp));
	}
	Tcl_DStringFree(&modeString);

	hPtr = Tcl_FindHashEntry(linksFollowed, (char *)pathPtr);
	if (hPtr != NULL) {
	    Tcl_Obj *valuePtr = (Tcl_Obj *) Tcl_GetHashValue(hPtr);
	    Tcl_DecrRefCount(valuePtr);
	    Tcl_DeleteHashEntry(hPtr);
	}

	Tcl_DecrRefCount(linkPathPtr);
	break;
      }
      case S_IFREG: {
	Mount *mountPtr;
	Tcl_Obj *archivePtr;
	Tcl_Channel archiveChan;
	TrofsChannel *tcPtr;
	Tcl_StatBuf stat;

	switch (mode & O_ACCMODE) {
	    case O_RDONLY:
		/* Reading is ok. */
		break;
	    default:
		FailMessage(interp, "cannot open", pathPtr,
			"write permission denied");
		Tcl_SetErrno(EPERM);
		goto done;
	}

	mountPtr = tpPtr->mountPtr;
	archivePtr = Tcl_NewStringObj(
		mountPtr->archivePath, mountPtr->archiveLen);
	Tcl_IncrRefCount(archivePtr);
	if ((-1 == Tcl_FSStat(archivePtr, &stat))
		|| (stat.st_mtime != mountPtr->mtime)) {
	    mountPtr->mtime = 0;
	    Tcl_DecrRefCount(archivePtr);
	    FailMessage(interp, prefix, pathPtr, "stale file handle");
	    goto done;
	}
	archiveChan = Tcl_FSOpenFileChannel(NULL, archivePtr, "r", 0);
	Tcl_DecrRefCount(archivePtr);
	if (NULL == archiveChan) {
	    goto done;
	}

	/* Create Channel clientData */
	tcPtr = (TrofsChannel *)Tcl_Alloc((int)sizeof(TrofsChannel));
	tcPtr->pathPtr = pathPtr;
	Tcl_IncrRefCount(pathPtr);
	tcPtr->mtime = mountPtr->mtime;
	tcPtr->startOffset = tpPtr->offset;
	tcPtr->leftToRead = tpPtr->size;
	tcPtr->endOffset = tcPtr->startOffset + tpPtr->size;
	Tcl_Seek(archiveChan, tcPtr->startOffset, SEEK_SET);
	tcPtr->channel = Tcl_StackChannel(interp, &trofsFileChannel,
		(ClientData)tcPtr, TCL_READABLE, archiveChan);
	result = tcPtr->channel;
      }
    }
done:
    CREW_Release(&trofsMountLock);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * MatchInDirectory --
 *
 * 	Part of the "trofs" Tcl_Filesystem.  
 * 	See docs for Tcl_FSMatchInDirectoryProc for details.
 *
 * Results:
 * 	Returns TCL_OK, and appends to *result the matching path names.
 * 	Returns TCL_ERROR only if pathPtr is not (any longer) a trofs path
 *
 *	NOTE: This can be called for any pathPtr at all, when mounts
 *	are being sought; not limited to those known to be in a trofs
 *	filesystem
 *
 *----------------------------------------------------------------------
 */

static int
MatchInDirectory(	Tcl_Interp *interp, Tcl_Obj *result, Tcl_Obj *pathPtr,
			const char *pattern, Tcl_GlobTypeData *types) {
    int i, objc, code = TCL_ERROR;
    Tcl_Obj *candidates, **objv;
    TrofsPath *tpPtr;

    CREW_ReadHold(&trofsMountLock);
    tpPtr = FetchTrofsPath(pathPtr);	
    if (NULL == tpPtr) {
	if (NULL != interp) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj("stale file handle", -1));
	}
	goto done;
    }
    candidates = Tcl_NewObj();
    Tcl_IncrRefCount(candidates);
    if (pattern == NULL) {
	/* pathPtr names the specific thing to match */
	Tcl_ListObjAppendElement(NULL, candidates, pathPtr);
    } else if (S_IFDIR == tpPtr->mode) {
	/* pathPtr is a directory, and we match its contents */
	Tcl_Obj *keyPtr;
	int done;
	Tcl_DictSearch search;
	Tcl_DictObjFirst(NULL, tpPtr->value, &search, &keyPtr, NULL, &done);
	for (; !done; Tcl_DictObjNext(&search, &keyPtr, NULL, &done)) {
	    if (Tcl_StringMatch(Tcl_GetString(keyPtr), pattern)) {
		Tcl_ListObjAppendElement(NULL, candidates,
			Tcl_FSJoinToPath(pathPtr, 1, &keyPtr));
	    }
	}
    }
    Tcl_ListObjGetElements(NULL, candidates, &objc, &objv);
    for (i=0; i<objc; i++) {
	tpPtr = FetchTrofsPath(objv[i]);
	if (NULL == tpPtr) continue;
	if (0 == tpPtr->mode) continue;
	if (types == NULL) {
	    /* No constraint.  Existence is enough */
	    Tcl_ListObjAppendElement(NULL, result, objv[i]);
	    continue;
	}
	/* Check against requested types->type and types->perm */
	if (types->perm &
		(TCL_GLOB_PERM_W | TCL_GLOB_PERM_X | TCL_GLOB_PERM_HIDDEN)) {
	    continue;
	}
	if (types->type != 0) {
	    switch (tpPtr->mode){
	    case S_IFLNK:
		if (0 == (types->type & TCL_GLOB_TYPE_LINK)) {
		    /* Check what the link points to. */
		    Tcl_StatBuf stat;
		    if (0 != Tcl_FSStat(objv[i], &stat)) {
			continue;
		    }
		    if ( ! ( (S_ISREG(stat.st_mode)
			    && (types->type & TCL_GLOB_TYPE_FILE))
			    || (S_ISDIR(stat.st_mode)
			    && (types->type & TCL_GLOB_TYPE_DIR)) ) ) {
			continue;
		    }
		}
		break;
	    case S_IFREG:
		if (0 == (types->type & TCL_GLOB_TYPE_FILE)) continue;
		break;
	    case S_IFDIR:
		if (0 == (types->type & 
			(TCL_GLOB_TYPE_DIR | TCL_GLOB_TYPE_MOUNT))) continue;
		if ((types->type & TCL_GLOB_TYPE_MOUNT)
			& (tpPtr->parent != NULL)) continue;
	    }
	}
	Tcl_ListObjAppendElement(NULL, result, objv[i]);
    }
    Tcl_DecrRefCount(candidates);
    code = TCL_OK;
done:
    CREW_Release(&trofsMountLock);
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * Link --
 *
 * 	Part of the "trofs" Tcl_Filesystem.  Reads a symlink.
 *
 * Results:
 * 	A Tcl_Obj holding the value of link path *linkNamePtr,
 * 	or NULL on error.
 *
 * Side effects:
 * 	errno is set on error.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
Link(Tcl_Obj *linkNamePtr, Tcl_Obj *toPtr, int linkAction) {
    TrofsPath *tpPtr;
    Tcl_Obj *result = NULL;

    if (toPtr != NULL) {
	/* Read-only filesystem - do not create links */
	Tcl_SetErrno(EROFS);
	return result;
    }

    CREW_ReadHold(&trofsMountLock);
    tpPtr = FetchTrofsPath(linkNamePtr);	
    if (NULL == tpPtr) {
	goto done;
    }
    if (0 == tpPtr->mode) {
	TrofsPath *parentPtr = FetchTrofsPath(tpPtr->parent);	
	if ((NULL == parentPtr) || (S_IFDIR != parentPtr->mode)) {
	    /* Other than stale file handles, seems hard to get here;
	     * if the parent path is not our directory, then the original
	     * path should not have been claimed by us in the first place. */
	    Tcl_SetErrno(ENOTDIR);
	} else {
	    Tcl_SetErrno(ENOENT);
	}
	goto done;
    }
    if (S_IFLNK != tpPtr->mode) {
	Tcl_SetErrno(EINVAL);
	goto done;
    }
    result = tpPtr->value;
    Tcl_IncrRefCount(tpPtr->value);
done:
    CREW_Release(&trofsMountLock);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ListVolumes		--
 * FileAttrStrings	--
 * FileAttrsGet 	--
 * FileAttrsSet 	--
 * FilesystemSeparator	--
 *
 * 	Remaining parts of "trofs" Tcl_Filesystem are basic
 * 	boilerplate, for a conventional read-only filesystem
 * 	that defines no new attributes.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
ListVolumes() {
    /* Claim management of the trofs:// volume as a place to put
     * auto-generated mountpoints */
    return Tcl_NewStringObj(PACKAGE_NAME "://", -1);
}

static const char *CONST86 *
FileAttrStrings(Tcl_Obj *pathPtr, Tcl_Obj **objPtrRef) {
    /* We recognize no attributes */
    *objPtrRef = Tcl_NewObj();
    return NULL;
}

static int
FileAttrsGet(	Tcl_Interp *interp, int index,
		Tcl_Obj *pathPtr, Tcl_Obj **objPtrRef) {
    /* This must be defined only for the sake of Tcl_FSFileAttrsGet */
    if (interp != NULL) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(PACKAGE_NAME
		" filesystem supports no such attribute", -1));
    }
    return TCL_ERROR;
}

static int
FileAttrsSet(	Tcl_Interp *interp, int index,
		Tcl_Obj *pathPtr, Tcl_Obj *objPtrRef) {
    /* This must be defined only for the sake of Tcl_FSFileAttrsSet,
     * and, by implication, CopyRenameOneFile, i.e. [file copy] . */
    if (interp != NULL) {
	Tcl_SetObjResult(interp, Tcl_NewStringObj(PACKAGE_NAME
		" filesystem supports no such attribute", -1));
    }
    return TCL_ERROR;
}

static Tcl_Obj *
FilesystemSeparator(Tcl_Obj *pathPtr) {
    /* This must be defined only for the sake of Tcl_FSPathSeparator,
     * and, by implication, [file separator] . */
    return Tcl_NewStringObj("/", 1);
}

static int
Utime(Tcl_Obj *pathPtr, struct utimbuf *tval) {
    /* Read-only filesystem cannot set time metadata */
    TrofsPath *tpPtr = FetchTrofsPath(pathPtr);	
    if (NULL == tpPtr) {
	/* This branch needed only for direct Tcl_FSUtime() calls */
	Tcl_SetErrno(ENOENT);
    } else {
	Tcl_SetErrno(EROFS);
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * DriverClose --
 *
 * 	Frees the TrofsChannel struct associated with this channel.
 *
 * Results:
 * 	Returns TCL_OK.
 *
 * Side effects:
 * 	Memory is freed.
 *
 *----------------------------------------------------------------------
 */

static int
DriverClose(ClientData instanceData, Tcl_Interp *interp) {
    /* Close the channel */
    TrofsChannel *tcPtr = (TrofsChannel *)instanceData;
    Tcl_DecrRefCount(tcPtr->pathPtr);
    Tcl_Free((char *)tcPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DriverInput --
 *
 * 	Reads bytes from the trofs file into buf.
 *
 * 	A trofs file is really just a contiguous sequence of
 * 	bytes within an archive file, so input from a trofs
 * 	file gets done as input from the archive file with
 * 	care taken not to read past the end of the trofs
 * 	file's byte sequence.
 *
 * Results:
 *	The number of bytes read from the channel, or -1 if an error
 *	prevented reading.  When -1 is returned an error code is
 *	written to *errorCodePtr.
 *
 * Side effects:
 *	May block waiting for mounts/unmounts in other threads
 *	to end (no writer of the trofsMountList) before proceeding.
 *
 *----------------------------------------------------------------------
 */

static int
DriverInput(	ClientData instanceData, char *buf,
		int bufSize, int *errorCodePtr) {
    /* read from channel into buf */
    TrofsChannel *tcPtr = (TrofsChannel *)instanceData;
    TrofsPath *tpPtr;
    Tcl_Channel parent = Tcl_GetStackedChannel(tcPtr->channel);
    int toRead, bytesRead;
    int result = -1;

    CREW_ReadHold(&trofsMountLock);
    tpPtr = FetchTrofsPath(tcPtr->pathPtr);
    if ((NULL == tpPtr) || (tpPtr->mountPtr->mtime != tcPtr->mtime)) {
	if (tpPtr) {
	    /* Probably can't happen.  Changes in archive mtime
	     * should have been caught by FetchTrofsPath */
	    tpPtr->mountPtr->mtime = 0;
	}
	Tcl_SetErrno(ESTALE);
	goto done;
    }

    toRead = (bufSize > tcPtr->leftToRead) ?
	(int)(tcPtr->leftToRead) : bufSize;
    Tcl_SetErrno(0);
    bytesRead = Tcl_ReadRaw(parent, buf, toRead);
    if (bytesRead > -1) {
	tcPtr->leftToRead -= bytesRead;
	result = bytesRead;
    }

done:
    CREW_Release(&trofsMountLock);
    *errorCodePtr = Tcl_GetErrno();
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DriverWideSeek --
 *
 * 	Seeks to the access point in the trofs file channel
 * 	determined by offset and seekMode.
 *
 * 	A trofs file is really just a contiguous sequence of
 * 	bytes within an archive file, so the seek on a trofs
 * 	file gets done as a seek on the archive file with
 * 	the results shifted according to the seek offset
 * 	of the trofs file's bytes within the archive.
 *
 * Results:
 *	The new access point on the channel, or -1 if an error
 *	prevented seeking.  When -1 is returned an error code is
 *	written to *errorCodePtr.
 *
 * Side effects:
 *	May block waiting for mounts/unmounts in other threads
 *	to end (no writer of the trofsMountList) before proceeding.
 *
 *----------------------------------------------------------------------
 */

static Tcl_WideInt
DriverWideSeek(	ClientData instanceData, Tcl_WideInt offset,
		int seekMode, int *errorCodePtr) {
    /* Do a seek on the channel */
    TrofsPath *tpPtr;
    Tcl_WideInt result = -1;
    Tcl_WideInt newOffset = 0;
    TrofsChannel *tcPtr = (TrofsChannel *)instanceData;
    Tcl_Channel parent = Tcl_GetStackedChannel(tcPtr->channel);
    Tcl_DriverSeekProc *seekProc =
	    Tcl_ChannelSeekProc(Tcl_GetChannelType(parent));
    Tcl_DriverWideSeekProc *wideSeekProc =
	    Tcl_ChannelWideSeekProc(Tcl_GetChannelType(parent));

    CREW_ReadHold(&trofsMountLock);
    tpPtr = FetchTrofsPath(tcPtr->pathPtr);
    if ((NULL == tpPtr) || (tpPtr->mountPtr->mtime != tcPtr->mtime)) {
	if (tpPtr) {
	    /* Probably can't happen.  Changes in archive mtime
	     * should have been caught by FetchTrofsPath */
	    tpPtr->mountPtr->mtime = 0;
	}
	Tcl_SetErrno(ESTALE);
	goto done;
    }

    switch (seekMode) {
	case SEEK_SET:
	    newOffset = tcPtr->startOffset;
	    break;
	case SEEK_END:
	    newOffset = tcPtr->endOffset;
	    break;
	case SEEK_CUR:
	    newOffset = tcPtr->endOffset - tcPtr->leftToRead;
    }
    newOffset += offset;
    if ((newOffset < tcPtr->startOffset)
	    || (newOffset > tcPtr->endOffset)) {
	Tcl_SetErrno(EINVAL);
	goto done;
    }

    if (NULL != wideSeekProc) {
	Tcl_WideInt resultOffset = (wideSeekProc)(
		Tcl_GetChannelInstanceData(parent),
		newOffset, SEEK_SET, errorCodePtr);
	if (-1 != resultOffset) {
	    tcPtr->leftToRead = tcPtr->endOffset - resultOffset;
	    result = resultOffset - tcPtr->startOffset;
	}
    } else if (NULL != seekProc) {
	int resultOffset = (seekProc)(Tcl_GetChannelInstanceData(parent),
		(long)newOffset, SEEK_SET, errorCodePtr);
	if (-1 != resultOffset) {
	    tcPtr->leftToRead = tcPtr->endOffset - resultOffset;
	    result = resultOffset - tcPtr->startOffset;
	}
    }

done:
    CREW_Release(&trofsMountLock);
    *errorCodePtr = Tcl_GetErrno();
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DriverOutput	 	--
 * DriverSeek	 	--
 * DriverSetOption	--
 * DriverGetOption	--
 * DriverWatch		--
 * DriverBlockMode	--
 * DriverHandler	--
 *
 * 	The remaining routines that make up the trofs
 * 	Tcl_ChannelType are all just routine boilerplate
 * 	for a stacked channel.
 *
 *----------------------------------------------------------------------
 */
static int
DriverOutput(	ClientData instanceData, const char *buf,
		int toWrite, int *errorCodePtr) {
    /* Read-only filesystem -> no writing to files */
    *errorCodePtr = EROFS;
    return -1;
}

static int
DriverSeek(	ClientData instanceData, long offset,
		int seekMode, int *errorCodePtr) {
    return (int)DriverWideSeek(instanceData, (Tcl_WideInt)offset,
	    seekMode, errorCodePtr);
}

static int
DriverSetOption(ClientData instanceData, Tcl_Interp *interp,
		const char *optionName, const char *value) {
    /* Set channel options - delegate all */
    TrofsChannel *tcPtr = (TrofsChannel *)instanceData;
    Tcl_Channel parent = Tcl_GetStackedChannel(tcPtr->channel);
    Tcl_DriverSetOptionProc *setOptionProc =
	    Tcl_ChannelSetOptionProc(Tcl_GetChannelType(parent));

    if (NULL != setOptionProc) {
	return (setOptionProc)(Tcl_GetChannelInstanceData(parent),
		interp, optionName, value);
    }
    return TCL_ERROR;
}

static int
DriverGetOption(ClientData instanceData, Tcl_Interp *interp,
		const char *optionName, Tcl_DString *dsPtr) {
    /* Get channel options - delegate all */
    TrofsChannel *tcPtr = (TrofsChannel *)instanceData;
    Tcl_Channel parent = Tcl_GetStackedChannel(tcPtr->channel);
    Tcl_DriverGetOptionProc *getOptionProc =
	    Tcl_ChannelGetOptionProc(Tcl_GetChannelType(parent));

    if (NULL != getOptionProc) {
	return (getOptionProc)(Tcl_GetChannelInstanceData(parent),
		interp, optionName, dsPtr);
    }
    if (NULL == optionName) {
	return TCL_OK;
    } else {
	return TCL_ERROR;
    }
}

static void
DriverWatch(ClientData instanceData, int mask) {
    TrofsChannel *tcPtr = (TrofsChannel *)instanceData;
    Tcl_Channel parent = Tcl_GetStackedChannel(tcPtr->channel);
    Tcl_DriverWatchProc *watchProc =
	    Tcl_ChannelWatchProc(Tcl_GetChannelType(parent));

    if (NULL != watchProc) {
	/* Filter out interest in writability */
	(watchProc)(Tcl_GetChannelInstanceData(parent), mask & ~TCL_WRITABLE);
    }
}

static int
DriverBlockMode(ClientData instanceData, int mode) {
    return TCL_OK;
}

static int
DriverHandler(ClientData instanceData, int interestMask) {
    return interestMask;
}

/*
 *----------------------------------------------------------------------
 *
 * VerifyTrofsArchive --
 *
 * 	Checks whether archive names a valid trofs archive file.
 * 	chan must be the channel produce by opening archive for
 * 	reading.
 *
 * Results:
 * 	If archive names a valid trofs archive file, TCL_OK is
 * 	returned, and the seek offset and size of the root directory
 * 	index of the archive are written to *offsetPtr and
 * 	*indexSizePtr, respectively.
 *
 * 	If archive does not name a valid trofs archive file, TCL_ERROR
 * 	is returned, and and error message is left inther interp
 * 	result.
 *
 *----------------------------------------------------------------------
 */

static int
VerifyTrofsArchive(	Tcl_Interp *interp, Tcl_Obj *archive, Tcl_Channel chan,
			Tcl_WideInt *offsetPtr, int *indexSizePtr) {
    int numBytes, code = TCL_ERROR;
    Tcl_Obj *signature = NULL;
    Tcl_WideInt offset = Tcl_Seek(chan, (Tcl_WideInt)-12, SEEK_END);
    unsigned char *bytes;

    if (-1 == offset) {
	FailMessage(interp, "could not mount", archive,
		"not a " PACKAGE_NAME " archive");
	return code;
    }

    /* read bytes */
    Tcl_SetChannelOption(NULL, chan, "-translation", "binary");
    signature = Tcl_NewByteArrayObj(NULL,0);
    Tcl_IncrRefCount(signature);
    Tcl_ReadChars(chan, signature, 12, 1);
    bytes = Tcl_GetByteArrayFromObj(signature, &numBytes);
    if ((numBytes != 12) || (memcmp(bytes, "\32trofs", (size_t)6) != 0)) {
	FailMessage(interp, "could not mount", archive,
		"not a " PACKAGE_NAME " archive");
	goto done;
    }

    /* Simple archive format version check */
    switch (bytes[6]) {
	case '0':
	    switch (bytes[7]) {
		case '1':
		    code = TCL_OK;
		    break;
		default:
		    FailMessage(interp, "could not mount", archive,
			    "incompatible " PACKAGE_NAME " archive");
		    goto done;
	    }
	    break;
	default:
	    FailMessage(interp, "could not mount", archive,
		    "incompatible " PACKAGE_NAME " archive");
	    goto done;
    }
    *indexSizePtr	=  ((unsigned int) bytes[11])
	    		| (((unsigned int) bytes[10]) <<  8)
	    		| (((unsigned int) bytes[ 9]) << 16)
			| (((unsigned int) bytes[ 8]) << 24);
    *offsetPtr = offset;
done:
    Tcl_DecrRefCount(signature);
    return code;
}


/*
 *----------------------------------------------------------------------
 *
 * MountObjCmd --
 *
 * 	This procedure implements the [trofs::mount] command.
 *
 * 	trofs::mount $archive ?$mountpoint?
 *
 * 	$archive must name a readable file in the trofs archive 
 * 	format.  This procedure will mount that archive so that
 * 	$mountpoint becomes the name of a directory in the filesystem
 * 	and the contents of that directory (and its subdirectories, etc.)
 * 	will be the contents of the archive.  If no $mountpoint is
 * 	given, a suitable one is created.
 *
 * 	The thread of the caller should hold no read locks on
 * 	trofsMountLock when calling this routine.
 *
 * Results:
 *	When TCL_OK is returned, the $mountpoint is left as the
 *	interpreter result.  When TCL_ERROR is returned, no mount
 *	was added, and an error message is left in the interpreter
 *	result.
 *
 * Side effects:
 *	May block waiting for filesystem operations in other threads
 *	to end (no readers of the trofsMountList) before proceeding.
 * 	A new mount may be added to Tcl's virtual filesystem.
 *
 *----------------------------------------------------------------------
 */

static int
MountObjCmd(	ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *const  objv[]) {
    Tcl_Obj *archive, *mountPoint;
    Tcl_Channel chan;
    Tcl_WideInt headerOffset = (Tcl_WideInt) 0;
    int indexSize = 0, numBytes, code;
    const char *path, *prefix = "could not mount";
    char *buffer;
    Mount *mountPtr;
    Tcl_StatBuf stat;
    Tcl_Filesystem *testFS;

    if (objc != 2 && objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "archive ?mountpoint?");
	return TCL_ERROR;
    }
    archive = objv[1];

    /* Check archive */
    if (-1 == Tcl_FSStat(archive, &stat)) {
	FailMessage(interp, prefix, archive, Tcl_PosixError(interp));
	return TCL_ERROR;
    }
    if (!S_ISREG(stat.st_mode)) {
	FailMessage(interp, prefix, archive, "not a regular file");
	return TCL_ERROR;
    }
    chan = Tcl_FSOpenFileChannel(interp, archive, "r", 0);
    if (NULL == chan) {
	FailMessage(interp, prefix, archive, Tcl_PosixError(interp));
	return TCL_ERROR;
    }
    code = VerifyTrofsArchive(interp, archive, chan, &headerOffset, &indexSize);
    Tcl_Close(NULL, chan);
    if (code != TCL_OK) {
	return code;
    }

    /* Check mountpoint */
    if (3 == objc) {
	int length;

	mountPoint = objv[2];
	Tcl_GetStringFromObj(mountPoint, &length);
	if (0 == length) {
	    FailMessage(interp, "could not mount onto", mountPoint,
		    "empty string mountpoint is not supported");
	    return TCL_ERROR;
	}
    } else {
	Tcl_Obj *counter;
nextTry:
	mountPoint = Tcl_NewStringObj(PACKAGE_NAME "://", -1);
	Tcl_IncrRefCount(mountPoint);

	Tcl_MutexLock(&trofsGeneratorMutex);
	counter = Tcl_NewIntObj(++trofsGenerator);
	Tcl_MutexUnlock(&trofsGeneratorMutex);
	Tcl_IncrRefCount(counter);
	Tcl_AppendObjToObj(mountPoint, counter);
	Tcl_DecrRefCount(counter);
    }

    testFS = Tcl_FSGetFileSystemForPath(mountPoint);

    if (testFS && (testFS != &trofsFilesystem)
	    && (0 != strcmp(testFS->typeName, "native"))) {
	FailMessage(interp, "could not mount onto", mountPoint,
		    "cannot nest filesystems (Tcl Bug 941872)");
	if (3 == objc) {
	    return TCL_ERROR;
	} else {
	    Tcl_DecrRefCount(mountPoint);
	    goto nextTry;
	}
    }
    if (0 == Tcl_FSAccess(mountPoint, F_OK)) {
	FailMessage(interp, "could not mount onto", mountPoint, "file exists");
	if (3 == objc) {
	    return TCL_ERROR;
	} else {
	    Tcl_DecrRefCount(mountPoint);
	    goto nextTry;
	}
    }	

    /* At this point, we're satisfied we have a compatible trofs
     * archive and an acceptable mountpoint.
     * Configure a trofs filesystem mount to access it */

    mountPtr = (Mount *) Tcl_AttemptAlloc((unsigned int)sizeof(Mount));
    if (NULL == mountPtr) {
	FailMessage(interp, "could not mount", archive,
		"insufficient memory");
	if (3 != objc) {
	    Tcl_DecrRefCount(mountPoint);
	}
	return TCL_ERROR;
    }

    mountPtr->refCount = 1;	/* adding to MountList */

    path = Tcl_GetStringFromObj(
	    Tcl_FSGetNormalizedPath(NULL, archive), &numBytes);
    buffer = Tcl_AttemptAlloc((unsigned int)(numBytes+1));
    if (NULL == buffer) {
	FailMessage(interp, "could not mount", archive, "insufficient memory");
	Tcl_Free((char *)mountPtr);
	if (3 != objc) {
	    Tcl_DecrRefCount(mountPoint);
	}
	return TCL_ERROR;
    }
    memcpy(buffer, path, (size_t)(numBytes+1));
    mountPtr->archivePath = buffer;
    mountPtr->archiveLen = numBytes;

    path = Tcl_GetStringFromObj(
	    Tcl_FSGetNormalizedPath(NULL, mountPoint), &numBytes);
    buffer = Tcl_AttemptAlloc((unsigned int)(numBytes+1));
    if (NULL == buffer) {
	FailMessage(interp, "could not mount", archive, "insufficient memory");
	Tcl_Free((char *)mountPtr->archivePath);
	Tcl_Free((char *)mountPtr);
	if (3 != objc) {
	    Tcl_DecrRefCount(mountPoint);
	}
	return TCL_ERROR;
    }
    memcpy(buffer, path, (size_t)(numBytes+1));
    mountPtr->path = buffer;
    mountPtr->pathLen = numBytes;

    mountPtr->mtime = (Tcl_WideInt)stat.st_mtime;
    mountPtr->indexOffset = headerOffset - indexSize;
    mountPtr->indexSize = indexSize;

    CREW_WriteHold(&trofsMountLock);
    mountPtr->next = trofsMountList;
    trofsMountList = mountPtr;
    trofsMountEpoch++;
    CREW_Release(&trofsMountLock);

    Tcl_FSMountsChanged(&trofsFilesystem);

    /* Return mountpoint in string form only to workaround
     * epoch safety error in Tcl (Bug 932314)
     *
     * Bug 932314 now fixed in 8.4.7+.
     *
    path = Tcl_GetStringFromObj(
	    Tcl_FSGetNormalizedPath(NULL, mountPoint), &numBytes);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(path, numBytes));
     */
    Tcl_SetObjResult(interp, Tcl_FSGetNormalizedPath(NULL, mountPoint));
    if (3 != objc) {
	Tcl_DecrRefCount(mountPoint);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Unmount --
 *	Finds the Mount on the trofsMountList that has path as the
 *	value of its path field, and remove that Mount from the
 *	trofsMountlist.
 *
 * 	The thread of the caller should hold no read locks on
 * 	trofsMountLock when calling this routine.
 *
 * Results:
 *	A standard Tcl result
 *
 * Side effects:
 *	May block waiting for filesystem operations in other threads
 *	to end (no readers of the trofsMountList) before proceeding.
 * 	A mount is removed from Tcl's virtual filesystem.
 *
 *----------------------------------------------------------------------
 */
static int
Unmount(const char *path, int numBytes) {
    Mount *mountPtr, *last = NULL;

    CREW_WriteHold(&trofsMountLock);
    for (mountPtr = trofsMountList; mountPtr != NULL;
	    last = mountPtr, mountPtr = mountPtr->next) {
	if (memcmp(mountPtr->path, path, (size_t)numBytes) == 0) {
	    if (last == NULL) {
		trofsMountList = mountPtr->next;
	    } else {
		last->next = mountPtr->next;
	    }
	    trofsMountEpoch++;
	    MountRelease(mountPtr);
	    break;
	}
    }
    CREW_Release(&trofsMountLock);

    if (mountPtr == NULL) {
	return TCL_ERROR;
    }

    Tcl_FSMountsChanged(&trofsFilesystem);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UnmountObjCmd --
 *
 * 	This procedure implements the [trofs::unmount] command.
 *
 * 	trofs::mount $mountpoint
 *
 * 	$mountpoint must be the mount point of a trofs mount formerly
 * 	established by [trofs::mount].  This procedure will undo that
 * 	mount, return authority of that path back to the original
 * 	filesystem that owned it first.
 *
 * Results:
 *	A standard Tcl result
 *
 * Side effects:
 * 	A mount is removed from Tcl's virtual filesystem.
 *
 *----------------------------------------------------------------------
 */

static int
UnmountObjCmd(	ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *const objv[]) {
    int numBytes;
    const char *path;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "mountpoint");
	return TCL_ERROR;
    }

    path = Tcl_GetStringFromObj(
	    Tcl_FSGetNormalizedPath(interp, objv[1]), &numBytes);
    if (TCL_ERROR == Unmount(path, numBytes)) {
	FailMessage(interp, "could not unmount", objv[1],
		"not a " PACKAGE_NAME " mount");
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ExitHandler --
 *	Exit handler used by Trofs_Init to unregister the trofs
 *	Tcl_Filesystem during process exit.
 *
 * Side effects:
 * 	Unregisters the trofs filesystem, and frees all process-wide
 * 	resources of the package.
 *
 *----------------------------------------------------------------------
 */

void
ExitHandler(ClientData clientData) {
    /* TODO: Consider Unmount() of all active mountpoints? 
     * Can we confirm that any thread calling Tcl_Finalize()
     * cannot possibly have a read hold active on the trofs
     * mount list? */
    Tcl_FSUnregister(&trofsFilesystem);
    CREW_Finalize(&trofsMountLock);
}


/*
 *----------------------------------------------------------------------
 *
 * Trofs_Init --
 *
 * 	Initialize the trofs package
 *
 * Results:
 *	A standard Tcl result
 *
 * Side effects:
 * 	Registers the trofsFilesystem, if not already registered.
 * 	When TCL_OK is returned, provides the "trofs" package in interp.
 * 	When TCL_ERROR is returned, leaves an error message in interp
 * 	explaining why the "trofs" package could not be provided.
 *
 *----------------------------------------------------------------------
 */

DLLEXPORT int
Trofs_Init(Tcl_Interp *interp) {
    int code;
    if (Tcl_InitStubs(interp, TCL_VERSION,
#if (TCL_MINOR_VERSION == 4)
	    1	/* "dict" stubs can't bridge Tcl 8.4 -> 8.5 transition */
		/* so, builds against Tcl 8.4 must load only in Tcl 8.4 */
#else
	    0
#endif
	    ) == NULL) {
	return TCL_ERROR;
    }
#if (TCL_MINOR_VERSION == 4)
    /* We use [dict]s.  They arrived in Tcl 8.5.  If we only have
     * Tcl 8.4, add the "dict" package. */
    if (Dict_InitStubs(interp, "8.5", 0) == NULL) {
	return TCL_ERROR;
    }
#endif
    if (TCL_OK != Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION)) {
	/* Conflicting version already provided - shouldn't happen */
	return TCL_ERROR;
    }
    if (NULL == Tcl_FSData(&trofsFilesystem)) {
	Tcl_FSRegister((ClientData) 1, &trofsFilesystem);
	Tcl_CreateExitHandler(ExitHandler, NULL);
    }
    Tcl_CreateObjCommand(interp, PACKAGE_NAME "::mount", MountObjCmd,
	    NULL, NULL);
    Tcl_CreateObjCommand(interp, PACKAGE_NAME "::unmount", UnmountObjCmd,
	    NULL, NULL);

#if (TCL_MINOR_VERSION >= 5)
    /* TODO: Get Makefile to tell us the right encoding instead of
     * hard-coding iso8859-1 */
    Tcl_RegisterConfig(interp, PACKAGE_NAME, trofsConfig, "iso8859-1");
#endif

    code = Tcl_Eval(interp, "\n"
"namespace eval ::" PACKAGE_NAME " {\n"
"    namespace export mount unmount archive\n"
"\n"
"    interp alias {} [namespace current]::write {} puts -nonewline\n"
"\n"
"    proc AppendDirToArchive {dir toChan toFile} {\n"
"	array set index {}\n"
"	foreach link [glob -nocomplain -tails -types l -directory $dir *] {\n"
"	    set index($link) [list L [file readlink [file join $dir $link]]]\n"
"	}\n"
"\n"
"	foreach file [glob -nocomplain -tails -types {r f} -directory $dir *] {\n"
"	    set path [file join $dir $file]\n"
"	    if {[file type $path] eq \"link\"} {\n"
"		continue\n"
"	    }\n"
"	    set normFile [file normalize $path]\n"
"	    if {[string equal $toFile $normFile]} {\n"
"		# Do not recursively archive the archive...\n"
"		continue\n"
"	    }\n"
"	    set start [tell $toChan]\n"
"	    set fromChan [open $path r]\n"
"	    fconfigure $fromChan -translation binary\n"
"	    fcopy $fromChan $toChan\n"
"	    close $fromChan\n"
"	    set end [tell $toChan]\n"
"	    set index($file) [list F [expr {wide($end) - wide($start)}] $start]\n"
"	}\n"
"\n"
"	foreach sub [glob -nocomplain -tails -types {r d} -directory $dir *] {\n"
"	    set path [file join $dir $sub]\n"
"	    if {[file type $path] eq \"link\"} {\n"
"		continue\n"
"	    }\n"
"	    set start [AppendDirToArchive $path $toChan $toFile]\n"
"	    set end [tell $toChan]\n"
"	    set index($sub) [list D [expr {wide($end) - wide($start)}] $start]\n"
"	}\n"
"\n"
"	# Record offset for start of index\n"
"	set start [tell $toChan]\n"
"	# fixup the offsets relative to the index start\n"
"	foreach {key value} [array get index] {\n"
"	    if {[llength $value] < 3} continue\n"
"	    set index($key) [lreplace $value 2 2 \\\n"
"		    [expr {wide($start) - wide([lindex $value 2])}]]\n"
"	}\n"
"	fconfigure $toChan -encoding utf-8\n"
"	write $toChan [array get index]\n"
"	fconfigure $toChan -encoding binary\n"
"	return $start\n"
"    }\n"
"\n"
"    proc archive {directory archive} {\n"
"	if {0 != [catch {\n"
"	    set f [open $archive a+]\n"
"	    fconfigure $f -translation binary\n"
"	    seek $f 0 end\n"
"	} result]} {\n"
"	    return -code error $result\n"
"	}\n"
"	write $f \\u001A\n"
"	set start [AppendDirToArchive $directory $f [file normalize $archive]]\n"
"	set end [tell $f]\n"
"	write $f \\u001Atrofs01\n"
"	write $f [binary format I [expr {wide($end) - wide($start)}]]\n"
"	close $f\n"
"    }\n"
"\n"
"}\n");
    if (code != TCL_OK) {
#if (TCL_MINOR_VERSION >= 5)
	Tcl_Obj *options = Tcl_GetReturnOptions(interp, code);
#endif
	Tcl_Obj *result = Tcl_GetObjResult(interp); 
	Tcl_IncrRefCount(result);
	Tcl_EvalEx(interp, "namespace delete " PACKAGE_NAME,
		-1, TCL_EVAL_GLOBAL);
	Tcl_EvalEx(interp, "package forget " PACKAGE_NAME, -1, TCL_EVAL_GLOBAL);
	Tcl_SetObjResult(interp, result);
	Tcl_DecrRefCount(result);
#if (TCL_MINOR_VERSION >= 5)
	return Tcl_SetReturnOptions(interp, options);
#endif
	return code;
    }
    return TCL_OK;
}

/*
 * TODO:	Figure out if there's a reasonable Trofs_SafeInit()
 * 		that can be defined.  Likely not.  Traditionally,
 * 		the ability to modify the filesystem is one of the things
 * 		forbidden to safe interps.  mount/unmount capability is
 * 		as dangerous as write capability.
 *
 * 		Figure out if there's any way to provide Trofs_Unload()
 */

