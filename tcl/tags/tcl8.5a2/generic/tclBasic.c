/* 
 * tclBasic.c --
 *
 *	Contains the basic facilities for TCL command interpretation,
 *	including interpreter creation and deletion, command creation
 *	and deletion, and command/script execution. 
 *
 * Copyright (c) 1987-1994 The Regents of the University of California.
 * Copyright (c) 1994-1997 Sun Microsystems, Inc.
 * Copyright (c) 1998-1999 by Scriptics Corporation.
 * Copyright (c) 2001, 2002 by Kevin B. Kenny.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclBasic.c,v 1.136 2004/11/30 19:34:46 dgp Exp $
 */

#include "tclInt.h"
#include "tclCompile.h"

/*
 * Static procedures in this file:
 */

static char *		CallCommandTraces _ANSI_ARGS_((Interp *iPtr, 
			    Command *cmdPtr, CONST char *oldName, 
			    CONST char* newName, int flags));
static void		DeleteInterpProc _ANSI_ARGS_((Tcl_Interp *interp));
static void		ProcessUnexpectedResult _ANSI_ARGS_((
			    Tcl_Interp *interp, int returnCode));

extern TclStubs tclStubs;

/*
 * The following structure defines the commands in the Tcl core.
 */

typedef struct {
    char *name;			/* Name of object-based command. */
    Tcl_ObjCmdProc *objProc;	/* Object-based procedure for command. */
    CompileProc *compileProc;	/* Procedure called to compile command. */
    int isSafe;			/* If non-zero, command will be present
                                 * in safe interpreter. Otherwise it will
                                 * be hidden. */
} CmdInfo;

/*
 * The built-in commands, and the procedures that implement them:
 */

static CmdInfo builtInCmds[] = {
    /*
     * Commands in the generic core. 
     */

    {"append",		Tcl_AppendObjCmd,	TclCompileAppendCmd,	1},
    {"array",		Tcl_ArrayObjCmd,	(CompileProc *) NULL,	1},
    {"binary",		Tcl_BinaryObjCmd,	(CompileProc *) NULL,	1},
    {"break",		Tcl_BreakObjCmd,	TclCompileBreakCmd,	1},
    {"case",		Tcl_CaseObjCmd,		(CompileProc *) NULL,	1},
    {"catch",		Tcl_CatchObjCmd,	TclCompileCatchCmd,	1},
    {"concat",		Tcl_ConcatObjCmd,	(CompileProc *) NULL,	1},
    {"continue",	Tcl_ContinueObjCmd,	TclCompileContinueCmd,	1},
    {"dict",		Tcl_DictObjCmd,		(CompileProc *) NULL,	1},
    {"encoding",	Tcl_EncodingObjCmd,	(CompileProc *) NULL,	0},
    {"error",		Tcl_ErrorObjCmd,	(CompileProc *) NULL,	1},
    {"eval",		Tcl_EvalObjCmd,		(CompileProc *) NULL,	1},
    {"exit",		Tcl_ExitObjCmd,		(CompileProc *) NULL,	0},
    {"expr",		Tcl_ExprObjCmd,		TclCompileExprCmd,	1},
    {"fcopy",		Tcl_FcopyObjCmd,	(CompileProc *) NULL,	1},
    {"fileevent",	Tcl_FileEventObjCmd,	(CompileProc *) NULL,	1},
    {"for",		Tcl_ForObjCmd,		TclCompileForCmd,	1},
    {"foreach",		Tcl_ForeachObjCmd,	TclCompileForeachCmd,	1},
    {"format",		Tcl_FormatObjCmd,	(CompileProc *) NULL,	1},
    {"global",		Tcl_GlobalObjCmd,	(CompileProc *) NULL,	1},
    {"if",		Tcl_IfObjCmd,		TclCompileIfCmd,	1},
    {"incr",		Tcl_IncrObjCmd,		TclCompileIncrCmd,	1},
    {"info",		Tcl_InfoObjCmd,		(CompileProc *) NULL,	1},
    {"join",		Tcl_JoinObjCmd,		(CompileProc *) NULL,	1},
    {"lappend",		Tcl_LappendObjCmd,	TclCompileLappendCmd,	1},
    {"lassign",		Tcl_LassignObjCmd,	TclCompileLassignCmd,	1},
    {"lindex",		Tcl_LindexObjCmd,	TclCompileLindexCmd,	1},
    {"linsert",		Tcl_LinsertObjCmd,	(CompileProc *) NULL,	1},
    {"list",		Tcl_ListObjCmd,		TclCompileListCmd,	1},
    {"llength",		Tcl_LlengthObjCmd,	TclCompileLlengthCmd,	1},
    {"load",		Tcl_LoadObjCmd,		(CompileProc *) NULL,	0},
    {"lrange",		Tcl_LrangeObjCmd,	(CompileProc *) NULL,	1},
    {"lrepeat",		Tcl_LrepeatObjCmd,	(CompileProc *) NULL,	1},
    {"lreplace",	Tcl_LreplaceObjCmd,	(CompileProc *) NULL,	1},
    {"lsearch",		Tcl_LsearchObjCmd,	(CompileProc *) NULL,	1},
    {"lset",		Tcl_LsetObjCmd,		TclCompileLsetCmd,	1},
    {"lsort",		Tcl_LsortObjCmd,	(CompileProc *) NULL,	1},
    {"namespace",	Tcl_NamespaceObjCmd,	(CompileProc *) NULL,	1},
    {"package",		Tcl_PackageObjCmd,	(CompileProc *) NULL,	1},
    {"proc",		Tcl_ProcObjCmd,		(CompileProc *) NULL,	1},
    {"regexp",		Tcl_RegexpObjCmd,	TclCompileRegexpCmd,	1},
    {"regsub",		Tcl_RegsubObjCmd,	(CompileProc *) NULL,	1},
    {"rename",		Tcl_RenameObjCmd,	(CompileProc *) NULL,	1},
    {"return",		Tcl_ReturnObjCmd,	TclCompileReturnCmd,	1},
    {"scan",		Tcl_ScanObjCmd,		(CompileProc *) NULL,	1},
    {"set",		Tcl_SetObjCmd,		TclCompileSetCmd,	1},
    {"split",		Tcl_SplitObjCmd,	(CompileProc *) NULL,	1},
    {"string",		Tcl_StringObjCmd,	TclCompileStringCmd,	1},
    {"subst",		Tcl_SubstObjCmd,	(CompileProc *) NULL,	1},
    {"switch",		Tcl_SwitchObjCmd,	TclCompileSwitchCmd,	1},
    {"trace",		Tcl_TraceObjCmd,	(CompileProc *) NULL,	1},
    {"unload",		Tcl_UnloadObjCmd,	(CompileProc *) NULL,	1},
    {"unset",		Tcl_UnsetObjCmd,	(CompileProc *) NULL,	1},
    {"uplevel",		Tcl_UplevelObjCmd,	(CompileProc *) NULL,	1},
    {"upvar",		Tcl_UpvarObjCmd,	(CompileProc *) NULL,	1},
    {"variable",	Tcl_VariableObjCmd,	(CompileProc *) NULL,	1},
    {"while",		Tcl_WhileObjCmd,	TclCompileWhileCmd,	1},

    /*
     * Commands in the UNIX core:
     */

#ifndef TCL_GENERIC_ONLY
    {"after",		Tcl_AfterObjCmd,	(CompileProc *) NULL,	1},
    {"cd",		Tcl_CdObjCmd,		(CompileProc *) NULL,	0},
    {"close",		Tcl_CloseObjCmd,	(CompileProc *) NULL,	1},
    {"eof",		Tcl_EofObjCmd,		(CompileProc *) NULL,	1},
    {"fblocked",	Tcl_FblockedObjCmd,	(CompileProc *) NULL,	1},
    {"fconfigure",	Tcl_FconfigureObjCmd,	(CompileProc *) NULL,	0},
    {"file",		Tcl_FileObjCmd,		(CompileProc *) NULL,	0},
    {"flush",		Tcl_FlushObjCmd,	(CompileProc *) NULL,	1},
    {"gets",		Tcl_GetsObjCmd,		(CompileProc *) NULL,	1},
    {"glob",		Tcl_GlobObjCmd,		(CompileProc *) NULL,	0},
    {"open",		Tcl_OpenObjCmd,		(CompileProc *) NULL,	0},
    {"pid",		Tcl_PidObjCmd,		(CompileProc *) NULL,	1},
    {"puts",		Tcl_PutsObjCmd,		(CompileProc *) NULL,	1},
    {"pwd",		Tcl_PwdObjCmd,		(CompileProc *) NULL,	0},
    {"read",		Tcl_ReadObjCmd,		(CompileProc *) NULL,	1},
    {"seek",		Tcl_SeekObjCmd,		(CompileProc *) NULL,	1},
    {"socket",		Tcl_SocketObjCmd,	(CompileProc *) NULL,	0},
    {"tell",		Tcl_TellObjCmd,		(CompileProc *) NULL,	1},
    {"time",		Tcl_TimeObjCmd,		(CompileProc *) NULL,	1},
    {"update",		Tcl_UpdateObjCmd,	(CompileProc *) NULL,	1},
    {"vwait",		Tcl_VwaitObjCmd,	(CompileProc *) NULL,	1},
    {"exec",		Tcl_ExecObjCmd,		(CompileProc *) NULL,	0},
    {"source",		Tcl_SourceObjCmd,	(CompileProc *) NULL,	0},
#endif /* TCL_GENERIC_ONLY */
    {NULL,	(Tcl_ObjCmdProc *) NULL,	(CompileProc *) NULL,	0}
};


/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateInterp --
 *
 *	Create a new TCL command interpreter.
 *
 * Results:
 *	The return value is a token for the interpreter, which may be
 *	used in calls to procedures like Tcl_CreateCmd, Tcl_Eval, or
 *	Tcl_DeleteInterp.
 *
 * Side effects:
 *	The command interpreter is initialized with the built-in commands
 *      and with the variables documented in tclvars(n).
 *
 *----------------------------------------------------------------------
 */

Tcl_Interp *
Tcl_CreateInterp()
{
    Interp *iPtr;
    Tcl_Interp *interp;
    Command *cmdPtr;
    BuiltinFunc *builtinFuncPtr;
    MathFunc *mathFuncPtr;
    Tcl_HashEntry *hPtr;
    const CmdInfo *cmdInfoPtr;
    int i;
    union {
	char c[sizeof(short)];
	short s;
    } order;
#ifdef TCL_COMPILE_STATS
    ByteCodeStats *statsPtr;
#endif /* TCL_COMPILE_STATS */

    TclInitSubsystems();

    /*
     * Panic if someone updated the CallFrame structure without
     * also updating the Tcl_CallFrame structure (or vice versa).
     */  

    if (sizeof(Tcl_CallFrame) != sizeof(CallFrame)) {
	/*NOTREACHED*/
        Tcl_Panic("Tcl_CallFrame and CallFrame are not the same size");
    }

    /*
     * Initialize support for namespaces and create the global namespace
     * (whose name is ""; an alias is "::"). This also initializes the
     * Tcl object type table and other object management code.
     */

    iPtr = (Interp *) ckalloc(sizeof(Interp));
    interp = (Tcl_Interp *) iPtr;

    iPtr->result		= iPtr->resultSpace;
    iPtr->freeProc		= NULL;
    iPtr->errorLine		= 0;
    iPtr->objResultPtr		= Tcl_NewObj();
    Tcl_IncrRefCount(iPtr->objResultPtr);
    iPtr->handle		= TclHandleCreate(iPtr);
    iPtr->globalNsPtr		= NULL;
    iPtr->hiddenCmdTablePtr	= NULL;
    iPtr->interpInfo		= NULL;
    Tcl_InitHashTable(&iPtr->mathFuncTable, TCL_STRING_KEYS);

    iPtr->numLevels = 0;
    iPtr->maxNestingDepth = MAX_NESTING_DEPTH;
    iPtr->framePtr = NULL;
    iPtr->varFramePtr = NULL;
    iPtr->activeVarTracePtr = NULL;

    iPtr->returnOpts = NULL;
    iPtr->errorInfo = NULL;
    iPtr->eiVar = Tcl_NewStringObj("errorInfo", -1);
    Tcl_IncrRefCount(iPtr->eiVar);
    iPtr->errorCode = NULL;
    iPtr->ecVar = Tcl_NewStringObj("errorCode", -1);
    Tcl_IncrRefCount(iPtr->ecVar);
    iPtr->returnLevel = 0;
    iPtr->returnCode = TCL_OK;

    iPtr->appendResult = NULL;
    iPtr->appendAvl = 0;
    iPtr->appendUsed = 0;

    Tcl_InitHashTable(&iPtr->packageTable, TCL_STRING_KEYS);
    iPtr->packageUnknown = NULL;
    iPtr->cmdCount = 0;
    TclInitLiteralTable(&(iPtr->literalTable));
    iPtr->compileEpoch = 0;
    iPtr->compiledProcPtr = NULL;
    iPtr->resolverPtr = NULL;
    iPtr->evalFlags = 0;
    iPtr->scriptFile = NULL;
    iPtr->flags = 0;
    iPtr->tracePtr = NULL;
    iPtr->tracesForbiddingInline = 0;
    iPtr->activeCmdTracePtr = NULL;
    iPtr->activeInterpTracePtr = NULL;
    iPtr->assocData = (Tcl_HashTable *) NULL;
    iPtr->execEnvPtr = NULL;	      /* set after namespaces initialized */
    iPtr->emptyObjPtr = Tcl_NewObj(); /* another empty object */
    Tcl_IncrRefCount(iPtr->emptyObjPtr);
    iPtr->resultSpace[0] = 0;

    iPtr->globalNsPtr = NULL;	/* force creation of global ns below */
    iPtr->globalNsPtr = (Namespace *) Tcl_CreateNamespace(interp, "",
	    (ClientData) NULL, (Tcl_NamespaceDeleteProc *) NULL);
    if (iPtr->globalNsPtr == NULL) {
        Tcl_Panic("Tcl_CreateInterp: can't create global namespace");
    }

    /*
     * Initialize support for code compilation and execution. We call
     * TclCreateExecEnv after initializing namespaces since it tries to
     * reference a Tcl variable (it links to the Tcl "tcl_traceExec"
     * variable).
     */

    iPtr->execEnvPtr = TclCreateExecEnv(interp);

    /*
     * Initialize the compilation and execution statistics kept for this
     * interpreter.
     */

#ifdef TCL_COMPILE_STATS
    statsPtr = &(iPtr->stats);
    statsPtr->numExecutions = 0;
    statsPtr->numCompilations = 0;
    statsPtr->numByteCodesFreed = 0;
    (VOID *) memset(statsPtr->instructionCount, 0,
	    sizeof(statsPtr->instructionCount));

    statsPtr->totalSrcBytes = 0.0;
    statsPtr->totalByteCodeBytes = 0.0;
    statsPtr->currentSrcBytes = 0.0;
    statsPtr->currentByteCodeBytes = 0.0;
    (VOID *) memset(statsPtr->srcCount, 0, sizeof(statsPtr->srcCount));
    (VOID *) memset(statsPtr->byteCodeCount, 0,
	    sizeof(statsPtr->byteCodeCount));
    (VOID *) memset(statsPtr->lifetimeCount, 0,
	    sizeof(statsPtr->lifetimeCount));
    
    statsPtr->currentInstBytes   = 0.0;
    statsPtr->currentLitBytes    = 0.0;
    statsPtr->currentExceptBytes = 0.0;
    statsPtr->currentAuxBytes    = 0.0;
    statsPtr->currentCmdMapBytes = 0.0;
    
    statsPtr->numLiteralsCreated    = 0;
    statsPtr->totalLitStringBytes   = 0.0;
    statsPtr->currentLitStringBytes = 0.0;
    (VOID *) memset(statsPtr->literalCount, 0,
            sizeof(statsPtr->literalCount));
#endif /* TCL_COMPILE_STATS */    

    /*
     * Initialise the stub table pointer.
     */

    iPtr->stubTable = &tclStubs;

    /*
     * Initialize the ensemble error message rewriting support.
     */

    iPtr->ensembleRewrite.sourceObjs = NULL;
    iPtr->ensembleRewrite.numRemovedObjs = 0;
    iPtr->ensembleRewrite.numInsertedObjs = 0;

    /*
     * TIP#143: Initialise the resource limit support.
     */

    TclInitLimitSupport(interp);

    /*
     * Create the core commands. Do it here, rather than calling
     * Tcl_CreateCommand, because it's faster (there's no need to check for
     * a pre-existing command by the same name). If a command has a
     * Tcl_CmdProc but no Tcl_ObjCmdProc, set the Tcl_ObjCmdProc to
     * TclInvokeStringCommand. This is an object-based wrapper procedure
     * that extracts strings, calls the string procedure, and creates an
     * object for the result. Similarly, if a command has a Tcl_ObjCmdProc
     * but no Tcl_CmdProc, set the Tcl_CmdProc to TclInvokeObjectCommand.
     */

    for (cmdInfoPtr = builtInCmds;  cmdInfoPtr->name != NULL; cmdInfoPtr++) {
	int new;
	Tcl_HashEntry *hPtr;

	if ((cmdInfoPtr->objProc == (Tcl_ObjCmdProc *) NULL)
	        && (cmdInfoPtr->compileProc == (CompileProc *) NULL)) {
	    Tcl_Panic("Tcl_CreateInterp: builtin command with NULL object command proc and a NULL compile proc\n");
	}
	
	hPtr = Tcl_CreateHashEntry(&iPtr->globalNsPtr->cmdTable,
	        cmdInfoPtr->name, &new);
	if (new) {
	    cmdPtr = (Command *) ckalloc(sizeof(Command));
	    cmdPtr->hPtr = hPtr;
	    cmdPtr->nsPtr = iPtr->globalNsPtr;
	    cmdPtr->refCount = 1;
	    cmdPtr->cmdEpoch = 0;
	    cmdPtr->compileProc = cmdInfoPtr->compileProc;
	    cmdPtr->proc = TclInvokeObjectCommand;
	    cmdPtr->clientData = (ClientData) cmdPtr;
	    cmdPtr->objProc = cmdInfoPtr->objProc;
	    cmdPtr->objClientData = (ClientData) NULL;
	    cmdPtr->deleteProc = NULL;
	    cmdPtr->deleteData = (ClientData) NULL;
	    cmdPtr->flags = 0;
	    cmdPtr->importRefPtr = NULL;
	    cmdPtr->tracePtr = NULL;
	    Tcl_SetHashValue(hPtr, cmdPtr);
	}
    }

    /*
     * Register the clock commands.  These *do* go through 
     * Tcl_CreateObjCommand, since they aren't in the global namespace.
     */

    Tcl_CreateObjCommand( interp,	 "::tcl::clock::clicks",
	    TclClockClicksObjCmd,	 (ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );
    Tcl_CreateObjCommand( interp,	 "::tcl::clock::getenv",
	    TclClockGetenvObjCmd,	 (ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );
    Tcl_CreateObjCommand( interp,	 "::tcl::clock::microseconds",
	    TclClockMicrosecondsObjCmd,	 (ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );
    Tcl_CreateObjCommand( interp,	 "::tcl::clock::milliseconds",
	    TclClockMillisecondsObjCmd,	 (ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );
    Tcl_CreateObjCommand( interp,	 "::tcl::clock::seconds",
	    TclClockSecondsObjCmd,	(ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );
    Tcl_CreateObjCommand( interp,	 "::tcl::clock::Localtime",
	    TclClockLocaltimeObjCmd,	(ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );
    Tcl_CreateObjCommand( interp,	"::tcl::clock::Mktime",
	    TclClockMktimeObjCmd,	(ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );
    Tcl_CreateObjCommand( interp,	"::tcl::clock::Oldscan",
	    TclClockOldscanObjCmd,	(ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );

    /* Register the default [interp bgerror] handler. */

    Tcl_CreateObjCommand( interp,	"::tcl::Bgerror",
	    TclDefaultBgErrorHandlerObjCmd,	(ClientData) NULL,
	    (Tcl_CmdDeleteProc*) NULL );

    /*
     * Register the builtin math functions.
     */

    i = 0;
    for (builtinFuncPtr = tclBuiltinFuncTable;  builtinFuncPtr->name != NULL;
	    builtinFuncPtr++) {
	Tcl_CreateMathFunc((Tcl_Interp *) iPtr, builtinFuncPtr->name,
		builtinFuncPtr->numArgs, builtinFuncPtr->argTypes,
		(Tcl_MathProc *) NULL, (ClientData) 0);
	hPtr = Tcl_FindHashEntry(&iPtr->mathFuncTable,
		builtinFuncPtr->name);
	if (hPtr == NULL) {
	    Tcl_Panic("Tcl_CreateInterp: Tcl_CreateMathFunc incorrectly registered '%s'", builtinFuncPtr->name);
	    return NULL;
	}
	mathFuncPtr = (MathFunc *) Tcl_GetHashValue(hPtr);
	mathFuncPtr->builtinFuncIndex = i;
	i++;
    }

    /*
     * Do Multiple/Safe Interps Tcl init stuff
     */

    TclInterpInit(interp);

#ifndef TCL_GENERIC_ONLY
    TclSetupEnv(interp);
#endif

    /*
     * TIP #59: Make embedded configuration information
     * available.
     */

    TclInitEmbeddedConfigurationInformation (interp);

    /*
     * Compute the byte order of this machine.
     */

    order.s = 1;
    Tcl_SetVar2(interp, "tcl_platform", "byteOrder",
	    ((order.c[0] == 1) ? "littleEndian" : "bigEndian"),
	    TCL_GLOBAL_ONLY);

    Tcl_SetVar2Ex(interp, "tcl_platform", "wordSize",
	    Tcl_NewLongObj((long) sizeof(long)), TCL_GLOBAL_ONLY);

    /*
     * Set up other variables such as tcl_version and tcl_library
     */

    Tcl_SetVar(interp, "tcl_patchLevel", TCL_PATCH_LEVEL, TCL_GLOBAL_ONLY);
    Tcl_SetVar(interp, "tcl_version", TCL_VERSION, TCL_GLOBAL_ONLY);
    Tcl_TraceVar2(interp, "tcl_precision", (char *) NULL,
	    TCL_GLOBAL_ONLY|TCL_TRACE_READS|TCL_TRACE_WRITES|TCL_TRACE_UNSETS,
	    TclPrecTraceProc, (ClientData) NULL);
    TclpSetVariables(interp);

#ifdef TCL_THREADS
    /*
     * The existence of the "threaded" element of the tcl_platform array indicates
     * that this particular Tcl shell has been compiled with threads turned on.
     * Using "info exists tcl_platform(threaded)" a Tcl script can introspect on the 
     * interpreter level of thread safety.
     */


    Tcl_SetVar2(interp, "tcl_platform", "threaded", "1",
	    TCL_GLOBAL_ONLY);
#endif

    /*
     * Register Tcl's version number.
     */

    Tcl_PkgProvideEx(interp, "Tcl", TCL_VERSION, (ClientData) &tclStubs);
    
#ifdef Tcl_InitStubs
#undef Tcl_InitStubs
#endif
    Tcl_InitStubs(interp, TCL_VERSION, 1);

    return interp;
}

/*
 *----------------------------------------------------------------------
 *
 * TclHideUnsafeCommands --
 *
 *	Hides base commands that are not marked as safe from this
 *	interpreter.
 *
 * Results:
 *	TCL_OK if it succeeds, TCL_ERROR else.
 *
 * Side effects:
 *	Hides functionality in an interpreter.
 *
 *----------------------------------------------------------------------
 */

int
TclHideUnsafeCommands(interp)
    Tcl_Interp *interp;		/* Hide commands in this interpreter. */
{
    register const CmdInfo *cmdInfoPtr;

    if (interp == (Tcl_Interp *) NULL) {
        return TCL_ERROR;
    }
    for (cmdInfoPtr = builtInCmds; cmdInfoPtr->name != NULL; cmdInfoPtr++) {
        if (!cmdInfoPtr->isSafe) {
            Tcl_HideCommand(interp, cmdInfoPtr->name, cmdInfoPtr->name);
        }
    }
    return TCL_OK;
}

/*
 *--------------------------------------------------------------
 *
 * Tcl_CallWhenDeleted --
 *
 *	Arrange for a procedure to be called before a given
 *	interpreter is deleted. The procedure is called as soon
 *	as Tcl_DeleteInterp is called; if Tcl_CallWhenDeleted is
 *	called on an interpreter that has already been deleted,
 *	the procedure will be called when the last Tcl_Release is
 *	done on the interpreter.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	When Tcl_DeleteInterp is invoked to delete interp,
 *	proc will be invoked.  See the manual entry for
 *	details.
 *
 *--------------------------------------------------------------
 */

void
Tcl_CallWhenDeleted(interp, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter to watch. */
    Tcl_InterpDeleteProc *proc;	/* Procedure to call when interpreter
				 * is about to be deleted. */
    ClientData clientData;	/* One-word value to pass to proc. */
{
    Interp *iPtr = (Interp *) interp;
    static int assocDataCounter = 0;
#ifdef TCL_THREADS
    static Tcl_Mutex assocMutex;
#endif
    int new;
    char buffer[32 + TCL_INTEGER_SPACE];
    AssocData *dPtr = (AssocData *) ckalloc(sizeof(AssocData));
    Tcl_HashEntry *hPtr;

    Tcl_MutexLock(&assocMutex);
    sprintf(buffer, "Assoc Data Key #%d", assocDataCounter);
    assocDataCounter++;
    Tcl_MutexUnlock(&assocMutex);

    if (iPtr->assocData == (Tcl_HashTable *) NULL) {
        iPtr->assocData = (Tcl_HashTable *) ckalloc(sizeof(Tcl_HashTable));
        Tcl_InitHashTable(iPtr->assocData, TCL_STRING_KEYS);
    }
    hPtr = Tcl_CreateHashEntry(iPtr->assocData, buffer, &new);
    dPtr->proc = proc;
    dPtr->clientData = clientData;
    Tcl_SetHashValue(hPtr, dPtr);
}

/*
 *--------------------------------------------------------------
 *
 * Tcl_DontCallWhenDeleted --
 *
 *	Cancel the arrangement for a procedure to be called when
 *	a given interpreter is deleted.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If proc and clientData were previously registered as a
 *	callback via Tcl_CallWhenDeleted, they are unregistered.
 *	If they weren't previously registered then nothing
 *	happens.
 *
 *--------------------------------------------------------------
 */

void
Tcl_DontCallWhenDeleted(interp, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter to watch. */
    Tcl_InterpDeleteProc *proc;	/* Procedure to call when interpreter
				 * is about to be deleted. */
    ClientData clientData;	/* One-word value to pass to proc. */
{
    Interp *iPtr = (Interp *) interp;
    Tcl_HashTable *hTablePtr;
    Tcl_HashSearch hSearch;
    Tcl_HashEntry *hPtr;
    AssocData *dPtr;

    hTablePtr = iPtr->assocData;
    if (hTablePtr == (Tcl_HashTable *) NULL) {
        return;
    }
    for (hPtr = Tcl_FirstHashEntry(hTablePtr, &hSearch); hPtr != NULL;
	    hPtr = Tcl_NextHashEntry(&hSearch)) {
        dPtr = (AssocData *) Tcl_GetHashValue(hPtr);
        if ((dPtr->proc == proc) && (dPtr->clientData == clientData)) {
            ckfree((char *) dPtr);
            Tcl_DeleteHashEntry(hPtr);
            return;
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetAssocData --
 *
 *	Creates a named association between user-specified data, a delete
 *	function and this interpreter. If the association already exists
 *	the data is overwritten with the new data. The delete function will
 *	be invoked when the interpreter is deleted.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets the associated data, creates the association if needed.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetAssocData(interp, name, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter to associate with. */
    CONST char *name;		/* Name for association. */
    Tcl_InterpDeleteProc *proc;	/* Proc to call when interpreter is
                                 * about to be deleted. */
    ClientData clientData;	/* One-word value to pass to proc. */
{
    Interp *iPtr = (Interp *) interp;
    AssocData *dPtr;
    Tcl_HashEntry *hPtr;
    int new;

    if (iPtr->assocData == (Tcl_HashTable *) NULL) {
        iPtr->assocData = (Tcl_HashTable *) ckalloc(sizeof(Tcl_HashTable));
        Tcl_InitHashTable(iPtr->assocData, TCL_STRING_KEYS);
    }
    hPtr = Tcl_CreateHashEntry(iPtr->assocData, name, &new);
    if (new == 0) {
        dPtr = (AssocData *) Tcl_GetHashValue(hPtr);
    } else {
        dPtr = (AssocData *) ckalloc(sizeof(AssocData));
    }
    dPtr->proc = proc;
    dPtr->clientData = clientData;

    Tcl_SetHashValue(hPtr, dPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteAssocData --
 *
 *	Deletes a named association of user-specified data with
 *	the specified interpreter.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Deletes the association.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DeleteAssocData(interp, name)
    Tcl_Interp *interp;			/* Interpreter to associate with. */
    CONST char *name;			/* Name of association. */
{
    Interp *iPtr = (Interp *) interp;
    AssocData *dPtr;
    Tcl_HashEntry *hPtr;

    if (iPtr->assocData == (Tcl_HashTable *) NULL) {
        return;
    }
    hPtr = Tcl_FindHashEntry(iPtr->assocData, name);
    if (hPtr == (Tcl_HashEntry *) NULL) {
        return;
    }
    dPtr = (AssocData *) Tcl_GetHashValue(hPtr);
    if (dPtr->proc != NULL) {
        (dPtr->proc) (dPtr->clientData, interp);
    }
    ckfree((char *) dPtr);
    Tcl_DeleteHashEntry(hPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetAssocData --
 *
 *	Returns the client data associated with this name in the
 *	specified interpreter.
 *
 * Results:
 *	The client data in the AssocData record denoted by the named
 *	association, or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ClientData
Tcl_GetAssocData(interp, name, procPtr)
    Tcl_Interp *interp;			/* Interpreter associated with. */
    CONST char *name;			/* Name of association. */
    Tcl_InterpDeleteProc **procPtr;	/* Pointer to place to store address
					 * of current deletion callback. */
{
    Interp *iPtr = (Interp *) interp;
    AssocData *dPtr;
    Tcl_HashEntry *hPtr;

    if (iPtr->assocData == (Tcl_HashTable *) NULL) {
        return (ClientData) NULL;
    }
    hPtr = Tcl_FindHashEntry(iPtr->assocData, name);
    if (hPtr == (Tcl_HashEntry *) NULL) {
        return (ClientData) NULL;
    }
    dPtr = (AssocData *) Tcl_GetHashValue(hPtr);
    if (procPtr != (Tcl_InterpDeleteProc **) NULL) {
        *procPtr = dPtr->proc;
    }
    return dPtr->clientData;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_InterpDeleted --
 *
 *	Returns nonzero if the interpreter has been deleted with a call
 *	to Tcl_DeleteInterp.
 *
 * Results:
 *	Nonzero if the interpreter is deleted, zero otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_InterpDeleted(interp)
    Tcl_Interp *interp;
{
    return (((Interp *) interp)->flags & DELETED) ? 1 : 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteInterp --
 *
 *	Ensures that the interpreter will be deleted eventually. If there
 *	are no Tcl_Preserve calls in effect for this interpreter, it is
 *	deleted immediately, otherwise the interpreter is deleted when
 *	the last Tcl_Preserve is matched by a call to Tcl_Release. In either
 *	case, the procedure runs the currently registered deletion callbacks. 
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The interpreter is marked as deleted. The caller may still use it
 *	safely if there are calls to Tcl_Preserve in effect for the
 *	interpreter, but further calls to Tcl_Eval etc in this interpreter
 *	will fail.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DeleteInterp(interp)
    Tcl_Interp *interp;		/* Token for command interpreter (returned
				 * by a previous call to Tcl_CreateInterp). */
{
    Interp *iPtr = (Interp *) interp;

    /*
     * If the interpreter has already been marked deleted, just punt.
     */

    if (iPtr->flags & DELETED) {
        return;
    }
    
    /*
     * Mark the interpreter as deleted. No further evals will be allowed.
     * Increase the compileEpoch as a signal to compiled bytecodes.
     */

    iPtr->flags |= DELETED;
    iPtr->compileEpoch++;


    /*
     * Ensure that the interpreter is eventually deleted.
     */

    Tcl_EventuallyFree((ClientData) interp,
            (Tcl_FreeProc *) DeleteInterpProc);
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteInterpProc --
 *
 *	Helper procedure to delete an interpreter. This procedure is
 *	called when the last call to Tcl_Preserve on this interpreter
 *	is matched by a call to Tcl_Release. The procedure cleans up
 *	all resources used in the interpreter and calls all currently
 *	registered interpreter deletion callbacks.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Whatever the interpreter deletion callbacks do. Frees resources
 *	used by the interpreter.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteInterpProc(interp)
    Tcl_Interp *interp;			/* Interpreter to delete. */
{
    Interp *iPtr = (Interp *) interp;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    Tcl_HashTable *hTablePtr;
    ResolverScheme *resPtr, *nextResPtr;

    /*
     * Punt if there is an error in the Tcl_Release/Tcl_Preserve matchup.
     */
    
    if (iPtr->numLevels > 0) {
        Tcl_Panic("DeleteInterpProc called with active evals");
    }

    /*
     * The interpreter should already be marked deleted; otherwise how
     * did we get here?
     */

    if (!(iPtr->flags & DELETED)) {
        Tcl_Panic("DeleteInterpProc called on interpreter not marked deleted");
    }

    /*
     * Shut down all limit handler callback scripts that call back
     * into this interpreter.  Then eliminate all limit handlers for
     * this interpreter.
     */

    TclRemoveScriptLimitCallbacks(interp);
    TclLimitRemoveAllHandlers(interp);

    /*
     * Dismantle the namespace here, before we clear the assocData. If any
     * background errors occur here, they will be deleted below.
     *
     * Dismantle the namespace after freeing the iPtr->handle so that each
     * bytecode releases its literals without caring to update the literal
     * table, as it will be freed later in this function without further use.
     */
    
    TclCleanupLiteralTable(interp, &(iPtr->literalTable));
    TclHandleFree(iPtr->handle);
    TclTeardownNamespace(iPtr->globalNsPtr);

    /*
     * Delete all the hidden commands.
     */
     
    hTablePtr = iPtr->hiddenCmdTablePtr;
    if (hTablePtr != NULL) {
	/*
	 * Non-pernicious deletion.  The deletion callbacks will not be
	 * allowed to create any new hidden or non-hidden commands.
	 * Tcl_DeleteCommandFromToken() will remove the entry from the
	 * hiddenCmdTablePtr.
	 */
	 
	hPtr = Tcl_FirstHashEntry(hTablePtr, &search);
	for ( ; hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
	    Tcl_DeleteCommandFromToken(interp,
		    (Tcl_Command) Tcl_GetHashValue(hPtr));
	}
	Tcl_DeleteHashTable(hTablePtr);
	ckfree((char *) hTablePtr);
    }
    /*
     * Tear down the math function table.
     */

    for (hPtr = Tcl_FirstHashEntry(&iPtr->mathFuncTable, &search);
	     hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search)) {
	ckfree((char *) Tcl_GetHashValue(hPtr));
    }
    Tcl_DeleteHashTable(&iPtr->mathFuncTable);

    /*
     * Invoke deletion callbacks; note that a callback can create new
     * callbacks, so we iterate.
     */

    while (iPtr->assocData != (Tcl_HashTable *) NULL) {
	AssocData *dPtr;
	
        hTablePtr = iPtr->assocData;
        iPtr->assocData = (Tcl_HashTable *) NULL;
        for (hPtr = Tcl_FirstHashEntry(hTablePtr, &search);
                 hPtr != NULL;
                 hPtr = Tcl_FirstHashEntry(hTablePtr, &search)) {
            dPtr = (AssocData *) Tcl_GetHashValue(hPtr);
            Tcl_DeleteHashEntry(hPtr);
            if (dPtr->proc != NULL) {
                (*dPtr->proc)(dPtr->clientData, interp);
            }
            ckfree((char *) dPtr);
        }
        Tcl_DeleteHashTable(hTablePtr);
        ckfree((char *) hTablePtr);
    }

    /*
     * Finish deleting the global namespace.
     */
    
    Tcl_DeleteNamespace((Tcl_Namespace *) iPtr->globalNsPtr);

    /*
     * Free up the result *after* deleting variables, since variable
     * deletion could have transferred ownership of the result string
     * to Tcl.
     */

    Tcl_FreeResult(interp);
    interp->result = NULL;
    Tcl_DecrRefCount(iPtr->objResultPtr);
    iPtr->objResultPtr = NULL;
    Tcl_DecrRefCount(iPtr->ecVar);
    if (iPtr->errorCode) {
	Tcl_DecrRefCount(iPtr->errorCode);
	iPtr->errorCode = NULL;
    }
    Tcl_DecrRefCount(iPtr->eiVar);
    if (iPtr->errorInfo) {
	Tcl_DecrRefCount(iPtr->errorInfo);
	iPtr->errorInfo = NULL;
    }
    if (iPtr->returnOpts) {
	Tcl_DecrRefCount(iPtr->returnOpts);
    }
    if (iPtr->appendResult != NULL) {
	ckfree(iPtr->appendResult);
        iPtr->appendResult = NULL;
    }
    TclFreePackageInfo(iPtr);
    while (iPtr->tracePtr != NULL) {
	Tcl_DeleteTrace((Tcl_Interp*) iPtr, (Tcl_Trace) iPtr->tracePtr);
    }
    if (iPtr->execEnvPtr != NULL) {
	TclDeleteExecEnv(iPtr->execEnvPtr);
    }
    Tcl_DecrRefCount(iPtr->emptyObjPtr);
    iPtr->emptyObjPtr = NULL;

    resPtr = iPtr->resolverPtr;
    while (resPtr) {
	nextResPtr = resPtr->nextPtr;
	ckfree(resPtr->name);
	ckfree((char *) resPtr);
        resPtr = nextResPtr;
    }
    
    /*
     * Free up literal objects created for scripts compiled by the
     * interpreter.
     */

    TclDeleteLiteralTable(interp, &(iPtr->literalTable));
    ckfree((char *) iPtr);
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_HideCommand --
 *
 *	Makes a command hidden so that it cannot be invoked from within
 *	an interpreter, only from within an ancestor.
 *
 * Results:
 *	A standard Tcl result; also leaves a message in the interp's result
 *	if an error occurs.
 *
 * Side effects:
 *	Removes a command from the command table and create an entry
 *      into the hidden command table under the specified token name.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_HideCommand(interp, cmdName, hiddenCmdToken)
    Tcl_Interp *interp;		/* Interpreter in which to hide command. */
    CONST char *cmdName;	/* Name of command to hide. */
    CONST char *hiddenCmdToken;	/* Token name of the to-be-hidden command. */
{
    Interp *iPtr = (Interp *) interp;
    Tcl_Command cmd;
    Command *cmdPtr;
    Tcl_HashTable *hiddenCmdTablePtr;
    Tcl_HashEntry *hPtr;
    int new;

    if (iPtr->flags & DELETED) {

        /*
         * The interpreter is being deleted. Do not create any new
         * structures, because it is not safe to modify the interpreter.
         */
        
        return TCL_ERROR;
    }

    /*
     * Disallow hiding of commands that are currently in a namespace or
     * renaming (as part of hiding) into a namespace.
     *
     * (because the current implementation with a single global table
     *  and the needed uniqueness of names cause problems with namespaces)
     *
     * we don't need to check for "::" in cmdName because the real check is
     * on the nsPtr below.
     *
     * hiddenCmdToken is just a string which is not interpreted in any way.
     * It may contain :: but the string is not interpreted as a namespace
     * qualifier command name. Thus, hiding foo::bar to foo::bar and then
     * trying to expose or invoke ::foo::bar will NOT work; but if the
     * application always uses the same strings it will get consistent
     * behaviour.
     *
     * But as we currently limit ourselves to the global namespace only
     * for the source, in order to avoid potential confusion,
     * lets prevent "::" in the token too.  --dl
     */

    if (strstr(hiddenCmdToken, "::") != NULL) {
        Tcl_AppendResult(interp,
                "cannot use namespace qualifiers in hidden command",
		" token (rename)", (char *) NULL);
        return TCL_ERROR;
    }

    /*
     * Find the command to hide. An error is returned if cmdName can't
     * be found. Look up the command only from the global namespace.
     * Full path of the command must be given if using namespaces.
     */

    cmd = Tcl_FindCommand(interp, cmdName, (Tcl_Namespace *) NULL,
	    /*flags*/ TCL_LEAVE_ERR_MSG | TCL_GLOBAL_ONLY);
    if (cmd == (Tcl_Command) NULL) {
	return TCL_ERROR;
    }
    cmdPtr = (Command *) cmd;

    /*
     * Check that the command is really in global namespace
     */

    if ( cmdPtr->nsPtr != iPtr->globalNsPtr ) {
        Tcl_AppendResult(interp, "can only hide global namespace commands",
		" (use rename then hide)", (char *) NULL);
        return TCL_ERROR;
    }
    
    /*
     * Initialize the hidden command table if necessary.
     */

    hiddenCmdTablePtr = iPtr->hiddenCmdTablePtr;
    if (hiddenCmdTablePtr == NULL) {
        hiddenCmdTablePtr = (Tcl_HashTable *)
	        ckalloc((unsigned) sizeof(Tcl_HashTable));
        Tcl_InitHashTable(hiddenCmdTablePtr, TCL_STRING_KEYS);
	iPtr->hiddenCmdTablePtr = hiddenCmdTablePtr;
    }

    /*
     * It is an error to move an exposed command to a hidden command with
     * hiddenCmdToken if a hidden command with the name hiddenCmdToken already
     * exists.
     */
    
    hPtr = Tcl_CreateHashEntry(hiddenCmdTablePtr, hiddenCmdToken, &new);
    if (!new) {
        Tcl_AppendResult(interp, "hidden command named \"", hiddenCmdToken,
		"\" already exists", (char *) NULL);
        return TCL_ERROR;
    }

    /*
     * Nb : This code is currently 'like' a rename to a specialy set apart
     * name table. Changes here and in TclRenameCommand must
     * be kept in synch untill the common parts are actually
     * factorized out.
     */

    /*
     * Remove the hash entry for the command from the interpreter command
     * table. This is like deleting the command, so bump its command epoch;
     * this invalidates any cached references that point to the command.
     */

    if (cmdPtr->hPtr != NULL) {
        Tcl_DeleteHashEntry(cmdPtr->hPtr);
        cmdPtr->hPtr = (Tcl_HashEntry *) NULL;
	cmdPtr->cmdEpoch++;
    }

    /*
     * The list of command exported from the namespace might have
     * changed.  However, we do not need to recompute this just yet;
     * next time we need the info will be soon enough.
     */

    TclInvalidateNsCmdLookup(cmdPtr->nsPtr);

    /*
     * Now link the hash table entry with the command structure.
     * We ensured above that the nsPtr was right.
     */
    
    cmdPtr->hPtr = hPtr;
    Tcl_SetHashValue(hPtr, (ClientData) cmdPtr);

    /*
     * If the command being hidden has a compile procedure, increment the
     * interpreter's compileEpoch to invalidate its compiled code. This
     * makes sure that we don't later try to execute old code compiled with
     * command-specific (i.e., inline) bytecodes for the now-hidden
     * command. This field is checked in Tcl_EvalObj and ObjInterpProc,
     * and code whose compilation epoch doesn't match is recompiled.
     */

    if (cmdPtr->compileProc != NULL) {
	iPtr->compileEpoch++;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ExposeCommand --
 *
 *	Makes a previously hidden command callable from inside the
 *	interpreter instead of only by its ancestors.
 *
 * Results:
 *	A standard Tcl result. If an error occurs, a message is left
 *	in the interp's result.
 *
 * Side effects:
 *	Moves commands from one hash table to another.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ExposeCommand(interp, hiddenCmdToken, cmdName)
    Tcl_Interp *interp;		/* Interpreter in which to make command
                                 * callable. */
    CONST char *hiddenCmdToken;	/* Name of hidden command. */
    CONST char *cmdName;	/* Name of to-be-exposed command. */
{
    Interp *iPtr = (Interp *) interp;
    Command *cmdPtr;
    Namespace *nsPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashTable *hiddenCmdTablePtr;
    int new;

    if (iPtr->flags & DELETED) {
        /*
         * The interpreter is being deleted. Do not create any new
         * structures, because it is not safe to modify the interpreter.
         */
        
        return TCL_ERROR;
    }

    /*
     * Check that we have a regular name for the command
     * (that the user is not trying to do an expose and a rename
     *  (to another namespace) at the same time)
     */

    if (strstr(cmdName, "::") != NULL) {
        Tcl_AppendResult(interp, "can not expose to a namespace ",
		"(use expose to toplevel, then rename)", (char *) NULL);
        return TCL_ERROR;
    }

    /*
     * Get the command from the hidden command table:
     */

    hPtr = NULL;
    hiddenCmdTablePtr = iPtr->hiddenCmdTablePtr;
    if (hiddenCmdTablePtr != NULL) {
	hPtr = Tcl_FindHashEntry(hiddenCmdTablePtr, hiddenCmdToken);
    }
    if (hPtr == (Tcl_HashEntry *) NULL) {
        Tcl_AppendResult(interp, "unknown hidden command \"", hiddenCmdToken,
                "\"", (char *) NULL);
        return TCL_ERROR;
    }
    cmdPtr = (Command *) Tcl_GetHashValue(hPtr);
    

    /*
     * Check that we have a true global namespace
     * command (enforced by Tcl_HideCommand() but let's double
     * check. (If it was not, we would not really know how to
     * handle it).
     */
    if ( cmdPtr->nsPtr != iPtr->globalNsPtr ) {
	/* 
	 * This case is theoritically impossible,
	 * we might rather Tcl_Panic() than 'nicely' erroring out ?
	 */
        Tcl_AppendResult(interp,
                "trying to expose a non global command name space command",
		(char *) NULL);
        return TCL_ERROR;
    }
    
    /* This is the global table */
    nsPtr = cmdPtr->nsPtr;

    /*
     * It is an error to overwrite an existing exposed command as a result
     * of exposing a previously hidden command.
     */

    hPtr = Tcl_CreateHashEntry(&nsPtr->cmdTable, cmdName, &new);
    if (!new) {
        Tcl_AppendResult(interp, "exposed command \"", cmdName,
                "\" already exists", (char *) NULL);
        return TCL_ERROR;
    }

    /*
     * The list of command exported from the namespace might have
     * changed.  However, we do not need to recompute this just yet;
     * next time we need the info will be soon enough.
     */

    TclInvalidateNsCmdLookup(nsPtr);

    /*
     * Remove the hash entry for the command from the interpreter hidden
     * command table.
     */

    if (cmdPtr->hPtr != NULL) {
        Tcl_DeleteHashEntry(cmdPtr->hPtr);
        cmdPtr->hPtr = NULL;
    }

    /*
     * Now link the hash table entry with the command structure.
     * This is like creating a new command, so deal with any shadowing
     * of commands in the global namespace.
     */
    
    cmdPtr->hPtr = hPtr;

    Tcl_SetHashValue(hPtr, (ClientData) cmdPtr);

    /*
     * Not needed as we are only in the global namespace
     * (but would be needed again if we supported namespace command hiding)
     *
     * TclResetShadowedCmdRefs(interp, cmdPtr);
     */


    /*
     * If the command being exposed has a compile procedure, increment
     * interpreter's compileEpoch to invalidate its compiled code. This
     * makes sure that we don't later try to execute old code compiled
     * assuming the command is hidden. This field is checked in Tcl_EvalObj
     * and ObjInterpProc, and code whose compilation epoch doesn't match is
     * recompiled.
     */

    if (cmdPtr->compileProc != NULL) {
	iPtr->compileEpoch++;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateCommand --
 *
 *	Define a new command in a command table.
 *
 * Results:
 *	The return value is a token for the command, which can
 *	be used in future calls to Tcl_GetCommandName.
 *
 * Side effects:
 *	If a command named cmdName already exists for interp, it is deleted.
 *	In the future, when cmdName is seen as the name of a command by
 *	Tcl_Eval, proc will be called. To support the bytecode interpreter,
 *	the command is created with a wrapper Tcl_ObjCmdProc
 *	(TclInvokeStringCommand) that eventially calls proc. When the
 *	command is deleted from the table, deleteProc will be called.
 *	See the manual entry for details on the calling sequence.
 *
 *----------------------------------------------------------------------
 */

Tcl_Command
Tcl_CreateCommand(interp, cmdName, proc, clientData, deleteProc)
    Tcl_Interp *interp;		/* Token for command interpreter returned by
				 * a previous call to Tcl_CreateInterp. */
    CONST char *cmdName;	/* Name of command. If it contains namespace
				 * qualifiers, the new command is put in the
				 * specified namespace; otherwise it is put
				 * in the global namespace. */
    Tcl_CmdProc *proc;		/* Procedure to associate with cmdName. */
    ClientData clientData;	/* Arbitrary value passed to string proc. */
    Tcl_CmdDeleteProc *deleteProc;
				/* If not NULL, gives a procedure to call
				 * when this command is deleted. */
{
    Interp *iPtr = (Interp *) interp;
    ImportRef *oldRefPtr = NULL;
    Namespace *nsPtr, *dummy1, *dummy2;
    Command *cmdPtr, *refCmdPtr;
    Tcl_HashEntry *hPtr;
    CONST char *tail;
    int new;
    ImportedCmdData *dataPtr;

    if (iPtr->flags & DELETED) {
	/*
	 * The interpreter is being deleted.  Don't create any new
	 * commands; it's not safe to muck with the interpreter anymore.
	 */

	return (Tcl_Command) NULL;
    }

    /*
     * Determine where the command should reside. If its name contains 
     * namespace qualifiers, we put it in the specified namespace; 
     * otherwise, we always put it in the global namespace.
     */

    if (strstr(cmdName, "::") != NULL) {
       TclGetNamespaceForQualName(interp, cmdName, (Namespace *) NULL,
           TCL_CREATE_NS_IF_UNKNOWN, &nsPtr, &dummy1, &dummy2, &tail);
       if ((nsPtr == NULL) || (tail == NULL)) {
	    return (Tcl_Command) NULL;
	}
    } else {
	nsPtr = iPtr->globalNsPtr;
	tail = cmdName;
    }
    
    hPtr = Tcl_CreateHashEntry(&nsPtr->cmdTable, tail, &new);
    if (!new) {
	/*
	 * Command already exists. Delete the old one.
	 * Be careful to preserve any existing import links so we can
	 * restore them down below.  That way, you can redefine a
	 * command and its import status will remain intact.
	 */

	cmdPtr = (Command *) Tcl_GetHashValue(hPtr);
	oldRefPtr = cmdPtr->importRefPtr;
	cmdPtr->importRefPtr = NULL;

	Tcl_DeleteCommandFromToken(interp, (Tcl_Command) cmdPtr);
	hPtr = Tcl_CreateHashEntry(&nsPtr->cmdTable, tail, &new);
	if (!new) {
	    /*
	     * If the deletion callback recreated the command, just throw
             * away the new command (if we try to delete it again, we
             * could get stuck in an infinite loop).
	     */

	     ckfree((char*) Tcl_GetHashValue(hPtr));
	}
    } else {
	/*
	 * The list of command exported from the namespace might have
	 * changed.  However, we do not need to recompute this just
	 * yet; next time we need the info will be soon enough.
	 */

	TclInvalidateNsCmdLookup(nsPtr);
    }
    cmdPtr = (Command *) ckalloc(sizeof(Command));
    Tcl_SetHashValue(hPtr, cmdPtr);
    cmdPtr->hPtr = hPtr;
    cmdPtr->nsPtr = nsPtr;
    cmdPtr->refCount = 1;
    cmdPtr->cmdEpoch = 0;
    cmdPtr->compileProc = (CompileProc *) NULL;
    cmdPtr->objProc = TclInvokeStringCommand;
    cmdPtr->objClientData = (ClientData) cmdPtr;
    cmdPtr->proc = proc;
    cmdPtr->clientData = clientData;
    cmdPtr->deleteProc = deleteProc;
    cmdPtr->deleteData = clientData;
    cmdPtr->flags = 0;
    cmdPtr->importRefPtr = NULL;
    cmdPtr->tracePtr = NULL;

    /*
     * Plug in any existing import references found above.  Be sure
     * to update all of these references to point to the new command.
     */

    if (oldRefPtr != NULL) {
	cmdPtr->importRefPtr = oldRefPtr;
	while (oldRefPtr != NULL) {
	    refCmdPtr = oldRefPtr->importedCmdPtr;
	    dataPtr = (ImportedCmdData*)refCmdPtr->objClientData;
	    dataPtr->realCmdPtr = cmdPtr;
	    oldRefPtr = oldRefPtr->nextPtr;
	}
    }

    /*
     * We just created a command, so in its namespace and all of its parent
     * namespaces, it may shadow global commands with the same name. If any
     * shadowed commands are found, invalidate all cached command references
     * in the affected namespaces.
     */
    
    TclResetShadowedCmdRefs(interp, cmdPtr);
    return (Tcl_Command) cmdPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateObjCommand --
 *
 *	Define a new object-based command in a command table.
 *
 * Results:
 *	The return value is a token for the command, which can
 *	be used in future calls to Tcl_GetCommandName.
 *
 * Side effects:
 *	If no command named "cmdName" already exists for interp, one is
 *	created. Otherwise, if a command does exist, then if the
 *	object-based Tcl_ObjCmdProc is TclInvokeStringCommand, we assume
 *	Tcl_CreateCommand was called previously for the same command and
 *	just set its Tcl_ObjCmdProc to the argument "proc"; otherwise, we
 *	delete the old command.
 *
 *	In the future, during bytecode evaluation when "cmdName" is seen as
 *	the name of a command by Tcl_EvalObj or Tcl_Eval, the object-based
 *	Tcl_ObjCmdProc proc will be called. When the command is deleted from
 *	the table, deleteProc will be called. See the manual entry for
 *	details on the calling sequence.
 *
 *----------------------------------------------------------------------
 */

Tcl_Command
Tcl_CreateObjCommand(interp, cmdName, proc, clientData, deleteProc)
    Tcl_Interp *interp;		/* Token for command interpreter (returned
				 * by previous call to Tcl_CreateInterp). */
    CONST char *cmdName;	/* Name of command. If it contains namespace
				 * qualifiers, the new command is put in the
				 * specified namespace; otherwise it is put
				 * in the global namespace. */
    Tcl_ObjCmdProc *proc;	/* Object-based procedure to associate with
				 * name. */
    ClientData clientData;	/* Arbitrary value to pass to object
    				 * procedure. */
    Tcl_CmdDeleteProc *deleteProc;
				/* If not NULL, gives a procedure to call
				 * when this command is deleted. */
{
    Interp *iPtr = (Interp *) interp;
    ImportRef *oldRefPtr = NULL;
    Namespace *nsPtr, *dummy1, *dummy2;
    Command *cmdPtr, *refCmdPtr;
    Tcl_HashEntry *hPtr;
    CONST char *tail;
    int new;
    ImportedCmdData *dataPtr;

    if (iPtr->flags & DELETED) {
	/*
	 * The interpreter is being deleted.  Don't create any new
	 * commands;  it's not safe to muck with the interpreter anymore.
	 */

	return (Tcl_Command) NULL;
    }

    /*
     * Determine where the command should reside. If its name contains 
     * namespace qualifiers, we put it in the specified namespace; 
     * otherwise, we always put it in the global namespace.
     */

    if (strstr(cmdName, "::") != NULL) {
       TclGetNamespaceForQualName(interp, cmdName, (Namespace *) NULL,
           TCL_CREATE_NS_IF_UNKNOWN, &nsPtr, &dummy1, &dummy2, &tail);
       if ((nsPtr == NULL) || (tail == NULL)) {
	    return (Tcl_Command) NULL;
	}
    } else {
	nsPtr = iPtr->globalNsPtr;
	tail = cmdName;
    }

    hPtr = Tcl_CreateHashEntry(&nsPtr->cmdTable, tail, &new);
    if (!new) {
	cmdPtr = (Command *) Tcl_GetHashValue(hPtr);

	/*
	 * Command already exists. If its object-based Tcl_ObjCmdProc is
	 * TclInvokeStringCommand, we just set its Tcl_ObjCmdProc to the
	 * argument "proc". Otherwise, we delete the old command. 
	 */

	if (cmdPtr->objProc == TclInvokeStringCommand) {
	    cmdPtr->objProc = proc;
	    cmdPtr->objClientData = clientData;
            cmdPtr->deleteProc = deleteProc;
            cmdPtr->deleteData = clientData;
	    return (Tcl_Command) cmdPtr;
	}

	/*
	 * Otherwise, we delete the old command.  Be careful to preserve
	 * any existing import links so we can restore them down below.
	 * That way, you can redefine a command and its import status
	 * will remain intact.
	 */

	oldRefPtr = cmdPtr->importRefPtr;
	cmdPtr->importRefPtr = NULL;

	Tcl_DeleteCommandFromToken(interp, (Tcl_Command) cmdPtr);
	hPtr = Tcl_CreateHashEntry(&nsPtr->cmdTable, tail, &new);
	if (!new) {
	    /*
	     * If the deletion callback recreated the command, just throw
	     * away the new command (if we try to delete it again, we
	     * could get stuck in an infinite loop).
	     */

	     ckfree((char *) Tcl_GetHashValue(hPtr));
	}
    } else {
	/*
	 * The list of command exported from the namespace might have
	 * changed.  However, we do not need to recompute this just
	 * yet; next time we need the info will be soon enough.
	 */

	TclInvalidateNsCmdLookup(nsPtr);
    }
    cmdPtr = (Command *) ckalloc(sizeof(Command));
    Tcl_SetHashValue(hPtr, cmdPtr);
    cmdPtr->hPtr = hPtr;
    cmdPtr->nsPtr = nsPtr;
    cmdPtr->refCount = 1;
    cmdPtr->cmdEpoch = 0;
    cmdPtr->compileProc = (CompileProc *) NULL;
    cmdPtr->objProc = proc;
    cmdPtr->objClientData = clientData;
    cmdPtr->proc = TclInvokeObjectCommand;
    cmdPtr->clientData = (ClientData) cmdPtr;
    cmdPtr->deleteProc = deleteProc;
    cmdPtr->deleteData = clientData;
    cmdPtr->flags = 0;
    cmdPtr->importRefPtr = NULL;
    cmdPtr->tracePtr = NULL;

    /*
     * Plug in any existing import references found above.  Be sure
     * to update all of these references to point to the new command.
     */

    if (oldRefPtr != NULL) {
	cmdPtr->importRefPtr = oldRefPtr;
	while (oldRefPtr != NULL) {
	    refCmdPtr = oldRefPtr->importedCmdPtr;
	    dataPtr = (ImportedCmdData*)refCmdPtr->objClientData;
	    dataPtr->realCmdPtr = cmdPtr;
	    oldRefPtr = oldRefPtr->nextPtr;
	}
    }
    
    /*
     * We just created a command, so in its namespace and all of its parent
     * namespaces, it may shadow global commands with the same name. If any
     * shadowed commands are found, invalidate all cached command references
     * in the affected namespaces.
     */
    
    TclResetShadowedCmdRefs(interp, cmdPtr);
    return (Tcl_Command) cmdPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclInvokeStringCommand --
 *
 *	"Wrapper" Tcl_ObjCmdProc used to call an existing string-based
 *	Tcl_CmdProc if no object-based procedure exists for a command. A
 *	pointer to this procedure is stored as the Tcl_ObjCmdProc in a
 *	Command structure. It simply turns around and calls the string
 *	Tcl_CmdProc in the Command structure.
 *
 * Results:
 *	A standard Tcl object result value.
 *
 * Side effects:
 *	Besides those side effects of the called Tcl_CmdProc,
 *	TclInvokeStringCommand allocates and frees storage.
 *
 *----------------------------------------------------------------------
 */

int
TclInvokeStringCommand(clientData, interp, objc, objv)
    ClientData clientData;	/* Points to command's Command structure. */
    Tcl_Interp *interp;		/* Current interpreter. */
    register int objc;		/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    register Command *cmdPtr = (Command *) clientData;
    register int i;
    int result;

    /*
     * This procedure generates an argv array for the string arguments. It
     * starts out with stack-allocated space but uses dynamically-allocated
     * storage if needed.
     */

#define NUM_ARGS 20
    CONST char *(argStorage[NUM_ARGS]);
    CONST char **argv = argStorage;

    /*
     * Create the string argument array "argv". Make sure argv is large
     * enough to hold the objc arguments plus 1 extra for the zero
     * end-of-argv word.
     */

    if ((objc + 1) > NUM_ARGS) {
	argv = (CONST char **) ckalloc((unsigned)(objc + 1) * sizeof(char *));
    }

    for (i = 0;  i < objc;  i++) {
	argv[i] = Tcl_GetString(objv[i]);
    }
    argv[objc] = 0;

    /*
     * Invoke the command's string-based Tcl_CmdProc.
     */

    result = (*cmdPtr->proc)(cmdPtr->clientData, interp, objc, argv);

    /*
     * Free the argv array if malloc'ed storage was used.
     */

    if (argv != argStorage) {
	ckfree((char *) argv);
    }
    return result;
#undef NUM_ARGS
}

/*
 *----------------------------------------------------------------------
 *
 * TclInvokeObjectCommand --
 *
 *	"Wrapper" Tcl_CmdProc used to call an existing object-based
 *	Tcl_ObjCmdProc if no string-based procedure exists for a command.
 *	A pointer to this procedure is stored as the Tcl_CmdProc in a
 *	Command structure. It simply turns around and calls the object
 *	Tcl_ObjCmdProc in the Command structure.
 *
 * Results:
 *	A standard Tcl string result value.
 *
 * Side effects:
 *	Besides those side effects of the called Tcl_CmdProc,
 *	TclInvokeStringCommand allocates and frees storage.
 *
 *----------------------------------------------------------------------
 */

int
TclInvokeObjectCommand(clientData, interp, argc, argv)
    ClientData clientData;	/* Points to command's Command structure. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int argc;			/* Number of arguments. */
    register CONST char **argv;	/* Argument strings. */
{
    Command *cmdPtr = (Command *) clientData;
    register Tcl_Obj *objPtr;
    register int i;
    int length, result;

    /*
     * This procedure generates an objv array for object arguments that hold
     * the argv strings. It starts out with stack-allocated space but uses
     * dynamically-allocated storage if needed.
     */

#define NUM_ARGS 20
    Tcl_Obj *(argStorage[NUM_ARGS]);
    register Tcl_Obj **objv = argStorage;

    /*
     * Create the object argument array "objv". Make sure objv is large
     * enough to hold the objc arguments plus 1 extra for the zero
     * end-of-objv word.
     */

    if (argc > NUM_ARGS) {
	objv = (Tcl_Obj **)
	    ckalloc((unsigned)(argc * sizeof(Tcl_Obj *)));
    }

    for (i = 0;  i < argc;  i++) {
	length = strlen(argv[i]);
	TclNewObj(objPtr);
	TclInitStringRep(objPtr, argv[i], length);
	Tcl_IncrRefCount(objPtr);
	objv[i] = objPtr;
    }

    /*
     * Invoke the command's object-based Tcl_ObjCmdProc.
     */

    result = (*cmdPtr->objProc)(cmdPtr->objClientData, interp, argc, objv);

    /*
     * Move the interpreter's object result to the string result, 
     * then reset the object result.
     */

    (void) Tcl_GetStringResult(interp);
    
    /*
     * Decrement the ref counts for the argument objects created above,
     * then free the objv array if malloc'ed storage was used.
     */

    for (i = 0;  i < argc;  i++) {
	objPtr = objv[i];
	Tcl_DecrRefCount(objPtr);
    }
    if (objv != argStorage) {
	ckfree((char *) objv);
    }
    return result;
#undef NUM_ARGS
}

/*
 *----------------------------------------------------------------------
 *
 * TclRenameCommand --
 *
 *      Called to give an existing Tcl command a different name. Both the
 *      old command name and the new command name can have "::" namespace
 *      qualifiers. If the new command has a different namespace context,
 *      the command will be moved to that namespace and will execute in
 *	the context of that new namespace.
 *
 *      If the new command name is NULL or the null string, the command is
 *      deleted.
 *
 * Results:
 *      Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *      If anything goes wrong, an error message is returned in the
 *      interpreter's result object.
 *
 *----------------------------------------------------------------------
 */

int
TclRenameCommand(interp, oldName, newName)
    Tcl_Interp *interp;                 /* Current interpreter. */
    char *oldName;                      /* Existing command name. */
    char *newName;                      /* New command name. */
{
    Interp *iPtr = (Interp *) interp;
    CONST char *newTail;
    Namespace *cmdNsPtr, *newNsPtr, *dummy1, *dummy2;
    Tcl_Command cmd;
    Command *cmdPtr;
    Tcl_HashEntry *hPtr, *oldHPtr;
    int new, result;
    Tcl_Obj* oldFullName;
    Tcl_DString newFullName;

    /*
     * Find the existing command. An error is returned if cmdName can't
     * be found.
     */

    cmd = Tcl_FindCommand(interp, oldName, (Tcl_Namespace *) NULL,
	/*flags*/ 0);
    cmdPtr = (Command *) cmd;
    if (cmdPtr == NULL) {
	Tcl_AppendResult(interp, "can't ",
                ((newName == NULL)||(*newName == '\0'))? "delete":"rename",
                " \"", oldName, "\": command doesn't exist", (char *) NULL);
	return TCL_ERROR;
    }
    cmdNsPtr = cmdPtr->nsPtr;
    oldFullName = Tcl_NewObj();
    Tcl_IncrRefCount( oldFullName );
    Tcl_GetCommandFullName( interp, cmd, oldFullName );

    /*
     * If the new command name is NULL or empty, delete the command. Do this
     * with Tcl_DeleteCommandFromToken, since we already have the command.
     */
    
    if ((newName == NULL) || (*newName == '\0')) {
	Tcl_DeleteCommandFromToken(interp, cmd);
	result = TCL_OK;
	goto done;
    }

    /*
     * Make sure that the destination command does not already exist.
     * The rename operation is like creating a command, so we should
     * automatically create the containing namespaces just like
     * Tcl_CreateCommand would.
     */

    TclGetNamespaceForQualName(interp, newName, (Namespace *) NULL,
       TCL_CREATE_NS_IF_UNKNOWN, &newNsPtr, &dummy1, &dummy2, &newTail);

    if ((newNsPtr == NULL) || (newTail == NULL)) {
	Tcl_AppendResult(interp, "can't rename to \"", newName,
		"\": bad command name", (char *) NULL);
	result = TCL_ERROR;
	goto done;
    }
    if (Tcl_FindHashEntry(&newNsPtr->cmdTable, newTail) != NULL) {
	Tcl_AppendResult(interp, "can't rename to \"", newName,
		 "\": command already exists", (char *) NULL);
	result = TCL_ERROR;
	goto done;
    }

    /*
     * Warning: any changes done in the code here are likely
     * to be needed in Tcl_HideCommand() code too.
     * (until the common parts are extracted out)     --dl
     */

    /*
     * Put the command in the new namespace so we can check for an alias
     * loop. Since we are adding a new command to a namespace, we must
     * handle any shadowing of the global commands that this might create.
     */
    
    oldHPtr = cmdPtr->hPtr;
    hPtr = Tcl_CreateHashEntry(&newNsPtr->cmdTable, newTail, &new);
    Tcl_SetHashValue(hPtr, (ClientData) cmdPtr);
    cmdPtr->hPtr = hPtr;
    cmdPtr->nsPtr = newNsPtr;
    TclResetShadowedCmdRefs(interp, cmdPtr);

    /*
     * Now check for an alias loop. If we detect one, put everything back
     * the way it was and report the error.
     */

    result = TclPreventAliasLoop(interp, interp, (Tcl_Command) cmdPtr);
    if (result != TCL_OK) {
        Tcl_DeleteHashEntry(cmdPtr->hPtr);
        cmdPtr->hPtr = oldHPtr;
        cmdPtr->nsPtr = cmdNsPtr;
	goto done;
    }

    /*
     * The list of command exported from the namespace might have
     * changed.  However, we do not need to recompute this just yet;
     * next time we need the info will be soon enough.  These might
     * refer to the same variable, but that's no big deal.
     */

    TclInvalidateNsCmdLookup(cmdNsPtr);
    TclInvalidateNsCmdLookup(cmdPtr->nsPtr);

    /*
     * Script for rename traces can delete the command "oldName".
     * Therefore increment the reference count for cmdPtr so that
     * it's Command structure is freed only towards the end of this
     * function by calling TclCleanupCommand.
     *
     * The trace procedure needs to get a fully qualified name for
     * old and new commands [Tcl bug #651271], or else there's no way
     * for the trace procedure to get the namespace from which the old
     * command is being renamed!
     */

    Tcl_DStringInit( &newFullName );
    Tcl_DStringAppend( &newFullName, newNsPtr->fullName, -1 );
    if ( newNsPtr != iPtr->globalNsPtr ) {
	Tcl_DStringAppend( &newFullName, "::", 2 );
    }
    Tcl_DStringAppend( &newFullName, newTail, -1 );
    cmdPtr->refCount++;
    CallCommandTraces( iPtr, cmdPtr,
		       Tcl_GetString( oldFullName ),
		       Tcl_DStringValue( &newFullName ),
		       TCL_TRACE_RENAME);
    Tcl_DStringFree( &newFullName );

    /*
     * The new command name is okay, so remove the command from its
     * current namespace. This is like deleting the command, so bump
     * the cmdEpoch to invalidate any cached references to the command.
     */
    
    Tcl_DeleteHashEntry(oldHPtr);
    cmdPtr->cmdEpoch++;

    /*
     * If the command being renamed has a compile procedure, increment the
     * interpreter's compileEpoch to invalidate its compiled code. This
     * makes sure that we don't later try to execute old code compiled for
     * the now-renamed command.
     */

    if (cmdPtr->compileProc != NULL) {
	iPtr->compileEpoch++;
    }

    /*
     * Now free the Command structure, if the "oldName" command has
     * been deleted by invocation of rename traces.
     */
    TclCleanupCommand(cmdPtr);
    result = TCL_OK;

    done:
    TclDecrRefCount( oldFullName );
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetCommandInfo --
 *
 *	Modifies various information about a Tcl command. Note that
 *	this procedure will not change a command's namespace; use
 *	TclRenameCommand to do that. Also, the isNativeObjectProc
 *	member of *infoPtr is ignored.
 *
 * Results:
 *	If cmdName exists in interp, then the information at *infoPtr
 *	is stored with the command in place of the current information
 *	and 1 is returned. If the command doesn't exist then 0 is
 *	returned. 
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_SetCommandInfo(interp, cmdName, infoPtr)
    Tcl_Interp *interp;			/* Interpreter in which to look
					 * for command. */
    CONST char *cmdName;		/* Name of desired command. */
    CONST Tcl_CmdInfo *infoPtr;		/* Where to find information
					 * to store in the command. */
{
    Tcl_Command cmd;

    cmd = Tcl_FindCommand(interp, cmdName, (Tcl_Namespace *) NULL,
            /*flags*/ 0);

    return Tcl_SetCommandInfoFromToken( cmd, infoPtr );

}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetCommandInfoFromToken --
 *
 *	Modifies various information about a Tcl command. Note that
 *	this procedure will not change a command's namespace; use
 *	TclRenameCommand to do that. Also, the isNativeObjectProc
 *	member of *infoPtr is ignored.
 *
 * Results:
 *	If cmdName exists in interp, then the information at *infoPtr
 *	is stored with the command in place of the current information
 *	and 1 is returned. If the command doesn't exist then 0 is
 *	returned. 
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_SetCommandInfoFromToken( cmd, infoPtr )
    Tcl_Command cmd;
    CONST Tcl_CmdInfo* infoPtr;
{
    Command* cmdPtr;		/* Internal representation of the command */

    if (cmd == (Tcl_Command) NULL) {
	return 0;
    }

    /*
     * The isNativeObjectProc and nsPtr members of *infoPtr are ignored.
     */
    
    cmdPtr = (Command *) cmd;
    cmdPtr->proc = infoPtr->proc;
    cmdPtr->clientData = infoPtr->clientData;
    if (infoPtr->objProc == (Tcl_ObjCmdProc *) NULL) {
	cmdPtr->objProc = TclInvokeStringCommand;
	cmdPtr->objClientData = (ClientData) cmdPtr;
    } else {
	cmdPtr->objProc = infoPtr->objProc;
	cmdPtr->objClientData = infoPtr->objClientData;
    }
    cmdPtr->deleteProc = infoPtr->deleteProc;
    cmdPtr->deleteData = infoPtr->deleteData;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetCommandInfo --
 *
 *	Returns various information about a Tcl command.
 *
 * Results:
 *	If cmdName exists in interp, then *infoPtr is modified to
 *	hold information about cmdName and 1 is returned.  If the
 *	command doesn't exist then 0 is returned and *infoPtr isn't
 *	modified.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetCommandInfo(interp, cmdName, infoPtr)
    Tcl_Interp *interp;			/* Interpreter in which to look
					 * for command. */
    CONST char *cmdName;		/* Name of desired command. */
    Tcl_CmdInfo *infoPtr;		/* Where to store information about
					 * command. */
{
    Tcl_Command cmd;

    cmd = Tcl_FindCommand(interp, cmdName, (Tcl_Namespace *) NULL,
            /*flags*/ 0);

    return Tcl_GetCommandInfoFromToken( cmd, infoPtr );

}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetCommandInfoFromToken --
 *
 *	Returns various information about a Tcl command.
 *
 * Results:
 *	Copies information from the command identified by 'cmd' into
 *	a caller-supplied structure and returns 1.  If the 'cmd' is
 *	NULL, leaves the structure untouched and returns 0.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetCommandInfoFromToken( cmd, infoPtr )
    Tcl_Command cmd;
    Tcl_CmdInfo* infoPtr;
{

    Command* cmdPtr;		/* Internal representation of the command */

    if ( cmd == (Tcl_Command) NULL ) {
	return 0;
    }

    /*
     * Set isNativeObjectProc 1 if objProc was registered by a call to
     * Tcl_CreateObjCommand. Otherwise set it to 0.
     */

    cmdPtr = (Command *) cmd;
    infoPtr->isNativeObjectProc =
	    (cmdPtr->objProc != TclInvokeStringCommand);
    infoPtr->objProc = cmdPtr->objProc;
    infoPtr->objClientData = cmdPtr->objClientData;
    infoPtr->proc = cmdPtr->proc;
    infoPtr->clientData = cmdPtr->clientData;
    infoPtr->deleteProc = cmdPtr->deleteProc;
    infoPtr->deleteData = cmdPtr->deleteData;
    infoPtr->namespacePtr = (Tcl_Namespace *) cmdPtr->nsPtr;

    return 1;

}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetCommandName --
 *
 *	Given a token returned by Tcl_CreateCommand, this procedure
 *	returns the current name of the command (which may have changed
 *	due to renaming).
 *
 * Results:
 *	The return value is the name of the given command.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

CONST char *
Tcl_GetCommandName(interp, command)
    Tcl_Interp *interp;		/* Interpreter containing the command. */
    Tcl_Command command;	/* Token for command returned by a previous
				 * call to Tcl_CreateCommand. The command
				 * must not have been deleted. */
{
    Command *cmdPtr = (Command *) command;

    if ((cmdPtr == NULL) || (cmdPtr->hPtr == NULL)) {

	/*
	 * This should only happen if command was "created" after the
	 * interpreter began to be deleted, so there isn't really any
	 * command. Just return an empty string.
	 */

	return "";
    }
    return Tcl_GetHashKey(cmdPtr->hPtr->tablePtr, cmdPtr->hPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetCommandFullName --
 *
 *	Given a token returned by, e.g., Tcl_CreateCommand or
 *	Tcl_FindCommand, this procedure appends to an object the command's
 *	full name, qualified by a sequence of parent namespace names. The
 *	command's fully-qualified name may have changed due to renaming.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The command's fully-qualified name is appended to the string
 *	representation of objPtr. 
 *
 *----------------------------------------------------------------------
 */

void
Tcl_GetCommandFullName(interp, command, objPtr)
    Tcl_Interp *interp;		/* Interpreter containing the command. */
    Tcl_Command command;	/* Token for command returned by a previous
				 * call to Tcl_CreateCommand. The command
				 * must not have been deleted. */
    Tcl_Obj *objPtr;		/* Points to the object onto which the
				 * command's full name is appended. */

{
    Interp *iPtr = (Interp *) interp;
    register Command *cmdPtr = (Command *) command;
    char *name;

    /*
     * Add the full name of the containing namespace, followed by the "::"
     * separator, and the command name.
     */

    if (cmdPtr != NULL) {
	if (cmdPtr->nsPtr != NULL) {
	    Tcl_AppendToObj(objPtr, cmdPtr->nsPtr->fullName, -1);
	    if (cmdPtr->nsPtr != iPtr->globalNsPtr) {
		Tcl_AppendToObj(objPtr, "::", 2);
	    }
	}
	if (cmdPtr->hPtr != NULL) {
	    name = Tcl_GetHashKey(cmdPtr->hPtr->tablePtr, cmdPtr->hPtr);
	    Tcl_AppendToObj(objPtr, name, -1);
	} 
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteCommand --
 *
 *	Remove the given command from the given interpreter.
 *
 * Results:
 *	0 is returned if the command was deleted successfully.
 *	-1 is returned if there didn't exist a command by that name.
 *
 * Side effects:
 *	cmdName will no longer be recognized as a valid command for
 *	interp.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_DeleteCommand(interp, cmdName)
    Tcl_Interp *interp;		/* Token for command interpreter (returned
				 * by a previous Tcl_CreateInterp call). */
    CONST char *cmdName;	/* Name of command to remove. */
{
    Tcl_Command cmd;

    /*
     *  Find the desired command and delete it.
     */

    cmd = Tcl_FindCommand(interp, cmdName, (Tcl_Namespace *) NULL,
            /*flags*/ 0);
    if (cmd == (Tcl_Command) NULL) {
	return -1;
    }
    return Tcl_DeleteCommandFromToken(interp, cmd);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteCommandFromToken --
 *
 *	Removes the given command from the given interpreter. This procedure
 *	resembles Tcl_DeleteCommand, but takes a Tcl_Command token instead
 *	of a command name for efficiency.
 *
 * Results:
 *	0 is returned if the command was deleted successfully.
 *	-1 is returned if there didn't exist a command by that name.
 *
 * Side effects:
 *	The command specified by "cmd" will no longer be recognized as a
 *	valid command for "interp".
 *
 *----------------------------------------------------------------------
 */

int
Tcl_DeleteCommandFromToken(interp, cmd)
    Tcl_Interp *interp;		/* Token for command interpreter returned by
				 * a previous call to Tcl_CreateInterp. */
    Tcl_Command cmd;            /* Token for command to delete. */
{
    Interp *iPtr = (Interp *) interp;
    Command *cmdPtr = (Command *) cmd;
    ImportRef *refPtr, *nextRefPtr;
    Tcl_Command importCmd;

    /*
     * The code here is tricky.  We can't delete the hash table entry
     * before invoking the deletion callback because there are cases
     * where the deletion callback needs to invoke the command (e.g.
     * object systems such as OTcl). However, this means that the
     * callback could try to delete or rename the command. The deleted
     * flag allows us to detect these cases and skip nested deletes.
     */

    if (cmdPtr->flags & CMD_IS_DELETED) {
	/*
	 * Another deletion is already in progress.  Remove the hash
	 * table entry now, but don't invoke a callback or free the
	 * command structure.
	 */

        Tcl_DeleteHashEntry(cmdPtr->hPtr);
	cmdPtr->hPtr = NULL;
	return 0;
    }

    /* 
     * We must delete this command, even though both traces and
     * delete procs may try to avoid this (renaming the command etc).
     * Also traces and delete procs may try to delete the command
     * themsevles.  This flag declares that a delete is in progress
     * and that recursive deletes should be ignored.
     */
    cmdPtr->flags |= CMD_IS_DELETED;

    /*
     * Call trace procedures for the command being deleted. Then delete
     * its traces. 
     */

    if (cmdPtr->tracePtr != NULL) {
	CommandTrace *tracePtr;
	CallCommandTraces(iPtr,cmdPtr,NULL,NULL,TCL_TRACE_DELETE);
	/* Now delete these traces */
	tracePtr = cmdPtr->tracePtr;
	while (tracePtr != NULL) {
	    CommandTrace *nextPtr = tracePtr->nextPtr;
	    if ((--tracePtr->refCount) <= 0) {
		ckfree((char*)tracePtr);
	    }
	    tracePtr = nextPtr;
	}
	cmdPtr->tracePtr = NULL;
    }

    /*
     * The list of command exported from the namespace might have
     * changed.  However, we do not need to recompute this just yet;
     * next time we need the info will be soon enough.
     */

    TclInvalidateNsCmdLookup(cmdPtr->nsPtr);

    /*
     * If the command being deleted has a compile procedure, increment the
     * interpreter's compileEpoch to invalidate its compiled code. This
     * makes sure that we don't later try to execute old code compiled with
     * command-specific (i.e., inline) bytecodes for the now-deleted
     * command. This field is checked in Tcl_EvalObj and ObjInterpProc, and
     * code whose compilation epoch doesn't match is recompiled.
     */

    if (cmdPtr->compileProc != NULL) {
        iPtr->compileEpoch++;
    }

    if (cmdPtr->deleteProc != NULL) {
	/*
	 * Delete the command's client data. If this was an imported command
	 * created when a command was imported into a namespace, this client
	 * data will be a pointer to a ImportedCmdData structure describing
	 * the "real" command that this imported command refers to.
	 */
	
	/*
	 * If you are getting a crash during the call to deleteProc and
	 * cmdPtr->deleteProc is a pointer to the function free(), the
	 * most likely cause is that your extension allocated memory
	 * for the clientData argument to Tcl_CreateObjCommand() with
	 * the ckalloc() macro and you are now trying to deallocate
	 * this memory with free() instead of ckfree(). You should
	 * pass a pointer to your own method that calls ckfree().
	 */

	(*cmdPtr->deleteProc)(cmdPtr->deleteData);
    }

    /*
     * Bump the command epoch counter. This will invalidate all cached
     * references that point to this command.
     */
    
    cmdPtr->cmdEpoch++;

    /*
     * If this command was imported into other namespaces, then imported
     * commands were created that refer back to this command. Delete these
     * imported commands now.
     */

    for (refPtr = cmdPtr->importRefPtr;  refPtr != NULL;
            refPtr = nextRefPtr) {
	nextRefPtr = refPtr->nextPtr;
	importCmd = (Tcl_Command) refPtr->importedCmdPtr;
        Tcl_DeleteCommandFromToken(interp, importCmd);
    }

    /*
     * Don't use hPtr to delete the hash entry here, because it's
     * possible that the deletion callback renamed the command.
     * Instead, use cmdPtr->hptr, and make sure that no-one else
     * has already deleted the hash entry.
     */

    if (cmdPtr->hPtr != NULL) {
	Tcl_DeleteHashEntry(cmdPtr->hPtr);
    }

    /*
     * Mark the Command structure as no longer valid. This allows
     * TclExecuteByteCode to recognize when a Command has logically been
     * deleted and a pointer to this Command structure cached in a CmdName
     * object is invalid. TclExecuteByteCode will look up the command again
     * in the interpreter's command hashtable.
     */

    cmdPtr->objProc = NULL;

    /*
     * Now free the Command structure, unless there is another reference to
     * it from a CmdName Tcl object in some ByteCode code sequence. In that
     * case, delay the cleanup until all references are either discarded
     * (when a ByteCode is freed) or replaced by a new reference (when a
     * cached CmdName Command reference is found to be invalid and
     * TclExecuteByteCode looks up the command in the command hashtable).
     */
    
    TclCleanupCommand(cmdPtr);
    return 0;
}

static char *
CallCommandTraces(iPtr, cmdPtr, oldName, newName, flags)
    Interp *iPtr;		/* Interpreter containing command. */
    Command *cmdPtr;		/* Command whose traces are to be
				 * invoked. */
    CONST char *oldName;        /* Command's old name, or NULL if we
                                 * must get the name from cmdPtr */
    CONST char *newName;        /* Command's new name, or NULL if
                                 * the command is not being renamed */
    int flags;			/* Flags indicating the type of traces
				 * to trigger, either TCL_TRACE_DELETE
				 * or TCL_TRACE_RENAME. */
{
    register CommandTrace *tracePtr;
    ActiveCommandTrace active;
    char *result;
    Tcl_Obj *oldNamePtr = NULL;

    if (cmdPtr->flags & CMD_TRACE_ACTIVE) {
	/* 
	 * While a rename trace is active, we will not process any more
	 * rename traces; while a delete trace is active we will never
	 * reach here -- because Tcl_DeleteCommandFromToken checks for the
	 * condition (cmdPtr->flags & CMD_IS_DELETED) and returns immediately
	 * when a command deletion is in progress.  For all other traces,
	 * delete traces will not be invoked but a call to TraceCommandProc
	 * will ensure that tracePtr->clientData is freed whenever the
	 * command "oldName" is deleted.
	 */
	if (cmdPtr->flags & TCL_TRACE_RENAME) {
	    flags &= ~TCL_TRACE_RENAME;
	}
	if (flags == 0) {
	    return NULL;
	}
    }
    cmdPtr->flags |= CMD_TRACE_ACTIVE;
    cmdPtr->refCount++;
    
    result = NULL;
    active.nextPtr = iPtr->activeCmdTracePtr;
    iPtr->activeCmdTracePtr = &active;

    if (flags & TCL_TRACE_DELETE) {
	flags |= TCL_TRACE_DESTROYED;
    }
    active.cmdPtr = cmdPtr;
    
    Tcl_Preserve((ClientData) iPtr);
    
    for (tracePtr = cmdPtr->tracePtr; tracePtr != NULL;
	 tracePtr = active.nextTracePtr) {
	active.nextTracePtr = tracePtr->nextPtr;
	if (!(tracePtr->flags & flags)) {
	    continue;
	}
	cmdPtr->flags |= tracePtr->flags;
	if (oldName == NULL) {
	    TclNewObj(oldNamePtr);
	    Tcl_IncrRefCount(oldNamePtr);
	    Tcl_GetCommandFullName((Tcl_Interp *) iPtr, 
	            (Tcl_Command) cmdPtr, oldNamePtr);
	    oldName = TclGetString(oldNamePtr);
	}
	tracePtr->refCount++;
	(*tracePtr->traceProc)(tracePtr->clientData,
		(Tcl_Interp *) iPtr, oldName, newName, flags);
	cmdPtr->flags &= ~tracePtr->flags;
	if ((--tracePtr->refCount) <= 0) {
	    ckfree((char*)tracePtr);
	}
    }

    /*
     * If a new object was created to hold the full oldName,
     * free it now.
     */

    if (oldNamePtr != NULL) {
	TclDecrRefCount(oldNamePtr);
    }

    /*
     * Restore the variable's flags, remove the record of our active
     * traces, and then return.
     */

    cmdPtr->flags &= ~CMD_TRACE_ACTIVE;
    cmdPtr->refCount--;
    iPtr->activeCmdTracePtr = active.nextPtr;
    Tcl_Release((ClientData) iPtr);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCleanupCommand --
 *
 *	This procedure frees up a Command structure unless it is still
 *	referenced from an interpreter's command hashtable or from a CmdName
 *	Tcl object representing the name of a command in a ByteCode
 *	instruction sequence. 
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory gets freed unless a reference to the Command structure still
 *	exists. In that case the cleanup is delayed until the command is
 *	deleted or when the last ByteCode referring to it is freed.
 *
 *----------------------------------------------------------------------
 */

void
TclCleanupCommand(cmdPtr)
    register Command *cmdPtr;	/* Points to the Command structure to
				 * be freed. */
{
    cmdPtr->refCount--;
    if (cmdPtr->refCount <= 0) {
	ckfree((char *) cmdPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateMathFunc --
 *
 *	Creates a new math function for expressions in a given
 *	interpreter.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The function defined by "name" is created or redefined. If the
 *	function already exists then its definition is replaced; this
 *	includes the builtin functions. Redefining a builtin function forces
 *	all existing code to be invalidated since that code may be compiled
 *	using an instruction specific to the replaced function. In addition,
 *	redefioning a non-builtin function will force existing code to be
 *	invalidated if the number of arguments has changed.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_CreateMathFunc(interp, name, numArgs, argTypes, proc, clientData)
    Tcl_Interp *interp;			/* Interpreter in which function is
					 * to be available. */
    CONST char *name;			/* Name of function (e.g. "sin"). */
    int numArgs;			/* Nnumber of arguments required by
					 * function. */
    Tcl_ValueType *argTypes;		/* Array of types acceptable for
					 * each argument. */
    Tcl_MathProc *proc;			/* Procedure that implements the
					 * math function. */
    ClientData clientData;		/* Additional value to pass to the
					 * function. */
{
    Interp *iPtr = (Interp *) interp;
    Tcl_HashEntry *hPtr;
    MathFunc *mathFuncPtr;
    int new, i;

    hPtr = Tcl_CreateHashEntry(&iPtr->mathFuncTable, name, &new);
    if (new) {
	Tcl_SetHashValue(hPtr, ckalloc(sizeof(MathFunc)));
    }
    mathFuncPtr = (MathFunc *) Tcl_GetHashValue(hPtr);

    if (!new) {	
	if (mathFuncPtr->builtinFuncIndex >= 0) {
	    /*
	     * We are redefining a builtin math function. Invalidate the
             * interpreter's existing code by incrementing its
             * compileEpoch member. This field is checked in Tcl_EvalObj
             * and ObjInterpProc, and code whose compilation epoch doesn't
             * match is recompiled. Newly compiled code will no longer
             * treat the function as builtin.
	     */

	    iPtr->compileEpoch++;
	} else {
	    /*
	     * A non-builtin function is being redefined. We must invalidate
             * existing code if the number of arguments has changed. This
	     * is because existing code was compiled assuming that number.
	     */

	    if (numArgs != mathFuncPtr->numArgs) {
		iPtr->compileEpoch++;
	    }
	}
    }
    
    mathFuncPtr->builtinFuncIndex = -1;	/* can't be a builtin function */
    if (numArgs > MAX_MATH_ARGS) {
	numArgs = MAX_MATH_ARGS;
    }
    mathFuncPtr->numArgs = numArgs;
    for (i = 0;  i < numArgs;  i++) {
	mathFuncPtr->argTypes[i] = argTypes[i];
    }
    mathFuncPtr->proc = proc;
    mathFuncPtr->clientData = clientData;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetMathFuncInfo --
 *
 *	Discovers how a particular math function was created in a given
 *	interpreter.
 *
 * Results:
 *	TCL_OK if it succeeds, TCL_ERROR else (leaving an error message
 *	in the interpreter result if that happens.)
 *
 * Side effects:
 *	If this function succeeds, the variables pointed to by the
 *	numArgsPtr and argTypePtr arguments will be updated to detail the
 *	arguments allowed by the function.  The variable pointed to by the
 *	procPtr argument will be set to NULL if the function is a builtin
 *	function, and will be set to the address of the C function used to
 *	implement the math function otherwise (in which case the variable
 *	pointed to by the clientDataPtr argument will also be updated.)
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetMathFuncInfo(interp, name, numArgsPtr, argTypesPtr, procPtr,
		    clientDataPtr)
    Tcl_Interp *interp;
    CONST char *name;
    int *numArgsPtr;
    Tcl_ValueType **argTypesPtr;
    Tcl_MathProc **procPtr;
    ClientData *clientDataPtr;
{
    Interp *iPtr = (Interp *) interp;
    Tcl_HashEntry *hPtr;
    MathFunc *mathFuncPtr;
    Tcl_ValueType *argTypes;
    int i,numArgs;

    hPtr = Tcl_FindHashEntry(&iPtr->mathFuncTable, name);
    if (hPtr == NULL) {
        Tcl_AppendResult(interp, "math function \"", name,
		"\" not known in this interpreter", (char *) NULL);
	return TCL_ERROR;
    }
    mathFuncPtr = (MathFunc *) Tcl_GetHashValue(hPtr);

    *numArgsPtr = numArgs = mathFuncPtr->numArgs;
    if (numArgs == 0) {
	/* Avoid doing zero-sized allocs... */
	numArgs = 1;
    }
    *argTypesPtr = argTypes =
	(Tcl_ValueType *)ckalloc(numArgs * sizeof(Tcl_ValueType));
    for (i = 0; i < mathFuncPtr->numArgs; i++) {
	argTypes[i] = mathFuncPtr->argTypes[i];
    }

    if (mathFuncPtr->builtinFuncIndex == -1) {
	*procPtr = (Tcl_MathProc *) NULL;
    } else {
	*procPtr = mathFuncPtr->proc;
	*clientDataPtr = mathFuncPtr->clientData;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListMathFuncs --
 *
 *	Produces a list of all the math functions defined in a given
 *	interpreter.
 *
 * Results:
 *	A pointer to a Tcl_Obj structure with a reference count of zero,
 *	or NULL in the case of an error (in which case a suitable error
 *	message will be left in the interpreter result.)
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
Tcl_ListMathFuncs(interp, pattern)
    Tcl_Interp *interp;
    CONST char *pattern;
{
    Interp *iPtr = (Interp *) interp;
    Tcl_Obj *resultList = Tcl_NewObj();
    register Tcl_HashEntry *hPtr;
    Tcl_HashSearch hSearch;
    CONST char *name;

    for (hPtr = Tcl_FirstHashEntry(&iPtr->mathFuncTable, &hSearch);
	 hPtr != NULL; hPtr = Tcl_NextHashEntry(&hSearch)) {
        name = Tcl_GetHashKey(&iPtr->mathFuncTable, hPtr);
	if ((pattern == NULL || Tcl_StringMatch(name, pattern)) &&
	    /* I don't expect this to fail, but... */
	    Tcl_ListObjAppendElement(interp, resultList,
				     Tcl_NewStringObj(name,-1)) != TCL_OK) {
	    Tcl_DecrRefCount(resultList);
	    return NULL;
	}
    }
    return resultList;
}

/*
 *----------------------------------------------------------------------
 *
 * TclInterpReady --
 *
 *	Check if an interpreter is ready to eval commands or scripts, 
 *      i.e., if it was not deleted and if the nesting level is not 
 *      too high.
 *
 * Results:
 *	The return value is TCL_OK if it the interpreter is ready, 
 *      TCL_ERROR otherwise.
 *
 * Side effects:
 *	The interpreters object and string results are cleared.
 *
 *----------------------------------------------------------------------
 */

int 
TclInterpReady(interp)
    Tcl_Interp *interp;
{
    register Interp *iPtr = (Interp *) interp;

    /*
     * Reset both the interpreter's string and object results and clear 
     * out any previous error information. 
     */

    Tcl_ResetResult(interp);

    /*
     * If the interpreter has been deleted, return an error.
     */
    
    if (iPtr->flags & DELETED) {
	Tcl_ResetResult(interp);
	Tcl_AppendResult(interp,
	        "attempt to call eval in deleted interpreter", (char *) NULL);
	Tcl_SetErrorCode(interp, "CORE", "IDELETE",
	        "attempt to call eval in deleted interpreter",
		(char *) NULL);
	return TCL_ERROR;
    }

    /*
     * Check depth of nested calls to Tcl_Eval:  if this gets too large,
     * it's probably because of an infinite loop somewhere.
     */

    if (((iPtr->numLevels) > iPtr->maxNestingDepth) 
	    || (TclpCheckStackSpace() == 0)) {
	Tcl_AppendResult(interp,
		"too many nested evaluations (infinite loop?)", (char *) NULL); 
	return TCL_ERROR;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclEvalObjvInternal --
 *
 *	This procedure evaluates a Tcl command that has already been
 *	parsed into words, with one Tcl_Obj holding each word. The caller
 *      is responsible for managing the iPtr->numLevels.
 *
 * Results:
 *	The return value is a standard Tcl completion code such as
 *	TCL_OK or TCL_ERROR.  A result or error message is left in
 *	interp's result.  If an error occurs, this procedure does
 *	NOT add any information to the errorInfo variable.
 *
 * Side effects:
 *	Depends on the command.
 *
 *----------------------------------------------------------------------
 */

int
TclEvalObjvInternal(interp, objc, objv, command, length, flags)
    Tcl_Interp *interp;		/* Interpreter in which to evaluate the
				 * command.  Also used for error
				 * reporting. */
    int objc;			/* Number of words in command. */
    Tcl_Obj *CONST objv[];	/* An array of pointers to objects that are
				 * the words that make up the command. */
    CONST char *command;	/* Points to the beginning of the string
				 * representation of the command; this
				 * is used for traces.  If the string
				 * representation of the command is
				 * unknown, an empty string should be
				 * supplied. If it is NULL, no traces will
				 * be called. */
    int length;			/* Number of bytes in command; if -1, all
				 * characters up to the first null byte are
				 * used. */
    int flags;			/* Collection of OR-ed bits that control
				 * the evaluation of the script.  Only
				 * TCL_EVAL_GLOBAL and TCL_EVAL_INVOKE are
				 * currently supported. */

{
    Command *cmdPtr;
    Interp *iPtr = (Interp *) interp;
    Tcl_Obj **newObjv;
    int i;
    CallFrame *savedVarFramePtr;	/* Saves old copy of iPtr->varFramePtr
					 * in case TCL_EVAL_GLOBAL was set. */
    int code = TCL_OK;
    int traceCode = TCL_OK;
    int checkTraces = 1;

    if (TclInterpReady(interp) == TCL_ERROR) {
	return TCL_ERROR;
    }

    if (objc == 0) {
	return TCL_OK;
    }

    /*
     * If any execution traces rename or delete the current command,
     * we may need (at most) two passes here.
     */
    while (1) {
    
        /*
         * Find the procedure to execute this command. If there isn't one,
         * then see if there is a command "unknown".  If so, create a new
         * word array with "unknown" as the first word and the original
         * command words as arguments.  Then call ourselves recursively
         * to execute it.
	 *
	 * If caller requests, or if we're resolving the target end of
	 * an interpeter alias (TCL_EVAL_INVOKE), be sure to do command
	 * name resolution in the global namespace.
         */

	savedVarFramePtr = iPtr->varFramePtr;
	if (flags & (TCL_EVAL_INVOKE | TCL_EVAL_GLOBAL)) {
	    iPtr->varFramePtr = NULL;
	}
        cmdPtr = (Command *) Tcl_GetCommandFromObj(interp, objv[0]);
	iPtr->varFramePtr = savedVarFramePtr;

        if (cmdPtr == NULL) {
	    newObjv = (Tcl_Obj **) ckalloc((unsigned)
		((objc + 1) * sizeof (Tcl_Obj *)));
	    for (i = objc-1; i >= 0; i--) {
	        newObjv[i+1] = objv[i];
	    }
	    newObjv[0] = Tcl_NewStringObj("::unknown", -1);
	    Tcl_IncrRefCount(newObjv[0]);
	    cmdPtr = (Command *) Tcl_GetCommandFromObj(interp, newObjv[0]);
	    if (cmdPtr == NULL) {
	        Tcl_AppendResult(interp, "invalid command name \"",
			Tcl_GetString(objv[0]), "\"", (char *) NULL);
	        code = TCL_ERROR;
	    } else {
	        iPtr->numLevels++;
	        code = TclEvalObjvInternal(interp, objc+1, newObjv, command, length, 0);
	        iPtr->numLevels--;
	    }
	    Tcl_DecrRefCount(newObjv[0]);
	    ckfree((char *) newObjv);
	    goto done;
        }
    
        /*
         * Call trace procedures if needed.
         */
        if ((checkTraces) && (command != NULL)) {
            int cmdEpoch = cmdPtr->cmdEpoch;
            cmdPtr->refCount++;
            /* 
             * If the first set of traces modifies/deletes the command or
             * any existing traces, then the set checkTraces to 0 and
             * go through this while loop one more time.
             */
            if (iPtr->tracePtr != NULL && traceCode == TCL_OK) {
                traceCode = TclCheckInterpTraces(interp, command, length,
                               cmdPtr, code, TCL_TRACE_ENTER_EXEC, objc, objv);
            }
            if ((cmdPtr->flags & CMD_HAS_EXEC_TRACES) 
		    && (traceCode == TCL_OK)) {
                traceCode = TclCheckExecutionTraces(interp, command, length,
                               cmdPtr, code, TCL_TRACE_ENTER_EXEC, objc, objv);
            }
            cmdPtr->refCount--;
            if (cmdEpoch != cmdPtr->cmdEpoch) {
                /* The command has been modified in some way */
                checkTraces = 0;
                continue;
            }
        }
        break;
    }

    /*
     * Finally, invoke the command's Tcl_ObjCmdProc.
     */
    cmdPtr->refCount++;
    iPtr->cmdCount++;
    if (code == TCL_OK && traceCode == TCL_OK && !Tcl_LimitExceeded(interp)) {
	savedVarFramePtr = iPtr->varFramePtr;
	if (flags & TCL_EVAL_GLOBAL) {
	    iPtr->varFramePtr = NULL;
	}
	if (!(flags & TCL_EVAL_INVOKE) &&
		(iPtr->ensembleRewrite.sourceObjs != NULL) &&
		!TclIsEnsemble(cmdPtr)) {
	    iPtr->ensembleRewrite.sourceObjs = NULL;
	}
	code = (*cmdPtr->objProc)(cmdPtr->objClientData, interp, objc, objv);
	iPtr->varFramePtr = savedVarFramePtr;
    }
    if (Tcl_AsyncReady()) {
	code = Tcl_AsyncInvoke(interp, code);
    }
    if (code == TCL_OK && Tcl_LimitReady(interp)) {
	code = Tcl_LimitCheck(interp);
    }

    /*
     * Call 'leave' command traces
     */
    if (!(cmdPtr->flags & CMD_IS_DELETED)) {
        if ((cmdPtr->flags & CMD_HAS_EXEC_TRACES) && (traceCode == TCL_OK)) {
            traceCode = TclCheckExecutionTraces(interp, command, length,
                   cmdPtr, code, TCL_TRACE_LEAVE_EXEC, objc, objv);
        }
        if (iPtr->tracePtr != NULL && traceCode == TCL_OK) {
            traceCode = TclCheckInterpTraces(interp, command, length,
                   cmdPtr, code, TCL_TRACE_LEAVE_EXEC, objc, objv);
        }
    }
    TclCleanupCommand(cmdPtr);

    /*
     * If one of the trace invocation resulted in error, then 
     * change the result code accordingly. Note, that the
     * interp->result should already be set correctly by the
     * call to TraceExecutionProc.  
     */

    if (traceCode != TCL_OK) {
	code = traceCode;
    }
    
    /*
     * If the interpreter has a non-empty string result, the result
     * object is either empty or stale because some procedure set
     * interp->result directly. If so, move the string result to the
     * result object, then reset the string result.
     */
    
    if (*(iPtr->result) != 0) {
	(void) Tcl_GetObjResult(interp);
    }

    done:
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_EvalObjv --
 *
 *	This procedure evaluates a Tcl command that has already been
 *	parsed into words, with one Tcl_Obj holding each word.
 *
 * Results:
 *	The return value is a standard Tcl completion code such as
 *	TCL_OK or TCL_ERROR.  A result or error message is left in
 *	interp's result.
 *
 * Side effects:
 *	Depends on the command.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_EvalObjv(interp, objc, objv, flags)
    Tcl_Interp *interp;		/* Interpreter in which to evaluate the
				 * command.  Also used for error
				 * reporting. */
    int objc;			/* Number of words in command. */
    Tcl_Obj *CONST objv[];	/* An array of pointers to objects that are
				 * the words that make up the command. */
    int flags;			/* Collection of OR-ed bits that control
				 * the evaluation of the script.  Only
				 * TCL_EVAL_GLOBAL and TCL_EVAL_INVOKE
				 * are  currently supported. */
{
    Interp *iPtr = (Interp *)interp;
    Trace *tracePtr;
    Tcl_DString cmdBuf;
    char *cmdString = "";	/* A command string is only necessary for
				 * command traces or error logs; it will be
				 * generated to replace this default value if
				 * necessary. */
    int cmdLen = 0;		/* a non-zero value indicates that a command
				 * string was generated. */
    int code = TCL_OK;
    int i;
    int allowExceptions = (iPtr->evalFlags & TCL_ALLOW_EXCEPTIONS);

    for (tracePtr = iPtr->tracePtr; tracePtr; tracePtr = tracePtr->nextPtr) {
	if ((tracePtr->level == 0) || (iPtr->numLevels <= tracePtr->level)) {
	    /*
	     * The command may be needed for an execution trace.  Generate a
	     * command string.
	     */
	    
	    Tcl_DStringInit(&cmdBuf);
	    for (i = 0; i < objc; i++) {
		Tcl_DStringAppendElement(&cmdBuf, Tcl_GetString(objv[i]));
	    }
	    cmdString = Tcl_DStringValue(&cmdBuf);
	    cmdLen = Tcl_DStringLength(&cmdBuf);
	    break;
	}
    }

    iPtr->numLevels++;
    code = TclEvalObjvInternal(interp, objc, objv, cmdString, cmdLen, flags);
    iPtr->numLevels--;

    /*
     * If we are again at the top level, process any unusual 
     * return code returned by the evaluated code. 
     */
	
    if (iPtr->numLevels == 0) {
	if (code == TCL_RETURN) {
	    code = TclUpdateReturnInfo(iPtr);
	}
	if ((code != TCL_OK) && (code != TCL_ERROR) 
	    && !allowExceptions) {
	    ProcessUnexpectedResult(interp, code);
	    code = TCL_ERROR;
	}
    }
	    
    if ((code == TCL_ERROR) && !(flags & TCL_EVAL_INVOKE)) {

	/* 
	 * If there was an error, a command string will be needed for the 
	 * error log: generate it now if it was not done previously.
	 */

	if (cmdLen == 0) {
	    Tcl_DStringInit(&cmdBuf);
	    for (i = 0; i < objc; i++) {
		Tcl_DStringAppendElement(&cmdBuf, Tcl_GetString(objv[i]));
	    }
	    cmdString = Tcl_DStringValue(&cmdBuf);
	    cmdLen = Tcl_DStringLength(&cmdBuf);
	}
	Tcl_LogCommandInfo(interp, cmdString, cmdString, cmdLen);
    }

    if (cmdLen != 0) {
	Tcl_DStringFree(&cmdBuf);
    }
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_LogCommandInfo --
 *
 *	This procedure is invoked after an error occurs in an interpreter.
 *	It adds information to iPtr->errorInfo field to describe the
 *	command that was being executed when the error occurred.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Information about the command is added to errorInfo and the
 *	line number stored internally in the interpreter is set.  
 *
 *----------------------------------------------------------------------
 */

void
Tcl_LogCommandInfo(interp, script, command, length)
    Tcl_Interp *interp;		/* Interpreter in which to log information. */
    CONST char *script;		/* First character in script containing
				 * command (must be <= command). */
    CONST char *command;	/* First character in command that
				 * generated the error. */
    int length;			/* Number of bytes in command (-1 means
				 * use all bytes up to first null byte). */
{
    register CONST char *p;
    Interp *iPtr = (Interp *) interp;
    Tcl_Obj *message;

    if (iPtr->flags & ERR_ALREADY_LOGGED) {
	/*
	 * Someone else has already logged error information for this
	 * command; we shouldn't add anything more.
	 */

	return;
    }

    /*
     * Compute the line number where the error occurred.
     */

    iPtr->errorLine = 1;
    for (p = script; p != command; p++) {
	if (*p == '\n') {
	    iPtr->errorLine++;
	}
    }

    if (iPtr->errorInfo == NULL) {
	message = Tcl_NewStringObj("\n    while executing\n\"", -1);
    } else {
	message = Tcl_NewStringObj("\n    invoked from within\n\"", -1);
    }
    Tcl_IncrRefCount(message);
    TclAppendLimitedToObj(message, command, length, 153, NULL);
    Tcl_AppendToObj(message, "\"", -1);
    TclAppendObjToErrorInfo(interp, message);
    Tcl_DecrRefCount(message);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_EvalTokensStandard --
 *
 *	Given an array of tokens parsed from a Tcl command (e.g., the
 *	tokens that make up a word or the index for an array variable)
 *	this procedure evaluates the tokens and concatenates their
 *	values to form a single result value.
 * 
 * Results:
 *	The return value is a standard Tcl completion code such as
 *	TCL_OK or TCL_ERROR.  A result or error message is left in
 *	interp's result.
 *
 * Side effects:
 *	Depends on the array of tokens being evaled.
  *
 *----------------------------------------------------------------------
 */

int
Tcl_EvalTokensStandard(interp, tokenPtr, count)
    Tcl_Interp *interp;		/* Interpreter in which to lookup
				 * variables, execute nested commands,
				 * and report errors. */
    Tcl_Token *tokenPtr;	/* Pointer to first in an array of tokens
				 * to evaluate and concatenate. */
    int count;			/* Number of tokens to consider at tokenPtr.
				 * Must be at least 1. */
{
    return TclSubstTokens(interp, tokenPtr, count, /* numLeftPtr */ NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Tcl_EvalTokens --
 *
 *	Given an array of tokens parsed from a Tcl command (e.g., the
 *	tokens that make up a word or the index for an array variable)
 *	this procedure evaluates the tokens and concatenates their
 *	values to form a single result value.
 *
 * Results:
 *	The return value is a pointer to a newly allocated Tcl_Obj
 *	containing the value of the array of tokens.  The reference
 *	count of the returned object has been incremented.  If an error
 *	occurs in evaluating the tokens then a NULL value is returned
 *	and an error message is left in interp's result.
 *
 * Side effects:
 *	A new object is allocated to hold the result.
 *
 *----------------------------------------------------------------------
 *
 * This uses a non-standard return convention; its use is now deprecated.
 * It is a wrapper for the new function Tcl_EvalTokensStandard, and is not 
 * used in the core any longer. It is only kept for backward compatibility.
 */

Tcl_Obj *
Tcl_EvalTokens(interp, tokenPtr, count)
    Tcl_Interp *interp;		/* Interpreter in which to lookup
				 * variables, execute nested commands,
				 * and report errors. */
    Tcl_Token *tokenPtr;	/* Pointer to first in an array of tokens
				 * to evaluate and concatenate. */
    int count;			/* Number of tokens to consider at tokenPtr.
				 * Must be at least 1. */
{
    int code;
    Tcl_Obj *resPtr;
    
    code = Tcl_EvalTokensStandard(interp, tokenPtr, count);
    if (code == TCL_OK) {
	resPtr = Tcl_GetObjResult(interp);
	Tcl_IncrRefCount(resPtr);
	Tcl_ResetResult(interp);
	return resPtr;
    } else {
	return NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Tcl_EvalEx --
 *
 *	This procedure evaluates a Tcl script without using the compiler
 *	or byte-code interpreter.  It just parses the script, creates
 *	values for each word of each command, then calls EvalObjv
 *	to execute each command.
 *
 * Results:
 *	The return value is a standard Tcl completion code such as
 *	TCL_OK or TCL_ERROR.  A result or error message is left in
 *	interp's result.
 *
 * Side effects:
 *	Depends on the script.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_EvalEx(interp, script, numBytes, flags)
    Tcl_Interp *interp;		/* Interpreter in which to evaluate the
				 * script.  Also used for error reporting. */
    CONST char *script;		/* First character of script to evaluate. */
    int numBytes;		/* Number of bytes in script.  If < 0, the
				 * script consists of all bytes up to the
				 * first null character. */
    int flags;			/* Collection of OR-ed bits that control
				 * the evaluation of the script.  Only
				 * TCL_EVAL_GLOBAL is currently
				 * supported. */
{
    Interp *iPtr = (Interp *) interp;
    CONST char *p, *next;
    Tcl_Parse parse;
#define NUM_STATIC_OBJS 20
    Tcl_Obj *staticObjArray[NUM_STATIC_OBJS], **objv, **objvSpace;
    int expandStatic[NUM_STATIC_OBJS], *expand;
    Tcl_Token *tokenPtr;
    int i, code, commandLength, bytesLeft, expandRequested;
    CallFrame *savedVarFramePtr;   /* Saves old copy of iPtr->varFramePtr
				    * in case TCL_EVAL_GLOBAL was set. */
    int allowExceptions = (iPtr->evalFlags & TCL_ALLOW_EXCEPTIONS);
    
    /*
     * The variables below keep track of how much state has been
     * allocated while evaluating the script, so that it can be freed
     * properly if an error occurs.
     */

    int gotParse = 0, objectsUsed = 0;

    if (numBytes < 0) {
	numBytes = strlen(script);
    }
    Tcl_ResetResult(interp);

    savedVarFramePtr = iPtr->varFramePtr;
    if (flags & TCL_EVAL_GLOBAL) {
	iPtr->varFramePtr = NULL;
    }

    /*
     * Each iteration through the following loop parses the next
     * command from the script and then executes it.
     */

    objv = objvSpace = staticObjArray;
    expand = expandStatic;
    p = script;
    bytesLeft = numBytes;
    iPtr->evalFlags = 0;
    do {
	if (Tcl_ParseCommand(interp, p, bytesLeft, 0, &parse)
	        != TCL_OK) {
	    code = TCL_ERROR;
	    goto error;
	}
	gotParse = 1; 
	if (parse.numWords > 0) {
	    /*
	     * Generate an array of objects for the words of the command.
	     */
	    int objectsNeeded = 0;
    
	    if (parse.numWords > NUM_STATIC_OBJS) {
		expand = (int *) ckalloc((unsigned)
		    (parse.numWords * sizeof (int)));
		objvSpace = (Tcl_Obj **) ckalloc((unsigned)
		    (parse.numWords * sizeof (Tcl_Obj *)));
	    }
	    expandRequested = 0;
	    objv = objvSpace;
	    for (objectsUsed = 0, tokenPtr = parse.tokenPtr;
		    objectsUsed < parse.numWords;
		    objectsUsed++, tokenPtr += (tokenPtr->numComponents + 1)) {
		code = TclSubstTokens(interp, tokenPtr+1, 
		            tokenPtr->numComponents, NULL);
		if (code != TCL_OK) {
		    goto error;
		}
		objv[objectsUsed] = Tcl_GetObjResult(interp);
		Tcl_IncrRefCount(objv[objectsUsed]);
		if (tokenPtr->type == TCL_TOKEN_EXPAND_WORD) {
		    int numElements;

		    code = Tcl_ListObjLength(interp,
			    objv[objectsUsed], &numElements);
		    if (code == TCL_ERROR) {
			/* Attempt to expand a non-list */
			Tcl_Obj *msg = 
				Tcl_NewStringObj("\n    (expanding word ", -1);
			Tcl_Obj *wordNum = Tcl_NewIntObj(objectsUsed);
			Tcl_IncrRefCount(wordNum);
			Tcl_IncrRefCount(msg);
			Tcl_AppendObjToObj(msg, wordNum);
			Tcl_DecrRefCount(wordNum);
			Tcl_AppendToObj(msg, ")", -1);
			TclAppendObjToErrorInfo(interp, msg);
			Tcl_DecrRefCount(msg);
			Tcl_DecrRefCount(objv[objectsUsed]);
			goto error;
		    }
		    expandRequested = 1;
		    expand[objectsUsed] = 1;
		    objectsNeeded += (numElements ? numElements : 1);
		} else {
		    expand[objectsUsed] = 0;
		    objectsNeeded++;
		}
	    }
	    if (expandRequested) {
		/* Some word expansion was requested.  Check for objv resize */
		Tcl_Obj **copy = objvSpace;
		int wordIdx = parse.numWords;
		int objIdx = objectsNeeded - 1;

		if ((parse.numWords > NUM_STATIC_OBJS)
			|| (objectsNeeded > NUM_STATIC_OBJS)) {
		    objv = objvSpace = (Tcl_Obj **) ckalloc((unsigned)
			    (objectsNeeded * sizeof (Tcl_Obj *)));
		}

		objectsUsed = 0;
		while (wordIdx--) {
		    if (expand[wordIdx]) {
			int numElements;
			Tcl_Obj **elements, *temp = copy[wordIdx];
			Tcl_ListObjGetElements(NULL, temp,
				&numElements, &elements);
			objectsUsed += numElements;
			while (numElements--) {
			    objv[objIdx--] = elements[numElements];
			    Tcl_IncrRefCount(elements[numElements]);
			}
			Tcl_DecrRefCount(temp);
		    } else {
			objv[objIdx--] = copy[wordIdx];
			objectsUsed++;
		    }
		}
		objv += objIdx+1;

		if (copy != staticObjArray) {
		    ckfree((char *) copy);
		}
	    }
    
	    /*
	     * Execute the command and free the objects for its words.
	     */

	    iPtr->numLevels++;    
	    code = TclEvalObjvInternal(interp, objectsUsed, objv, 
	            parse.commandStart, parse.commandSize, 0);
	    iPtr->numLevels--;
	    if (code != TCL_OK) {
		if (iPtr->numLevels == 0) {
		    if (code == TCL_RETURN) {
			code = TclUpdateReturnInfo(iPtr);
		    }
		    if ((code != TCL_OK) && (code != TCL_ERROR) 
			&& !allowExceptions) {
			ProcessUnexpectedResult(interp, code);
			code = TCL_ERROR;
		    }
		}
		goto error;
	    }
	    for (i = 0; i < objectsUsed; i++) {
		Tcl_DecrRefCount(objv[i]);
	    }
	    objectsUsed = 0;
	    if (objvSpace != staticObjArray) {
		ckfree((char *) objvSpace);
		objvSpace = staticObjArray;
	    }
	    /* 
	     * Free expand separately since objvSpace could have been
	     * reallocated above. 
	     */
	    if (expand != expandStatic) {
		ckfree((char *) expand);
		expand = expandStatic;
	    }
	}

	/*
	 * Advance to the next command in the script.
	 */

	next = parse.commandStart + parse.commandSize;
	bytesLeft -= next - p;
	p = next;
	Tcl_FreeParse(&parse);
	gotParse = 0;
    } while (bytesLeft > 0);
    iPtr->varFramePtr = savedVarFramePtr;
    return TCL_OK;

    error:
    /* Generate and log various pieces of error information. */

    if ((code == TCL_ERROR) && !(iPtr->flags & ERR_ALREADY_LOGGED)) { 
	commandLength = parse.commandSize;
	if (parse.term == parse.commandStart + commandLength - 1) {
	    /*
	     * The terminator character (such as ; or ]) of the command where
	     * the error occurred is the last character in the parsed command.
	     * Reduce the length by one so that the error message doesn't
	     * include the terminator character.
	     */
	    
	    commandLength -= 1;
	}
	Tcl_LogCommandInfo(interp, script, parse.commandStart, commandLength);
    }
    iPtr->flags &= ~ERR_ALREADY_LOGGED;
    
    /* Then free resources that had been allocated to the command. */

    for (i = 0; i < objectsUsed; i++) {
	Tcl_DecrRefCount(objv[i]);
    }
    if (gotParse) {
	Tcl_FreeParse(&parse);
    }
    if (objvSpace != staticObjArray) {
	ckfree((char *) objvSpace);
    }
    if (expand != expandStatic) {
	ckfree((char *) expand);
    }
    iPtr->varFramePtr = savedVarFramePtr;
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Eval --
 *
 *	Execute a Tcl command in a string.  This procedure executes the
 *	script directly, rather than compiling it to bytecodes.  Before
 *	the arrival of the bytecode compiler in Tcl 8.0 Tcl_Eval was
 *	the main procedure used for executing Tcl commands, but nowadays
 *	it isn't used much.
 *
 * Results:
 *	The return value is one of the return codes defined in tcl.h
 *	(such as TCL_OK), and interp's result contains a value
 *	to supplement the return code. The value of the result
 *	will persist only until the next call to Tcl_Eval or Tcl_EvalObj:
 *	you must copy it or lose it!
 *
 * Side effects:
 *	Can be almost arbitrary, depending on the commands in the script.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_Eval(interp, string)
    Tcl_Interp *interp;		/* Token for command interpreter (returned
				 * by previous call to Tcl_CreateInterp). */
    CONST char *string;		/* Pointer to TCL command to execute. */
{
    int code = Tcl_EvalEx(interp, string, -1, 0);

    /*
     * For backwards compatibility with old C code that predates the
     * object system in Tcl 8.0, we have to mirror the object result
     * back into the string result (some callers may expect it there).
     */

    (void) Tcl_GetStringResult(interp);
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_EvalObj, Tcl_GlobalEvalObj --
 *
 *	These functions are deprecated but we keep them around for backwards
 *	compatibility reasons.
 *
 * Results:
 *	See the functions they call.
 *
 * Side effects:
 *	See the functions they call.
 *
 *----------------------------------------------------------------------
 */

#undef Tcl_EvalObj
int
Tcl_EvalObj(interp, objPtr)
    Tcl_Interp * interp;
    Tcl_Obj * objPtr;
{
    return Tcl_EvalObjEx(interp, objPtr, 0);
}

#undef Tcl_GlobalEvalObj
int
Tcl_GlobalEvalObj(interp, objPtr)
    Tcl_Interp * interp;
    Tcl_Obj * objPtr;
{
    return Tcl_EvalObjEx(interp, objPtr, TCL_EVAL_GLOBAL);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_EvalObjEx --
 *
 *	Execute Tcl commands stored in a Tcl object. These commands are
 *	compiled into bytecodes if necessary, unless TCL_EVAL_DIRECT
 *	is specified.
 *
 * Results:
 *	The return value is one of the return codes defined in tcl.h
 *	(such as TCL_OK), and the interpreter's result contains a value
 *	to supplement the return code.
 *
 * Side effects:
 *	The object is converted, if necessary, to a ByteCode object that
 *	holds the bytecode instructions for the commands. Executing the
 *	commands will almost certainly have side effects that depend
 *	on those commands.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_EvalObjEx(interp, objPtr, flags)
    Tcl_Interp *interp;			/* Token for command interpreter
					 * (returned by a previous call to
					 * Tcl_CreateInterp). */
    register Tcl_Obj *objPtr;		/* Pointer to object containing
					 * commands to execute. */
    int flags;				/* Collection of OR-ed bits that
					 * control the evaluation of the
					 * script.  Supported values are
					 * TCL_EVAL_GLOBAL and
					 * TCL_EVAL_DIRECT. */
{
    register Interp *iPtr = (Interp *) interp;
    char *script;
    int numSrcBytes;
    int result;
    CallFrame *savedVarFramePtr;	/* Saves old copy of iPtr->varFramePtr
					 * in case TCL_EVAL_GLOBAL was set. */
    int allowExceptions = (iPtr->evalFlags & TCL_ALLOW_EXCEPTIONS);

    Tcl_IncrRefCount(objPtr);

    if (flags & TCL_EVAL_DIRECT) {
	/*
	 * We're not supposed to use the compiler or byte-code interpreter.
	 * Let Tcl_EvalEx evaluate the command directly (and probably
	 * more slowly).
	 *
	 * Pure List Optimization (no string representation).  In this
	 * case, we can safely use Tcl_EvalObjv instead and get an
	 * appreciable improvement in execution speed.  This is because it
	 * allows us to avoid a setFromAny step that would just pack
	 * everything into a string and back out again.
	 */
	if ((objPtr->typePtr == &tclListType) && /* is a list... */
		(objPtr->bytes == NULL) /* ...without a string rep */) {
	    register List *listRepPtr =
		(List *) objPtr->internalRep.twoPtrValue.ptr1;
	    result = Tcl_EvalObjv(interp, listRepPtr->elemCount,
		    listRepPtr->elements, flags);
	} else {
	    script = Tcl_GetStringFromObj(objPtr, &numSrcBytes);
	    result = Tcl_EvalEx(interp, script, numSrcBytes, flags);
	}
    } else {
	/*
	 * Let the compiler/engine subsystem do the evaluation.
	 */

	savedVarFramePtr = iPtr->varFramePtr;
	if (flags & TCL_EVAL_GLOBAL) {
	    iPtr->varFramePtr = NULL;
	}

	result = TclCompEvalObj(interp, objPtr);

	/*
	 * If we are again at the top level, process any unusual 
	 * return code returned by the evaluated code. 
	 */
	
	if (iPtr->numLevels == 0) {
	    if (result == TCL_RETURN) {
		result = TclUpdateReturnInfo(iPtr);
	    }
	    if ((result != TCL_OK) && (result != TCL_ERROR) 
	        && !allowExceptions) {
		ProcessUnexpectedResult(interp, result);
		result = TCL_ERROR;
		script = Tcl_GetStringFromObj(objPtr, &numSrcBytes);
		Tcl_LogCommandInfo(interp, script, script, numSrcBytes);
	    }
	}
	iPtr->evalFlags = 0;
	iPtr->varFramePtr = savedVarFramePtr; 
    }

    TclDecrRefCount(objPtr);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ProcessUnexpectedResult --
 *
 *	Procedure called by Tcl_EvalObj to set the interpreter's result
 *	value to an appropriate error message when the code it evaluates
 *	returns an unexpected result code (not TCL_OK and not TCL_ERROR) to
 *	the topmost evaluation level.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The interpreter result is set to an error message appropriate to
 *	the result code.
 *
 *----------------------------------------------------------------------
 */

static void
ProcessUnexpectedResult(interp, returnCode)
    Tcl_Interp *interp;		/* The interpreter in which the unexpected
				 * result code was returned. */
    int returnCode;		/* The unexpected result code. */
{
    Tcl_ResetResult(interp);
    if (returnCode == TCL_BREAK) {
	Tcl_AppendResult(interp,
		"invoked \"break\" outside of a loop", (char *) NULL);
    } else if (returnCode == TCL_CONTINUE) {
	Tcl_AppendResult(interp,
		"invoked \"continue\" outside of a loop", (char *) NULL);
    } else {
        char buf[30 + TCL_INTEGER_SPACE];

	sprintf(buf, "command returned bad code: %d", returnCode);
	Tcl_SetResult(interp, buf, TCL_VOLATILE);
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_ExprLong, Tcl_ExprDouble, Tcl_ExprBoolean --
 *
 *	Procedures to evaluate an expression and return its value in a
 *	particular form.
 *
 * Results:
 *	Each of the procedures below returns a standard Tcl result. If an
 *	error occurs then an error message is left in the interp's result.
 *	Otherwise the value of the expression, in the appropriate form,
 *	is stored at *ptr. If the expression had a result that was
 *	incompatible with the desired form then an error is returned.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_ExprLong(interp, string, ptr)
    Tcl_Interp *interp;		/* Context in which to evaluate the
				 * expression. */
    CONST char *string;		/* Expression to evaluate. */
    long *ptr;			/* Where to store result. */
{
    register Tcl_Obj *exprPtr;
    Tcl_Obj *resultPtr;
    int length = strlen(string);
    int result = TCL_OK;

    if (length > 0) {
	exprPtr = Tcl_NewStringObj(string, length);
	Tcl_IncrRefCount(exprPtr);
	result = Tcl_ExprObj(interp, exprPtr, &resultPtr);
	if (result == TCL_OK) {
	    /*
	     * Store an integer based on the expression result.
	     */
	    
	    if (resultPtr->typePtr == &tclIntType) {
		*ptr = resultPtr->internalRep.longValue;
	    } else if (resultPtr->typePtr == &tclDoubleType) {
		*ptr = (long) resultPtr->internalRep.doubleValue;
	    } else {
		Tcl_SetResult(interp,
		        "expression didn't have numeric value", TCL_STATIC);
		result = TCL_ERROR;
	    }
	    Tcl_DecrRefCount(resultPtr);  /* discard the result object */
	} else {
	    /*
	     * Move the interpreter's object result to the string result, 
	     * then reset the object result.
	     */

	    (void) Tcl_GetStringResult(interp);
	}
	Tcl_DecrRefCount(exprPtr);  /* discard the expression object */	
    } else {
	/*
	 * An empty string. Just set the result integer to 0.
	 */
	
	*ptr = 0;
    }
    return result;
}

int
Tcl_ExprDouble(interp, string, ptr)
    Tcl_Interp *interp;		/* Context in which to evaluate the
				 * expression. */
    CONST char *string;		/* Expression to evaluate. */
    double *ptr;		/* Where to store result. */
{
    register Tcl_Obj *exprPtr;
    Tcl_Obj *resultPtr;
    int length = strlen(string);
    int result = TCL_OK;

    if (length > 0) {
	exprPtr = Tcl_NewStringObj(string, length);
	Tcl_IncrRefCount(exprPtr);
	result = Tcl_ExprObj(interp, exprPtr, &resultPtr);
	if (result == TCL_OK) {
	    /*
	     * Store a double  based on the expression result.
	     */
	    
	    if (resultPtr->typePtr == &tclIntType) {
		*ptr = (double) resultPtr->internalRep.longValue;
	    } else if (resultPtr->typePtr == &tclDoubleType) {
		*ptr = resultPtr->internalRep.doubleValue;
	    } else {
		Tcl_SetResult(interp,
		        "expression didn't have numeric value", TCL_STATIC);
		result = TCL_ERROR;
	    }
	    Tcl_DecrRefCount(resultPtr);  /* discard the result object */
	} else {
	    /*
	     * Move the interpreter's object result to the string result, 
	     * then reset the object result.
	     */

	    (void) Tcl_GetStringResult(interp);
	}
	Tcl_DecrRefCount(exprPtr);  /* discard the expression object */
    } else {
	/*
	 * An empty string. Just set the result double to 0.0.
	 */
	
	*ptr = 0.0;
    }
    return result;
}

int
Tcl_ExprBoolean(interp, string, ptr)
    Tcl_Interp *interp;		/* Context in which to evaluate the
			         * expression. */
    CONST char *string;		/* Expression to evaluate. */
    int *ptr;			/* Where to store 0/1 result. */
{
    register Tcl_Obj *exprPtr;
    Tcl_Obj *resultPtr;
    int length = strlen(string);
    int result = TCL_OK;

    if (length > 0) {
	exprPtr = Tcl_NewStringObj(string, length);
	Tcl_IncrRefCount(exprPtr);
	result = Tcl_ExprObj(interp, exprPtr, &resultPtr);
	if (result == TCL_OK) {
	    /*
	     * Store a boolean based on the expression result.
	     */
	    
	    if (resultPtr->typePtr == &tclIntType) {
		*ptr = (resultPtr->internalRep.longValue != 0);
	    } else if (resultPtr->typePtr == &tclDoubleType) {
		*ptr = (resultPtr->internalRep.doubleValue != 0.0);
	    } else {
		result = Tcl_GetBooleanFromObj(interp, resultPtr, ptr);
	    }
	    Tcl_DecrRefCount(resultPtr);  /* discard the result object */
	}
	if (result != TCL_OK) {
	    /*
	     * Move the interpreter's object result to the string result, 
	     * then reset the object result.
	     */

	    (void) Tcl_GetStringResult(interp);
	}
	Tcl_DecrRefCount(exprPtr); /* discard the expression object */
    } else {
	/*
	 * An empty string. Just set the result boolean to 0 (false).
	 */
	
	*ptr = 0;
    }
    return result;
}

/*
 *--------------------------------------------------------------
 *
 * Tcl_ExprLongObj, Tcl_ExprDoubleObj, Tcl_ExprBooleanObj --
 *
 *	Procedures to evaluate an expression in an object and return its
 *	value in a particular form.
 *
 * Results:
 *	Each of the procedures below returns a standard Tcl result
 *	object. If an error occurs then an error message is left in the
 *	interpreter's result. Otherwise the value of the expression, in the
 *	appropriate form, is stored at *ptr. If the expression had a result
 *	that was incompatible with the desired form then an error is
 *	returned.
 *
 * Side effects:
 *	None.
 *
 *--------------------------------------------------------------
 */

int
Tcl_ExprLongObj(interp, objPtr, ptr)
    Tcl_Interp *interp;			/* Context in which to evaluate the
					 * expression. */
    register Tcl_Obj *objPtr;		/* Expression to evaluate. */
    long *ptr;				/* Where to store long result. */
{
    Tcl_Obj *resultPtr;
    int result;

    result = Tcl_ExprObj(interp, objPtr, &resultPtr);
    if (result == TCL_OK) {
	if (resultPtr->typePtr == &tclIntType) {
	    *ptr = resultPtr->internalRep.longValue;
	} else if (resultPtr->typePtr == &tclDoubleType) {
	    *ptr = (long) resultPtr->internalRep.doubleValue;
	} else {
	    result = Tcl_GetLongFromObj(interp, resultPtr, ptr);
	    if (result != TCL_OK) {
		return result;
	    }
	}
	Tcl_DecrRefCount(resultPtr);  /* discard the result object */
    }
    return result;
}

int
Tcl_ExprDoubleObj(interp, objPtr, ptr)
    Tcl_Interp *interp;			/* Context in which to evaluate the
					 * expression. */
    register Tcl_Obj *objPtr;		/* Expression to evaluate. */
    double *ptr;			/* Where to store double result. */
{
    Tcl_Obj *resultPtr;
    int result;

    result = Tcl_ExprObj(interp, objPtr, &resultPtr);
    if (result == TCL_OK) {
	if (resultPtr->typePtr == &tclIntType) {
	    *ptr = (double) resultPtr->internalRep.longValue;
	} else if (resultPtr->typePtr == &tclDoubleType) {
	    *ptr = resultPtr->internalRep.doubleValue;
	} else {
	    result = Tcl_GetDoubleFromObj(interp, resultPtr, ptr);
	    if (result != TCL_OK) {
		return result;
	    }
	}
	Tcl_DecrRefCount(resultPtr);  /* discard the result object */
    }
    return result;
}

int
Tcl_ExprBooleanObj(interp, objPtr, ptr)
    Tcl_Interp *interp;			/* Context in which to evaluate the
					 * expression. */
    register Tcl_Obj *objPtr;		/* Expression to evaluate. */
    int *ptr;				/* Where to store 0/1 result. */
{
    Tcl_Obj *resultPtr;
    int result;

    result = Tcl_ExprObj(interp, objPtr, &resultPtr);
    if (result == TCL_OK) {
	if (resultPtr->typePtr == &tclIntType) {
	    *ptr = (resultPtr->internalRep.longValue != 0);
	} else if (resultPtr->typePtr == &tclDoubleType) {
	    *ptr = (resultPtr->internalRep.doubleValue != 0.0);
	} else {
	    result = Tcl_GetBooleanFromObj(interp, resultPtr, ptr);
	}
	Tcl_DecrRefCount(resultPtr);  /* discard the result object */
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TclObjInvokeNamespace --
 *
 *	Object version: Invokes a Tcl command, given an objv/objc, from
 *	either the exposed or hidden set of commands in the given
 *	interpreter.
 *	NOTE: The command is invoked in the global stack frame of the
 *	interpreter or namespace, thus it cannot see any current state on
 *	the stack of that interpreter.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Whatever the command does.
 *
 *----------------------------------------------------------------------
 */

int
TclObjInvokeNamespace(interp, objc, objv, nsPtr, flags)
    Tcl_Interp *interp;		/* Interpreter in which command is to be
				 * invoked. */
    int objc;			/* Count of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects; objv[0] points to the
				 * name of the command to invoke. */
    Tcl_Namespace *nsPtr;	/* The namespace to use. */
    int flags;			/* Combination of flags controlling the
				 * call: TCL_INVOKE_HIDDEN,
				 * TCL_INVOKE_NO_UNKNOWN, or
				 * TCL_INVOKE_NO_TRACEBACK. */
{
    Tcl_CallFrame frame;
    int result;

    /*
     * Make the specified namespace the current namespace and invoke
     * the command.
     */

    result = Tcl_PushCallFrame(interp, &frame, nsPtr, /*isProcCallFrame*/ 0);
    if (result != TCL_OK) {
        return TCL_ERROR;
    }

    result = TclObjInvoke(interp, objc, objv, flags);

    Tcl_PopCallFrame(interp);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TclObjInvoke --
 *
 *	Invokes a Tcl command, given an objv/objc, from either the
 *	exposed or the hidden sets of commands in the given interpreter.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	Whatever the command does.
 *
 *----------------------------------------------------------------------
 */

int
TclObjInvoke(interp, objc, objv, flags)
    Tcl_Interp *interp;		/* Interpreter in which command is to be
				 * invoked. */
    int objc;			/* Count of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects; objv[0] points to the
				 * name of the command to invoke. */
    int flags;			/* Combination of flags controlling the
				 * call: TCL_INVOKE_HIDDEN,
				 * TCL_INVOKE_NO_UNKNOWN, or
				 * TCL_INVOKE_NO_TRACEBACK. */
{
    register Interp *iPtr = (Interp *) interp;
    Tcl_HashTable *hTblPtr;	/* Table of hidden commands. */
    char *cmdName;		/* Name of the command from objv[0]. */
    Tcl_HashEntry *hPtr = NULL;
    Command *cmdPtr;
    int result;

    if (interp == (Tcl_Interp *) NULL) {
        return TCL_ERROR;
    }

    if ((objc < 1) || (objv == (Tcl_Obj **) NULL)) {
        Tcl_AppendResult(interp, "illegal argument vector", (char *) NULL);
        return TCL_ERROR;
    }

    if ((flags & TCL_INVOKE_HIDDEN) == 0) {
	Tcl_Panic("TclObjInvoke: called without TCL_INVOKE_HIDDEN");
    }

    if (TclInterpReady(interp) == TCL_ERROR) {
	return TCL_ERROR;
    }

    cmdName = Tcl_GetString(objv[0]);
    hTblPtr = iPtr->hiddenCmdTablePtr;
    if (hTblPtr != NULL) {
	hPtr = Tcl_FindHashEntry(hTblPtr, cmdName);
    }
    if (hPtr == NULL) {
	Tcl_AppendResult(interp, "invalid hidden command name \"",
		cmdName, "\"", (char *) NULL);
	return TCL_ERROR;
    }
    cmdPtr = (Command *) Tcl_GetHashValue(hPtr);

    /* Invoke the command procedure. */

    iPtr->cmdCount++;
    result = (*cmdPtr->objProc)(cmdPtr->objClientData, interp, objc, objv);

    /*
     * If an error occurred, record information about what was being
     * executed when the error occurred.
     */

    if ((result == TCL_ERROR)
	    && ((flags & TCL_INVOKE_NO_TRACEBACK) == 0)
	    && ((iPtr->flags & ERR_ALREADY_LOGGED) == 0)) {
	int length;
	Tcl_Obj *command = Tcl_NewListObj(objc, objv);
	CONST char* cmdString = Tcl_GetStringFromObj(command, &length);

	Tcl_LogCommandInfo(interp, cmdString, cmdString, length);
	iPtr->flags &= ~ERR_ALREADY_LOGGED;
    }
    return result;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_ExprString --
 *
 *	Evaluate an expression in a string and return its value in string
 *	form.
 *
 * Results:
 *	A standard Tcl result. If the result is TCL_OK, then the interp's
 *	result is set to the string value of the expression. If the result
 *	is TCL_ERROR, then the interp's result contains an error message.
 *
 * Side effects:
 *	A Tcl object is allocated to hold a copy of the expression string.
 *	This expression object is passed to Tcl_ExprObj and then
 *	deallocated.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_ExprString(interp, string)
    Tcl_Interp *interp;		/* Context in which to evaluate the
				 * expression. */
    CONST char *string;		/* Expression to evaluate. */
{
    register Tcl_Obj *exprPtr;
    Tcl_Obj *resultPtr;
    int length = strlen(string);
    char buf[TCL_DOUBLE_SPACE];
    int result = TCL_OK;

    if (length > 0) {
	TclNewObj(exprPtr);
	TclInitStringRep(exprPtr, string, length);
	Tcl_IncrRefCount(exprPtr);

	result = Tcl_ExprObj(interp, exprPtr, &resultPtr);
	if (result == TCL_OK) {
	    /*
	     * Set the interpreter's string result from the result object.
	     */
	    
	    if (resultPtr->typePtr == &tclIntType) {
		sprintf(buf, "%ld", resultPtr->internalRep.longValue);
		Tcl_SetResult(interp, buf, TCL_VOLATILE);
	    } else if (resultPtr->typePtr == &tclDoubleType) {
		Tcl_PrintDouble((Tcl_Interp *) NULL,
		        resultPtr->internalRep.doubleValue, buf);
		Tcl_SetResult(interp, buf, TCL_VOLATILE);
	    } else {
		/*
		 * Set interpreter's string result from the result object.
		 */
	    
		Tcl_SetResult(interp, TclGetString(resultPtr),
		        TCL_VOLATILE);
	    }
	    Tcl_DecrRefCount(resultPtr);  /* discard the result object */
	} else {
	    /*
	     * Move the interpreter's object result to the string result, 
	     * then reset the object result.
	     */
	    
	    (void) Tcl_GetStringResult(interp);
	}
	Tcl_DecrRefCount(exprPtr); /* discard the expression object */
    } else {
	/*
	 * An empty string. Just set the interpreter's result to 0.
	 */
	
	Tcl_SetResult(interp, "0", TCL_VOLATILE);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TclAppendObjToErrorInfo --
 *
 *	Add a Tcl_Obj value to the errorInfo field that describes the
 *	current error.
 *
 * Results:
 *	None.
 *
 * Side effects:
 * 	The value of the Tcl_obj is appended to the errorInfo field.
 *	If we are just starting to log an error, errorInfo is initialized
 *	from the error message in the interpreter's result.
 *
 *----------------------------------------------------------------------
 */

void
TclAppendObjToErrorInfo(interp, objPtr)
    Tcl_Interp *interp;		/* Interpreter to which error information
				 * pertains. */
    Tcl_Obj *objPtr;		/* Message to record. */
{
    int length;
    CONST char *message = Tcl_GetStringFromObj(objPtr, &length);
    Tcl_AddObjErrorInfo(interp, message, length);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AddErrorInfo --
 *
 *	Add information to the errorInfo field that describes the
 *	current error.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The contents of message are appended to the errorInfo field.
 *	If we are just starting to log an error, errorInfo is initialized
 *	from the error message in the interpreter's result.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AddErrorInfo(interp, message)
    Tcl_Interp *interp;		/* Interpreter to which error information
				 * pertains. */
    CONST char *message;	/* Message to record. */
{
    Tcl_AddObjErrorInfo(interp, message, -1);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AddObjErrorInfo --
 *
 *	Add information to the errorInfo field that describes the
 *	current error. This routine differs from Tcl_AddErrorInfo by
 *	taking a byte pointer and length.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	"length" bytes from "message" are appended to the errorInfo field.
 *	If "length" is negative, use bytes up to the first NULL byte.
 *	If we are just starting to log an error, errorInfo is initialized
 *	from the error message in the interpreter's result.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AddObjErrorInfo(interp, message, length)
    Tcl_Interp *interp;		/* Interpreter to which error information
				 * pertains. */
    CONST char *message;	/* Points to the first byte of an array of
				 * bytes of the message. */
    int length;			/* The number of bytes in the message.
				 * If < 0, then append all bytes up to a
				 * NULL byte. */
{
    register Interp *iPtr = (Interp *) interp;
    
    /*
     * If we are just starting to log an error, errorInfo is initialized
     * from the error message in the interpreter's result.
     */

    if (iPtr->errorInfo == NULL) { /* just starting to log error */
	if (iPtr->result[0] != 0) {
	    /*
	     * The interp's string result is set, apparently by some
	     * extension making a deprecated direct write to it.
	     * That extension may expect interp->result to continue
	     * to be set, so we'll take special pains to avoid clearing
	     * it, until we drop support for interp->result completely.
	     */
	    iPtr->errorInfo = Tcl_NewStringObj(interp->result, -1);
	} else {
	    iPtr->errorInfo = iPtr->objResultPtr;
	}
	Tcl_IncrRefCount(iPtr->errorInfo);
	if (!iPtr->errorCode) {
	    Tcl_SetErrorCode(interp, "NONE", NULL);
	}
    }

    /*
     * Now append "message" to the end of errorInfo.
     */

    if (length != 0) {
	if (Tcl_IsShared(iPtr->errorInfo)) {
	    Tcl_DecrRefCount(iPtr->errorInfo);
	    iPtr->errorInfo = Tcl_DuplicateObj(iPtr->errorInfo);
	    Tcl_IncrRefCount(iPtr->errorInfo);
	}
	Tcl_AppendToObj(iPtr->errorInfo, message, length);
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_VarEvalVA --
 *
 *	Given a variable number of string arguments, concatenate them
 *	all together and execute the result as a Tcl command.
 *
 * Results:
 *	A standard Tcl return result.  An error message or other result may
 *	be left in the interp's result.
 *
 * Side effects:
 *	Depends on what was done by the command.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_VarEvalVA (interp, argList)
    Tcl_Interp *interp;		/* Interpreter in which to evaluate command. */
    va_list argList;		/* Variable argument list. */
{
    Tcl_DString buf;
    char *string;
    int result;

    /*
     * Copy the strings one after the other into a single larger
     * string.  Use stack-allocated space for small commands, but if
     * the command gets too large than call ckalloc to create the
     * space.
     */

    Tcl_DStringInit(&buf);
    while (1) {
	string = va_arg(argList, char *);
	if (string == NULL) {
	    break;
	}
	Tcl_DStringAppend(&buf, string, -1);
    }

    result = Tcl_Eval(interp, Tcl_DStringValue(&buf));
    Tcl_DStringFree(&buf);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_VarEval --
 *
 *	Given a variable number of string arguments, concatenate them
 *	all together and execute the result as a Tcl command.
 *
 * Results:
 *	A standard Tcl return result.  An error message or other
 *	result may be left in interp->result.
 *
 * Side effects:
 *	Depends on what was done by the command.
 *
 *----------------------------------------------------------------------
 */
	/* VARARGS2 */ /* ARGSUSED */
int
Tcl_VarEval TCL_VARARGS_DEF(Tcl_Interp *,arg1)
{
    Tcl_Interp *interp;
    va_list argList;
    int result;

    interp = TCL_VARARGS_START(Tcl_Interp *,arg1,argList);
    result = Tcl_VarEvalVA(interp, argList);
    va_end(argList);

    return result;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_GlobalEval --
 *
 *	Evaluate a command at global level in an interpreter.
 *
 * Results:
 *	A standard Tcl result is returned, and the interp's result is
 *	modified accordingly.
 *
 * Side effects:
 *	The command string is executed in interp, and the execution
 *	is carried out in the variable context of global level (no
 *	procedures active), just as if an "uplevel #0" command were
 *	being executed.
 *
 ---------------------------------------------------------------------------
 */

int
Tcl_GlobalEval(interp, command)
    Tcl_Interp *interp;		/* Interpreter in which to evaluate command. */
    CONST char *command;	/* Command to evaluate. */
{
    register Interp *iPtr = (Interp *) interp;
    int result;
    CallFrame *savedVarFramePtr;

    savedVarFramePtr = iPtr->varFramePtr;
    iPtr->varFramePtr = NULL;
    result = Tcl_Eval(interp, command);
    iPtr->varFramePtr = savedVarFramePtr;
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetRecursionLimit --
 *
 *	Set the maximum number of recursive calls that may be active
 *	for an interpreter at once.
 *
 * Results:
 *	The return value is the old limit on nesting for interp.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_SetRecursionLimit(interp, depth)
    Tcl_Interp *interp;			/* Interpreter whose nesting limit
					 * is to be set. */
    int depth;				/* New value for maximimum depth. */
{
    Interp *iPtr = (Interp *) interp;
    int old;

    old = iPtr->maxNestingDepth;
    if (depth > 0) {
	iPtr->maxNestingDepth = depth;
    }
    return old;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AllowExceptions --
 *
 *	Sets a flag in an interpreter so that exceptions can occur
 *	in the next call to Tcl_Eval without them being turned into
 *	errors.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The TCL_ALLOW_EXCEPTIONS flag gets set in the interpreter's
 *	evalFlags structure.  See the reference documentation for
 *	more details.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AllowExceptions(interp)
    Tcl_Interp *interp;		/* Interpreter in which to set flag. */
{
    Interp *iPtr = (Interp *) interp;

    iPtr->evalFlags |= TCL_ALLOW_EXCEPTIONS;
}


/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetVersion
 *
 *	Get the Tcl major, minor, and patchlevel version numbers and
 *      the release type.  A patch is a release type TCL_FINAL_RELEASE
 *      with a patchLevel > 0.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_GetVersion(majorV, minorV, patchLevelV, type)
    int *majorV;
    int *minorV;
    int *patchLevelV;
    int *type;
{
    if (majorV != NULL) {
        *majorV = TCL_MAJOR_VERSION;
    }
    if (minorV != NULL) {
        *minorV = TCL_MINOR_VERSION;
    }
    if (patchLevelV != NULL) {
        *patchLevelV = TCL_RELEASE_SERIAL;
    }
    if (type != NULL) {
        *type = TCL_RELEASE_LEVEL;
    }
}
 
