/* 
 * tclEvent.c --
 *
 *	This file implements some general event related interfaces including
 *	background errors, exit handlers, and the "vwait" and "update"
 *	command procedures. 
 *
 * Copyright (c) 1990-1994 The Regents of the University of California.
 * Copyright (c) 1994-1998 Sun Microsystems, Inc.
 * Copyright (c) 2004 by Zoran Vasiljevic.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclEvent.c,v 1.55 2004/12/04 21:19:18 dgp Exp $
 */

#include "tclInt.h"

/*
 * The data structure below is used to report background errors.  One
 * such structure is allocated for each error;  it holds information
 * about the interpreter and the error until an idle handler command
 * can be invoked.
 */

typedef struct BgError {
    Tcl_Obj *errorMsg;		/* Copy of the error message (the interp's
				 * result when the error occurred). */
    Tcl_Obj *returnOpts;	/* Active return options when the
				 * error occurred */
    struct BgError *nextPtr;	/* Next in list of all pending error
				 * reports for this interpreter, or NULL
				 * for end of list. */
} BgError;

/*
 * One of the structures below is associated with the "tclBgError"
 * assoc data for each interpreter.  It keeps track of the head and
 * tail of the list of pending background errors for the interpreter.
 */

typedef struct ErrAssocData {
    Tcl_Interp *interp;		/* Interpreter in which error occurred. */
    Tcl_Obj *cmdPrefix;		/* First word(s) of the handler command */
    BgError *firstBgPtr;	/* First in list of all background errors
				 * waiting to be processed for this
				 * interpreter (NULL if none). */
    BgError *lastBgPtr;		/* Last in list of all background errors
				 * waiting to be processed for this
				 * interpreter (NULL if none). */
} ErrAssocData;

/*
 * For each exit handler created with a call to Tcl_CreateExitHandler
 * there is a structure of the following type:
 */

typedef struct ExitHandler {
    Tcl_ExitProc *proc;		/* Procedure to call when process exits. */
    ClientData clientData;	/* One word of information to pass to proc. */
    struct ExitHandler *nextPtr;/* Next in list of all exit handlers for
				 * this application, or NULL for end of list. */
} ExitHandler;

/*
 * There is both per-process and per-thread exit handlers.
 * The first list is controlled by a mutex.  The other is in
 * thread local storage.
 */

static ExitHandler *firstExitPtr = NULL;
				/* First in list of all exit handlers for
				 * application. */
TCL_DECLARE_MUTEX(exitMutex)

/*
 * This variable is set to 1 when Tcl_Finalize is called, and at the end of
 * its work, it is reset to 0. The variable is checked by TclInExit() to
 * allow different behavior for exit-time processing, e.g. in closing of
 * files and pipes.
 */

static int inFinalize = 0;
static int subsystemsInitialized = 0;

/*
 * This variable contains the application wide exit handler.  It will be
 * called by Tcl_Exit instead of the C-runtime exit if this variable is set
 * to a non-NULL value.
 */

static Tcl_ExitProc *appExitPtr = NULL;

typedef struct ThreadSpecificData {
    ExitHandler *firstExitPtr;  /* First in list of all exit handlers for
				 * this thread. */
    int inExit;			/* True when this thread is exiting. This
				 * is used as a hack to decide to close
				 * the standard channels. */
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

#ifdef TCL_THREADS

typedef struct {
    Tcl_ThreadCreateProc *proc;	/* Main() function of the thread */
    ClientData clientData;	/* The one argument to Main() */
} ThreadClientData;
static Tcl_ThreadCreateType NewThreadProc _ANSI_ARGS_((
           ClientData clientData));
#endif

/*
 * Prototypes for procedures referenced only in this file:
 */

static void		BgErrorDeleteProc _ANSI_ARGS_((ClientData clientData,
			    Tcl_Interp *interp));
static void		HandleBgErrors _ANSI_ARGS_((ClientData clientData));
static char *		VwaitVarProc _ANSI_ARGS_((ClientData clientData,
			    Tcl_Interp *interp, CONST char *name1, 
			    CONST char *name2, int flags));

/*
 *----------------------------------------------------------------------
 *
 * Tcl_BackgroundError --
 *
 *	This procedure is invoked to handle errors that occur in Tcl
 *	commands that are invoked in "background" (e.g. from event or
 *	timer bindings).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A handler command is invoked later as an idle handler to
 *	process the error, passing it the interp result and return
 *	options.  
 *
 *----------------------------------------------------------------------
 */

void
Tcl_BackgroundError(interp)
    Tcl_Interp *interp;		/* Interpreter in which an error has
				 * occurred. */
{
    BgError *errPtr;
    ErrAssocData *assocPtr;

    errPtr = (BgError *) ckalloc(sizeof(BgError));
    errPtr->errorMsg = Tcl_GetObjResult(interp);
    Tcl_IncrRefCount(errPtr->errorMsg);
    errPtr->returnOpts = Tcl_GetReturnOptions(interp, TCL_ERROR);
    Tcl_IncrRefCount(errPtr->returnOpts);
    errPtr->nextPtr = NULL;

    (void) TclGetBgErrorHandler(interp);
    assocPtr = (ErrAssocData *) Tcl_GetAssocData(interp, "tclBgError",
	    (Tcl_InterpDeleteProc **) NULL);
    if (assocPtr->firstBgPtr == NULL) {
	assocPtr->firstBgPtr = errPtr;
	Tcl_DoWhenIdle(HandleBgErrors, (ClientData) assocPtr);
    } else {
	assocPtr->lastBgPtr->nextPtr = errPtr;
    }
    assocPtr->lastBgPtr = errPtr;
    Tcl_ResetResult(interp);
}

/*
 *----------------------------------------------------------------------
 *
 * HandleBgErrors --
 *
 *	This procedure is invoked as an idle handler to process all of
 *	the accumulated background errors.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Depends on what actions the handler command takes for the errors.
 *
 *----------------------------------------------------------------------
 */

static void
HandleBgErrors(clientData)
    ClientData clientData;	/* Pointer to ErrAssocData structure. */
{
    ErrAssocData *assocPtr = (ErrAssocData *) clientData;
    Tcl_Interp *interp = assocPtr->interp;
    BgError *errPtr;

    /*
     * Not bothering to save/restore the interp state.  Assume that
     * any code that has interp state it needs to keep will make
     * its own Tcl_SaveInterpState call before calling something like
     * Tcl_DoOneEvent() that could lead us here.
     */

    Tcl_Preserve((ClientData) assocPtr);
    Tcl_Preserve((ClientData) interp);
    while (assocPtr->firstBgPtr != NULL) {
	int code, prefixObjc;
	Tcl_Obj **prefixObjv, **tempObjv;

	errPtr = assocPtr->firstBgPtr;

	Tcl_IncrRefCount(assocPtr->cmdPrefix);
	Tcl_ListObjGetElements(NULL, assocPtr->cmdPrefix,
		&prefixObjc, &prefixObjv);
	tempObjv = (Tcl_Obj **) ckalloc((prefixObjc+2)*sizeof(Tcl_Obj *));
	memcpy(tempObjv, prefixObjv, prefixObjc*sizeof(Tcl_Obj *));
	tempObjv[prefixObjc] = errPtr->errorMsg;
	tempObjv[prefixObjc+1] = errPtr->returnOpts;
	Tcl_AllowExceptions(interp);
	code = Tcl_EvalObjv(interp, prefixObjc+2, tempObjv, TCL_EVAL_GLOBAL);

	/*
	 * Discard the command and the information about the error report.
	 */

	Tcl_DecrRefCount(assocPtr->cmdPrefix);
	Tcl_DecrRefCount(errPtr->errorMsg);
	Tcl_DecrRefCount(errPtr->returnOpts);
	assocPtr->firstBgPtr = errPtr->nextPtr;
	ckfree((char *) errPtr);

	if (code == TCL_BREAK) {
	    /*
	     * Break means cancel any remaining error reports for this
	     * interpreter.
	     */
	    while (assocPtr->firstBgPtr != NULL) {
		errPtr = assocPtr->firstBgPtr;
		assocPtr->firstBgPtr = errPtr->nextPtr;
		Tcl_DecrRefCount(errPtr->errorMsg);
		Tcl_DecrRefCount(errPtr->returnOpts);
		ckfree((char *) errPtr);
	    }
	} else if ((code == TCL_ERROR) && !Tcl_IsSafe(interp)) {
	    Tcl_Channel errChannel = Tcl_GetStdChannel(TCL_STDERR);
	    if (errChannel != (Tcl_Channel) NULL) {
		Tcl_Obj *options = Tcl_GetReturnOptions(interp, code);
		Tcl_Obj *keyPtr = Tcl_NewStringObj("-errorinfo", -1);
		Tcl_Obj *valuePtr;

		Tcl_IncrRefCount(keyPtr);
		Tcl_DictObjGet(NULL, options, keyPtr, &valuePtr);
		Tcl_DecrRefCount(keyPtr);

		Tcl_WriteChars(errChannel,
			"error in background error handler:\n", -1);
		if (valuePtr) {
		    Tcl_WriteObj(errChannel, valuePtr);
		} else {
		    Tcl_WriteObj(errChannel, Tcl_GetObjResult(interp));
		}
		Tcl_WriteChars(errChannel, "\n", 1);
                Tcl_Flush(errChannel);
	    }
	}
    }
    assocPtr->lastBgPtr = NULL;
    Tcl_Release((ClientData) interp);
    Tcl_Release((ClientData) assocPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TclDefaultBgErrorHandlerObjCmd --
 *
 *	This procedure is invoked to process the "::tcl::Bgerror" Tcl
 *	command.  It is the default handler command registered with
 *	[interp bgerror] for the sake of compatibility with older Tcl
 *	releases.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	Depends on what actions the "bgerror" command takes for the errors.
 *
 *----------------------------------------------------------------------
 */

int
TclDefaultBgErrorHandlerObjCmd(dummy, interp, objc, objv)
    ClientData dummy;           /* Not used. */
    Tcl_Interp *interp;         /* Current interpreter. */
    int objc;                   /* Number of arguments. */
    Tcl_Obj *CONST objv[];      /* Argument objects. */
{
    Tcl_Obj *keyPtr, *valuePtr;
    Tcl_Obj *tempObjv[2];
    int code;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "msg options");
	return TCL_ERROR;
    }

    /*
     * Restore important state variables to what they were at
     * the time the error occurred.
     *
     * Need to set the variables, not the interp fields, because
     * Tcl_EvalObjv() calls Tcl_ResetResult() which would destroy
     * anything we write to the interp fields.
     */

    keyPtr = Tcl_NewStringObj("-errorcode", -1);
    Tcl_IncrRefCount(keyPtr);
    Tcl_DictObjGet(NULL, objv[2], keyPtr, &valuePtr);
    Tcl_DecrRefCount(keyPtr);
    if (valuePtr) {
	Tcl_SetVar2Ex(interp, "errorCode", NULL, valuePtr, TCL_GLOBAL_ONLY);
    }

    keyPtr = Tcl_NewStringObj("-errorinfo", -1);
    Tcl_IncrRefCount(keyPtr);
    Tcl_DictObjGet(NULL, objv[2], keyPtr, &valuePtr);
    Tcl_DecrRefCount(keyPtr);
    if (valuePtr) {
	Tcl_SetVar2Ex(interp, "errorInfo", NULL, valuePtr, TCL_GLOBAL_ONLY);
    }

    /* Create and invoke the bgerror command. */

    tempObjv[0] = Tcl_NewStringObj("bgerror", -1);
    Tcl_IncrRefCount(tempObjv[0]);
    tempObjv[1] = objv[1];
    Tcl_AllowExceptions(interp);
    code = Tcl_EvalObjv(interp, 2, tempObjv, TCL_EVAL_GLOBAL);
    if (code == TCL_ERROR) {
        /*
         * If the interpreter is safe, we look for a hidden command
         * named "bgerror" and call that with the error information.
         * Otherwise, simply ignore the error. The rationale is that
         * this could be an error caused by a malicious applet trying
         * to cause an infinite barrage of error messages. The hidden
         * "bgerror" command can be used by a security policy to
         * interpose on such attacks and e.g. kill the applet after a
         * few attempts.
         */
	if (Tcl_IsSafe(interp)) {
	    Tcl_ResetResult(interp);
	    TclObjInvoke(interp, 2, tempObjv, TCL_INVOKE_HIDDEN);
	} else {
	    Tcl_Channel errChannel = Tcl_GetStdChannel(TCL_STDERR);
	    if (errChannel != (Tcl_Channel) NULL) {
		Tcl_Obj *resultPtr = Tcl_GetObjResult(interp);

		Tcl_IncrRefCount(resultPtr);
		if (Tcl_FindCommand(interp, "bgerror",
			    NULL, TCL_GLOBAL_ONLY) == NULL) {
		    if (valuePtr) {
			Tcl_WriteObj(errChannel, valuePtr);
			Tcl_WriteChars(errChannel, "\n", -1);
		    }
                } else {
		    Tcl_WriteChars(errChannel,
			    "bgerror failed to handle background error.\n", -1);
		    Tcl_WriteChars(errChannel, "    Original error: ", -1);
		    Tcl_WriteObj(errChannel, objv[1]);
		    Tcl_WriteChars(errChannel, "\n", -1);
		    Tcl_WriteChars(errChannel,
				"    Error in bgerror: ", -1);
		    Tcl_WriteObj(errChannel, resultPtr);
		    Tcl_WriteChars(errChannel, "\n", -1);
                }
		Tcl_DecrRefCount(resultPtr);
                Tcl_Flush(errChannel);
	    }
	}
	code = TCL_OK;
    }
    Tcl_DecrRefCount(tempObjv[0]);
    Tcl_ResetResult(interp);
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclSetBgErrorHandler --
 *
 *	This procedure sets the command prefix to be used to handle
 *	background errors in interp.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Error handler is registered.
 *
 *----------------------------------------------------------------------
 */

void
TclSetBgErrorHandler(interp, cmdPrefix)
    Tcl_Interp *interp;
    Tcl_Obj *cmdPrefix;
{
    ErrAssocData *assocPtr = (ErrAssocData *) Tcl_GetAssocData(interp,
	    "tclBgError", (Tcl_InterpDeleteProc **) NULL);

    if (cmdPrefix == NULL) {
	Tcl_Panic("TclSetBgErrorHandler: NULL cmdPrefix argument");
    }
    if (assocPtr == NULL) {
	/* First access: initialize */
	assocPtr = (ErrAssocData *) ckalloc(sizeof(ErrAssocData));
	assocPtr->interp = interp;
	assocPtr->cmdPrefix = NULL;
	assocPtr->firstBgPtr = NULL;
	assocPtr->lastBgPtr = NULL;
	Tcl_SetAssocData(interp, "tclBgError", BgErrorDeleteProc,
		(ClientData) assocPtr);
    }
    if (assocPtr->cmdPrefix) {
	Tcl_DecrRefCount(assocPtr->cmdPrefix);
    }
    assocPtr->cmdPrefix = cmdPrefix;
    Tcl_IncrRefCount(assocPtr->cmdPrefix);
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetBgErrorHandler --
 *
 *	This procedure retrieves the command prefix currently used
 *	to handle background errors in interp.
 *
 * Results:
 *	A (Tcl_Obj *) to a list of words (command prefix).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclGetBgErrorHandler(interp)
    Tcl_Interp *interp;
{
    ErrAssocData *assocPtr = (ErrAssocData *) Tcl_GetAssocData(interp,
	    "tclBgError", (Tcl_InterpDeleteProc **) NULL);

    if (assocPtr == NULL) {
	TclSetBgErrorHandler(interp, Tcl_NewStringObj("::tcl::Bgerror", -1));
	assocPtr = (ErrAssocData *) Tcl_GetAssocData(interp,
	    "tclBgError", (Tcl_InterpDeleteProc **) NULL);
    }
    return assocPtr->cmdPrefix;
}

/*
 *----------------------------------------------------------------------
 *
 * BgErrorDeleteProc --
 *
 *	This procedure is associated with the "tclBgError" assoc data
 *	for an interpreter;  it is invoked when the interpreter is
 *	deleted in order to free the information assoicated with any
 *	pending error reports.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Background error information is freed: if there were any
 *	pending error reports, they are cancelled.
 *
 *----------------------------------------------------------------------
 */

static void
BgErrorDeleteProc(clientData, interp)
    ClientData clientData;	/* Pointer to ErrAssocData structure. */
    Tcl_Interp *interp;		/* Interpreter being deleted. */
{
    ErrAssocData *assocPtr = (ErrAssocData *) clientData;
    BgError *errPtr;

    while (assocPtr->firstBgPtr != NULL) {
	errPtr = assocPtr->firstBgPtr;
	assocPtr->firstBgPtr = errPtr->nextPtr;
	Tcl_DecrRefCount(errPtr->errorMsg);
	Tcl_DecrRefCount(errPtr->returnOpts);
	ckfree((char *) errPtr);
    }
    Tcl_CancelIdleCall(HandleBgErrors, (ClientData) assocPtr);
    Tcl_DecrRefCount(assocPtr->cmdPrefix);
    Tcl_EventuallyFree((ClientData) assocPtr, TCL_DYNAMIC);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateExitHandler --
 *
 *	Arrange for a given procedure to be invoked just before the
 *	application exits.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Proc will be invoked with clientData as argument when the
 *	application exits.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_CreateExitHandler(proc, clientData)
    Tcl_ExitProc *proc;		/* Procedure to invoke. */
    ClientData clientData;	/* Arbitrary value to pass to proc. */
{
    ExitHandler *exitPtr;

    exitPtr = (ExitHandler *) ckalloc(sizeof(ExitHandler));
    exitPtr->proc = proc;
    exitPtr->clientData = clientData;
    Tcl_MutexLock(&exitMutex);
    exitPtr->nextPtr = firstExitPtr;
    firstExitPtr = exitPtr;
    Tcl_MutexUnlock(&exitMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteExitHandler --
 *
 *	This procedure cancels an existing exit handler matching proc
 *	and clientData, if such a handler exits.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If there is an exit handler corresponding to proc and clientData
 *	then it is cancelled;  if no such handler exists then nothing
 *	happens.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DeleteExitHandler(proc, clientData)
    Tcl_ExitProc *proc;		/* Procedure that was previously registered. */
    ClientData clientData;	/* Arbitrary value to pass to proc. */
{
    ExitHandler *exitPtr, *prevPtr;

    Tcl_MutexLock(&exitMutex);
    for (prevPtr = NULL, exitPtr = firstExitPtr; exitPtr != NULL;
	    prevPtr = exitPtr, exitPtr = exitPtr->nextPtr) {
	if ((exitPtr->proc == proc)
		&& (exitPtr->clientData == clientData)) {
	    if (prevPtr == NULL) {
		firstExitPtr = exitPtr->nextPtr;
	    } else {
		prevPtr->nextPtr = exitPtr->nextPtr;
	    }
	    ckfree((char *) exitPtr);
	    break;
	}
    }
    Tcl_MutexUnlock(&exitMutex);
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateThreadExitHandler --
 *
 *	Arrange for a given procedure to be invoked just before the
 *	current thread exits.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Proc will be invoked with clientData as argument when the
 *	application exits.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_CreateThreadExitHandler(proc, clientData)
    Tcl_ExitProc *proc;		/* Procedure to invoke. */
    ClientData clientData;	/* Arbitrary value to pass to proc. */
{
    ExitHandler *exitPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    exitPtr = (ExitHandler *) ckalloc(sizeof(ExitHandler));
    exitPtr->proc = proc;
    exitPtr->clientData = clientData;
    exitPtr->nextPtr = tsdPtr->firstExitPtr;
    tsdPtr->firstExitPtr = exitPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteThreadExitHandler --
 *
 *	This procedure cancels an existing exit handler matching proc
 *	and clientData, if such a handler exits.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If there is an exit handler corresponding to proc and clientData
 *	then it is cancelled;  if no such handler exists then nothing
 *	happens.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DeleteThreadExitHandler(proc, clientData)
    Tcl_ExitProc *proc;		/* Procedure that was previously registered. */
    ClientData clientData;	/* Arbitrary value to pass to proc. */
{
    ExitHandler *exitPtr, *prevPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&dataKey);

    for (prevPtr = NULL, exitPtr = tsdPtr->firstExitPtr; exitPtr != NULL;
	    prevPtr = exitPtr, exitPtr = exitPtr->nextPtr) {
	if ((exitPtr->proc == proc)
		&& (exitPtr->clientData == clientData)) {
	    if (prevPtr == NULL) {
		tsdPtr->firstExitPtr = exitPtr->nextPtr;
	    } else {
		prevPtr->nextPtr = exitPtr->nextPtr;
	    }
	    ckfree((char *) exitPtr);
	    return;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetExitProc --
 *
 *	This procedure sets the application wide exit handler that
 *	will be called by Tcl_Exit in place of the C-runtime exit.  If
 *	the application wide exit handler is NULL, the C-runtime exit
 *	will be used instead.
 *
 * Results:
 *	The previously set application wide exit handler.
 *
 * Side effects:
 *	Sets the application wide exit handler to the specified value.
 *
 *----------------------------------------------------------------------
 */

Tcl_ExitProc *
Tcl_SetExitProc(proc)
    Tcl_ExitProc *proc; /* new exit handler for app or NULL */
{
    Tcl_ExitProc *prevExitProc;

    /*
     * Swap the old exit proc for the new one, saving the old one for
     * our return value.
     */

    Tcl_MutexLock(&exitMutex);
    prevExitProc = appExitPtr;
    appExitPtr = proc;
    Tcl_MutexUnlock(&exitMutex);

    return prevExitProc;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Exit --
 *
 *	This procedure is called to terminate the application.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	All existing exit handlers are invoked, then the application
 *	ends.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_Exit(status)
    int status;			/* Exit status for application;  typically
				 * 0 for normal return, 1 for error return. */
{
    Tcl_ExitProc *currentAppExitPtr;

    Tcl_MutexLock(&exitMutex);
    currentAppExitPtr = appExitPtr;
    Tcl_MutexUnlock(&exitMutex);

    if (currentAppExitPtr) {
	/*
	 * Warning: this code SHOULD NOT return, as there is code that
	 * depends on Tcl_Exit never returning.  In fact, we will
	 * Tcl_Panic if anyone returns, so critical is this dependcy.
	 */
	currentAppExitPtr((ClientData) status);
	Tcl_Panic("AppExitProc returned unexpectedly");
    } else {
	/* use default handling */
	Tcl_Finalize();
	TclpExit(status);
	Tcl_Panic("OS exit failed!");
    }
}

/*
 *-------------------------------------------------------------------------
 *
 * TclInitSubsystems --
 *
 *	Initialize various subsytems in Tcl.  This should be called the
 *	first time an interp is created, or before any of the subsystems
 *	are used.  This function ensures an order for the initialization 
 *	of subsystems:
 *
 *	1. that cannot be initialized in lazy order because they are 
 *	mutually dependent.
 *
 *	2. so that they can be finalized in a known order w/o causing
 *	the subsequent re-initialization of a subsystem in the act of
 *	shutting down another.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Varied, see the respective initialization routines.
 *
 *-------------------------------------------------------------------------
 */

void
TclInitSubsystems()
{
    if (inFinalize != 0) {
	Tcl_Panic("TclInitSubsystems called while finalizing");
    }

    if (subsystemsInitialized == 0) {
	/* 
	 * Double check inside the mutex.  There are definitly calls
	 * back into this routine from some of the procedures below.
	 */

	TclpInitLock();
	if (subsystemsInitialized == 0) {
	    /*
	     * Have to set this bit here to avoid deadlock with the
	     * routines below us that call into TclInitSubsystems.
	     */

	    subsystemsInitialized = 1;

	    /*
	     * Initialize locks used by the memory allocators before anything
	     * interesting happens so we can use the allocators in the
	     * implementation of self-initializing locks.
	     */
#if USE_TCLALLOC
	    TclInitAlloc(); /* process wide mutex init */
#endif
#ifdef TCL_MEM_DEBUG
	    TclInitDbCkalloc(); /* process wide mutex init */
#endif

	    TclpInitPlatform(); /* creates signal handler(s) */
    	    TclInitObjSubsystem(); /* register obj types, create mutexes */
	    TclInitIOSubsystem(); /* inits a tsd key (noop) */
	    TclInitEncodingSubsystem(); /* process wide encoding init */
	    TclpSetInterfaces();
    	    TclInitNamespaceSubsystem(); /* register ns obj type (mutexed) */
	}
	TclpInitUnlock();
    }
    TclInitNotifier();
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Finalize --
 *
 *	Shut down Tcl.  First calls registered exit handlers, then
 *	carefully shuts down various subsystems.
 *	Called by Tcl_Exit or when the Tcl shared library is being 
 *	unloaded.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Varied, see the respective finalization routines.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_Finalize()
{
    ExitHandler *exitPtr;
    
    /*
     * Invoke exit handlers first.
     */

    Tcl_MutexLock(&exitMutex);
    inFinalize = 1;
    for (exitPtr = firstExitPtr; exitPtr != NULL; exitPtr = firstExitPtr) {
	/*
	 * Be careful to remove the handler from the list before
	 * invoking its callback.  This protects us against
	 * double-freeing if the callback should call
	 * Tcl_DeleteExitHandler on itself.
	 */

	firstExitPtr = exitPtr->nextPtr;
	Tcl_MutexUnlock(&exitMutex);
	(*exitPtr->proc)(exitPtr->clientData);
	ckfree((char *) exitPtr);
	Tcl_MutexLock(&exitMutex);
    }    
    firstExitPtr = NULL;
    Tcl_MutexUnlock(&exitMutex);

    TclpInitLock();
    if (subsystemsInitialized != 0) {
	subsystemsInitialized = 0;

	/*
	 * Ensure the thread-specific data is initialised as it is
	 * used in Tcl_FinalizeThread()
	 */

	(void) TCL_TSD_INIT(&dataKey);

	/*
	 * Clean up after the current thread now, after exit handlers.
	 * In particular, the testexithandler command sets up something
	 * that writes to standard output, which gets closed.
	 * Note that there is no thread-local storage after this call.
	 */

	Tcl_FinalizeThread();

	/*
	 * Now finalize the Tcl execution environment.  Note that this
	 * must be done after the exit handlers, because there are
	 * order dependencies.
	 */

	TclFinalizeCompExecEnv();
	TclFinalizeEnvironment();

	/* 
	 * Finalizing the filesystem must come after anything which
	 * might conceivably interact with the 'Tcl_FS' API. 
	 */
	TclFinalizeFilesystem();

	/* 
	 * We must be sure the encoding finalization doesn't need
	 * to examine the filesystem in any way.  Since it only
	 * needs to clean up internal data structures, this is
	 * fine.
	 */
	TclFinalizeEncodingSubsystem();

	Tcl_SetPanicProc(NULL);

	/*
	 * Repeat finalization of the thread local storage once more.
	 * Although this step is already done by the Tcl_FinalizeThread
	 * call above, series of events happening afterwards may
	 * re-initialize TSD slots.  Those need to be finalized again,
	 * otherwise we're leaking memory chunks.
	 * Very important to note is that things happening afterwards
	 * should not reference anything which may re-initialize TSD's.
	 * This includes freeing Tcl_Objs's, among other things.
	 *
	 * This fixes the Tcl Bug #990552.
	 */
	TclFinalizeThreadData();

	/*
	 * Free synchronization objects.  There really should only be one
	 * thread alive at this moment.
	 */
	TclFinalizeSynchronization();

	/*
	 * We defer unloading of packages until very late 
	 * to avoid memory access issues.  Both exit callbacks and
	 * synchronization variables may be stored in packages.
	 * 
	 * Note that TclFinalizeLoad unloads packages in the reverse
	 * of the order they were loaded in (i.e. last to be loaded
	 * is the first to be unloaded).  This can be important for
	 * correct unloading when dependencies exist.
	 * 
	 * Once load has been finalized, we will have deleted any
	 * temporary copies of shared libraries and can therefore
	 * reset the filesystem to its original state.
	 */

	TclFinalizeLoad();
	TclResetFilesystem();
	
	/*
	 * There shouldn't be any malloc'ed memory after this.
	 */
#if defined(TCL_THREADS) && defined(USE_THREAD_ALLOC)
  TclFinalizeThreadAlloc();
#endif
	TclFinalizeMemorySubsystem();
	inFinalize = 0;
    }
    TclFinalizeLock();
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FinalizeThread --
 *
 *	Runs the exit handlers to allow Tcl to clean up its state
 *	about a particular thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Varied, see the respective finalization routines.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_FinalizeThread()
{
    ExitHandler *exitPtr;
    ThreadSpecificData *tsdPtr =
	    (ThreadSpecificData *)TclThreadDataKeyGet(&dataKey);

    if (tsdPtr != NULL) {
	tsdPtr->inExit = 1;

	for (exitPtr = tsdPtr->firstExitPtr; exitPtr != NULL;
		exitPtr = tsdPtr->firstExitPtr) {
	    /*
	     * Be careful to remove the handler from the list before invoking
	     * its callback.  This protects us against double-freeing if the
	     * callback should call Tcl_DeleteThreadExitHandler on itself.
	     */

	    tsdPtr->firstExitPtr = exitPtr->nextPtr;
	    (*exitPtr->proc)(exitPtr->clientData);
	    ckfree((char *) exitPtr);
	}
	TclFinalizeIOSubsystem();
	TclFinalizeNotifier();
	TclFinalizeAsync();
    }

    /*
     * Blow away all thread local storage blocks.
     *
     * Note that Tcl API allows creation of threads which do not use any
     * Tcl interp or other Tcl subsytems. Those threads might, however,
     * use thread local storage, so we must unconditionally finalize it.
     *
     * Fix [Bug #571002]
     */

     TclFinalizeThreadData();
}

/*
 *----------------------------------------------------------------------
 *
 * TclInExit --
 *
 *	Determines if we are in the middle of exit-time cleanup.
 *
 * Results:
 *	If we are in the middle of exiting, 1, otherwise 0.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TclInExit()
{
    return inFinalize;
}

/*
 *----------------------------------------------------------------------
 *
 * TclInThreadExit --
 *
 *	Determines if we are in the middle of thread exit-time cleanup.
 *
 * Results:
 *	If we are in the middle of exiting this thread, 1, otherwise 0.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TclInThreadExit()
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    TclThreadDataKeyGet(&dataKey);
    if (tsdPtr == NULL) {
	return 0;
    } else {
	return tsdPtr->inExit;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_VwaitObjCmd --
 *
 *	This procedure is invoked to process the "vwait" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_VwaitObjCmd(clientData, interp, objc, objv)
    ClientData clientData;	/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int done, foundEvent;
    char *nameString;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    nameString = Tcl_GetString(objv[1]);
    if (Tcl_TraceVar(interp, nameString,
	    TCL_GLOBAL_ONLY|TCL_TRACE_WRITES|TCL_TRACE_UNSETS,
	    VwaitVarProc, (ClientData) &done) != TCL_OK) {
	return TCL_ERROR;
    };
    done = 0;
    foundEvent = 1;
    while (!done && foundEvent) {
	foundEvent = Tcl_DoOneEvent(TCL_ALL_EVENTS);
	if (Tcl_LimitExceeded(interp)) {
	    return TCL_ERROR;
	}
    }
    Tcl_UntraceVar(interp, nameString,
	    TCL_GLOBAL_ONLY|TCL_TRACE_WRITES|TCL_TRACE_UNSETS,
	    VwaitVarProc, (ClientData) &done);

    /*
     * Clear out the interpreter's result, since it may have been set
     * by event handlers.
     */

    Tcl_ResetResult(interp);
    if (!foundEvent) {
	Tcl_AppendResult(interp, "can't wait for variable \"", nameString,
		"\":  would wait forever", (char *) NULL);
	return TCL_ERROR;
    }
    return TCL_OK;
}

	/* ARGSUSED */
static char *
VwaitVarProc(clientData, interp, name1, name2, flags)
    ClientData clientData;	/* Pointer to integer to set to 1. */
    Tcl_Interp *interp;		/* Interpreter containing variable. */
    CONST char *name1;		/* Name of variable. */
    CONST char *name2;		/* Second part of variable name. */
    int flags;			/* Information about what happened. */
{
    int *donePtr = (int *) clientData;

    *donePtr = 1;
    return (char *) NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UpdateObjCmd --
 *
 *	This procedure is invoked to process the "update" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_UpdateObjCmd(clientData, interp, objc, objv)
    ClientData clientData;	/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int optionIndex;
    int flags = 0;		/* Initialized to avoid compiler warning. */
    static CONST char *updateOptions[] = {"idletasks", (char *) NULL};
    enum updateOptions {REGEXP_IDLETASKS};

    if (objc == 1) {
	flags = TCL_ALL_EVENTS|TCL_DONT_WAIT;
    } else if (objc == 2) {
	if (Tcl_GetIndexFromObj(interp, objv[1], updateOptions,
		"option", 0, &optionIndex) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch ((enum updateOptions) optionIndex) {
	    case REGEXP_IDLETASKS: {
		flags = TCL_WINDOW_EVENTS|TCL_IDLE_EVENTS|TCL_DONT_WAIT;
		break;
	    }
	    default: {
		Tcl_Panic("Tcl_UpdateObjCmd: bad option index to UpdateOptions");
	    }
	}
    } else {
        Tcl_WrongNumArgs(interp, 1, objv, "?idletasks?");
	return TCL_ERROR;
    }
    
    while (Tcl_DoOneEvent(flags) != 0) {
	if (Tcl_LimitExceeded(interp)) {
	    return TCL_ERROR;
	}
    }

    /*
     * Must clear the interpreter's result because event handlers could
     * have executed commands.
     */

    Tcl_ResetResult(interp);
    return TCL_OK;
}

#ifdef TCL_THREADS
/*
 *-----------------------------------------------------------------------------
 *
 *  NewThreadProc --
 *
 * 	Bootstrap function of a new Tcl thread.
 *
 * Results:
 *	None.
 *
 * Side Effects:
 *	Initializes Tcl notifier for the current thread.
 *
 *-----------------------------------------------------------------------------
 */

static Tcl_ThreadCreateType
NewThreadProc(ClientData clientData)
{
    ThreadClientData *cdPtr;
    ClientData threadClientData;
    Tcl_ThreadCreateProc *threadProc;

    cdPtr  = (ThreadClientData *)clientData;
    threadProc = cdPtr->proc;
    threadClientData = cdPtr->clientData;
    Tcl_Free((char*)clientData); /* Allocated in Tcl_CreateThread() */

    (*threadProc)(threadClientData);

    TCL_THREAD_CREATE_RETURN;
}
#endif
/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateThread --
 *
 *	This procedure creates a new thread. This actually belongs
 *	to the tclThread.c file but since we use some private 
 *	data structures local to this file, it is placed here.
 *
 * Results:
 *	TCL_OK if the thread could be created.  The thread ID is
 *	returned in a parameter.
 *
 * Side effects:
 *	A new thread is created.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_CreateThread(idPtr, proc, clientData, stackSize, flags)
    Tcl_ThreadId *idPtr;		/* Return, the ID of the thread */
    Tcl_ThreadCreateProc proc;		/* Main() function of the thread */
    ClientData clientData;		/* The one argument to Main() */
    int stackSize;			/* Size of stack for the new thread */
    int flags;				/* Flags controlling behaviour of
					 * the new thread */
{
#ifdef TCL_THREADS
    ThreadClientData *cdPtr;

    cdPtr = (ThreadClientData*)Tcl_Alloc(sizeof(ThreadClientData));
    cdPtr->proc = proc;
    cdPtr->clientData = clientData;

    return TclpThreadCreate(idPtr, NewThreadProc, (ClientData)cdPtr,
                           stackSize, flags);
#else
    return TCL_ERROR;
#endif /* TCL_THREADS */
}
