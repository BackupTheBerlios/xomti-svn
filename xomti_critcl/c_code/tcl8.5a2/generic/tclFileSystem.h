/* 
 * tclFileSystem.h --
 *
 *	This file contains the common defintions and prototypes for
 *	use by Tcl's filesystem and path handling layers.
 *
 * Copyright (c) 2003 Vince Darley.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclFileSystem.h,v 1.10 2004/11/03 00:53:05 davygrvy Exp $
 */

#ifndef _TCLFILESYSTEM
#define _TCLFILESYSTEM

#ifndef _TCL
#include "tcl.h"
#endif

/* 
 * struct FilesystemRecord --
 * 
 * A filesystem record is used to keep track of each
 * filesystem currently registered with the core,
 * in a linked list.  Pointers to these structures
 * are also kept by each "path" Tcl_Obj, and we must
 * retain a refCount on the number of such references.
 */
typedef struct FilesystemRecord {
    ClientData	     clientData;  /* Client specific data for the new
				   * filesystem (can be NULL) */
    Tcl_Filesystem *fsPtr;        /* Pointer to filesystem dispatch
				   * table. */
    int fileRefCount;             /* How many Tcl_Obj's use this
				   * filesystem. */
    struct FilesystemRecord *nextPtr;  
				  /* The next filesystem registered
				   * to Tcl, or NULL if no more. */
    struct FilesystemRecord *prevPtr;
				  /* The previous filesystem registered
				   * to Tcl, or NULL if no more. */
} FilesystemRecord;

/*
 * This structure holds per-thread private copy of the
 * current directory maintained by the global cwdPathPtr.
 * This structure holds per-thread private copies of
 * some global data. This way we avoid most of the
 * synchronization calls which boosts performance, at
 * cost of having to update this information each
 * time the corresponding epoch counter changes.
 */
typedef struct ThreadSpecificData {
    int initialized;
    int cwdPathEpoch;
    int filesystemEpoch;
    Tcl_Obj *cwdPathPtr;
    ClientData cwdClientData;
    FilesystemRecord *filesystemList;
} ThreadSpecificData;

/* 
 * The internal TclFS API provides routines for handling and
 * manipulating paths efficiently, taking direct advantage of
 * the "path" Tcl_Obj type.
 * 
 * These functions are not exported at all at present.
 */

MODULE_SCOPE int	TclFSCwdPointerEquals _ANSI_ARGS_((
			    Tcl_Obj** pathPtrPtr));
MODULE_SCOPE int	TclFSMakePathFromNormalized _ANSI_ARGS_((
			    Tcl_Interp *interp, Tcl_Obj *pathPtr,
			    ClientData clientData));
MODULE_SCOPE int	TclFSNormalizeToUniquePath _ANSI_ARGS_((
			    Tcl_Interp *interp, Tcl_Obj *pathPtr,
			    int startAt, ClientData *clientDataPtr));
MODULE_SCOPE Tcl_Obj *	TclFSMakePathRelative _ANSI_ARGS_((
			    Tcl_Interp *interp, Tcl_Obj *pathPtr,
			    Tcl_Obj *cwdPtr));
MODULE_SCOPE Tcl_Obj *	TclFSInternalToNormalized _ANSI_ARGS_((
			    Tcl_Filesystem *fromFilesystem,
			    ClientData clientData,
			    FilesystemRecord **fsRecPtrPtr));
MODULE_SCOPE int	TclFSEnsureEpochOk _ANSI_ARGS_((Tcl_Obj* pathPtr,
			    Tcl_Filesystem **fsPtrPtr));
MODULE_SCOPE void	TclFSSetPathDetails _ANSI_ARGS_((Tcl_Obj *pathPtr,
			    FilesystemRecord *fsRecPtr,
			    ClientData clientData ));
MODULE_SCOPE Tcl_Obj *	TclFSNormalizeAbsolutePath _ANSI_ARGS_((
			    Tcl_Interp* interp, Tcl_Obj *pathPtr,
			    ClientData *clientDataPtr));

/* 
 * Private shared variables for use by tclIOUtil.c and tclPathObj.c
 */
MODULE_SCOPE Tcl_Filesystem tclNativeFilesystem;
MODULE_SCOPE Tcl_ThreadDataKey tclFsDataKey;

/* 
 * Private shared functions for use by tclIOUtil.c, tclPathObj.c
 * and tclFileName.c, and any platform-specific filesystem code.
 */
MODULE_SCOPE Tcl_PathType   TclFSGetPathType  _ANSI_ARGS_((
				Tcl_Obj *pathPtr,
				Tcl_Filesystem **filesystemPtrPtr,
				int *driveNameLengthPtr));
MODULE_SCOPE Tcl_PathType   TclFSNonnativePathType  _ANSI_ARGS_((
				CONST char *pathPtr, int pathLen,
				Tcl_Filesystem **filesystemPtrPtr,
				int *driveNameLengthPtr,
				Tcl_Obj **driveNameRef));
MODULE_SCOPE Tcl_PathType   TclGetPathType  _ANSI_ARGS_((
				Tcl_Obj *pathPtr,
				Tcl_Filesystem **filesystemPtrPtr,
				int *driveNameLengthPtr,
				Tcl_Obj **driveNameRef));
MODULE_SCOPE int	    TclFSEpochOk _ANSI_ARGS_((
				int filesystemEpoch));
MODULE_SCOPE int	    TclFSCwdIsNative _ANSI_ARGS_((void));
MODULE_SCOPE Tcl_Obj*	    TclWinVolumeRelativeNormalize _ANSI_ARGS_((
				Tcl_Interp *interp, CONST char *path,
				Tcl_Obj **useThisCwdPtr));

MODULE_SCOPE Tcl_FSPathInFilesystemProc TclNativePathInFilesystem;
MODULE_SCOPE Tcl_FSCreateInternalRepProc TclNativeCreateNativeRep;

#endif /* _TCLFILESYSTEM */
