/* 
 * tclWinFile.c --
 *
 *      This file contains temporary wrappers around UNIX file handling
 *      functions. These wrappers map the UNIX functions to Win32 HANDLE-style
 *      files, which can be manipulated through the Win32 console redirection
 *      interfaces.
 *
 * Copyright (c) 1995-1998 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclWinFile.c,v 1.72 2004/12/01 23:18:55 dgp Exp $
 */

//#define _WIN32_WINNT  0x0500

#include "tclWinInt.h"
#include "tclFileSystem.h"
#include <winioctl.h>
#include <sys/stat.h>
#include <shlobj.h>
#include <lmaccess.h>		/* For TclpGetUserHome(). */

/*
 * The number of 100-ns intervals between the Windows system epoch 
 * (1601-01-01 on the proleptic Gregorian calendar) and the
 * Posix epoch (1970-01-01).
 */

#define POSIX_EPOCH_AS_FILETIME 116444736000000000

/*
 * Declarations for 'link' related information.  This information
 * should come with VC++ 6.0, but is not in some older SDKs.
 * In any case it is not well documented.
 */
#ifndef IO_REPARSE_TAG_RESERVED_ONE
#  define IO_REPARSE_TAG_RESERVED_ONE 0x000000001
#endif
#ifndef IO_REPARSE_TAG_RESERVED_RANGE
#  define IO_REPARSE_TAG_RESERVED_RANGE 0x000000001
#endif
#ifndef IO_REPARSE_TAG_VALID_VALUES
#  define IO_REPARSE_TAG_VALID_VALUES 0x0E000FFFF
#endif
#ifndef IO_REPARSE_TAG_HSM
#  define IO_REPARSE_TAG_HSM 0x0C0000004
#endif
#ifndef IO_REPARSE_TAG_NSS
#  define IO_REPARSE_TAG_NSS 0x080000005
#endif
#ifndef IO_REPARSE_TAG_NSSRECOVER
#  define IO_REPARSE_TAG_NSSRECOVER 0x080000006
#endif
#ifndef IO_REPARSE_TAG_SIS
#  define IO_REPARSE_TAG_SIS 0x080000007
#endif
#ifndef IO_REPARSE_TAG_DFS
#  define IO_REPARSE_TAG_DFS 0x080000008
#endif

#ifndef IO_REPARSE_TAG_RESERVED_ZERO
#  define IO_REPARSE_TAG_RESERVED_ZERO 0x00000000
#endif
#ifndef FILE_FLAG_OPEN_REPARSE_POINT
#  define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#  define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003
#endif
#ifndef IsReparseTagValid
#  define IsReparseTagValid(x) (!((x)&~IO_REPARSE_TAG_VALID_VALUES)&&((x)>IO_REPARSE_TAG_RESERVED_RANGE))
#endif
#ifndef IO_REPARSE_TAG_SYMBOLIC_LINK
#  define IO_REPARSE_TAG_SYMBOLIC_LINK IO_REPARSE_TAG_RESERVED_ZERO
#endif
#ifndef FILE_SPECIAL_ACCESS
#  define FILE_SPECIAL_ACCESS         (FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_SET_REPARSE_POINT
#  define FSCTL_SET_REPARSE_POINT    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 41, METHOD_BUFFERED, FILE_SPECIAL_ACCESS)
#  define FSCTL_GET_REPARSE_POINT    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 42, METHOD_BUFFERED, FILE_ANY_ACCESS) 
#  define FSCTL_DELETE_REPARSE_POINT CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 43, METHOD_BUFFERED, FILE_SPECIAL_ACCESS) 
#endif

/* 
 * Maximum reparse buffer info size. The max user defined reparse
 * data is 16KB, plus there's a header.
 */

#define MAX_REPARSE_SIZE	17000

/*
 * Undocumented REPARSE_MOUNTPOINT_HEADER_SIZE structure definition.
 * This is found in winnt.h.
 * 
 * IMPORTANT: caution when using this structure, since the actual
 * structures used will want to store a full path in the 'PathBuffer'
 * field, but there isn't room (there's only a single WCHAR!).  Therefore
 * one must artificially create a larger space of memory and then cast it
 * to this type.  We use the 'DUMMY_REPARSE_BUFFER' struct just below to
 * deal with this problem.
 */

#define REPARSE_MOUNTPOINT_HEADER_SIZE   8
#ifndef REPARSE_DATA_BUFFER_HEADER_SIZE
typedef struct _REPARSE_DATA_BUFFER {
    DWORD  ReparseTag;
    WORD   ReparseDataLength;
    WORD   Reserved;
    union {
        struct {
            WORD   SubstituteNameOffset;
            WORD   SubstituteNameLength;
            WORD   PrintNameOffset;
            WORD   PrintNameLength;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            WORD   SubstituteNameOffset;
            WORD   SubstituteNameLength;
            WORD   PrintNameOffset;
            WORD   PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            BYTE   DataBuffer[1];
        } GenericReparseBuffer;
    };
} REPARSE_DATA_BUFFER;
#endif

typedef struct {
    REPARSE_DATA_BUFFER dummy;
    WCHAR  dummyBuf[MAX_PATH*3];
} DUMMY_REPARSE_BUFFER;

#if defined(_MSC_VER) && ( _MSC_VER <= 1100 )
#undef  HAVE_NO_FINDEX_ENUMS
#define HAVE_NO_FINDEX_ENUMS
#elif !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0400)
#undef  HAVE_NO_FINDEX_ENUMS
#define HAVE_NO_FINDEX_ENUMS
#endif

#ifdef HAVE_NO_FINDEX_ENUMS
/* These two aren't in VC++ 5.2 headers */
typedef enum _FINDEX_INFO_LEVELS {
	FindExInfoStandard,
	FindExInfoMaxInfoLevel
} FINDEX_INFO_LEVELS;
typedef enum _FINDEX_SEARCH_OPS {
	FindExSearchNameMatch,
	FindExSearchLimitToDirectories,
	FindExSearchLimitToDevices,
	FindExSearchMaxSearchOp
} FINDEX_SEARCH_OPS;
#endif /* HAVE_NO_FINDEX_ENUMS */

/* Other typedefs required by this code */

static time_t		ToCTime(FILETIME fileTime);
static void             FromCTime( time_t posixTime,
				   FILETIME* fileTime );

typedef NET_API_STATUS NET_API_FUNCTION NETUSERGETINFOPROC
	(LPWSTR servername, LPWSTR username, DWORD level, LPBYTE *bufptr);

typedef NET_API_STATUS NET_API_FUNCTION NETAPIBUFFERFREEPROC
	(LPVOID Buffer);

typedef NET_API_STATUS NET_API_FUNCTION NETGETDCNAMEPROC
	(LPWSTR servername, LPWSTR domainname, LPBYTE *bufptr);

/*
 * Declarations for local procedures defined in this file:
 */

static int NativeAccess(CONST TCHAR *path, int mode);
static int NativeStat(CONST TCHAR *path, Tcl_StatBuf *statPtr, int checkLinks);
static unsigned short NativeStatMode(DWORD attr, int checkLinks, int isExec);
static int NativeIsExec(CONST TCHAR *path);
static int NativeReadReparse(CONST TCHAR* LinkDirectory, 
			     REPARSE_DATA_BUFFER* buffer);
static int NativeWriteReparse(CONST TCHAR* LinkDirectory, 
			      REPARSE_DATA_BUFFER* buffer);
static int NativeMatchType(int isDrive, DWORD attr, CONST TCHAR* nativeName, 
			   Tcl_GlobTypeData *types);
static int WinIsDrive(CONST char *name, int nameLen);
static Tcl_Obj* WinReadLink(CONST TCHAR* LinkSource);
static Tcl_Obj* WinReadLinkDirectory(CONST TCHAR* LinkDirectory);
static int WinLink(CONST TCHAR* LinkSource, CONST TCHAR* LinkTarget, 
		   int linkAction);
static int WinSymLinkDirectory(CONST TCHAR* LinkDirectory, 
			       CONST TCHAR* LinkTarget);

/*
 *--------------------------------------------------------------------
 *
 * WinLink
 *
 * Make a link from source to target. 
 *--------------------------------------------------------------------
 */
static int 
WinLink(LinkSource, LinkTarget, linkAction)
    CONST TCHAR* LinkSource;
    CONST TCHAR* LinkTarget;
    int linkAction;
{
    WCHAR	tempFileName[MAX_PATH];
    TCHAR*	tempFilePart;
    int         attr;
    
    /* Get the full path referenced by the target */
    if (!(*tclWinProcs->getFullPathNameProc)(LinkTarget, 
			  MAX_PATH, tempFileName, &tempFilePart)) {
	/* Invalid file */
	TclWinConvertError(GetLastError());
	return -1;
    }

    /* Make sure source file doesn't exist */
    attr = (*tclWinProcs->getFileAttributesProc)(LinkSource);
    if (attr != 0xffffffff) {
	Tcl_SetErrno(EEXIST);
	return -1;
    }

    /* Get the full path referenced by the source file/directory */
    if (!(*tclWinProcs->getFullPathNameProc)(LinkSource, 
			  MAX_PATH, tempFileName, &tempFilePart)) {
	/* Invalid file */
	TclWinConvertError(GetLastError());
	return -1;
    }
    /* Check the target */
    attr = (*tclWinProcs->getFileAttributesProc)(LinkTarget);
    if (attr == 0xffffffff) {
	/* The target doesn't exist */
	TclWinConvertError(GetLastError());
	return -1;
    } else if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
	/* It is a file */
	if (tclWinProcs->createHardLinkProc == NULL) {
	    Tcl_SetErrno(ENOTDIR);
	    return -1;
	}
	if (linkAction & TCL_CREATE_HARD_LINK) {
	    if (!(*tclWinProcs->createHardLinkProc)(LinkSource, LinkTarget, NULL)) {
		TclWinConvertError(GetLastError());
		return -1;
	    }
	    return 0;
	} else if (linkAction & TCL_CREATE_SYMBOLIC_LINK) {
	    /* Can't symlink files */
	    Tcl_SetErrno(ENOTDIR);
	    return -1;
	} else {
	    Tcl_SetErrno(ENODEV);
	    return -1;
	}
    } else {
	if (linkAction & TCL_CREATE_SYMBOLIC_LINK) {
	    return WinSymLinkDirectory(LinkSource, LinkTarget);
	} else if (linkAction & TCL_CREATE_HARD_LINK) {
	    /* Can't hard link directories */
	    Tcl_SetErrno(EISDIR);
	    return -1;
	} else {
	    Tcl_SetErrno(ENODEV);
	    return -1;
	}
    }
}

/*
 *--------------------------------------------------------------------
 *
 * WinReadLink
 *
 * What does 'LinkSource' point to? 
 *--------------------------------------------------------------------
 */
static Tcl_Obj* 
WinReadLink(LinkSource)
    CONST TCHAR* LinkSource;
{
    WCHAR	tempFileName[MAX_PATH];
    TCHAR*	tempFilePart;
    int         attr;
    
    /* Get the full path referenced by the target */
    if (!(*tclWinProcs->getFullPathNameProc)(LinkSource, 
			  MAX_PATH, tempFileName, &tempFilePart)) {
	/* Invalid file */
	TclWinConvertError(GetLastError());
	return NULL;
    }

    /* Make sure source file does exist */
    attr = (*tclWinProcs->getFileAttributesProc)(LinkSource);
    if (attr == 0xffffffff) {
	/* The source doesn't exist */
	TclWinConvertError(GetLastError());
	return NULL;
    } else if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
	/* It is a file - this is not yet supported */
	Tcl_SetErrno(ENOTDIR);
	return NULL;
    } else {
	return WinReadLinkDirectory(LinkSource);
    }
}

/*
 *--------------------------------------------------------------------
 *
 * WinSymLinkDirectory
 *
 * This routine creates a NTFS junction, using the undocumented
 * FSCTL_SET_REPARSE_POINT structure Win2K uses for mount points
 * and junctions.
 *
 * Assumption that LinkTarget is a valid, existing directory.
 * 
 * Returns zero on success.
 *--------------------------------------------------------------------
 */
static int 
WinSymLinkDirectory(LinkDirectory, LinkTarget)
    CONST TCHAR* LinkDirectory;
    CONST TCHAR* LinkTarget;
{
    DUMMY_REPARSE_BUFFER dummy;
    REPARSE_DATA_BUFFER *reparseBuffer = (REPARSE_DATA_BUFFER*)&dummy;
    int         len;
    WCHAR       nativeTarget[MAX_PATH];
    WCHAR       *loop;
    
    /* Make the native target name */
    memcpy((VOID*)nativeTarget, (VOID*)L"\\??\\", 4*sizeof(WCHAR));
    memcpy((VOID*)(nativeTarget + 4), (VOID*)LinkTarget, 
	   sizeof(WCHAR)*(1+wcslen((WCHAR*)LinkTarget)));
    len = wcslen(nativeTarget);
    /* 
     * We must have backslashes only.  This is VERY IMPORTANT.
     * If we have any forward slashes everything appears to work,
     * but the resulting symlink is useless!
     */
    for (loop = nativeTarget; *loop != 0; loop++) {
	if (*loop == L'/') *loop = L'\\';
    }
    if ((nativeTarget[len-1] == L'\\') && (nativeTarget[len-2] != L':')) {
	nativeTarget[len-1] = 0;
    }
    
    /* Build the reparse info */
    memset(reparseBuffer, 0, sizeof(DUMMY_REPARSE_BUFFER));
    reparseBuffer->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    reparseBuffer->SymbolicLinkReparseBuffer.SubstituteNameLength = 
      wcslen(nativeTarget) * sizeof(WCHAR);
    reparseBuffer->Reserved = 0;
    reparseBuffer->SymbolicLinkReparseBuffer.PrintNameLength = 0;
    reparseBuffer->SymbolicLinkReparseBuffer.PrintNameOffset = 
      reparseBuffer->SymbolicLinkReparseBuffer.SubstituteNameLength 
      + sizeof(WCHAR);
    memcpy(reparseBuffer->SymbolicLinkReparseBuffer.PathBuffer, nativeTarget, 
      sizeof(WCHAR) 
      + reparseBuffer->SymbolicLinkReparseBuffer.SubstituteNameLength);
    reparseBuffer->ReparseDataLength = 
      reparseBuffer->SymbolicLinkReparseBuffer.SubstituteNameLength + 12;
	
    return NativeWriteReparse(LinkDirectory, reparseBuffer);
}

/*
 *--------------------------------------------------------------------
 *
 * TclWinSymLinkCopyDirectory
 *
 * Copy a Windows NTFS junction.  This function assumes that
 * LinkOriginal exists and is a valid junction point, and that
 * LinkCopy does not exist.
 * 
 * Returns zero on success.
 *--------------------------------------------------------------------
 */
int 
TclWinSymLinkCopyDirectory(LinkOriginal, LinkCopy)
    CONST TCHAR* LinkOriginal;  /* Existing junction - reparse point */
    CONST TCHAR* LinkCopy;      /* Will become a duplicate junction */
{
    DUMMY_REPARSE_BUFFER dummy;
    REPARSE_DATA_BUFFER *reparseBuffer = (REPARSE_DATA_BUFFER*)&dummy;
    
    if (NativeReadReparse(LinkOriginal, reparseBuffer)) {
	return -1;
    }
    return NativeWriteReparse(LinkCopy, reparseBuffer);
}

/*
 *--------------------------------------------------------------------
 *
 * TclWinSymLinkDelete
 *
 * Delete a Windows NTFS junction.  Once the junction information
 * is deleted, the filesystem object becomes an ordinary directory.
 * Unless 'linkOnly' is given, that directory is also removed.
 * 
 * Assumption that LinkOriginal is a valid, existing junction.
 * 
 * Returns zero on success.
 *--------------------------------------------------------------------
 */
int 
TclWinSymLinkDelete(LinkOriginal, linkOnly)
    CONST TCHAR* LinkOriginal;
    int linkOnly;
{
    /* It is a symbolic link -- remove it */
    DUMMY_REPARSE_BUFFER dummy;
    REPARSE_DATA_BUFFER *reparseBuffer = (REPARSE_DATA_BUFFER*)&dummy;
    HANDLE hFile;
    DWORD returnedLength;
    memset(reparseBuffer, 0, sizeof(DUMMY_REPARSE_BUFFER));
    reparseBuffer->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    hFile = (*tclWinProcs->createFileProc)(LinkOriginal, GENERIC_WRITE, 0,
	NULL, OPEN_EXISTING, 
	FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
	if (!DeviceIoControl(hFile, FSCTL_DELETE_REPARSE_POINT, reparseBuffer, 
			     REPARSE_MOUNTPOINT_HEADER_SIZE,
			     NULL, 0, &returnedLength, NULL)) {	
	    /* Error setting junction */
	    TclWinConvertError(GetLastError());
	    CloseHandle(hFile);
	} else {
	    CloseHandle(hFile);
	    if (!linkOnly) {
	        (*tclWinProcs->removeDirectoryProc)(LinkOriginal);
	    }
	    return 0;
	}
    }
    return -1;
}

/*
 *--------------------------------------------------------------------
 *
 * WinReadLinkDirectory
 *
 * This routine reads a NTFS junction, using the undocumented
 * FSCTL_GET_REPARSE_POINT structure Win2K uses for mount points
 * and junctions.
 *
 * Assumption that LinkDirectory is a valid, existing directory.
 * 
 * Returns a Tcl_Obj with refCount of 1 (i.e. owned by the caller),
 * or NULL if anything went wrong.
 * 
 * In the future we should enhance this to return a path object
 * rather than a string.
 *--------------------------------------------------------------------
 */
static Tcl_Obj* 
WinReadLinkDirectory(LinkDirectory)
    CONST TCHAR* LinkDirectory;
{
    int attr;
    DUMMY_REPARSE_BUFFER dummy;
    REPARSE_DATA_BUFFER *reparseBuffer = (REPARSE_DATA_BUFFER*)&dummy;
    
    attr = (*tclWinProcs->getFileAttributesProc)(LinkDirectory);
    if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
	Tcl_SetErrno(EINVAL);
	return NULL;
    }
    if (NativeReadReparse(LinkDirectory, reparseBuffer)) {
        return NULL;
    }
    
    switch (reparseBuffer->ReparseTag) {
	case 0x80000000|IO_REPARSE_TAG_SYMBOLIC_LINK: 
	case IO_REPARSE_TAG_SYMBOLIC_LINK: 
	case IO_REPARSE_TAG_MOUNT_POINT: {
	    Tcl_Obj *retVal;
	    Tcl_DString ds;
	    CONST char *copy;
	    int len;
	    int offset = 0;
	    
	    /* 
	     * Certain native path representations on Windows have a
	     * special prefix to indicate that they are to be treated
	     * specially.  For example extremely long paths, or symlinks,
	     * or volumes mounted inside directories.
	     * 
	     * There is an assumption in this code that 'wide' interfaces
	     * are being used (see tclWin32Dll.c), which is true for the
	     * only systems which support reparse tags at present.  If
	     * that changes in the future, this code will have to be
	     * generalised.
	     */
	    if (reparseBuffer->SymbolicLinkReparseBuffer.PathBuffer[0] 
		                                                 == L'\\') {
		/* Check whether this is a mounted volume */
		if (wcsncmp(reparseBuffer->SymbolicLinkReparseBuffer.PathBuffer, 
			    L"\\??\\Volume{",11) == 0) {
		    char drive;
		    /* 
		     * There is some confusion between \??\ and \\?\ which
		     * we have to fix here.  It doesn't seem very well
		     * documented.
		     */
		    reparseBuffer->SymbolicLinkReparseBuffer
		                                      .PathBuffer[1] = L'\\';
		    /* 
		     * Check if a corresponding drive letter exists, and
		     * use that if it is found
		     */
		    drive = TclWinDriveLetterForVolMountPoint(reparseBuffer
					->SymbolicLinkReparseBuffer.PathBuffer);
		    if (drive != -1) {
			char driveSpec[3] = {
			    '\0', ':', '\0'
			};
			driveSpec[0] = drive;
			retVal = Tcl_NewStringObj(driveSpec,2);
			Tcl_IncrRefCount(retVal);
			return retVal;
		    }
		    /* 
		     * This is actually a mounted drive, which doesn't
		     * exists as a DOS drive letter.  This means the path
		     * isn't actually a link, although we partially treat
		     * it like one ('file type' will return 'link'), but
		     * then the link will actually just be treated like
		     * an ordinary directory.  I don't believe any
		     * serious inconsistency will arise from this, but it
		     * is something to be aware of.
		     */
		    Tcl_SetErrno(EINVAL);
		    return NULL;
		} else if (wcsncmp(reparseBuffer->SymbolicLinkReparseBuffer
				   .PathBuffer, L"\\\\?\\",4) == 0) {
		    /* Strip off the prefix */
		    offset = 4;
		} else if (wcsncmp(reparseBuffer->SymbolicLinkReparseBuffer
				   .PathBuffer, L"\\??\\",4) == 0) {
		    /* Strip off the prefix */
		    offset = 4;
		}
	    }
	    
	    Tcl_WinTCharToUtf(
		(CONST char*)reparseBuffer->SymbolicLinkReparseBuffer.PathBuffer, 
		(int)reparseBuffer->SymbolicLinkReparseBuffer
		.SubstituteNameLength, &ds);
	
	    copy = Tcl_DStringValue(&ds)+offset;
	    len = Tcl_DStringLength(&ds)-offset;
	    retVal = Tcl_NewStringObj(copy,len);
	    Tcl_IncrRefCount(retVal);
	    Tcl_DStringFree(&ds);
	    return retVal;
	}
    }
    Tcl_SetErrno(EINVAL);
    return NULL;
}

/*
 *--------------------------------------------------------------------
 *
 * NativeReadReparse
 *
 * Read the junction/reparse information from a given NTFS directory.
 *
 * Assumption that LinkDirectory is a valid, existing directory.
 * 
 * Returns zero on success.
 *--------------------------------------------------------------------
 */
static int 
NativeReadReparse(LinkDirectory, buffer)
    CONST TCHAR* LinkDirectory;   /* The junction to read */
    REPARSE_DATA_BUFFER* buffer;  /* Pointer to buffer. Cannot be NULL */
{
    HANDLE hFile;
    DWORD returnedLength;
   
    hFile = (*tclWinProcs->createFileProc)(LinkDirectory, GENERIC_READ, 0,
	NULL, OPEN_EXISTING, 
	FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
	/* Error creating directory */
	TclWinConvertError(GetLastError());
	return -1;
    }
    /* Get the link */
    if (!DeviceIoControl(hFile, FSCTL_GET_REPARSE_POINT, NULL, 
			 0, buffer, sizeof(DUMMY_REPARSE_BUFFER), 
			 &returnedLength, NULL)) {	
	/* Error setting junction */
	TclWinConvertError(GetLastError());
	CloseHandle(hFile);
	return -1;
    }
    CloseHandle(hFile);
    
    if (!IsReparseTagValid(buffer->ReparseTag)) {
	Tcl_SetErrno(EINVAL);
	return -1;
    }
    return 0;
}

/*
 *--------------------------------------------------------------------
 *
 * NativeWriteReparse
 *
 * Write the reparse information for a given directory.
 * 
 * Assumption that LinkDirectory does not exist.
 *--------------------------------------------------------------------
 */
static int 
NativeWriteReparse(LinkDirectory, buffer)
    CONST TCHAR* LinkDirectory;
    REPARSE_DATA_BUFFER* buffer;
{
    HANDLE hFile;
    DWORD returnedLength;
    
    /* Create the directory - it must not already exist */
    if ((*tclWinProcs->createDirectoryProc)(LinkDirectory, NULL) == 0) {
	/* Error creating directory */
	TclWinConvertError(GetLastError());
	return -1;
    }
    hFile = (*tclWinProcs->createFileProc)(LinkDirectory, GENERIC_WRITE, 0,
	NULL, OPEN_EXISTING, 
	FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
	/* Error creating directory */
	TclWinConvertError(GetLastError());
	return -1;
    }
    /* Set the link */
    if (!DeviceIoControl(hFile, FSCTL_SET_REPARSE_POINT, buffer, 
			 (DWORD) buffer->ReparseDataLength 
			 + REPARSE_MOUNTPOINT_HEADER_SIZE,
			 NULL, 0, &returnedLength, NULL)) {	
	/* Error setting junction */
	TclWinConvertError(GetLastError());
	CloseHandle(hFile);
	(*tclWinProcs->removeDirectoryProc)(LinkDirectory);
	return -1;
    }
    CloseHandle(hFile);
    /* We succeeded */
    return 0;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpFindExecutable --
 *
 *	This procedure computes the absolute path name of the current
 *	application.
 *
 * Results:
 * 	None.
 *
 * Side effects:
 *	The computed path is stored.
 *
 *---------------------------------------------------------------------------
 */

void
TclpFindExecutable(argv0)
    CONST char *argv0;		/* The value of the application's argv[0]
				 * (native). */
{
    WCHAR wName[MAX_PATH];
    char name[MAX_PATH * TCL_UTF_MAX];

    /*
     * Under Windows we ignore argv0, and return the path for the file used to
     * create this process.
     */

    if (GetModuleFileNameW(NULL, wName, MAX_PATH) == 0) {
	GetModuleFileNameA(NULL, name, sizeof(name));
	/*
	 * Convert to WCHAR to get out of ANSI codepage
	 */
	MultiByteToWideChar(CP_ACP, 0, name, -1, wName, MAX_PATH);
    }
    WideCharToMultiByte(CP_UTF8, 0, wName, -1, name, sizeof(name), NULL, NULL);
    TclWinNoBackslash(name);
    TclSetObjNameOfExecutable(Tcl_NewStringObj(name, -1), NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * TclpMatchInDirectory --
 *
 *	This routine is used by the globbing code to search a
 *	directory for all files which match a given pattern.
 *
 * Results: 
 *	
 *	The return value is a standard Tcl result indicating whether an
 *	error occurred in globbing.  Errors are left in interp, good
 *	results are lappended to resultPtr (which must be a valid object)
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------- */

int
TclpMatchInDirectory(interp, resultPtr, pathPtr, pattern, types)
    Tcl_Interp *interp;		/* Interpreter to receive errors. */
    Tcl_Obj *resultPtr;		/* List object to lappend results. */
    Tcl_Obj *pathPtr;	        /* Contains path to directory to search. */
    CONST char *pattern;	/* Pattern to match against. */
    Tcl_GlobTypeData *types;	/* Object containing list of acceptable types.
				 * May be NULL. In particular the directory
				 * flag is very important. */
{
    CONST TCHAR *native;

    if (types != NULL && types->type == TCL_GLOB_TYPE_MOUNT) {
	/* The native filesystem never adds mounts */
	return TCL_OK;
    }

    if (pattern == NULL || (*pattern == '\0')) {
	Tcl_Obj *norm = Tcl_FSGetNormalizedPath(NULL, pathPtr);
	if (norm != NULL) {
	    /* Match a single file directly */
	    int len;
	    DWORD attr;
	    CONST char *str = Tcl_GetStringFromObj(norm,&len);

	    native = (CONST TCHAR*) Tcl_FSGetNativePath(pathPtr);
	    
	    if (tclWinProcs->getFileAttributesExProc == NULL) {
		attr = (*tclWinProcs->getFileAttributesProc)(native);
		if (attr == 0xffffffff) {
		    return TCL_OK;
		}
	    } else {
		WIN32_FILE_ATTRIBUTE_DATA data;
		if ((*tclWinProcs->getFileAttributesExProc)(native,
			GetFileExInfoStandard, &data) != TRUE) {
		    return TCL_OK;
		}
		attr = data.dwFileAttributes;
	    }
	    if (NativeMatchType(WinIsDrive(str,len), attr, 
				native, types)) {
		Tcl_ListObjAppendElement(interp, resultPtr, pathPtr);
	    }
	}
	return TCL_OK;
    } else {
	DWORD attr;
	HANDLE handle;
	WIN32_FIND_DATAT data;
	CONST char *dirName;   /* utf-8 dir name, later 
	                        * with pattern appended */
	int dirLength;
	int matchSpecialDots;
	Tcl_DString ds;        /* native encoding of dir, also used
	                        * temporarily for other things. */
	Tcl_DString dsOrig;    /* utf-8 encoding of dir */
	Tcl_Obj *fileNamePtr;
	char lastChar;

	/*
	 * Get the normalized path representation 
	 * (the main thing is we dont want any '~' sequences).
	 */

	fileNamePtr = Tcl_FSGetNormalizedPath(interp, pathPtr);
	if (fileNamePtr == NULL) {
	    return TCL_ERROR;
	}

	/*
	 * Verify that the specified path exists and 
	 * is actually a directory.
	 */
	native = Tcl_FSGetNativePath(pathPtr);
	if (native == NULL) {
	    return TCL_OK;
	}
	attr = (*tclWinProcs->getFileAttributesProc)(native);

	if ((attr == 0xffffffff) || ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0)) {
	    return TCL_OK;
	}

	/* 
	 * Build up the directory name for searching, including
	 * a trailing directory separator.
	 */

	Tcl_DStringInit(&dsOrig);
	dirName = Tcl_GetStringFromObj(fileNamePtr, &dirLength);
	Tcl_DStringAppend(&dsOrig, dirName, dirLength);
	
	lastChar = dirName[dirLength -1];
	if ((lastChar != '\\') && (lastChar != '/') && (lastChar != ':')) {
	    Tcl_DStringAppend(&dsOrig, "/", 1);
	    dirLength++;
	}
	dirName = Tcl_DStringValue(&dsOrig);

	/*
	 * We need to check all files in the directory, so we append
	 * '*.*' to the path, unless the pattern we've been given is
	 * rather simple, when we can use that instead.
	 */

	if (strpbrk(pattern, "[]\\") == NULL) {
	    /* 
	     * The pattern is a simple one containing just '*' and/or '?'.
	     * This means we can get the OS to help us, by passing
	     * it the pattern.
	     */
	    dirName = Tcl_DStringAppend(&dsOrig, pattern, -1);
	} else {
	    dirName = Tcl_DStringAppend(&dsOrig, "*.*", 3);
	}
	native = Tcl_WinUtfToTChar(dirName, -1, &ds);
	if (tclWinProcs->findFirstFileExProc == NULL 
	    || (types == NULL)
	    || (types->type != TCL_GLOB_TYPE_DIR)) {
	    handle = (*tclWinProcs->findFirstFileProc)(native, &data);
	} else {
	    /* We can be more efficient, for pure directory requests */
	    handle = (*tclWinProcs->findFirstFileExProc)(native, 
			FindExInfoStandard, &data, 
			FindExSearchLimitToDirectories, NULL, 0);
	}

	if (handle == INVALID_HANDLE_VALUE) {
	    DWORD err = GetLastError();
	    Tcl_DStringFree(&ds);
	    if (err == ERROR_FILE_NOT_FOUND) {
	        /* 
	         * We used our 'pattern' above, and matched nothing
	         * This means we just return TCL_OK, indicating
	         * no results found.
	         */
		Tcl_DStringFree(&dsOrig);
		return TCL_OK;
	    }
	    TclWinConvertError(err);
	    Tcl_ResetResult(interp);
	    Tcl_AppendResult(interp, "couldn't read directory \"",
		    Tcl_DStringValue(&dsOrig), "\": ", 
		    Tcl_PosixError(interp), (char *) NULL);
	    Tcl_DStringFree(&dsOrig);
	    return TCL_ERROR;
	}
	Tcl_DStringFree(&ds);

	/* 
	 * We may use this later, so we must restore it to its
	 * length including the directory delimiter
	 */
	Tcl_DStringSetLength(&dsOrig, dirLength);

	/*
	 * Check to see if the pattern should match the special
	 * . and .. names, referring to the current directory,
	 * or the directory above.  We need a special check for
	 * this because paths beginning with a dot are not considered
	 * hidden on Windows, and so otherwise a relative glob like
	 * 'glob -join * *' will actually return './. ../..' etc.
	 */

	if ((pattern[0] == '.')
		|| ((pattern[0] == '\\') && (pattern[1] == '.'))) {
	    matchSpecialDots = 1;
	} else {
	    matchSpecialDots = 0;
	}

	/*
	 * Now iterate over all of the files in the directory, starting
	 * with the first one we found.
	 */

	do {
	    CONST char *utfname;
	    int checkDrive = 0;
	    int isDrive;
	    DWORD attr;
	    
	    if (tclWinProcs->useWide) {
		native = (CONST TCHAR *) data.w.cFileName;
		attr = data.w.dwFileAttributes;
	    } else {
		native = (CONST TCHAR *) data.a.cFileName;
		attr = data.a.dwFileAttributes;
	    }
	    
	    utfname = Tcl_WinTCharToUtf(native, -1, &ds);

	    if (!matchSpecialDots) {
		/* If it is exactly '.' or '..' then we ignore it */
		if ((utfname[0] == '.') && (utfname[1] == '\0' 
			|| (utfname[1] == '.' && utfname[2] == '\0'))) {
		    Tcl_DStringFree(&ds);
		    continue;
		}
	    } else if (utfname[0] == '.' && utfname[1] == '.'
		    && utfname[2] == '\0') {
		/* 
		 * Have to check if this is a drive below, so we can
		 * correctly match 'hidden' and not hidden files.
		 */
		checkDrive = 1;
	    }
	    
	    /*
	     * Check to see if the file matches the pattern.  Note that
	     * we are ignoring the case sensitivity flag because Windows
	     * doesn't honor case even if the volume is case sensitive.
	     * If the volume also doesn't preserve case, then we
	     * previously returned the lower case form of the name.  This
	     * didn't seem quite right since there are
	     * non-case-preserving volumes that actually return mixed
	     * case.  So now we are returning exactly what we get from
	     * the system.
	     */

	    if (Tcl_StringCaseMatch(utfname, pattern, 1)) {
		/*
		 * If the file matches, then we need to process the remainder
		 * of the path.
		 */

		if (checkDrive) {
		    CONST char *fullname = Tcl_DStringAppend(&dsOrig, utfname,
			    Tcl_DStringLength(&ds));
		    isDrive = WinIsDrive(fullname, Tcl_DStringLength(&dsOrig));
		    Tcl_DStringSetLength(&dsOrig, dirLength);
		} else {
		    isDrive = 0;
		}
		if (NativeMatchType(isDrive, attr, native, types)) {
		    Tcl_ListObjAppendElement(interp, resultPtr, 
			    TclNewFSPathObj(pathPtr, utfname,
				    Tcl_DStringLength(&ds)));
		}
	    }

	    /*
	     * Free ds here to ensure that native is valid above.
	     */
	    Tcl_DStringFree(&ds);
	} while ((*tclWinProcs->findNextFileProc)(handle, &data) == TRUE);

	FindClose(handle);
	Tcl_DStringFree(&dsOrig);
	return TCL_OK;
    }
}

/* 
 * Does the given path represent a root volume?  We need this special
 * case because for NTFS root volumes, the getFileAttributesProc returns
 * a 'hidden' attribute when it should not.
 */
static int
WinIsDrive(
    CONST char *name,     /* Name (UTF-8) */
    int len)              /* Length of name */
{
    int remove = 0;
    while (len > 4) {
        if ((name[len-1] != '.' || name[len-2] != '.') 
	    || (name[len-3] != '/' && name[len-3] != '\\')) {
            /* We don't have '/..' at the end */
	    if (remove == 0) {
	        break;
	    }
	    remove--;
	    while (len > 0) {
		len--;
		if (name[len] == '/' || name[len] == '\\') {
		    break;
		}
	    }
	    if (len < 4) {
	        len++;
		break;
	    }
        } else {
	    /* We do have '/..' */
	    len -= 3;
	    remove++;
        }
    }
    if (len < 4) {
	if (len == 0) {
	    /* 
	     * Not sure if this is possible, but we pass it on
	     * anyway 
	     */
	} else if (len == 1 && (name[0] == '/' || name[0] == '\\')) {
	    /* Path is pointing to the root volume */
	    return 1;
	} else if ((name[1] == ':') 
		   && (len == 2 || (name[2] == '/' || name[2] == '\\'))) {
	    /* Path is of the form 'x:' or 'x:/' or 'x:\' */
	    return 1;
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 * 
 * NativeMatchType --
 * 
 * This function needs a special case for a path which is a root
 * volume, because for NTFS root volumes, the getFileAttributesProc
 * returns a 'hidden' attribute when it should not.
 * 
 * We never make any calss to a 'get attributes' routine here,
 * since we have arranged things so that our caller already knows
 * such information.
 * 
 * Results:
 *  0 = file doesn't match
 *  1 = file matches
 * 
 *----------------------------------------------------------------------
 */
static int 
NativeMatchType(
    int isDrive,              /* Is this a drive */
    DWORD attr,               /* We already know the attributes 
                               * for the file */
    CONST TCHAR* nativeName,  /* Native path to check */
    Tcl_GlobTypeData *types)  /* Type description to match against */
{
    /*
     * 'attr' represents the attributes of the file, but we only
     * want to retrieve this info if it is absolutely necessary
     * because it is an expensive call.  Unfortunately, to deal
     * with hidden files properly, we must always retrieve it.
     */

    if (types == NULL) {
	/* If invisible, don't return the file */
	if (attr & FILE_ATTRIBUTE_HIDDEN && !isDrive) {
	    return 0;
	}
    } else {
	if (attr & FILE_ATTRIBUTE_HIDDEN && !isDrive) {
	    /* If invisible */
	    if ((types->perm == 0) || 
		    !(types->perm & TCL_GLOB_PERM_HIDDEN)) {
		return 0;
	    }
	} else {
	    /* Visible */
	    if (types->perm & TCL_GLOB_PERM_HIDDEN) {
		return 0;
	    }
	}
	
	if (types->perm != 0) {
	    if (
		((types->perm & TCL_GLOB_PERM_RONLY) &&
			!(attr & FILE_ATTRIBUTE_READONLY)) ||
		((types->perm & TCL_GLOB_PERM_R) &&
			(0 /* File exists => R_OK on Windows */)) ||
		((types->perm & TCL_GLOB_PERM_W) &&
			(attr & FILE_ATTRIBUTE_READONLY)) ||
		((types->perm & TCL_GLOB_PERM_X) &&
			(!(attr & FILE_ATTRIBUTE_DIRECTORY)
			 && !NativeIsExec(nativeName)))
		) {
		return 0;
	    }
	}
	if ((types->type & TCL_GLOB_TYPE_DIR) 
	    && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
	    /* Quicker test for directory, which is a common case */
	    return 1;
	} else if (types->type != 0) {
	    unsigned short st_mode;
	    int isExec = NativeIsExec(nativeName);
	    
	    st_mode = NativeStatMode(attr, 0, isExec);

	    /*
	     * In order bcdpfls as in 'find -t'
	     */
	    if (
		((types->type & TCL_GLOB_TYPE_BLOCK) &&
			S_ISBLK(st_mode)) ||
		((types->type & TCL_GLOB_TYPE_CHAR) &&
			S_ISCHR(st_mode)) ||
		((types->type & TCL_GLOB_TYPE_DIR) &&
			S_ISDIR(st_mode)) ||
		((types->type & TCL_GLOB_TYPE_PIPE) &&
			S_ISFIFO(st_mode)) ||
		((types->type & TCL_GLOB_TYPE_FILE) &&
			S_ISREG(st_mode))
#ifdef S_ISSOCK
		|| ((types->type & TCL_GLOB_TYPE_SOCK) &&
			S_ISSOCK(st_mode))
#endif
		) {
		/* Do nothing -- this file is ok */
	    } else {
#ifdef S_ISLNK
		if (types->type & TCL_GLOB_TYPE_LINK) {
		    st_mode = NativeStatMode(attr, 1, isExec);
		    if (S_ISLNK(st_mode)) {
			return 1;
		    }
		}
#endif
		return 0;
	    }
	}		
    } 
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * TclpGetUserHome --
 *
 *	This function takes the passed in user name and finds the
 *	corresponding home directory specified in the password file.
 *
 * Results:
 *	The result is a pointer to a string specifying the user's home
 *	directory, or NULL if the user's home directory could not be
 *	determined.  Storage for the result string is allocated in
 *	bufferPtr; the caller must call Tcl_DStringFree() when the result
 *	is no longer needed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

char *
TclpGetUserHome(name, bufferPtr)
    CONST char *name;		/* User name for desired home directory. */
    Tcl_DString *bufferPtr;	/* Uninitialized or free DString filled
				 * with name of user's home directory. */
{
    char *result;
    HINSTANCE netapiInst;

    result = NULL;

    Tcl_DStringInit(bufferPtr);

    netapiInst = LoadLibraryA("netapi32.dll");
    if (netapiInst != NULL) {
	NETAPIBUFFERFREEPROC *netApiBufferFreeProc;
	NETGETDCNAMEPROC *netGetDCNameProc;
	NETUSERGETINFOPROC *netUserGetInfoProc;

	netApiBufferFreeProc = (NETAPIBUFFERFREEPROC *)
		GetProcAddress(netapiInst, "NetApiBufferFree");
	netGetDCNameProc = (NETGETDCNAMEPROC *) 
		GetProcAddress(netapiInst, "NetGetDCName");
	netUserGetInfoProc = (NETUSERGETINFOPROC *) 
		GetProcAddress(netapiInst, "NetUserGetInfo");
	if ((netUserGetInfoProc != NULL) && (netGetDCNameProc != NULL)
		&& (netApiBufferFreeProc != NULL)) {
	    USER_INFO_1 *uiPtr;
	    Tcl_DString ds;
	    int nameLen, badDomain;
	    char *domain;
	    WCHAR *wName, *wHomeDir, *wDomain;
	    WCHAR buf[MAX_PATH];

	    badDomain = 0;
	    nameLen = -1;
	    wDomain = NULL;
	    domain = strchr(name, '@');
	    if (domain != NULL) {
		Tcl_DStringInit(&ds);
		wName = Tcl_UtfToUniCharDString(domain + 1, -1, &ds);
		badDomain = (*netGetDCNameProc)(NULL, wName,
			(LPBYTE *) &wDomain);
		Tcl_DStringFree(&ds);
		nameLen = domain - name;
	    }
	    if (badDomain == 0) {
		Tcl_DStringInit(&ds);
		wName = Tcl_UtfToUniCharDString(name, nameLen, &ds);
		if ((*netUserGetInfoProc)(wDomain, wName, 1,
			(LPBYTE *) &uiPtr) == 0) {
		    wHomeDir = uiPtr->usri1_home_dir;
		    if ((wHomeDir != NULL) && (wHomeDir[0] != L'\0')) {
			Tcl_UniCharToUtfDString(wHomeDir, lstrlenW(wHomeDir),
				bufferPtr);
		    } else {
			/* 
			 * User exists but has no home dir.  Return
			 * "{Windows Drive}:/users/default".
			 */

			GetWindowsDirectoryW(buf, MAX_PATH);
			Tcl_UniCharToUtfDString(buf, 2, bufferPtr);
			Tcl_DStringAppend(bufferPtr, "/users/default", -1);
		    }
		    result = Tcl_DStringValue(bufferPtr);
		    (*netApiBufferFreeProc)((void *) uiPtr);
		}
		Tcl_DStringFree(&ds);
	    }
	    if (wDomain != NULL) {
		(*netApiBufferFreeProc)((void *) wDomain);
	    }
	}
	FreeLibrary(netapiInst);
    }
    if (result == NULL) {
	/*
	 * Look in the "Password Lists" section of system.ini for the 
	 * local user.  There are also entries in that section that begin 
	 * with a "*" character that are used by Windows for other 
	 * purposes; ignore user names beginning with a "*".
	 */

	char buf[MAX_PATH];

	if (name[0] != '*') {
	    if (GetPrivateProfileStringA("Password Lists", name, "", buf, 
		    MAX_PATH, "system.ini") > 0) {
		/* 
		 * User exists, but there is no such thing as a home 
		 * directory in system.ini.  Return "{Windows drive}:/".
		 */

		GetWindowsDirectoryA(buf, MAX_PATH);
		Tcl_DStringAppend(bufferPtr, buf, 3);
		result = Tcl_DStringValue(bufferPtr);
	    }
	}
    }

    return result;
}

/*
 *---------------------------------------------------------------------------
 *
 * NativeAccess --
 *
 *	This function replaces the library version of access(), fixing the
 *	following bugs:
 * 
 *	1. access() returns that all files have execute permission.
 *
 * Results:
 *	See access documentation.
 *
 * Side effects:
 *	See access documentation.
 *
 *---------------------------------------------------------------------------
 */

static int
NativeAccess(nativePath, mode)
    CONST TCHAR *nativePath;	/* Path of file to access, native
                            	 * encoding. */
    int mode;			/* Permission setting. */
{
    DWORD attr;

    attr = (*tclWinProcs->getFileAttributesProc)(nativePath);

    if (attr == 0xffffffff) {
	/*
	 * File doesn't exist. 
	 */

	TclWinConvertError(GetLastError());
	return -1;
    }

    if ((mode & W_OK) && (attr & FILE_ATTRIBUTE_READONLY)) {
	/*
	 * File is not writable.
	 */
	Tcl_SetErrno(EACCES);
	return -1;
    }

    if (mode & X_OK) {
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY) && !NativeIsExec(nativePath)) {
	    /*
	     * It's not a directory and doesn't have the correct
	     * extension.  Therefore it can't be executable
	     */
	    Tcl_SetErrno(EACCES);
	    return -1;
	}
    }

    /* 
     * It looks as if the permissions are ok, but if we are on NT, 2000
     * or XP, we have a more complex permissions structure so we try to
     * check that.  The code below is remarkably complex for such a 
     * simple thing as finding what permissions the OS has set for a
     * file.
     * 
     * If we are simply checking for file existence, then we don't
     * need all these complications (which are really quite slow: 
     * with this code 'file readable' is 5-6 times slower than 'file
     * exists').
     */
    
    if ((mode != F_OK) && (tclWinProcs->getFileSecurityProc != NULL)) {
	SECURITY_DESCRIPTOR *sdPtr = NULL;
	unsigned long size;
	GENERIC_MAPPING genMap;
	HANDLE hToken = NULL;
	DWORD desiredAccess = 0;
	DWORD grantedAccess;
	BOOL accessYesNo;
	PRIVILEGE_SET privSet;
	DWORD privSetSize = sizeof(PRIVILEGE_SET);
	int error;
	
	/* 
	 * First find out how big the buffer needs to be 
	 */
	size = 0;
	(*tclWinProcs->getFileSecurityProc)(nativePath, 
		OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION 
		| DACL_SECURITY_INFORMATION, 0, 0, &size);

	/* 
	 * Should have failed with ERROR_INSUFFICIENT_BUFFER 
	 */
	error = GetLastError();
	if (error != ERROR_INSUFFICIENT_BUFFER) {
	    /* 
	     * Most likely case is ERROR_ACCESS_DENIED, which
	     * we will convert to EACCES - just what we want! 
	     */
	    TclWinConvertError(error);
	    return -1;
	}

	/* 
	 * Now size contains the size of buffer needed 
	 */
	sdPtr = (SECURITY_DESCRIPTOR *) HeapAlloc(GetProcessHeap(), 0, size);

	if (sdPtr == NULL) {
	    goto accessError;
	}

	/* 
	 * Call GetFileSecurity() for real 
	 */
	if (!(*tclWinProcs->getFileSecurityProc)(nativePath, 
		OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION 
		| DACL_SECURITY_INFORMATION, sdPtr, size, &size)) {
	    /* 
	     * Error getting owner SD
	     */
	    goto accessError;
	}

	/* 
	 * Perform security impersonation of the user and open the
	 * resulting thread token.
	 */
	if (!(*tclWinProcs->impersonateSelfProc)(SecurityImpersonation)) {
	    /* 
	     * Unable to perform security impersonation. 
	     */
	    goto accessError;
	}
	if (!(*tclWinProcs->openThreadTokenProc)(GetCurrentThread (), 
			TOKEN_DUPLICATE | TOKEN_QUERY, FALSE, &hToken)) {
	    /* 
	     * Unable to get current thread's token. 
	     */
	    goto accessError;
	}
	(*tclWinProcs->revertToSelfProc)();
	
	memset (&genMap, 0x00, sizeof (GENERIC_MAPPING));
	
	/* 
	 * Setup desiredAccess according to the access priveleges we
	 * are checking.
	 */
	genMap.GenericAll = 0;
	if (mode & R_OK) {
	    desiredAccess |= FILE_GENERIC_READ;
	}
	if (mode & W_OK) {
	    desiredAccess |= FILE_GENERIC_WRITE;
	}
	if (mode & X_OK) {
	    desiredAccess |= FILE_GENERIC_EXECUTE;
	}

	/* 
	 * Perform access check using the token. 
	 */
	if (!(*tclWinProcs->accessCheckProc )(sdPtr, hToken, desiredAccess, 
		&genMap, &privSet, &privSetSize, &grantedAccess,
		&accessYesNo)) {
	    /* 
	     * Unable to perform access check. 
	     */
	  accessError:
	    TclWinConvertError(GetLastError());
	    if (sdPtr != NULL) {
	        HeapFree(GetProcessHeap(), 0, sdPtr);
	    }
	    if (hToken != NULL) {
	        CloseHandle(hToken);
	    }
	    return -1;
	}
	/* 
	 * Clean up. 
	 */
	HeapFree(GetProcessHeap (), 0, sdPtr);
	CloseHandle(hToken);
	if (!accessYesNo) {
	    Tcl_SetErrno(EACCES);
	    return -1;
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * NativeIsExec --
 *
 *	Determines if a path is executable.  On windows this is 
 *	simply defined by whether the path ends in any of ".exe",
 *	".com", or ".bat"
 *
 * Results:
 *	1 = executable, 0 = not.
 *
 *----------------------------------------------------------------------
 */
static int
NativeIsExec(nativePath)
    CONST TCHAR *nativePath;
{
    if (tclWinProcs->useWide) {
	CONST WCHAR *path;
	int len;
	
	path = (CONST WCHAR*)nativePath;
	len = wcslen(path);
	
	if (len < 5) {
	    return 0;
	}
	
	if (path[len-4] != L'.') {
	    return 0;
	}
	
	/*
	 * Use wide-char case-insensitive comparison
	 */
	if ((_wcsicmp(path+len-3,L"exe") == 0)
	    || (_wcsicmp(path+len-3,L"com") == 0)
	    || (_wcsicmp(path+len-3,L"bat") == 0)) {
	    return 1;
	}
    } else {
	CONST char *p;
	
	/* We are only looking for pure ascii */
	
	p = strrchr((CONST char*)nativePath, '.');
	if (p != NULL) {
	    p++;
	    /* 
	     * Note: in the old code, stat considered '.pif' files as
	     * executable, whereas access did not.
	     */
	    if ((stricmp(p, "exe") == 0)
		    || (stricmp(p, "com") == 0)
		    || (stricmp(p, "bat") == 0)) {
		/*
		 * File that ends with .exe, .com, or .bat is executable.
		 */

		return 1;
	    }
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TclpObjChdir --
 *
 *	This function replaces the library version of chdir().
 *
 * Results:
 *	See chdir() documentation.
 *
 * Side effects:
 *	See chdir() documentation.  
 *
 *----------------------------------------------------------------------
 */

int 
TclpObjChdir(pathPtr)
    Tcl_Obj *pathPtr; 	/* Path to new working directory. */
{
    int result;
    CONST TCHAR *nativePath;
#ifdef __CYGWIN__
    extern int cygwin_conv_to_posix_path 
	_ANSI_ARGS_((CONST char *, char *));
    char posixPath[MAX_PATH+1];
    CONST char *path;
    Tcl_DString ds;
#endif /* __CYGWIN__ */

    nativePath = (CONST TCHAR *) Tcl_FSGetNativePath(pathPtr);
#ifdef __CYGWIN__
    /* Cygwin chdir only groks POSIX path. */
    path = Tcl_WinTCharToUtf(nativePath, -1, &ds);
    cygwin_conv_to_posix_path(path, posixPath);
    result = (chdir(posixPath) == 0 ? 1 : 0);
    Tcl_DStringFree(&ds);
#else /* __CYGWIN__ */
    result = (*tclWinProcs->setCurrentDirectoryProc)(nativePath);
#endif /* __CYGWIN__ */

    if (result == 0) {
	TclWinConvertError(GetLastError());
	return -1;
    }
    return 0;
}

#ifdef __CYGWIN__
/*
 *---------------------------------------------------------------------------
 *
 * TclpReadlink --
 *
 *     This function replaces the library version of readlink().
 *
 * Results:
 *     The result is a pointer to a string specifying the contents
 *     of the symbolic link given by 'path', or NULL if the symbolic
 *     link could not be read.  Storage for the result string is
 *     allocated in bufferPtr; the caller must call Tcl_DStringFree()
 *     when the result is no longer needed.
 *
 * Side effects:
 *     See readlink() documentation.
 *
 *---------------------------------------------------------------------------
 */

char *
TclpReadlink(path, linkPtr)
    CONST char *path;          /* Path of file to readlink (UTF-8). */
    Tcl_DString *linkPtr;      /* Uninitialized or free DString filled
                                * with contents of link (UTF-8). */
{
    char link[MAXPATHLEN];
    int length;
    char *native;
    Tcl_DString ds;

    native = Tcl_UtfToExternalDString(NULL, path, -1, &ds);
    length = readlink(native, link, sizeof(link));     /* INTL: Native. */
    Tcl_DStringFree(&ds);
    
    if (length < 0) {
	return NULL;
    }

    Tcl_ExternalToUtfDString(NULL, link, length, linkPtr);
    return Tcl_DStringValue(linkPtr);
}
#endif /* __CYGWIN__ */

/*
 *----------------------------------------------------------------------
 *
 * TclpGetCwd --
 *
 *	This function replaces the library version of getcwd().
 *      (Obsolete function, only retained for old extensions which
 *      may call it directly).
 *
 * Results:
 *	The result is a pointer to a string specifying the current
 *	directory, or NULL if the current directory could not be
 *	determined.  If NULL is returned, an error message is left in the
 *	interp's result.  Storage for the result string is allocated in
 *	bufferPtr; the caller must call Tcl_DStringFree() when the result
 *	is no longer needed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

CONST char *
TclpGetCwd(interp, bufferPtr)
    Tcl_Interp *interp;		/* If non-NULL, used for error reporting. */
    Tcl_DString *bufferPtr;	/* Uninitialized or free DString filled
				 * with name of current directory. */
{
    WCHAR buffer[MAX_PATH];
    char *p;

    if ((*tclWinProcs->getCurrentDirectoryProc)(MAX_PATH, buffer) == 0) {
	TclWinConvertError(GetLastError());
	if (interp != NULL) {
	    Tcl_AppendResult(interp,
		    "error getting working directory name: ",
		    Tcl_PosixError(interp), (char *) NULL);
	}
	return NULL;
    }

    /*
     * Watch for the weird Windows c:\\UNC syntax.
     */

    if (tclWinProcs->useWide) {
	WCHAR *native;

	native = (WCHAR *) buffer;
	if ((native[0] != '\0') && (native[1] == ':') 
		&& (native[2] == '\\') && (native[3] == '\\')) {
	    native += 2;
	}
	Tcl_WinTCharToUtf((TCHAR *) native, -1, bufferPtr);
    } else {
	char *native;

	native = (char *) buffer;
	if ((native[0] != '\0') && (native[1] == ':') 
		&& (native[2] == '\\') && (native[3] == '\\')) {
	    native += 2;
	}
	Tcl_WinTCharToUtf((TCHAR *) native, -1, bufferPtr);
    }

    /*
     * Convert to forward slashes for easier use in scripts.
     */
	      
    for (p = Tcl_DStringValue(bufferPtr); *p != '\0'; p++) {
	if (*p == '\\') {
	    *p = '/';
	}
    }
    return Tcl_DStringValue(bufferPtr);
}

int 
TclpObjStat(pathPtr, statPtr)
    Tcl_Obj *pathPtr;          /* Path of file to stat */
    Tcl_StatBuf *statPtr;      /* Filled with results of stat call. */
{
#ifdef OLD_API
    Tcl_Obj *transPtr;
    /*
     * Eliminate file names containing wildcard characters, or subsequent 
     * call to FindFirstFile() will expand them, matching some other file.
     */

    transPtr = Tcl_FSGetTranslatedPath(NULL, pathPtr);
    if (transPtr == NULL || (strpbrk(Tcl_GetString(transPtr), "?*") != NULL)) {
	if (transPtr != NULL) {
	    Tcl_DecrRefCount(transPtr);
	}
	Tcl_SetErrno(ENOENT);
	return -1;
    }
    Tcl_DecrRefCount(transPtr);
#endif
    
    /*
     * Ensure correct file sizes by forcing the OS to write any
     * pending data to disk. This is done only for channels which are
     * dirty, i.e. have been written to since the last flush here.
     */

    TclWinFlushDirtyChannels ();

    return NativeStat((CONST TCHAR*) Tcl_FSGetNativePath(pathPtr), statPtr, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * NativeStat --
 *
 *	This function replaces the library version of stat(), fixing 
 *	the following bugs:
 *
 *	1. stat("c:") returns an error.
 *	2. Borland stat() return time in GMT instead of localtime.
 *	3. stat("\\server\mount") would return error.
 *	4. Accepts slashes or backslashes.
 *	5. st_dev and st_rdev were wrong for UNC paths.
 *
 * Results:
 *	See stat documentation.
 *
 * Side effects:
 *	See stat documentation.
 *
 *----------------------------------------------------------------------
 */

static int 
NativeStat(nativePath, statPtr, checkLinks)
    CONST TCHAR *nativePath;   /* Path of file to stat */
    Tcl_StatBuf *statPtr;      /* Filled with results of stat call. */
    int checkLinks;            /* If non-zero, behave like 'lstat' */
{
    Tcl_DString ds;
    DWORD attr;
    WCHAR nativeFullPath[MAX_PATH];
    TCHAR *nativePart;
    CONST char *fullPath;
    int dev;
    unsigned short mode;
    
    if (tclWinProcs->getFileAttributesExProc == NULL) {
        /* 
         * We don't have the faster attributes proc, so we're
         * probably running on Win95
         */
	WIN32_FIND_DATAT data;
	HANDLE handle;

	handle = (*tclWinProcs->findFirstFileProc)(nativePath, &data);
	if (handle == INVALID_HANDLE_VALUE) {
	    /* 
	     * FindFirstFile() doesn't work on root directories, so call
	     * GetFileAttributes() to see if the specified file exists.
	     */

	    attr = (*tclWinProcs->getFileAttributesProc)(nativePath);
	    if (attr == 0xffffffff) {
		Tcl_SetErrno(ENOENT);
		return -1;
	    }

	    /* 
	     * Make up some fake information for this file.  It has the 
	     * correct file attributes and a time of 0.
	     */

	    memset(&data, 0, sizeof(data));
	    data.a.dwFileAttributes = attr;
	} else {
	    FindClose(handle);
	}

    
	(*tclWinProcs->getFullPathNameProc)(nativePath, MAX_PATH, nativeFullPath,
		&nativePart);

	fullPath = Tcl_WinTCharToUtf((TCHAR *) nativeFullPath, -1, &ds);

	dev = -1;
	if ((fullPath[0] == '\\') && (fullPath[1] == '\\')) {
	    CONST char *p;
	    DWORD dw;
	    CONST TCHAR *nativeVol;
	    Tcl_DString volString;

	    p = strchr(fullPath + 2, '\\');
	    p = strchr(p + 1, '\\');
	    if (p == NULL) {
		/*
		 * Add terminating backslash to fullpath or 
		 * GetVolumeInformation() won't work.
		 */

		fullPath = Tcl_DStringAppend(&ds, "\\", 1);
		p = fullPath + Tcl_DStringLength(&ds);
	    } else {
		p++;
	    }
	    nativeVol = Tcl_WinUtfToTChar(fullPath, p - fullPath, &volString);
	    dw = (DWORD) -1;
	    (*tclWinProcs->getVolumeInformationProc)(nativeVol, NULL, 0, &dw,
		    NULL, NULL, NULL, 0);
	    /*
	     * GetFullPathName() turns special devices like "NUL" into
	     * "\\.\NUL", but GetVolumeInformation() returns failure for
	     * "\\.\NUL".  This will cause "NUL" to get a drive number of
	     * -1, which makes about as much sense as anything since the
	     * special devices don't live on any drive.
	     */

	    dev = dw;
	    Tcl_DStringFree(&volString);
	} else if ((fullPath[0] != '\0') && (fullPath[1] == ':')) {
	    dev = Tcl_UniCharToLower(fullPath[0]) - 'a';
	}
	Tcl_DStringFree(&ds);
	
	attr = data.a.dwFileAttributes;

	statPtr->st_size  = ((Tcl_WideInt)data.a.nFileSizeLow) |
		(((Tcl_WideInt)data.a.nFileSizeHigh) << 32);
	statPtr->st_atime = ToCTime(data.a.ftLastAccessTime);
	statPtr->st_mtime = ToCTime(data.a.ftLastWriteTime);
	statPtr->st_ctime = ToCTime(data.a.ftCreationTime);
    } else {
	WIN32_FILE_ATTRIBUTE_DATA data;
	if((*tclWinProcs->getFileAttributesExProc)(nativePath,
						   GetFileExInfoStandard,
						   &data) != TRUE) {
	    Tcl_SetErrno(ENOENT);
	    return -1;
	}

    
	(*tclWinProcs->getFullPathNameProc)(nativePath, MAX_PATH, 
					    nativeFullPath, &nativePart);

	fullPath = Tcl_WinTCharToUtf((TCHAR *) nativeFullPath, -1, &ds);

	dev = -1;
	if ((fullPath[0] == '\\') && (fullPath[1] == '\\')) {
	    CONST char *p;
	    DWORD dw;
	    CONST TCHAR *nativeVol;
	    Tcl_DString volString;

	    p = strchr(fullPath + 2, '\\');
	    p = strchr(p + 1, '\\');
	    if (p == NULL) {
		/*
		 * Add terminating backslash to fullpath or 
		 * GetVolumeInformation() won't work.
		 */

		fullPath = Tcl_DStringAppend(&ds, "\\", 1);
		p = fullPath + Tcl_DStringLength(&ds);
	    } else {
		p++;
	    }
	    nativeVol = Tcl_WinUtfToTChar(fullPath, p - fullPath, &volString);
	    dw = (DWORD) -1;
	    (*tclWinProcs->getVolumeInformationProc)(nativeVol, NULL, 0, &dw,
		    NULL, NULL, NULL, 0);
	    /*
	     * GetFullPathName() turns special devices like "NUL" into
	     * "\\.\NUL", but GetVolumeInformation() returns failure for
	     * "\\.\NUL".  This will cause "NUL" to get a drive number of
	     * -1, which makes about as much sense as anything since the
	     * special devices don't live on any drive.
	     */

	    dev = dw;
	    Tcl_DStringFree(&volString);
	} else if ((fullPath[0] != '\0') && (fullPath[1] == ':')) {
	    dev = Tcl_UniCharToLower(fullPath[0]) - 'a';
	}
	Tcl_DStringFree(&ds);
	
	attr = data.dwFileAttributes;
	
	statPtr->st_size  = ((Tcl_WideInt)data.nFileSizeLow) |
		(((Tcl_WideInt)data.nFileSizeHigh) << 32);
	statPtr->st_atime = ToCTime(data.ftLastAccessTime);
	statPtr->st_mtime = ToCTime(data.ftLastWriteTime);
	statPtr->st_ctime = ToCTime(data.ftCreationTime);
    }

    mode = NativeStatMode(attr, checkLinks, NativeIsExec(nativePath));
    
    statPtr->st_dev	= (dev_t) dev;
    statPtr->st_ino	= 0;
    statPtr->st_mode	= mode;
    statPtr->st_nlink	= 1;
    statPtr->st_uid	= 0;
    statPtr->st_gid	= 0;
    statPtr->st_rdev	= (dev_t) dev;
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * NativeStatMode --
 *
 *	Calculate just the 'st_mode' field of a 'stat' structure.
 *	
 *	In many places we don't need the full stat structure, and
 *	it's much faster just to calculate these pieces, if that's
 *	all we need.
 *
 *----------------------------------------------------------------------
 */
static unsigned short
NativeStatMode(DWORD attr, int checkLinks, int isExec) 
{
    int mode;
    if (checkLinks && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
	/* It is a link */
	mode = S_IFLNK;
    } else {
	mode  = (attr & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR | S_IEXEC : S_IFREG;
    }
    mode |= (attr & FILE_ATTRIBUTE_READONLY) ? S_IREAD : S_IREAD | S_IWRITE;
    if (isExec) {
	mode |= S_IEXEC;
    }
    
    /*
     * Propagate the S_IREAD, S_IWRITE, S_IEXEC bits to the group and 
     * other positions.
     */

    mode |= (mode & 0x0700) >> 3;
    mode |= (mode & 0x0700) >> 6;
    return (unsigned short)mode;
}

/*
 *------------------------------------------------------------------------
 *
 * ToCTime --
 *
 *	Converts a Windows FILETIME to a time_t in UTC.
 *
 * Results:
 *	Returns the count of seconds from the Posix epoch.
 *
 *------------------------------------------------------------------------
 */

static time_t
ToCTime(FILETIME fileTime)	/* UTC time */
{
    LARGE_INTEGER convertedTime;
    convertedTime.LowPart = fileTime.dwLowDateTime;
    convertedTime.HighPart = (LONG) fileTime.dwHighDateTime;
    return (time_t) ((convertedTime.QuadPart
		      - (Tcl_WideInt) POSIX_EPOCH_AS_FILETIME)
		      / (Tcl_WideInt) 10000000);
}


/*
 *------------------------------------------------------------------------
 *
 * FromCTime --
 *
 *	Converts a time_t to a Windows FILETIME
 *
 * Results:
 *	Returns the count of 100-ns ticks seconds from the Windows epoch.
 *
 *------------------------------------------------------------------------
 */

static void
FromCTime(time_t posixTime,
	  FILETIME* fileTime)	/* UTC Time */
{
    LARGE_INTEGER convertedTime;
    convertedTime.QuadPart = ((LONGLONG) posixTime) * 10000000 
	+ POSIX_EPOCH_AS_FILETIME;
    fileTime->dwLowDateTime = convertedTime.LowPart;
    fileTime->dwHighDateTime = convertedTime.HighPart;
}

#if 0
/*
 *-------------------------------------------------------------------------
 *
 * TclWinResolveShortcut --
 *
 *	Resolve a potential Windows shortcut to get the actual file or 
 *	directory in question.  
 *
 * Results:
 *	Returns 1 if the shortcut could be resolved, or 0 if there was
 *	an error or if the filename was not a shortcut.
 *	If bufferPtr did hold the name of a shortcut, it is modified to
 *	hold the resolved target of the shortcut instead.
 *
 * Side effects:
 *	Loads and unloads OLE package to determine if filename refers to
 *	a shortcut.
 *
 *-------------------------------------------------------------------------
 */

int
TclWinResolveShortcut(bufferPtr)
    Tcl_DString *bufferPtr;	/* Holds name of file to resolve.  On 
				 * return, holds resolved file name. */
{
    HRESULT hres; 
    IShellLink *psl; 
    IPersistFile *ppf; 
    WIN32_FIND_DATA wfd; 
    WCHAR wpath[MAX_PATH];
    char *path, *ext;
    char realFileName[MAX_PATH];

    /*
     * Windows system calls do not automatically resolve
     * shortcuts like UNIX automatically will with symbolic links.
     */

    path = Tcl_DStringValue(bufferPtr);
    ext = strrchr(path, '.');
    if ((ext == NULL) || (stricmp(ext, ".lnk") != 0)) {
	return 0;
    }

    CoInitialize(NULL);
    path = Tcl_DStringValue(bufferPtr);
    realFileName[0] = '\0';
    hres = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, 
	    &IID_IShellLink, &psl); 
    if (SUCCEEDED(hres)) { 
	hres = psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, &ppf);
	if (SUCCEEDED(hres)) { 
	    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, sizeof(wpath));
	    hres = ppf->lpVtbl->Load(ppf, wpath, STGM_READ); 
	    if (SUCCEEDED(hres)) {
		hres = psl->lpVtbl->Resolve(psl, NULL, 
			SLR_ANY_MATCH | SLR_NO_UI); 
		if (SUCCEEDED(hres)) { 
		    hres = psl->lpVtbl->GetPath(psl, realFileName, MAX_PATH, 
			    &wfd, 0);
		} 
	    } 
	    ppf->lpVtbl->Release(ppf); 
	} 
	psl->lpVtbl->Release(psl); 
    } 
    CoUninitialize();

    if (realFileName[0] != '\0') {
	Tcl_DStringSetLength(bufferPtr, 0);
	Tcl_DStringAppend(bufferPtr, realFileName, -1);
	return 1;
    }
    return 0;
}
#endif

/*
 *---------------------------------------------------------------------------
 *
 * TclpGetNativeCwd --
 *
 *	This function replaces the library version of getcwd().
 *
 * Results:
 *	The input and output are filesystem paths in native form.  The
 *	result is either the given clientData, if the working directory
 *	hasn't changed, or a new clientData (owned by our caller),
 *	giving the new native path, or NULL if the current directory
 *	could not be determined.  If NULL is returned, the caller can
 *	examine the standard posix error codes to determine the cause of
 *	the problem.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ClientData
TclpGetNativeCwd(clientData)
    ClientData clientData;
{
    WCHAR buffer[MAX_PATH];
    
    if ((*tclWinProcs->getCurrentDirectoryProc)(MAX_PATH, buffer) == 0) {
	TclWinConvertError(GetLastError());
	return NULL;
    }

    if (clientData != NULL) {
        if (tclWinProcs->useWide) {
	    /* unicode representation when running on NT/2K/XP */
	    if (wcscmp((CONST WCHAR*)clientData, 
		       (CONST WCHAR*)buffer) == 0) {
		return clientData;
	    }
	} else {
	    /* ansi representation when running on 95/98/ME */
	    if (strcmp((CONST char*)clientData, 
		       (CONST char*)buffer) == 0) {
		return clientData;
	    }
	}
    }
    
    return TclNativeDupInternalRep((ClientData)buffer);
}

int 
TclpObjAccess(pathPtr, mode)
    Tcl_Obj *pathPtr;
    int mode;
{
    return NativeAccess((CONST TCHAR*) Tcl_FSGetNativePath(pathPtr), mode);
}

int 
TclpObjLstat(pathPtr, statPtr)
    Tcl_Obj *pathPtr;
    Tcl_StatBuf *statPtr; 
{
    /*
     * Ensure correct file sizes by forcing the OS to write any
     * pending data to disk. This is done only for channels which are
     * dirty, i.e. have been written to since the last flush here.
     */

    TclWinFlushDirtyChannels ();

    return NativeStat((CONST TCHAR*) Tcl_FSGetNativePath(pathPtr), statPtr, 1);
}

#ifdef S_IFLNK

Tcl_Obj* 
TclpObjLink(pathPtr, toPtr, linkAction)
    Tcl_Obj *pathPtr;
    Tcl_Obj *toPtr;
    int linkAction;
{
    if (toPtr != NULL) {
	int res;
#if 0
	TCHAR* LinkTarget = (TCHAR*)Tcl_FSGetNativePath(toPtr);
#else
	TCHAR* LinkTarget = (TCHAR*)Tcl_FSGetNativePath(Tcl_FSGetNormalizedPath(NULL,toPtr));
#endif
	TCHAR* LinkSource = (TCHAR*)Tcl_FSGetNativePath(pathPtr);
	if (LinkSource == NULL || LinkTarget == NULL) {
	    return NULL;
	}
	res = WinLink(LinkSource, LinkTarget, linkAction);
	if (res == 0) {
	    return toPtr;
	} else {
	    return NULL;
	}
    } else {
	TCHAR* LinkSource = (TCHAR*)Tcl_FSGetNativePath(pathPtr);
	if (LinkSource == NULL) {
	    return NULL;
	}
	return WinReadLink(LinkSource);
    }
}

#endif


/*
 *---------------------------------------------------------------------------
 *
 * TclpFilesystemPathType --
 *
 *      This function is part of the native filesystem support, and
 *      returns the path type of the given path.  Returns NTFS or FAT
 *      or whatever is returned by the 'volume information' proc.
 *
 * Results:
 *      NULL at present.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj*
TclpFilesystemPathType(pathPtr)
    Tcl_Obj* pathPtr;
{
#define VOL_BUF_SIZE 32
    int found;
    char volType[VOL_BUF_SIZE];
    char* firstSeparator;
    CONST char *path;
    
    Tcl_Obj *normPath = Tcl_FSGetNormalizedPath(NULL, pathPtr);
    if (normPath == NULL) return NULL;
    path = Tcl_GetString(normPath);
    if (path == NULL) return NULL;
    
    firstSeparator = strchr(path, '/');
    if (firstSeparator == NULL) {
	found = tclWinProcs->getVolumeInformationProc(
		Tcl_FSGetNativePath(pathPtr), NULL, 0, NULL, NULL, 
		NULL, (WCHAR *)volType, VOL_BUF_SIZE);
    } else {
	Tcl_Obj *driveName = Tcl_NewStringObj(path, firstSeparator - path+1);
	Tcl_IncrRefCount(driveName);
	found = tclWinProcs->getVolumeInformationProc(
		Tcl_FSGetNativePath(driveName), NULL, 0, NULL, NULL, 
		NULL, (WCHAR *)volType, VOL_BUF_SIZE);
	Tcl_DecrRefCount(driveName);
    }

    if (found == 0) {
	return NULL;
    } else {
	Tcl_DString ds;
	Tcl_Obj *objPtr;
	
	Tcl_WinTCharToUtf(volType, -1, &ds);
	objPtr = Tcl_NewStringObj(Tcl_DStringValue(&ds),Tcl_DStringLength(&ds));
	Tcl_DStringFree(&ds);
	return objPtr;
    }
#undef VOL_BUF_SIZE
}
/* 
 * This define can be turned on to experiment with a different way of
 * normalizing paths (using a different Windows API).  Unfortunately the
 * new path seems to take almost exactly the same amount of time as the
 * old path!  The primary time taken by normalization is in
 * GetFileAttributesEx/FindFirstFile or
 * GetFileAttributesEx/GetLongPathName.  Conversion to/from native is
 * not a significant factor at all.
 * 
 * Also, since we have to check for symbolic links (reparse points)
 * then we have to call GetFileAttributes on each path segment anyway,
 * so there's no benefit to doing anything clever there.
 */
/* #define TclNORM_LONG_PATH */

/*
 *---------------------------------------------------------------------------
 *
 * TclpObjNormalizePath --
 *
 *	This function scans through a path specification and replaces it,
 *	in place, with a normalized version.  This means using the
 *	'longname', and expanding any symbolic links contained within the
 *	path.
 *
 * Results:
 *	The new 'nextCheckpoint' value, giving as far as we could
 *	understand in the path.
 *
 * Side effects:
 *	The pathPtr string, which must contain a valid path, is
 *	possibly modified in place.
 *
 *---------------------------------------------------------------------------
 */
int
TclpObjNormalizePath(interp, pathPtr, nextCheckpoint)
    Tcl_Interp *interp;
    Tcl_Obj *pathPtr;
    int nextCheckpoint;
{
    char *lastValidPathEnd = NULL;
    /* This will hold the normalized string */
    Tcl_DString dsNorm;
    char *path;
    char *currentPathEndPosition;

    Tcl_DStringInit(&dsNorm);
    path = Tcl_GetString(pathPtr);

    if (TclWinGetPlatformId() == VER_PLATFORM_WIN32_WINDOWS) {
	/* 
	 * We're on Win95, 98 or ME.  There are two assumptions
	 * in this block of code.  First that the native (NULL)
	 * encoding is basically ascii, and second that symbolic
	 * links are not possible.  Both of these assumptions
	 * appear to be true of these operating systems.
	 */
	int isDrive = 1;
	Tcl_DString ds;

	currentPathEndPosition = path + nextCheckpoint;
        if (*currentPathEndPosition == '/') {
	    currentPathEndPosition++;
        }
	while (1) {
	    char cur = *currentPathEndPosition;
	    if ((cur == '/' || cur == 0) && (path != currentPathEndPosition)) {
		/* Reached directory separator, or end of string */
		CONST char *nativePath = Tcl_UtfToExternalDString(NULL, path, 
			    currentPathEndPosition - path, &ds);

		/*
		 * Now we convert the tail of the current path to its
		 * 'long form', and append it to 'dsNorm' which holds
		 * the current normalized path, if the file exists.
		 */
		if (isDrive) {
		    if (GetFileAttributesA(nativePath) 
			== 0xffffffff) {
			/* File doesn't exist */
			Tcl_DStringFree(&ds);
			break;
		    }
		    if (nativePath[0] >= 'a') {
			((char*)nativePath)[0] -= ('a' - 'A');
		    }
		    Tcl_DStringAppend(&dsNorm,nativePath,Tcl_DStringLength(&ds));
		} else {
		    char *checkDots = NULL;
		    
		    if (lastValidPathEnd[1] == '.') {
			checkDots = lastValidPathEnd + 1;
			while (checkDots < currentPathEndPosition) {
			    if (*checkDots != '.') {
				checkDots = NULL;
				break;
			    }
			    checkDots++;
			}
		    }
		    if (checkDots != NULL) {
			int dotLen = currentPathEndPosition - lastValidPathEnd;
			/* 
			 * Path is just dots.  We shouldn't really
			 * ever see a path like that.  However, to be
			 * nice we at least don't mangle the path -- 
			 * we just add the dots as a path segment and
			 * continue
			 */
			Tcl_DStringAppend(&dsNorm, (TCHAR*)(nativePath 
						   + Tcl_DStringLength(&ds)
						   - dotLen), dotLen);
		    } else {
			/* Normal path */
			WIN32_FIND_DATA fData;
			HANDLE handle;
			
			handle = FindFirstFileA(nativePath, &fData);
			if (handle == INVALID_HANDLE_VALUE) {
			    if (GetFileAttributesA(nativePath) 
				== 0xffffffff) {
				/* File doesn't exist */
				Tcl_DStringFree(&ds);
				break;
			    }
			    /* This is usually the '/' in 'c:/' at end of string */
			    Tcl_DStringAppend(&dsNorm,"/", 1);
			} else {
			    char *nativeName;
			    if (fData.cFileName[0] != '\0') {
				nativeName = fData.cFileName;
			    } else {
				nativeName = fData.cAlternateFileName;
			    }
			    FindClose(handle);
			    Tcl_DStringAppend(&dsNorm,"/", 1);
			    Tcl_DStringAppend(&dsNorm,nativeName,-1);
			}
		    }
		}
		Tcl_DStringFree(&ds);
		lastValidPathEnd = currentPathEndPosition;
		if (cur == 0) {
		    break;
		}
		/* 
		 * If we get here, we've got past one directory
		 * delimiter, so we know it is no longer a drive 
		 */
		isDrive = 0;
	    }
	    currentPathEndPosition++;
	}
    } else {
	/* We're on WinNT or 2000 or XP */
	Tcl_Obj *temp = NULL;
	int isDrive = 1;
	Tcl_DString ds;
	
	currentPathEndPosition = path + nextCheckpoint;
	if (*currentPathEndPosition == '/') {
	    currentPathEndPosition++;
	}
	while (1) {
	    char cur = *currentPathEndPosition;
	    if ((cur == '/' || cur == 0) && (path != currentPathEndPosition)) {
		/* Reached directory separator, or end of string */
		WIN32_FILE_ATTRIBUTE_DATA data;
		CONST char *nativePath = Tcl_WinUtfToTChar(path, 
			    currentPathEndPosition - path, &ds);
		if ((*tclWinProcs->getFileAttributesExProc)(nativePath,
		    GetFileExInfoStandard, &data) != TRUE) {
		    /* File doesn't exist */
		    Tcl_DStringFree(&ds);
		    break;
		}

		/* 
		 * File 'nativePath' does exist if we get here.  We
		 * now want to check if it is a symlink and otherwise
		 * continue with the rest of the path.
		 */
		
		/* 
		 * Check for symlinks, except at last component
		 * of path (we don't follow final symlinks). Also
		 * a drive (C:/) for example, may sometimes have
		 * the reparse flag set for some reason I don't
		 * understand.  We therefore don't perform this
		 * check for drives.
		 */
		if (cur != 0 && !isDrive 
		  && (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
		    Tcl_Obj *to = WinReadLinkDirectory(nativePath);
		    if (to != NULL) {
			/* 
			 * Read the reparse point ok.  Now, reparse
			 * points need not be normalized, otherwise
			 * we could use: 
			 * 
			 * Tcl_GetStringFromObj(to, &pathLen); 
			 * nextCheckpoint = pathLen
			 * 
			 * So, instead we have to start from the
			 * beginning.
			 */
			nextCheckpoint = 0;
			Tcl_AppendToObj(to, currentPathEndPosition, -1);
			/* Convert link to forward slashes */
			for (path = Tcl_GetString(to); *path != 0; path++) {
			    if (*path == '\\') *path = '/';
			}
			path = Tcl_GetString(to);
			currentPathEndPosition = path + nextCheckpoint;
			if (temp != NULL) {
			    Tcl_DecrRefCount(temp);
			}
			temp = to;
			/* Reset variables so we can restart normalization */
			isDrive = 1;
			Tcl_DStringFree(&dsNorm);
			Tcl_DStringInit(&dsNorm);
			Tcl_DStringFree(&ds);
			continue;
		    }
		}
#ifndef TclNORM_LONG_PATH
		/*
		 * Now we convert the tail of the current path to its
		 * 'long form', and append it to 'dsNorm' which holds
		 * the current normalized path
		 */
		if (isDrive) {
		    WCHAR drive = ((WCHAR*)nativePath)[0];
		    if (drive >= L'a') {
		        drive -= (L'a' - L'A');
			((WCHAR*)nativePath)[0] = drive;
		    }
		    Tcl_DStringAppend(&dsNorm,nativePath,Tcl_DStringLength(&ds));
		} else {
		    char *checkDots = NULL;
		    
		    if (lastValidPathEnd[1] == '.') {
			checkDots = lastValidPathEnd + 1;
			while (checkDots < currentPathEndPosition) {
			    if (*checkDots != '.') {
				checkDots = NULL;
				break;
			    }
			    checkDots++;
			}
		    }
		    if (checkDots != NULL) {
			int dotLen = currentPathEndPosition - lastValidPathEnd;
			/* 
			 * Path is just dots.  We shouldn't really
			 * ever see a path like that.  However, to be
			 * nice we at least don't mangle the path -- 
			 * we just add the dots as a path segment and
			 * continue
			 */
			Tcl_DStringAppend(&dsNorm,
					  (TCHAR*)((WCHAR*)(nativePath 
						+ Tcl_DStringLength(&ds)) 
						- dotLen),
					  (int)(dotLen * sizeof(WCHAR)));
		    } else {
			/* Normal path */
			WIN32_FIND_DATAW fData;
			HANDLE handle;

			handle = FindFirstFileW((WCHAR*)nativePath, &fData);
			if (handle == INVALID_HANDLE_VALUE) {
			    /* This is usually the '/' in 'c:/' at end of string */
			    Tcl_DStringAppend(&dsNorm,(CONST char*)L"/", 
					      sizeof(WCHAR));
			} else {
			    WCHAR *nativeName;
			    if (fData.cFileName[0] != '\0') {
				nativeName = fData.cFileName;
			    } else {
				nativeName = fData.cAlternateFileName;
			    }
			    FindClose(handle);
			    Tcl_DStringAppend(&dsNorm,(CONST char*)L"/", 
					      sizeof(WCHAR));
			    Tcl_DStringAppend(&dsNorm,(TCHAR*)nativeName, 
					      (int) (wcslen(nativeName)*sizeof(WCHAR)));
			}
		    }
		}
#endif
		Tcl_DStringFree(&ds);
		lastValidPathEnd = currentPathEndPosition;
		if (cur == 0) {
		    break;
		}
		/* 
		 * If we get here, we've got past one directory
		 * delimiter, so we know it is no longer a drive 
		 */
		isDrive = 0;
	    }
	    currentPathEndPosition++;
	}
#ifdef TclNORM_LONG_PATH
	/* 
	 * Convert the entire known path to long form.
	 */
	if (1) {
	    WCHAR wpath[MAX_PATH];
	    DWORD wpathlen;
	    CONST char *nativePath = Tcl_WinUtfToTChar(path, 
					lastValidPathEnd - path, &ds);
	    wpathlen = (*tclWinProcs->getLongPathNameProc)(nativePath, 
							   (TCHAR*)wpath, 
							   MAX_PATH);
	    /* We have to make the drive letter uppercase */
	    if (wpath[0] >= L'a') {
		wpath[0] -= (L'a' - L'A');
	    }
	    Tcl_DStringAppend(&dsNorm, (TCHAR*)wpath, wpathlen*sizeof(WCHAR));
	    Tcl_DStringFree(&ds);
	}
#endif
    }
    /* Common code path for all Windows platforms */
    nextCheckpoint = currentPathEndPosition - path;
    if (lastValidPathEnd != NULL) {
	/* 
	 * Concatenate the normalized string in dsNorm with the
	 * tail of the path which we didn't recognise.  The
	 * string in dsNorm is in the native encoding, so we
	 * have to convert it to Utf.
	 */
	Tcl_DString dsTemp;
	Tcl_WinTCharToUtf(Tcl_DStringValue(&dsNorm), 
			  Tcl_DStringLength(&dsNorm), &dsTemp);
	nextCheckpoint = Tcl_DStringLength(&dsTemp);
	if (*lastValidPathEnd != 0) {
	    /* Not the end of the string */
	    int len;
	    char *path;
	    Tcl_Obj *tmpPathPtr;
	    tmpPathPtr = Tcl_NewStringObj(Tcl_DStringValue(&dsTemp), 
					  nextCheckpoint);
	    Tcl_AppendToObj(tmpPathPtr, lastValidPathEnd, -1);
	    path = Tcl_GetStringFromObj(tmpPathPtr, &len);
	    Tcl_SetStringObj(pathPtr, path, len);
	    Tcl_DecrRefCount(tmpPathPtr);
	} else {
	    /* End of string was reached above */
	    Tcl_SetStringObj(pathPtr, Tcl_DStringValue(&dsTemp),
			     nextCheckpoint);
	}
	Tcl_DStringFree(&dsTemp);
    }
    Tcl_DStringFree(&dsNorm);
    return nextCheckpoint;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclWinVolumeRelativeNormalize --
 *
 *      Only Windows has volume-relative paths.  These paths are rather
 *      rare, but it is nice if Tcl can handle them.  It is much better
 *      if we can handle them here, rather than in the native fs code,
 *      because we really need to have a real absolute path just below.
 * 
 *      We do not let this block compile on non-Windows platforms
 *      because the test suite's manual forcing of tclPlatform can
 *      otherwise cause this code path to be executed, causing various
 *      errors because volume-relative paths really do not exist.
 *
 * Results:
 *      A valid normalized path.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj*
TclWinVolumeRelativeNormalize(interp, path, useThisCwdPtr)
    Tcl_Interp *interp;
    CONST char *path;
    Tcl_Obj **useThisCwdPtr;
{
    Tcl_Obj *absolutePath, *useThisCwd;
    
    useThisCwd = Tcl_FSGetCwd(interp);
    if (useThisCwd == NULL) {
        return NULL;
    }
    
    if (path[0] == '/') {
	/* 
	 * Path of form /foo/bar which is a path in the
	 * root directory of the current volume.
	 */
	CONST char *drive = Tcl_GetString(useThisCwd);
	
	absolutePath = Tcl_NewStringObj(drive,2);
	Tcl_AppendToObj(absolutePath, path, -1);
	Tcl_IncrRefCount(absolutePath);
	/* We have a refCount on the cwd */
    } else {
	/* 
	 * Path of form C:foo/bar, but this only makes
	 * sense if the cwd is also on drive C.
	 */
	
	int cwdLen;
	CONST char *drive = 
	        Tcl_GetStringFromObj(useThisCwd, &cwdLen);
	char drive_cur = path[0];
	
	if (drive_cur >= 'a') {
	    drive_cur -= ('a' - 'A');
	}
	if (drive[0] == drive_cur) {
	    absolutePath = Tcl_DuplicateObj(useThisCwd);
	    /* 
	     * We have a refCount on the cwd, which we
	     * will release later.
	     */

	    if (drive[cwdLen-1] != '/' && (path[2] != '\0')) {
		/* 
		 * Only add a trailing '/' if needed, which
		 * is if there isn't one already, and if we
		 * are going to be adding some more
		 * characters.
		 */
		Tcl_AppendToObj(absolutePath, "/", 1);
	    }
	} else {
	    Tcl_DecrRefCount(useThisCwd);
	    useThisCwd = NULL;
	    /* 
	     * The path is not in the current drive, but
	     * is volume-relative.  The way Tcl 8.3 handles
	     * this is that it treats such a path as
	     * relative to the root of the drive.  We
	     * therefore behave the same here.  This
	     * behaviour is, however, different to that
	     * of the windows command-line.  If we want
	     * to fix this at some point in the future
	     * (at the expense of a behaviour change to
	     * Tcl), we could use the '_dgetdcwd' Win32
	     * API to get the drive's cwd.
	     */
	    absolutePath = Tcl_NewStringObj(path, 2);
	    Tcl_AppendToObj(absolutePath, "/", 1);
	}
	Tcl_IncrRefCount(absolutePath);
	Tcl_AppendToObj(absolutePath, path+2, -1);
    }
    *useThisCwdPtr = useThisCwd;
    return absolutePath;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpNativeToNormalized --
 *
 *      Convert native format to a normalized path object, with refCount
 *      of zero.
 *      
 *      Currently assumes all native paths are actually normalized
 *      already, so if the path given is not normalized this will
 *      actually just convert to a valid string path, but not
 *      necessarily a normalized one.
 *
 * Results:
 *      A valid normalized path.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj* 
TclpNativeToNormalized(clientData)
    ClientData clientData;
{
    Tcl_DString ds;
    Tcl_Obj *objPtr;
    int len;
    
    char *copy;
    char *p;
    Tcl_WinTCharToUtf((CONST char*)clientData, -1, &ds);
    
    copy = Tcl_DStringValue(&ds);
    len = Tcl_DStringLength(&ds);

    /* 
     * Certain native path representations on Windows have this special
     * prefix to indicate that they are to be treated specially.  For
     * example extremely long paths, or symlinks 
     */
    if (*copy == '\\') {
	if (0 == strncmp(copy,"\\??\\",4)) {
	    copy += 4;
	    len -= 4;
	} else if (0 == strncmp(copy,"\\\\?\\",4)) {
	    copy += 4;
	    len -= 4;
	}
    }
    /* 
     * Ensure we are using forward slashes only.
     */
    for (p = copy; *p != '\0'; p++) {
	if (*p == '\\') {
	    *p = '/';
	}
    }

    objPtr = Tcl_NewStringObj(copy,len);
    Tcl_DStringFree(&ds);
    
    return objPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclNativeCreateNativeRep --
 *
 *      Create a native representation for the given path.
 *
 * Results:
 *      The nativePath representation.
 *
 * Side effects:
 *	Memory will be allocated.  The path may need to be normalized.
 *
 *---------------------------------------------------------------------------
 */
ClientData 
TclNativeCreateNativeRep(pathPtr)
    Tcl_Obj* pathPtr;
{
    char *nativePathPtr;
    Tcl_DString ds;
    Tcl_Obj* validPathPtr;
    int len;
    char *str;

    if (TclFSCwdIsNative()) {
	/* 
	 * The cwd is native, which means we can use the translated
	 * path without worrying about normalization (this will also
	 * usually be shorter so the utf-to-external conversion will
	 * be somewhat faster).
	 */
	validPathPtr = Tcl_FSGetTranslatedPath(NULL, pathPtr);
    } else {
	/* Make sure the normalized path is set */
	validPathPtr = Tcl_FSGetNormalizedPath(NULL, pathPtr);
	Tcl_IncrRefCount(validPathPtr);
    }

    str = Tcl_GetStringFromObj(validPathPtr, &len);
    Tcl_WinUtfToTChar(str, len, &ds);
    if (tclWinProcs->useWide) {
	len = Tcl_DStringLength(&ds) + sizeof(WCHAR);
    } else {
	len = Tcl_DStringLength(&ds) + sizeof(char);
    }
    Tcl_DecrRefCount(validPathPtr);
    nativePathPtr = ckalloc((unsigned) len);
    memcpy((VOID*)nativePathPtr, (VOID*)Tcl_DStringValue(&ds), (size_t) len);
	  
    Tcl_DStringFree(&ds);
    return (ClientData)nativePathPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclNativeDupInternalRep --
 *
 *      Duplicate the native representation.
 *
 * Results:
 *      The copied native representation, or NULL if it is not possible
 *      to copy the representation.
 *
 * Side effects:
 *	Memory allocation for the copy.
 *
 *---------------------------------------------------------------------------
 */
ClientData 
TclNativeDupInternalRep(clientData)
    ClientData clientData;
{
    char *copy;
    size_t len;

    if (clientData == NULL) {
	return NULL;
    }

    if (tclWinProcs->useWide) {
	/* unicode representation when running on NT/2K/XP */
	len = sizeof(WCHAR) + (wcslen((CONST WCHAR*)clientData) * sizeof(WCHAR));
    } else {
	/* ansi representation when running on 95/98/ME */
	len = sizeof(char) + (strlen((CONST char*)clientData) * sizeof(char));
    }
    
    copy = (char *) ckalloc(len);
    memcpy((VOID*)copy, (VOID*)clientData, len);
    return (ClientData)copy;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpUtime --
 *
 *	Set the modification date for a file.
 *
 * Results:
 *	0 on success, -1 on error.
 *
 * Side effects:
 *	Sets errno to a representation of any Windows problem that's
 *	observed in the process.
 *
 *---------------------------------------------------------------------------
 */
int
TclpUtime(pathPtr, tval)
    Tcl_Obj *pathPtr;      /* File to modify */
    struct utimbuf *tval;  /* New modification date structure */
{
    int res = 0;
    HANDLE fileHandle;
    FILETIME lastAccessTime, lastModTime;
    
    FromCTime(tval->actime, &lastAccessTime);
    FromCTime(tval->modtime, &lastModTime);
    
    /*
     * We use the native APIs (not 'utime') because there are
     * some daylight savings complications that utime gets wrong.
     */
    fileHandle = (tclWinProcs->createFileProc) (
         (CONST TCHAR*) Tcl_FSGetNativePath(pathPtr), 
	 FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 
	 FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (fileHandle == INVALID_HANDLE_VALUE
      || !SetFileTime(fileHandle, NULL, &lastAccessTime, &lastModTime)) {
	TclWinConvertError(GetLastError());
	res = -1;
    }
    if (fileHandle != INVALID_HANDLE_VALUE) {
	CloseHandle(fileHandle);
    }
    return res;
}
