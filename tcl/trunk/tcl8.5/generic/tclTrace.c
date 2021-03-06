/* 
 * tclTrace.c --
 *
 *	This file contains code to handle most trace management.
 *
 * Copyright (c) 1987-1993 The Regents of the University of California.
 * Copyright (c) 1994-1997 Sun Microsystems, Inc.
 * Copyright (c) 1998-2000 Scriptics Corporation.
 * Copyright (c) 2002 ActiveState Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclTrace.c,v 1.21 2004/11/15 21:47:23 dgp Exp $
 */

#include "tclInt.h"

/*
 * Structure used to hold information about variable traces:
 */

typedef struct {
    int flags;			/* Operations for which Tcl command is
				 * to be invoked. */
    size_t length;		/* Number of non-NULL chars. in command. */
    char command[4];		/* Space for Tcl command to invoke.  Actual
				 * size will be as large as necessary to
				 * hold command.  This field must be the
				 * last in the structure, so that it can
				 * be larger than 4 bytes. */
} TraceVarInfo;

/*
 * Structure used to hold information about command traces:
 */

typedef struct {
    int flags;			/* Operations for which Tcl command is
				 * to be invoked. */
    size_t length;		/* Number of non-NULL chars. in command. */
    Tcl_Trace stepTrace;        /* Used for execution traces, when tracing
                                 * inside the given command */
    int startLevel;             /* Used for bookkeeping with step execution
                                 * traces, store the level at which the step
                                 * trace was invoked */
    char *startCmd;             /* Used for bookkeeping with step execution
                                 * traces, store the command name which invoked
                                 * step trace */
    int curFlags;               /* Trace flags for the current command */
    int curCode;                /* Return code for the current command */
    int refCount;               /* Used to ensure this structure is
                                 * not deleted too early.  Keeps track
                                 * of how many pieces of code have
                                 * a pointer to this structure. */
    char command[4];		/* Space for Tcl command to invoke.  Actual
				 * size will be as large as necessary to
				 * hold command.  This field must be the
				 * last in the structure, so that it can
				 * be larger than 4 bytes. */
} TraceCommandInfo;

/* 
 * Used by command execution traces.  Note that we assume in the code
 * that the first two defines are exactly 4 times the
 * 'TCL_TRACE_ENTER_EXEC' and 'TCL_TRACE_LEAVE_EXEC' constants.
 * 
 * TCL_TRACE_ENTER_DURING_EXEC  - Trace each command inside the command
 *                                currently being traced, before execution.
 * TCL_TRACE_LEAVE_DURING_EXEC  - Trace each command inside the command
 *                                currently being traced, after execution.
 * TCL_TRACE_ANY_EXEC           - OR'd combination of all EXEC flags.
 * TCL_TRACE_EXEC_IN_PROGRESS   - The callback procedure on this trace
 *                                is currently executing.  Therefore we
 *                                don't let further traces execute.
 * TCL_TRACE_EXEC_DIRECT        - This execution trace is triggered directly
 *                                by the command being traced, not because
 *                                of an internal trace.
 * The flags 'TCL_TRACE_DESTROYED' and 'TCL_INTERP_DESTROYED' may also
 * be used in command execution traces.
 */
#define TCL_TRACE_ENTER_DURING_EXEC	4
#define TCL_TRACE_LEAVE_DURING_EXEC	8
#define TCL_TRACE_ANY_EXEC              15
#define TCL_TRACE_EXEC_IN_PROGRESS      0x10
#define TCL_TRACE_EXEC_DIRECT           0x20

/*
 * Forward declarations for procedures defined in this file:
 */

typedef int (Tcl_TraceTypeObjCmd) _ANSI_ARGS_((Tcl_Interp *interp,
	int optionIndex, int objc, Tcl_Obj *CONST objv[]));

Tcl_TraceTypeObjCmd TclTraceVariableObjCmd;
Tcl_TraceTypeObjCmd TclTraceCommandObjCmd;
Tcl_TraceTypeObjCmd TclTraceExecutionObjCmd;

/* 
 * Each subcommand has a number of 'types' to which it can apply.
 * Currently 'execution', 'command' and 'variable' are the only
 * types supported.  These three arrays MUST be kept in sync!
 * In the future we may provide an API to add to the list of
 * supported trace types.
 */
static CONST char *traceTypeOptions[] = {
    "execution", "command", "variable", (char*) NULL
};
static Tcl_TraceTypeObjCmd* traceSubCmds[] = {
    TclTraceExecutionObjCmd,
    TclTraceCommandObjCmd,
    TclTraceVariableObjCmd,
};

/*
 * Declarations for local procedures to this file:
 */
static int              CallTraceProcedure _ANSI_ARGS_((Tcl_Interp *interp,
                            Trace *tracePtr, Command *cmdPtr,
                            CONST char *command, int numChars,
                            int objc, Tcl_Obj *CONST objv[]));
static char *		TraceVarProc _ANSI_ARGS_((ClientData clientData,
			    Tcl_Interp *interp, CONST char *name1, 
                            CONST char *name2, int flags));
static void		TraceCommandProc _ANSI_ARGS_((ClientData clientData,
			    Tcl_Interp *interp, CONST char *oldName,
                            CONST char *newName, int flags));
static Tcl_CmdObjTraceProc TraceExecutionProc;
static int	        StringTraceProc _ANSI_ARGS_((ClientData clientData,
						     Tcl_Interp* interp,
						     int level,
						     CONST char* command,
						    Tcl_Command commandInfo,
						    int objc,
						    Tcl_Obj *CONST objv[]));
static void           StringTraceDeleteProc _ANSI_ARGS_((ClientData clientData));
static void		DisposeTraceResult _ANSI_ARGS_((int flags,
			    char *result));

/*
 * The following structure holds the client data for string-based
 * trace procs
 */

typedef struct StringTraceData {
    ClientData clientData;	/* Client data from Tcl_CreateTrace */
    Tcl_CmdTraceProc* proc;	/* Trace procedure from Tcl_CreateTrace */
} StringTraceData;

/*
 *----------------------------------------------------------------------
 *
 * Tcl_TraceObjCmd --
 *
 *	This procedure is invoked to process the "trace" Tcl command.
 *	See the user documentation for details on what it does.
 *	
 *	Standard syntax as of Tcl 8.4 is
 *	
 *	 trace {add|info|remove} {command|variable} name ops cmd
 *
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_TraceObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    int optionIndex;
    char *name, *flagOps, *p;
    /* Main sub commands to 'trace' */
    static CONST char *traceOptions[] = {
	"add", "info", "remove", 
#ifndef TCL_REMOVE_OBSOLETE_TRACES
	"variable", "vdelete", "vinfo", 
#endif
	(char *) NULL
    };
    /* 'OLD' options are pre-Tcl-8.4 style */
    enum traceOptions {
	TRACE_ADD, TRACE_INFO, TRACE_REMOVE, 
#ifndef TCL_REMOVE_OBSOLETE_TRACES
	TRACE_OLD_VARIABLE, TRACE_OLD_VDELETE, TRACE_OLD_VINFO
#endif
    };

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "option ?arg arg ...?");
	return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], traceOptions,
		"option", 0, &optionIndex) != TCL_OK) {
	return TCL_ERROR;
    }
    switch ((enum traceOptions) optionIndex) {
	case TRACE_ADD: 
	case TRACE_REMOVE: {
	    /* 
	     * All sub commands of trace add/remove must take at least
	     * one more argument.  Beyond that we let the subcommand itself
	     * control the argument structure.
	     */
	    int typeIndex;
	    if (objc < 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "type ?arg arg ...?");
		return TCL_ERROR;
	    }
	    if (Tcl_GetIndexFromObj(interp, objv[2], traceTypeOptions,
			"option", 0, &typeIndex) != TCL_OK) {
		return TCL_ERROR;
	    }
	    return (traceSubCmds[typeIndex])(interp, optionIndex, objc, objv);
	}
	case TRACE_INFO: {
	    /* 
	     * All sub commands of trace info must take exactly two
	     * more arguments which name the type of thing being
	     * traced and the name of the thing being traced.
	     */
	    int typeIndex;
	    if (objc < 3) {
		/*
		 * Delegate other complaints to the type-specific code
		 * which can give a better error message.
		 */
		Tcl_WrongNumArgs(interp, 2, objv, "type name");
		return TCL_ERROR;
	    }
	    if (Tcl_GetIndexFromObj(interp, objv[2], traceTypeOptions,
			"option", 0, &typeIndex) != TCL_OK) {
		return TCL_ERROR;
	    }
	    return (traceSubCmds[typeIndex])(interp, optionIndex, objc, objv);
	    break;
	}

#ifndef TCL_REMOVE_OBSOLETE_TRACES
	case TRACE_OLD_VARIABLE:
	case TRACE_OLD_VDELETE: {
	    Tcl_Obj *copyObjv[6];
	    Tcl_Obj *opsList;
	    int code, numFlags;

	    if (objc != 5) {
		Tcl_WrongNumArgs(interp, 2, objv, "name ops command");
		return TCL_ERROR;
	    }

	    opsList = Tcl_NewObj();
	    Tcl_IncrRefCount(opsList);
	    flagOps = Tcl_GetStringFromObj(objv[3], &numFlags);
	    if (numFlags == 0) {
		Tcl_DecrRefCount(opsList);
		goto badVarOps;
	    }
	    for (p = flagOps; *p != 0; p++) {
		if (*p == 'r') {
		    Tcl_ListObjAppendElement(NULL, opsList,
			    Tcl_NewStringObj("read", -1));
		} else if (*p == 'w') {
		    Tcl_ListObjAppendElement(NULL, opsList,
			    Tcl_NewStringObj("write", -1));
		} else if (*p == 'u') {
		    Tcl_ListObjAppendElement(NULL, opsList,
			    Tcl_NewStringObj("unset", -1));
		} else if (*p == 'a') {
		    Tcl_ListObjAppendElement(NULL, opsList,
			    Tcl_NewStringObj("array", -1));
		} else {
		    Tcl_DecrRefCount(opsList);
		    goto badVarOps;
		}
	    }
	    copyObjv[0] = NULL;
	    memcpy(copyObjv+1, objv, objc*sizeof(Tcl_Obj *));
	    copyObjv[4] = opsList;
	    if  (optionIndex == TRACE_OLD_VARIABLE) {
		code = (traceSubCmds[2])(interp,TRACE_ADD,objc+1,copyObjv);
	    } else {
		code = (traceSubCmds[2])(interp,TRACE_REMOVE,objc+1,copyObjv);
	    }
	    Tcl_DecrRefCount(opsList);
	    return code;
	}
	case TRACE_OLD_VINFO: {
	    ClientData clientData;
	    char ops[5];
	    Tcl_Obj *resultListPtr, *pairObjPtr, *elemObjPtr;

	    if (objc != 3) {
		Tcl_WrongNumArgs(interp, 2, objv, "name");
		return TCL_ERROR;
	    }
	    resultListPtr = Tcl_NewObj();
	    clientData = 0;
	    name = Tcl_GetString(objv[2]);
	    while ((clientData = Tcl_VarTraceInfo(interp, name, 0,
		    TraceVarProc, clientData)) != 0) {

		TraceVarInfo *tvarPtr = (TraceVarInfo *) clientData;

		pairObjPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
		p = ops;
		if (tvarPtr->flags & TCL_TRACE_READS) {
		    *p = 'r';
		    p++;
		}
		if (tvarPtr->flags & TCL_TRACE_WRITES) {
		    *p = 'w';
		    p++;
		}
		if (tvarPtr->flags & TCL_TRACE_UNSETS) {
		    *p = 'u';
		    p++;
		}
		if (tvarPtr->flags & TCL_TRACE_ARRAY) {
		    *p = 'a';
		    p++;
		}
		*p = '\0';

		/*
		 * Build a pair (2-item list) with the ops string as
		 * the first obj element and the tvarPtr->command string
		 * as the second obj element.  Append the pair (as an
		 * element) to the end of the result object list.
		 */

		elemObjPtr = Tcl_NewStringObj(ops, -1);
		Tcl_ListObjAppendElement(NULL, pairObjPtr, elemObjPtr);
		elemObjPtr = Tcl_NewStringObj(tvarPtr->command, -1);
		Tcl_ListObjAppendElement(NULL, pairObjPtr, elemObjPtr);
		Tcl_ListObjAppendElement(interp, resultListPtr, pairObjPtr);
	    }
	    Tcl_SetObjResult(interp, resultListPtr);
	    break;
	}
#endif /* TCL_REMOVE_OBSOLETE_TRACES */
    }
    return TCL_OK;

    badVarOps:
    Tcl_AppendResult(interp, "bad operations \"", flagOps,
	    "\": should be one or more of rwua", (char *) NULL);
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * TclTraceExecutionObjCmd --
 *
 *	Helper function for Tcl_TraceObjCmd; implements the
 *	[trace {add|remove|info} execution ...] subcommands.
 *	See the user documentation for details on what these do.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Depends on the operation (add, remove, or info) being performed;
 *	may add or remove command traces on a command.
 *
 *----------------------------------------------------------------------
 */

int
TclTraceExecutionObjCmd(interp, optionIndex, objc, objv)
    Tcl_Interp *interp;			/* Current interpreter. */
    int optionIndex;			/* Add, info or remove */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    int commandLength, index;
    char *name, *command;
    size_t length;
    enum traceOptions { TRACE_ADD, TRACE_INFO, TRACE_REMOVE };
    static CONST char *opStrings[] = { "enter", "leave", 
                                 "enterstep", "leavestep", (char *) NULL };
    enum operations { TRACE_EXEC_ENTER, TRACE_EXEC_LEAVE,
                      TRACE_EXEC_ENTER_STEP, TRACE_EXEC_LEAVE_STEP };
    
    switch ((enum traceOptions) optionIndex) {
	case TRACE_ADD: 
	case TRACE_REMOVE: {
	    int flags = 0;
	    int i, listLen, result;
	    Tcl_Obj **elemPtrs;
	    if (objc != 6) {
		Tcl_WrongNumArgs(interp, 3, objv, "name opList command");
		return TCL_ERROR;
	    }
	    /*
	     * Make sure the ops argument is a list object; get its length and
	     * a pointer to its array of element pointers.
	     */

	    result = Tcl_ListObjGetElements(interp, objv[4], &listLen,
		    &elemPtrs);
	    if (result != TCL_OK) {
		return result;
	    }
	    if (listLen == 0) {
		Tcl_SetResult(interp, "bad operation list \"\": must be "
	          "one or more of enter, leave, enterstep, or leavestep", 
		  TCL_STATIC);
		return TCL_ERROR;
	    }
	    for (i = 0; i < listLen; i++) {
		if (Tcl_GetIndexFromObj(interp, elemPtrs[i], opStrings,
			"operation", TCL_EXACT, &index) != TCL_OK) {
		    return TCL_ERROR;
		}
		switch ((enum operations) index) {
		    case TRACE_EXEC_ENTER:
			flags |= TCL_TRACE_ENTER_EXEC;
			break;
		    case TRACE_EXEC_LEAVE:
			flags |= TCL_TRACE_LEAVE_EXEC;
			break;
		    case TRACE_EXEC_ENTER_STEP:
			flags |= TCL_TRACE_ENTER_DURING_EXEC;
			break;
		    case TRACE_EXEC_LEAVE_STEP:
			flags |= TCL_TRACE_LEAVE_DURING_EXEC;
			break;
		}
	    }
	    command = Tcl_GetStringFromObj(objv[5], &commandLength);
	    length = (size_t) commandLength;
	    if ((enum traceOptions) optionIndex == TRACE_ADD) {
		TraceCommandInfo *tcmdPtr;
		tcmdPtr = (TraceCommandInfo *) ckalloc((unsigned)
			(sizeof(TraceCommandInfo) - sizeof(tcmdPtr->command)
				+ length + 1));
		tcmdPtr->flags = flags;
		tcmdPtr->stepTrace = NULL;
		tcmdPtr->startLevel = 0;
		tcmdPtr->startCmd = NULL;
		tcmdPtr->length = length;
		tcmdPtr->refCount = 1;
		flags |= TCL_TRACE_DELETE;
		if (flags & (TCL_TRACE_ENTER_DURING_EXEC |
			     TCL_TRACE_LEAVE_DURING_EXEC)) {
		    flags |= (TCL_TRACE_ENTER_EXEC | TCL_TRACE_LEAVE_EXEC);
		}
		strcpy(tcmdPtr->command, command);
		name = Tcl_GetString(objv[3]);
		if (Tcl_TraceCommand(interp, name, flags, TraceCommandProc,
			(ClientData) tcmdPtr) != TCL_OK) {
		    ckfree((char *) tcmdPtr);
		    return TCL_ERROR;
		}
	    } else {
		/*
		 * Search through all of our traces on this command to
		 * see if there's one with the given command.  If so, then
		 * delete the first one that matches.
		 */
		
		TraceCommandInfo *tcmdPtr;
		ClientData clientData = NULL;
		name = Tcl_GetString(objv[3]);

		/* First ensure the name given is valid */
		if (Tcl_FindCommand(interp, name, NULL, 
				    TCL_LEAVE_ERR_MSG) == NULL) {
		    return TCL_ERROR;
		}
				    
		while ((clientData = Tcl_CommandTraceInfo(interp, name, 0,
			TraceCommandProc, clientData)) != NULL) {
		    tcmdPtr = (TraceCommandInfo *) clientData;
		    /* 
		     * In checking the 'flags' field we must remove any
		     * extraneous flags which may have been temporarily
		     * added by various pieces of the trace mechanism.
		     */
		    if ((tcmdPtr->length == length)
			    && ((tcmdPtr->flags & (TCL_TRACE_ANY_EXEC | 
						   TCL_TRACE_RENAME | 
						   TCL_TRACE_DELETE)) == flags)
			    && (strncmp(command, tcmdPtr->command,
				    (size_t) length) == 0)) {
			flags |= TCL_TRACE_DELETE;
			if (flags & (TCL_TRACE_ENTER_DURING_EXEC | 
				     TCL_TRACE_LEAVE_DURING_EXEC)) {
			    flags |= (TCL_TRACE_ENTER_EXEC | 
				      TCL_TRACE_LEAVE_EXEC);
			}
			Tcl_UntraceCommand(interp, name,
				flags, TraceCommandProc, clientData);
			if (tcmdPtr->stepTrace != NULL) {
			    /* 
			     * We need to remove the interpreter-wide trace 
			     * which we created to allow 'step' traces.
			     */
			    Tcl_DeleteTrace(interp, tcmdPtr->stepTrace);
			    tcmdPtr->stepTrace = NULL;
                            if (tcmdPtr->startCmd != NULL) {
			        ckfree((char *)tcmdPtr->startCmd);
			    }
			}
			if (tcmdPtr->flags & TCL_TRACE_EXEC_IN_PROGRESS) {
			    /* Postpone deletion */
			    tcmdPtr->flags = 0;
			}
			if ((--tcmdPtr->refCount) <= 0) {
			    ckfree((char*)tcmdPtr);
			}
			break;
		    }
		}
	    }
	    break;
	}
	case TRACE_INFO: {
	    ClientData clientData;
	    Tcl_Obj *resultListPtr, *eachTraceObjPtr, *elemObjPtr;
	    if (objc != 4) {
		Tcl_WrongNumArgs(interp, 3, objv, "name");
		return TCL_ERROR;
	    }

	    clientData = NULL;
	    name = Tcl_GetString(objv[3]);
	    
	    /* First ensure the name given is valid */
	    if (Tcl_FindCommand(interp, name, NULL, 
				TCL_LEAVE_ERR_MSG) == NULL) {
		return TCL_ERROR;
	    }
				
	    resultListPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
	    while ((clientData = Tcl_CommandTraceInfo(interp, name, 0,
		    TraceCommandProc, clientData)) != NULL) {
		int numOps = 0;

		TraceCommandInfo *tcmdPtr = (TraceCommandInfo *) clientData;

		/*
		 * Build a list with the ops list as the first obj
		 * element and the tcmdPtr->command string as the
		 * second obj element.  Append this list (as an
		 * element) to the end of the result object list.
		 */

		elemObjPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
		Tcl_IncrRefCount(elemObjPtr);
		if (tcmdPtr->flags & TCL_TRACE_ENTER_EXEC) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("enter",5));
		}
		if (tcmdPtr->flags & TCL_TRACE_LEAVE_EXEC) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("leave",5));
		}
		if (tcmdPtr->flags & TCL_TRACE_ENTER_DURING_EXEC) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("enterstep",9));
		}
		if (tcmdPtr->flags & TCL_TRACE_LEAVE_DURING_EXEC) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("leavestep",9));
		}
		Tcl_ListObjLength(NULL, elemObjPtr, &numOps);
		if (0 == numOps) {
		    Tcl_DecrRefCount(elemObjPtr);
		    continue;
		}
		eachTraceObjPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
		Tcl_ListObjAppendElement(NULL, eachTraceObjPtr, elemObjPtr);
		Tcl_DecrRefCount(elemObjPtr);
		elemObjPtr = NULL;
		
		Tcl_ListObjAppendElement(NULL, eachTraceObjPtr, 
			Tcl_NewStringObj(tcmdPtr->command, -1));
		Tcl_ListObjAppendElement(interp, resultListPtr,
			eachTraceObjPtr);
	    }
	    Tcl_SetObjResult(interp, resultListPtr);
	    break;
	}
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * TclTraceCommandObjCmd --
 *
 *	Helper function for Tcl_TraceObjCmd; implements the
 *	[trace {add|info|remove} command ...] subcommands.
 *	See the user documentation for details on what these do.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Depends on the operation (add, remove, or info) being performed;
 *	may add or remove command traces on a command.
 *
 *----------------------------------------------------------------------
 */

int
TclTraceCommandObjCmd(interp, optionIndex, objc, objv)
    Tcl_Interp *interp;			/* Current interpreter. */
    int optionIndex;			/* Add, info or remove */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    int commandLength, index;
    char *name, *command;
    size_t length;
    enum traceOptions { TRACE_ADD, TRACE_INFO, TRACE_REMOVE };
    static CONST char *opStrings[] = { "delete", "rename", (char *) NULL };
    enum operations { TRACE_CMD_DELETE, TRACE_CMD_RENAME };
    
    switch ((enum traceOptions) optionIndex) {
	case TRACE_ADD: 
	case TRACE_REMOVE: {
	    int flags = 0;
	    int i, listLen, result;
	    Tcl_Obj **elemPtrs;
	    if (objc != 6) {
		Tcl_WrongNumArgs(interp, 3, objv, "name opList command");
		return TCL_ERROR;
	    }
	    /*
	     * Make sure the ops argument is a list object; get its length and
	     * a pointer to its array of element pointers.
	     */

	    result = Tcl_ListObjGetElements(interp, objv[4], &listLen,
		    &elemPtrs);
	    if (result != TCL_OK) {
		return result;
	    }
	    if (listLen == 0) {
		Tcl_SetResult(interp, "bad operation list \"\": must be "
			"one or more of delete or rename", TCL_STATIC);
		return TCL_ERROR;
	    }
	    for (i = 0; i < listLen; i++) {
		if (Tcl_GetIndexFromObj(interp, elemPtrs[i], opStrings,
			"operation", TCL_EXACT, &index) != TCL_OK) {
		    return TCL_ERROR;
		}
		switch ((enum operations) index) {
		    case TRACE_CMD_RENAME:
			flags |= TCL_TRACE_RENAME;
			break;
		    case TRACE_CMD_DELETE:
			flags |= TCL_TRACE_DELETE;
			break;
		}
	    }
	    command = Tcl_GetStringFromObj(objv[5], &commandLength);
	    length = (size_t) commandLength;
	    if ((enum traceOptions) optionIndex == TRACE_ADD) {
		TraceCommandInfo *tcmdPtr;
		tcmdPtr = (TraceCommandInfo *) ckalloc((unsigned)
			(sizeof(TraceCommandInfo) - sizeof(tcmdPtr->command)
				+ length + 1));
		tcmdPtr->flags = flags;
		tcmdPtr->stepTrace = NULL;
		tcmdPtr->startLevel = 0;
		tcmdPtr->startCmd = NULL;
		tcmdPtr->length = length;
		tcmdPtr->refCount = 1;
		flags |= TCL_TRACE_DELETE;
		strcpy(tcmdPtr->command, command);
		name = Tcl_GetString(objv[3]);
		if (Tcl_TraceCommand(interp, name, flags, TraceCommandProc,
			(ClientData) tcmdPtr) != TCL_OK) {
		    ckfree((char *) tcmdPtr);
		    return TCL_ERROR;
		}
	    } else {
		/*
		 * Search through all of our traces on this command to
		 * see if there's one with the given command.  If so, then
		 * delete the first one that matches.
		 */
		
		TraceCommandInfo *tcmdPtr;
		ClientData clientData = NULL;
		name = Tcl_GetString(objv[3]);
		
		/* First ensure the name given is valid */
		if (Tcl_FindCommand(interp, name, NULL, 
				    TCL_LEAVE_ERR_MSG) == NULL) {
		    return TCL_ERROR;
		}
				    
		while ((clientData = Tcl_CommandTraceInfo(interp, name, 0,
			TraceCommandProc, clientData)) != NULL) {
		    tcmdPtr = (TraceCommandInfo *) clientData;
		    if ((tcmdPtr->length == length)
			    && (tcmdPtr->flags == flags)
			    && (strncmp(command, tcmdPtr->command,
				    (size_t) length) == 0)) {
			Tcl_UntraceCommand(interp, name,
				flags | TCL_TRACE_DELETE,
				TraceCommandProc, clientData);
			tcmdPtr->flags |= TCL_TRACE_DESTROYED;
			if ((--tcmdPtr->refCount) <= 0) {
			    ckfree((char *) tcmdPtr);
			}
			break;
		    }
		}
	    }
	    break;
	}
	case TRACE_INFO: {
	    ClientData clientData;
	    Tcl_Obj *resultListPtr, *eachTraceObjPtr, *elemObjPtr;
	    if (objc != 4) {
		Tcl_WrongNumArgs(interp, 3, objv, "name");
		return TCL_ERROR;
	    }

	    clientData = NULL;
	    name = Tcl_GetString(objv[3]);
	    
	    /* First ensure the name given is valid */
	    if (Tcl_FindCommand(interp, name, NULL, 
				TCL_LEAVE_ERR_MSG) == NULL) {
		return TCL_ERROR;
	    }
				
	    resultListPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
	    while ((clientData = Tcl_CommandTraceInfo(interp, name, 0,
		    TraceCommandProc, clientData)) != NULL) {
		int numOps = 0;

		TraceCommandInfo *tcmdPtr = (TraceCommandInfo *) clientData;

		/*
		 * Build a list with the ops list as
		 * the first obj element and the tcmdPtr->command string
		 * as the second obj element.  Append this list (as an
		 * element) to the end of the result object list.
		 */

		elemObjPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
		Tcl_IncrRefCount(elemObjPtr);
		if (tcmdPtr->flags & TCL_TRACE_RENAME) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("rename",6));
		}
		if (tcmdPtr->flags & TCL_TRACE_DELETE) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("delete",6));
		}
		Tcl_ListObjLength(NULL, elemObjPtr, &numOps);
		if (0 == numOps) {
		    Tcl_DecrRefCount(elemObjPtr);
		    continue;
		}
		eachTraceObjPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
		Tcl_ListObjAppendElement(NULL, eachTraceObjPtr, elemObjPtr);
		Tcl_DecrRefCount(elemObjPtr);

		elemObjPtr = Tcl_NewStringObj(tcmdPtr->command, -1);
		Tcl_ListObjAppendElement(NULL, eachTraceObjPtr, elemObjPtr);
		Tcl_ListObjAppendElement(interp, resultListPtr,
			eachTraceObjPtr);
	    }
	    Tcl_SetObjResult(interp, resultListPtr);
	    break;
	}
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * TclTraceVariableObjCmd --
 *
 *	Helper function for Tcl_TraceObjCmd; implements the
 *	[trace {add|info|remove} variable ...] subcommands.
 *	See the user documentation for details on what these do.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Depends on the operation (add, remove, or info) being performed;
 *	may add or remove variable traces on a variable.
 *
 *----------------------------------------------------------------------
 */

int
TclTraceVariableObjCmd(interp, optionIndex, objc, objv)
    Tcl_Interp *interp;			/* Current interpreter. */
    int optionIndex;			/* Add, info or remove */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    int commandLength, index;
    char *name, *command;
    size_t length;
    enum traceOptions { TRACE_ADD, TRACE_INFO, TRACE_REMOVE };
    static CONST char *opStrings[] = { "array", "read", "unset", "write",
				     (char *) NULL };
    enum operations { TRACE_VAR_ARRAY, TRACE_VAR_READ, TRACE_VAR_UNSET,
			  TRACE_VAR_WRITE };
        
    switch ((enum traceOptions) optionIndex) {
	case TRACE_ADD: 
	case TRACE_REMOVE: {
	    int flags = 0;
	    int i, listLen, result;
	    Tcl_Obj **elemPtrs;
	    if (objc != 6) {
		Tcl_WrongNumArgs(interp, 3, objv, "name opList command");
		return TCL_ERROR;
	    }
	    /*
	     * Make sure the ops argument is a list object; get its length and
	     * a pointer to its array of element pointers.
	     */

	    result = Tcl_ListObjGetElements(interp, objv[4], &listLen,
		    &elemPtrs);
	    if (result != TCL_OK) {
		return result;
	    }
	    if (listLen == 0) {
		Tcl_SetResult(interp, "bad operation list \"\": must be "
			"one or more of array, read, unset, or write",
			TCL_STATIC);
		return TCL_ERROR;
	    }
	    for (i = 0; i < listLen ; i++) {
		if (Tcl_GetIndexFromObj(interp, elemPtrs[i], opStrings,
			"operation", TCL_EXACT, &index) != TCL_OK) {
		    return TCL_ERROR;
		}
		switch ((enum operations) index) {
		    case TRACE_VAR_ARRAY:
			flags |= TCL_TRACE_ARRAY;
			break;
		    case TRACE_VAR_READ:
			flags |= TCL_TRACE_READS;
			break;
		    case TRACE_VAR_UNSET:
			flags |= TCL_TRACE_UNSETS;
			break;
		    case TRACE_VAR_WRITE:
			flags |= TCL_TRACE_WRITES;
			break;
		}
	    }
	    command = Tcl_GetStringFromObj(objv[5], &commandLength);
	    length = (size_t) commandLength;
	    if ((enum traceOptions) optionIndex == TRACE_ADD) {
		TraceVarInfo *tvarPtr;
		tvarPtr = (TraceVarInfo *) ckalloc((unsigned)
			(sizeof(TraceVarInfo) - sizeof(tvarPtr->command)
				+ length + 1));
		tvarPtr->flags = flags;
		if (objv[0] == NULL) {
		    tvarPtr->flags |= TCL_TRACE_OLD_STYLE;
		}
		tvarPtr->length = length;
		flags |= TCL_TRACE_UNSETS | TCL_TRACE_RESULT_OBJECT;
		strcpy(tvarPtr->command, command);
		name = Tcl_GetString(objv[3]);
		if (Tcl_TraceVar(interp, name, flags, TraceVarProc,
			(ClientData) tvarPtr) != TCL_OK) {
		    ckfree((char *) tvarPtr);
		    return TCL_ERROR;
		}
	    } else {
		/*
		 * Search through all of our traces on this variable to
		 * see if there's one with the given command.  If so, then
		 * delete the first one that matches.
		 */
		
		TraceVarInfo *tvarPtr;
		ClientData clientData = 0;
		name = Tcl_GetString(objv[3]);
		while ((clientData = Tcl_VarTraceInfo(interp, name, 0,
			TraceVarProc, clientData)) != 0) {
		    tvarPtr = (TraceVarInfo *) clientData;
		    if ((tvarPtr->length == length)
			    && ((tvarPtr->flags & ~TCL_TRACE_OLD_STYLE)==flags)
			    && (strncmp(command, tvarPtr->command,
				    (size_t) length) == 0)) {
			Tcl_UntraceVar2(interp, name, NULL, 
			  flags | TCL_TRACE_UNSETS | TCL_TRACE_RESULT_OBJECT,
				TraceVarProc, clientData);
			Tcl_EventuallyFree((ClientData) tvarPtr, TCL_DYNAMIC);
			break;
		    }
		}
	    }
	    break;
	}
	case TRACE_INFO: {
	    ClientData clientData;
	    Tcl_Obj *resultListPtr, *eachTraceObjPtr, *elemObjPtr;
	    if (objc != 4) {
		Tcl_WrongNumArgs(interp, 3, objv, "name");
		return TCL_ERROR;
	    }

	    resultListPtr = Tcl_NewObj();
	    clientData = 0;
	    name = Tcl_GetString(objv[3]);
	    while ((clientData = Tcl_VarTraceInfo(interp, name, 0,
		    TraceVarProc, clientData)) != 0) {

		TraceVarInfo *tvarPtr = (TraceVarInfo *) clientData;

		/*
		 * Build a list with the ops list as
		 * the first obj element and the tcmdPtr->command string
		 * as the second obj element.  Append this list (as an
		 * element) to the end of the result object list.
		 */

		elemObjPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
		if (tvarPtr->flags & TCL_TRACE_ARRAY) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("array", 5));
		}
		if (tvarPtr->flags & TCL_TRACE_READS) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("read", 4));
		}
		if (tvarPtr->flags & TCL_TRACE_WRITES) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("write", 5));
		}
		if (tvarPtr->flags & TCL_TRACE_UNSETS) {
		    Tcl_ListObjAppendElement(NULL, elemObjPtr,
			    Tcl_NewStringObj("unset", 5));
		}
		eachTraceObjPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
		Tcl_ListObjAppendElement(NULL, eachTraceObjPtr, elemObjPtr);

		elemObjPtr = Tcl_NewStringObj(tvarPtr->command, -1);
		Tcl_ListObjAppendElement(NULL, eachTraceObjPtr, elemObjPtr);
		Tcl_ListObjAppendElement(interp, resultListPtr,
			eachTraceObjPtr);
	    }
	    Tcl_SetObjResult(interp, resultListPtr);
	    break;
	}
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Tcl_CommandTraceInfo --
 *
 *	Return the clientData value associated with a trace on a
 *	command.  This procedure can also be used to step through
 *	all of the traces on a particular command that have the
 *	same trace procedure.
 *
 * Results:
 *	The return value is the clientData value associated with
 *	a trace on the given command.  Information will only be
 *	returned for a trace with proc as trace procedure.  If
 *	the clientData argument is NULL then the first such trace is
 *	returned;  otherwise, the next relevant one after the one
 *	given by clientData will be returned.  If the command
 *	doesn't exist then an error message is left in the interpreter
 *	and NULL is returned.  Also, if there are no (more) traces for 
 *	the given command, NULL is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ClientData
Tcl_CommandTraceInfo(interp, cmdName, flags, proc, prevClientData)
    Tcl_Interp *interp;		/* Interpreter containing command. */
    CONST char *cmdName;	/* Name of command. */
    int flags;			/* OR-ed combo or TCL_GLOBAL_ONLY,
				 * TCL_NAMESPACE_ONLY (can be 0). */
    Tcl_CommandTraceProc *proc;	/* Procedure assocated with trace. */
    ClientData prevClientData;	/* If non-NULL, gives last value returned
				 * by this procedure, so this call will
				 * return the next trace after that one.
				 * If NULL, this call will return the
				 * first trace. */
{
    Command *cmdPtr;
    register CommandTrace *tracePtr;

    cmdPtr = (Command*)Tcl_FindCommand(interp, cmdName, 
		NULL, TCL_LEAVE_ERR_MSG);
    if (cmdPtr == NULL) {
	return NULL;
    }

    /*
     * Find the relevant trace, if any, and return its clientData.
     */

    tracePtr = cmdPtr->tracePtr;
    if (prevClientData != NULL) {
	for ( ;  tracePtr != NULL;  tracePtr = tracePtr->nextPtr) {
	    if ((tracePtr->clientData == prevClientData)
		    && (tracePtr->traceProc == proc)) {
		tracePtr = tracePtr->nextPtr;
		break;
	    }
	}
    }
    for ( ;  tracePtr != NULL;  tracePtr = tracePtr->nextPtr) {
	if (tracePtr->traceProc == proc) {
	    return tracePtr->clientData;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_TraceCommand --
 *
 *	Arrange for rename/deletes to a command to cause a
 *	procedure to be invoked, which can monitor the operations.
 *	
 *	Also optionally arrange for execution of that command
 *	to cause a procedure to be invoked.
 *
 * Results:
 *	A standard Tcl return value.
 *
 * Side effects:
 *	A trace is set up on the command given by cmdName, such that
 *	future changes to the command will be intermediated by
 *	proc.  See the manual entry for complete details on the calling
 *	sequence for proc.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_TraceCommand(interp, cmdName, flags, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter in which command is
				 * to be traced. */
    CONST char *cmdName;	/* Name of command. */
    int flags;			/* OR-ed collection of bits, including any
				 * of TCL_TRACE_RENAME, TCL_TRACE_DELETE,
				 * and any of the TRACE_*_EXEC flags */
    Tcl_CommandTraceProc *proc;	/* Procedure to call when specified ops are
				 * invoked upon cmdName. */
    ClientData clientData;	/* Arbitrary argument to pass to proc. */
{
    Command *cmdPtr;
    register CommandTrace *tracePtr;

    cmdPtr = (Command*)Tcl_FindCommand(interp, cmdName,
	    NULL, TCL_LEAVE_ERR_MSG);
    if (cmdPtr == NULL) {
	return TCL_ERROR;
    }

    /*
     * Set up trace information.
     */

    tracePtr = (CommandTrace *) ckalloc(sizeof(CommandTrace));
    tracePtr->traceProc = proc;
    tracePtr->clientData = clientData;
    tracePtr->flags = flags & (TCL_TRACE_RENAME | TCL_TRACE_DELETE
			       | TCL_TRACE_ANY_EXEC);
    tracePtr->nextPtr = cmdPtr->tracePtr;
    tracePtr->refCount = 1;
    cmdPtr->tracePtr = tracePtr;
    if (tracePtr->flags & TCL_TRACE_ANY_EXEC) {
        cmdPtr->flags |= CMD_HAS_EXEC_TRACES;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UntraceCommand --
 *
 *	Remove a previously-created trace for a command.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If there exists a trace for the command given by cmdName
 *	with the given flags, proc, and clientData, then that trace
 *	is removed.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_UntraceCommand(interp, cmdName, flags, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter containing command. */
    CONST char *cmdName;	/* Name of command. */
    int flags;			/* OR-ed collection of bits, including any
				 * of TCL_TRACE_RENAME, TCL_TRACE_DELETE,
				 * and any of the TRACE_*_EXEC flags */
    Tcl_CommandTraceProc *proc;	/* Procedure assocated with trace. */
    ClientData clientData;	/* Arbitrary argument to pass to proc. */
{
    register CommandTrace *tracePtr;
    CommandTrace *prevPtr;
    Command *cmdPtr;
    Interp *iPtr = (Interp *) interp;
    ActiveCommandTrace *activePtr;
    int hasExecTraces = 0;
    
    cmdPtr = (Command*)Tcl_FindCommand(interp, cmdName, 
		NULL, TCL_LEAVE_ERR_MSG);
    if (cmdPtr == NULL) {
	return;
    }

    flags &= (TCL_TRACE_RENAME | TCL_TRACE_DELETE | TCL_TRACE_ANY_EXEC);

    for (tracePtr = cmdPtr->tracePtr, prevPtr = NULL;  ;
	 prevPtr = tracePtr, tracePtr = tracePtr->nextPtr) {
	if (tracePtr == NULL) {
	    return;
	}
	if ((tracePtr->traceProc == proc) 
	    && ((tracePtr->flags & (TCL_TRACE_RENAME | TCL_TRACE_DELETE | 
				    TCL_TRACE_ANY_EXEC)) == flags)
		&& (tracePtr->clientData == clientData)) {
	    if (tracePtr->flags & TCL_TRACE_ANY_EXEC) {
		hasExecTraces = 1;
	    }
	    break;
	}
    }
    
    /*
     * The code below makes it possible to delete traces while traces
     * are active: it makes sure that the deleted trace won't be
     * processed by CallCommandTraces.
     */

    for (activePtr = iPtr->activeCmdTracePtr;  activePtr != NULL;
	 activePtr = activePtr->nextPtr) {
	if (activePtr->nextTracePtr == tracePtr) {
	    activePtr->nextTracePtr = tracePtr->nextPtr;
	}
    }
    if (prevPtr == NULL) {
	cmdPtr->tracePtr = tracePtr->nextPtr;
    } else {
	prevPtr->nextPtr = tracePtr->nextPtr;
    }
    tracePtr->flags = 0;
    
    if ((--tracePtr->refCount) <= 0) {
	ckfree((char*)tracePtr);
    }
    
    if (hasExecTraces) {
	for (tracePtr = cmdPtr->tracePtr, prevPtr = NULL; tracePtr != NULL ;
	     prevPtr = tracePtr, tracePtr = tracePtr->nextPtr) {
	    if (tracePtr->flags & TCL_TRACE_ANY_EXEC) {
	        return;
	    }
	}
	/* 
	 * None of the remaining traces on this command are execution
	 * traces.  We therefore remove this flag:
	 */
	cmdPtr->flags &= ~CMD_HAS_EXEC_TRACES;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TraceCommandProc --
 *
 *	This procedure is called to handle command changes that have
 *	been traced using the "trace" command, when using the 
 *	'rename' or 'delete' options.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Depends on the command associated with the trace.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static void
TraceCommandProc(clientData, interp, oldName, newName, flags)
    ClientData clientData;	/* Information about the command trace. */
    Tcl_Interp *interp;		/* Interpreter containing command. */
    CONST char *oldName;	/* Name of command being changed. */
    CONST char *newName;	/* New name of command.  Empty string
                  		 * or NULL means command is being deleted
                  		 * (renamed to ""). */
    int flags;			/* OR-ed bits giving operation and other
				 * information. */
{
    TraceCommandInfo *tcmdPtr = (TraceCommandInfo *) clientData;
    int code;
    Tcl_DString cmd;
    
    tcmdPtr->refCount++;
    
    if ((tcmdPtr->flags & flags) && !(flags & TCL_INTERP_DESTROYED)
	    && !Tcl_LimitExceeded(interp)) {
	/*
	 * Generate a command to execute by appending list elements
	 * for the old and new command name and the operation.
	 */

	Tcl_DStringInit(&cmd);
	Tcl_DStringAppend(&cmd, tcmdPtr->command, (int) tcmdPtr->length);
	Tcl_DStringAppendElement(&cmd, oldName);
	Tcl_DStringAppendElement(&cmd, (newName ? newName : ""));
	if (flags & TCL_TRACE_RENAME) {
	    Tcl_DStringAppend(&cmd, " rename", 7);
	} else if (flags & TCL_TRACE_DELETE) {
	    Tcl_DStringAppend(&cmd, " delete", 7);
	}

	/*
	 * Execute the command.  
	 * We discard any object result the command returns.
	 *
	 * Add the TCL_TRACE_DESTROYED flag to tcmdPtr to indicate to
	 * other areas that this will be destroyed by us, otherwise a
	 * double-free might occur depending on what the eval does.
	 */

	if (flags & TCL_TRACE_DESTROYED) {
	    tcmdPtr->flags |= TCL_TRACE_DESTROYED;
	}
	code = Tcl_EvalEx(interp, Tcl_DStringValue(&cmd),
		Tcl_DStringLength(&cmd), 0);
	if (code != TCL_OK) {	     
	    /* We ignore errors in these traced commands */
	    /*** QUESTION: Use Tcl_BackgroundError(interp); instead? ***/
	}
	Tcl_DStringFree(&cmd);
    }
    /*
     * We delete when the trace was destroyed or if this is a delete trace,
     * because command deletes are unconditional, so the trace must go away.
     */
    if (flags & (TCL_TRACE_DESTROYED | TCL_TRACE_DELETE)) {
	int untraceFlags = tcmdPtr->flags;

	if (tcmdPtr->stepTrace != NULL) {
	    Tcl_DeleteTrace(interp, tcmdPtr->stepTrace);
	    tcmdPtr->stepTrace = NULL;
            if (tcmdPtr->startCmd != NULL) {
	        ckfree((char *)tcmdPtr->startCmd);
	    }
	}
	if (tcmdPtr->flags & TCL_TRACE_EXEC_IN_PROGRESS) {
	    /* Postpone deletion, until exec trace returns */
	    tcmdPtr->flags = 0;
	}
	/*
	 * We need to construct the same flags for Tcl_UntraceCommand
	 * as were passed to Tcl_TraceCommand.  Reproduce the processing
	 * of [trace add execution/command].  Be careful to keep this
	 * code in sync with that.
	 */
	if (untraceFlags & TCL_TRACE_ANY_EXEC) {
	    untraceFlags |= TCL_TRACE_DELETE;
	    if (untraceFlags & (TCL_TRACE_ENTER_DURING_EXEC 
		    | TCL_TRACE_LEAVE_DURING_EXEC)) {
		untraceFlags |= (TCL_TRACE_ENTER_EXEC | TCL_TRACE_LEAVE_EXEC);
	    }
	} else if (untraceFlags & TCL_TRACE_RENAME) {
	    untraceFlags |= TCL_TRACE_DELETE;
	}
	/*
	 * Remove the trace since TCL_TRACE_DESTROYED tells us to, or the
	 * command we're tracing has just gone away.  Then decrement the
	 * clientData refCount that was set up by trace creation.
	 */
	Tcl_UntraceCommand(interp, oldName, untraceFlags,
		TraceCommandProc, clientData);
	tcmdPtr->refCount--;
    }
    if ((--tcmdPtr->refCount) <= 0) {
        ckfree((char*)tcmdPtr);
    }
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCheckExecutionTraces --
 *
 *	Checks on all current command execution traces, and invokes
 *	procedures which have been registered.  This procedure can be
 *	used by other code which performs execution to unify the
 *	tracing system, so that execution traces will function for that
 *	other code.
 *	
 *	For instance extensions like [incr Tcl] which use their
 *	own execution technique can make use of Tcl's tracing.
 *	
 *	This procedure is called by 'TclEvalObjvInternal'
 *
 * Results:
 *      The return value is a standard Tcl completion code such as
 *      TCL_OK or TCL_ERROR, etc.
 *
 * Side effects:
 *	Those side effects made by any trace procedures called.
 *
 *----------------------------------------------------------------------
 */
int 
TclCheckExecutionTraces(interp, command, numChars, cmdPtr, code, 
			traceFlags, objc, objv)
    Tcl_Interp *interp;		/* The current interpreter. */
    CONST char *command;        /* Pointer to beginning of the current 
				 * command string. */
    int numChars;               /* The number of characters in 'command' 
				 * which are part of the command string. */
    Command *cmdPtr;		/* Points to command's Command struct. */
    int code;                   /* The current result code. */
    int traceFlags;             /* Current tracing situation. */
    int objc;			/* Number of arguments for the command. */
    Tcl_Obj *CONST objv[];	/* Pointers to Tcl_Obj of each argument. */
{
    Interp *iPtr = (Interp *) interp;
    CommandTrace *tracePtr, *lastTracePtr;
    ActiveCommandTrace active;
    int curLevel;
    int traceCode = TCL_OK;
    TraceCommandInfo* tcmdPtr;
    Tcl_InterpState state = NULL;
    
    if (command == NULL || cmdPtr->tracePtr == NULL) {
	return traceCode;
    }
    
    curLevel = ((iPtr->varFramePtr == NULL) ? 0 : iPtr->varFramePtr->level);
    
    active.nextPtr = iPtr->activeCmdTracePtr;
    iPtr->activeCmdTracePtr = &active;

    active.cmdPtr = cmdPtr;
    lastTracePtr = NULL;
    for (tracePtr = cmdPtr->tracePtr; 
	 (traceCode == TCL_OK) && (tracePtr != NULL);
	 tracePtr = active.nextTracePtr) {
        if (traceFlags & TCL_TRACE_LEAVE_EXEC) {
            /* execute the trace command in order of creation for "leave" */
	    active.nextTracePtr = NULL;
            tracePtr = cmdPtr->tracePtr;
            while (tracePtr->nextPtr != lastTracePtr) {
	        active.nextTracePtr = tracePtr;
	        tracePtr = tracePtr->nextPtr;
            }
        } else {
	    active.nextTracePtr = tracePtr->nextPtr;
        }
	tcmdPtr = (TraceCommandInfo*)tracePtr->clientData;
	if (tcmdPtr->flags != 0) {
            tcmdPtr->curFlags = traceFlags | TCL_TRACE_EXEC_DIRECT;
            tcmdPtr->curCode  = code;
	    tcmdPtr->refCount++;
	    if (state == NULL) {
		state = Tcl_SaveInterpState(interp, code);
	    }
	    traceCode = TraceExecutionProc((ClientData)tcmdPtr, interp, 
	          curLevel, command, (Tcl_Command)cmdPtr, objc, objv);
	    if ((--tcmdPtr->refCount) <= 0) {
	        ckfree((char*)tcmdPtr);
	    }
	}
        lastTracePtr = tracePtr;
    }
    iPtr->activeCmdTracePtr = active.nextPtr;
    if (state) {
	(void) Tcl_RestoreInterpState(interp, state);
    }
    return(traceCode);
}

/*
 *----------------------------------------------------------------------
 *
 * TclCheckInterpTraces --
 *
 *	Checks on all current traces, and invokes procedures which
 *	have been registered.  This procedure can be used by other
 *	code which performs execution to unify the tracing system.
 *	For instance extensions like [incr Tcl] which use their
 *	own execution technique can make use of Tcl's tracing.
 *	
 *	This procedure is called by 'TclEvalObjvInternal'
 *
 * Results:
 *      The return value is a standard Tcl completion code such as
 *      TCL_OK or TCL_ERROR, etc.
 *
 * Side effects:
 *	Those side effects made by any trace procedures called.
 *
 *----------------------------------------------------------------------
 */
int 
TclCheckInterpTraces(interp, command, numChars, cmdPtr, code, 
		     traceFlags, objc, objv)
    Tcl_Interp *interp;		/* The current interpreter. */
    CONST char *command;        /* Pointer to beginning of the current 
				 * command string. */
    int numChars;               /* The number of characters in 'command' 
				 * which are part of the command string. */
    Command *cmdPtr;		/* Points to command's Command struct. */
    int code;                   /* The current result code. */
    int traceFlags;             /* Current tracing situation. */
    int objc;			/* Number of arguments for the command. */
    Tcl_Obj *CONST objv[];	/* Pointers to Tcl_Obj of each argument. */
{
    Interp *iPtr = (Interp *) interp;
    Trace *tracePtr, *lastTracePtr;
    ActiveInterpTrace active;
    int curLevel;
    int traceCode = TCL_OK;
    TraceCommandInfo* tcmdPtr;
    Tcl_InterpState state = NULL;
    
    if (command == NULL || iPtr->tracePtr == NULL ||
           (iPtr->flags & INTERP_TRACE_IN_PROGRESS)) {
	return(traceCode);
    }
    
    curLevel = iPtr->numLevels;
    
    active.nextPtr = iPtr->activeInterpTracePtr;
    iPtr->activeInterpTracePtr = &active;

    lastTracePtr = NULL;
    for ( tracePtr = iPtr->tracePtr;
          (traceCode == TCL_OK) && (tracePtr != NULL);
	  tracePtr = active.nextTracePtr) {
        if (traceFlags & TCL_TRACE_ENTER_EXEC) {
            /* 
             * Execute the trace command in reverse order of creation
             * for "enterstep" operation. The order is changed for
             * "enterstep" instead of for "leavestep" as was done in 
             * TclCheckExecutionTraces because for step traces,
             * Tcl_CreateObjTrace creates one more linked list of traces
             * which results in one more reversal of trace invocation.
             */
	    active.nextTracePtr = NULL;
            tracePtr = iPtr->tracePtr;
            while (tracePtr->nextPtr != lastTracePtr) {
	        active.nextTracePtr = tracePtr;
	        tracePtr = tracePtr->nextPtr;
            }
        } else {
	    active.nextTracePtr = tracePtr->nextPtr;
        }
	if (tracePtr->level > 0 && curLevel > tracePtr->level) {
	    continue;
	}
	if (!(tracePtr->flags & TCL_TRACE_EXEC_IN_PROGRESS)) {
            /*
	     * The proc invoked might delete the traced command which 
	     * which might try to free tracePtr.  We want to use tracePtr
	     * until the end of this if section, so we use
	     * Tcl_Preserve() and Tcl_Release() to be sure it is not
	     * freed while we still need it.
	     */
	    Tcl_Preserve((ClientData) tracePtr);
	    tracePtr->flags |= TCL_TRACE_EXEC_IN_PROGRESS;
	    if (state == NULL) {
		state = Tcl_SaveInterpState(interp, code);
	    }
	    
	    if (tracePtr->flags & (TCL_TRACE_ENTER_EXEC | TCL_TRACE_LEAVE_EXEC)) {
	        /* New style trace */
		if ((tracePtr->flags != TCL_TRACE_EXEC_IN_PROGRESS) &&
		    ((tracePtr->flags & traceFlags) != 0)) {
		    tcmdPtr = (TraceCommandInfo*)tracePtr->clientData;
		    tcmdPtr->curFlags = traceFlags;
		    tcmdPtr->curCode  = code;
		    traceCode = (tracePtr->proc)((ClientData)tcmdPtr, 
						 (Tcl_Interp*)interp,
						 curLevel, command,
						 (Tcl_Command)cmdPtr,
						 objc, objv);
		}
	    } else {
		/* Old-style trace */
		
		if (traceFlags & TCL_TRACE_ENTER_EXEC) {
		    /* 
		     * Old-style interpreter-wide traces only trigger
		     * before the command is executed.
		     */
		    traceCode = CallTraceProcedure(interp, tracePtr, cmdPtr,
				       command, numChars, objc, objv);
		}
	    }
	    tracePtr->flags &= ~TCL_TRACE_EXEC_IN_PROGRESS;
	    Tcl_Release((ClientData) tracePtr);
	}
        lastTracePtr = tracePtr;
    }
    iPtr->activeInterpTracePtr = active.nextPtr;
    if (state) {
	if (traceCode == TCL_OK) {
	    (void) Tcl_RestoreInterpState(interp, state);
	} else {
	    Tcl_DiscardInterpState(state);
	}
    }
    return(traceCode);
}

/*
 *----------------------------------------------------------------------
 *
 * CallTraceProcedure --
 *
 *	Invokes a trace procedure registered with an interpreter. These
 *	procedures trace command execution. Currently this trace procedure
 *	is called with the address of the string-based Tcl_CmdProc for the
 *	command, not the Tcl_ObjCmdProc.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Those side effects made by the trace procedure.
 *
 *----------------------------------------------------------------------
 */

static int
CallTraceProcedure(interp, tracePtr, cmdPtr, command, numChars, objc, objv)
    Tcl_Interp *interp;		/* The current interpreter. */
    register Trace *tracePtr;	/* Describes the trace procedure to call. */
    Command *cmdPtr;		/* Points to command's Command struct. */
    CONST char *command;	/* Points to the first character of the
				 * command's source before substitutions. */
    int numChars;		/* The number of characters in the
				 * command's source. */
    register int objc;		/* Number of arguments for the command. */
    Tcl_Obj *CONST objv[];	/* Pointers to Tcl_Obj of each argument. */
{
    Interp *iPtr = (Interp *) interp;
    char *commandCopy;
    int traceCode;

   /*
     * Copy the command characters into a new string.
     */

    commandCopy = (char *) ckalloc((unsigned) (numChars + 1));
    memcpy((VOID *) commandCopy, (VOID *) command, (size_t) numChars);
    commandCopy[numChars] = '\0';
    
    /*
     * Call the trace procedure then free allocated storage.
     */
    
    traceCode = (tracePtr->proc)( tracePtr->clientData, (Tcl_Interp*) iPtr,
                              iPtr->numLevels, commandCopy,
                              (Tcl_Command) cmdPtr, objc, objv );

    ckfree((char *) commandCopy);
    return(traceCode);
}

/*
 *----------------------------------------------------------------------
 *
 * CommandObjTraceDeleted --
 *
 *	Ensure the trace is correctly deleted by decrementing its
 *	refCount and only deleting if no other references exist.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	May release memory.
 *
 *----------------------------------------------------------------------
 */
static void 
CommandObjTraceDeleted(ClientData clientData) {
    TraceCommandInfo* tcmdPtr = (TraceCommandInfo*)clientData;
    if ((--tcmdPtr->refCount) <= 0) {
	ckfree((char*)tcmdPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TraceExecutionProc --
 *
 *	This procedure is invoked whenever code relevant to a
 *	'trace execution' command is executed.  It is called in one
 *	of two ways in Tcl's core:
 *	
 *	(i) by the TclCheckExecutionTraces, when an execution trace 
 *	has been triggered.
 *	(ii) by TclCheckInterpTraces, when a prior execution trace has
 *	created a trace of the internals of a procedure, passing in
 *	this procedure as the one to be called.
 *
 * Results:
 *      The return value is a standard Tcl completion code such as
 *      TCL_OK or TCL_ERROR, etc.
 *
 * Side effects:
 *	May invoke an arbitrary Tcl procedure, and may create or
 *	delete an interpreter-wide trace.
 *
 *----------------------------------------------------------------------
 */
static int
TraceExecutionProc(ClientData clientData, Tcl_Interp *interp, 
	      int level, CONST char* command, Tcl_Command cmdInfo,
	      int objc, struct Tcl_Obj *CONST objv[]) {
    int call = 0;
    Interp *iPtr = (Interp *) interp;
    TraceCommandInfo* tcmdPtr = (TraceCommandInfo*)clientData;
    int flags = tcmdPtr->curFlags;
    int code  = tcmdPtr->curCode;
    int traceCode  = TCL_OK;
    
    if (tcmdPtr->flags & TCL_TRACE_EXEC_IN_PROGRESS) {
	/* 
	 * Inside any kind of execution trace callback, we do
	 * not allow any further execution trace callbacks to
	 * be called for the same trace.
	 */
	return traceCode;
    }
    
    if (!(flags & TCL_INTERP_DESTROYED) && !Tcl_LimitExceeded(interp)) {
	/*
	 * Check whether the current call is going to eval arbitrary
	 * Tcl code with a generated trace, or whether we are only
	 * going to setup interpreter-wide traces to implement the
	 * 'step' traces.  This latter situation can happen if
	 * we create a command trace without either before or after
	 * operations, but with either of the step operations.
	 */
	if (flags & TCL_TRACE_EXEC_DIRECT) {
	    call = flags & tcmdPtr->flags & (TCL_TRACE_ENTER_EXEC | 
					     TCL_TRACE_LEAVE_EXEC);
	} else {
	    call = 1;
	}
	/*
	 * First, if we have returned back to the level at which we
	 * created an interpreter trace for enterstep and/or leavestep
         * execution traces, we remove it here.
	 */
	if (flags & TCL_TRACE_LEAVE_EXEC) {
	    if ((tcmdPtr->stepTrace != NULL) && (level == tcmdPtr->startLevel)
                && (strcmp(command, tcmdPtr->startCmd) == 0)) {
		Tcl_DeleteTrace(interp, tcmdPtr->stepTrace);
		tcmdPtr->stepTrace = NULL;
                if (tcmdPtr->startCmd != NULL) {
	            ckfree((char *)tcmdPtr->startCmd);
	        }
	    }
	}
	
	/*
	 * Second, create the tcl callback, if required.
	 */
	if (call) {
	    Tcl_DString cmd;
	    Tcl_DString sub;
	    int i;

	    Tcl_DStringInit(&cmd);
	    Tcl_DStringAppend(&cmd, tcmdPtr->command, (int)tcmdPtr->length);
	    /* Append command with arguments */
	    Tcl_DStringInit(&sub);
	    for (i = 0; i < objc; i++) {
	        char* str;
	        int len;
	        str = Tcl_GetStringFromObj(objv[i],&len);
	        Tcl_DStringAppendElement(&sub, str);
	    }
	    Tcl_DStringAppendElement(&cmd, Tcl_DStringValue(&sub));
	    Tcl_DStringFree(&sub);

	    if (flags & TCL_TRACE_ENTER_EXEC) {
		/* Append trace operation */
		if (flags & TCL_TRACE_EXEC_DIRECT) {
		    Tcl_DStringAppendElement(&cmd, "enter");
		} else {
		    Tcl_DStringAppendElement(&cmd, "enterstep");
		}
	    } else if (flags & TCL_TRACE_LEAVE_EXEC) {
		Tcl_Obj* resultCode;
		char* resultCodeStr;

		/* Append result code */
		resultCode = Tcl_NewIntObj(code);
		resultCodeStr = Tcl_GetString(resultCode);
		Tcl_DStringAppendElement(&cmd, resultCodeStr);
		Tcl_DecrRefCount(resultCode);
		
		/* Append result string */
		Tcl_DStringAppendElement(&cmd, Tcl_GetStringResult(interp));
		/* Append trace operation */
		if (flags & TCL_TRACE_EXEC_DIRECT) {
		    Tcl_DStringAppendElement(&cmd, "leave");
		} else {
		    Tcl_DStringAppendElement(&cmd, "leavestep");
		}
	    } else {
		Tcl_Panic("TraceExecutionProc: bad flag combination");
	    }
	    
	    /*
	     * Execute the command.  
	     * We discard any object result the command returns.
	     */

	    tcmdPtr->flags |= TCL_TRACE_EXEC_IN_PROGRESS;
	    iPtr->flags    |= INTERP_TRACE_IN_PROGRESS;
	    tcmdPtr->refCount++;
	    /* 
	     * This line can have quite arbitrary side-effects,
	     * including deleting the trace, the command being
	     * traced, or even the interpreter.
	     */
	    traceCode = Tcl_Eval(interp, Tcl_DStringValue(&cmd));
	    tcmdPtr->flags &= ~TCL_TRACE_EXEC_IN_PROGRESS;
	    iPtr->flags    &= ~INTERP_TRACE_IN_PROGRESS;
	    if (tcmdPtr->flags == 0) {
		flags |= TCL_TRACE_DESTROYED;
	    }
	    Tcl_DStringFree(&cmd);
	}
	
	/*
	 * Third, if there are any step execution traces for this proc,
         * we register an interpreter trace to invoke enterstep and/or
	 * leavestep traces.
	 * We also need to save the current stack level and the proc
         * string in startLevel and startCmd so that we can delete this
         * interpreter trace when it reaches the end of this proc.
	 */
	if ((flags & TCL_TRACE_ENTER_EXEC) && (tcmdPtr->stepTrace == NULL)
	    && (tcmdPtr->flags & (TCL_TRACE_ENTER_DURING_EXEC | 
				  TCL_TRACE_LEAVE_DURING_EXEC))) {
		tcmdPtr->startLevel = level;
		tcmdPtr->startCmd = 
		    (char *) ckalloc((unsigned) (strlen(command) + 1));
		strcpy(tcmdPtr->startCmd, command);
		tcmdPtr->refCount++;
		tcmdPtr->stepTrace = Tcl_CreateObjTrace(interp, 0,
		   (tcmdPtr->flags & TCL_TRACE_ANY_EXEC) >> 2, 
		   TraceExecutionProc, (ClientData)tcmdPtr, 
		   CommandObjTraceDeleted);
	}
    }
    if (flags & TCL_TRACE_DESTROYED) {
	if (tcmdPtr->stepTrace != NULL) {
	    Tcl_DeleteTrace(interp, tcmdPtr->stepTrace);
	    tcmdPtr->stepTrace = NULL;
            if (tcmdPtr->startCmd != NULL) {
	        ckfree((char *)tcmdPtr->startCmd);
	    }
	}
    }
    if (call) {
	if ((--tcmdPtr->refCount) <= 0) {
	    ckfree((char*)tcmdPtr);
	}
    }
    return traceCode;
}

/*
 *----------------------------------------------------------------------
 *
 * TraceVarProc --
 *
 *	This procedure is called to handle variable accesses that have
 *	been traced using the "trace" command.
 *
 * Results:
 *	Normally returns NULL.  If the trace command returns an error,
 *	then this procedure returns an error string.
 *
 * Side effects:
 *	Depends on the command associated with the trace.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static char *
TraceVarProc(clientData, interp, name1, name2, flags)
    ClientData clientData;	/* Information about the variable trace. */
    Tcl_Interp *interp;		/* Interpreter containing variable. */
    CONST char *name1;		/* Name of variable or array. */
    CONST char *name2;		/* Name of element within array;  NULL means
				 * scalar variable is being referenced. */
    int flags;			/* OR-ed bits giving operation and other
				 * information. */
{
    TraceVarInfo *tvarPtr = (TraceVarInfo *) clientData;
    char *result;
    int code;
    Tcl_DString cmd;

    /* 
     * We might call Tcl_Eval() below, and that might evaluate
     * [trace vdelete] which might try to free tvarPtr.  We want
     * to use tvarPtr until the end of this function, so we use
     * Tcl_Preserve() and Tcl_Release() to be sure it is not 
     * freed while we still need it.
     */

    Tcl_Preserve((ClientData) tvarPtr);

    result = NULL;
    if ((tvarPtr->flags & flags) && !(flags & TCL_INTERP_DESTROYED)
	    && !Tcl_LimitExceeded(interp)) {
	if (tvarPtr->length != (size_t) 0) {
	    /*
	     * Generate a command to execute by appending list elements
	     * for the two variable names and the operation. 
	     */

	    Tcl_DStringInit(&cmd);
	    Tcl_DStringAppend(&cmd, tvarPtr->command, (int) tvarPtr->length);
	    Tcl_DStringAppendElement(&cmd, name1);
	    Tcl_DStringAppendElement(&cmd, (name2 ? name2 : ""));
#ifndef TCL_REMOVE_OBSOLETE_TRACES
	    if (tvarPtr->flags & TCL_TRACE_OLD_STYLE) {
		if (flags & TCL_TRACE_ARRAY) {
		    Tcl_DStringAppend(&cmd, " a", 2);
		} else if (flags & TCL_TRACE_READS) {
		    Tcl_DStringAppend(&cmd, " r", 2);
		} else if (flags & TCL_TRACE_WRITES) {
		    Tcl_DStringAppend(&cmd, " w", 2);
		} else if (flags & TCL_TRACE_UNSETS) {
		    Tcl_DStringAppend(&cmd, " u", 2);
		}
	    } else {
#endif
		if (flags & TCL_TRACE_ARRAY) {
		    Tcl_DStringAppend(&cmd, " array", 6);
		} else if (flags & TCL_TRACE_READS) {
		    Tcl_DStringAppend(&cmd, " read", 5);
		} else if (flags & TCL_TRACE_WRITES) {
		    Tcl_DStringAppend(&cmd, " write", 6);
		} else if (flags & TCL_TRACE_UNSETS) {
		    Tcl_DStringAppend(&cmd, " unset", 6);
		}
#ifndef TCL_REMOVE_OBSOLETE_TRACES
	    }
#endif
	    
	    /*
	     * Execute the command.  
	     * We discard any object result the command returns.
	     *
	     * Add the TCL_TRACE_DESTROYED flag to tvarPtr to indicate to
	     * other areas that this will be destroyed by us, otherwise a
	     * double-free might occur depending on what the eval does.
	     */

	    if (flags & TCL_TRACE_DESTROYED) {
		tvarPtr->flags |= TCL_TRACE_DESTROYED;
	    }
	    code = Tcl_EvalEx(interp, Tcl_DStringValue(&cmd),
		    Tcl_DStringLength(&cmd), 0);
	    if (code != TCL_OK) {	     /* copy error msg to result */
		Tcl_Obj *errMsgObj = Tcl_GetObjResult(interp);
		Tcl_IncrRefCount(errMsgObj);
		result = (char *) errMsgObj;
	    }
	    Tcl_DStringFree(&cmd);
	}
    }
    if (flags & TCL_TRACE_DESTROYED) {
	if (result != NULL) {
	    register Tcl_Obj *errMsgObj = (Tcl_Obj *) result;

	    Tcl_DecrRefCount(errMsgObj);
	    result = NULL;
	}
	Tcl_EventuallyFree((ClientData) tvarPtr, TCL_DYNAMIC);
    }
    Tcl_Release((ClientData) tvarPtr);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateObjTrace --
 *
 *	Arrange for a procedure to be called to trace command execution.
 *
 * Results:
 *	The return value is a token for the trace, which may be passed
 *	to Tcl_DeleteTrace to eliminate the trace.
 *
 * Side effects:
 *	From now on, proc will be called just before a command procedure
 *	is called to execute a Tcl command.  Calls to proc will have the
 *	following form:
 *
 *      void proc( ClientData     clientData,
 *                 Tcl_Interp*    interp,
 *                 int            level,
 *                 CONST char*    command,
 *                 Tcl_Command    commandInfo,
 *                 int            objc,
 *                 Tcl_Obj *CONST objv[] );
 *
 *      The 'clientData' and 'interp' arguments to 'proc' will be the
 *      same as the arguments to Tcl_CreateObjTrace.  The 'level'
 *	argument gives the nesting depth of command interpretation within
 *	the interpreter.  The 'command' argument is the ASCII text of
 *	the command being evaluated -- before any substitutions are
 *	performed.  The 'commandInfo' argument gives a handle to the
 *	command procedure that will be evaluated.  The 'objc' and 'objv'
 *	parameters give the parameter vector that will be passed to the
 *	command procedure.  proc does not return a value.
 *
 *      It is permissible for 'proc' to call Tcl_SetCommandTokenInfo
 *      to change the command procedure or client data for the command
 *      being evaluated, and these changes will take effect with the
 *      current evaluation.
 *
 * The 'level' argument specifies the maximum nesting level of calls
 * to be traced.  If the execution depth of the interpreter exceeds
 * 'level', the trace callback is not executed.
 *
 * The 'flags' argument is either zero or the value,
 * TCL_ALLOW_INLINE_COMPILATION.  If the TCL_ALLOW_INLINE_COMPILATION
 * flag is not present, the bytecode compiler will not generate inline
 * code for Tcl's built-in commands.  This behavior will have a significant
 * impact on performance, but will ensure that all command evaluations are
 * traced.  If the TCL_ALLOW_INLINE_COMPILATION flag is present, the
 * bytecode compiler will have its normal behavior of compiling in-line
 * code for some of Tcl's built-in commands.  In this case, the tracing
 * will be imprecise -- in-line code will not be traced -- but run-time
 * performance will be improved.  The latter behavior is desired for
 * many applications such as profiling of run time.
 *
 * When the trace is deleted, the 'delProc' procedure will be invoked,
 * passing it the original client data.  
 *
 *----------------------------------------------------------------------
 */

Tcl_Trace
Tcl_CreateObjTrace( interp, level, flags, proc, clientData, delProc )
    Tcl_Interp* interp;		/* Tcl interpreter */
    int level;			/* Maximum nesting level */
    int flags;			/* Flags, see above */
    Tcl_CmdObjTraceProc* proc;	/* Trace callback */
    ClientData clientData;	/* Client data for the callback */
    Tcl_CmdObjTraceDeleteProc* delProc;
				/* Procedure to call when trace is deleted */
{
    register Trace *tracePtr;
    register Interp *iPtr = (Interp *) interp;

    /* Test if this trace allows inline compilation of commands */

    if (!(flags & TCL_ALLOW_INLINE_COMPILATION)) {
	if (iPtr->tracesForbiddingInline == 0) {

	    /*
	     * When the first trace forbidding inline compilation is
	     * created, invalidate existing compiled code for this
	     * interpreter and arrange (by setting the
	     * DONT_COMPILE_CMDS_INLINE flag) that when compiling new
	     * code, no commands will be compiled inline (i.e., into
	     * an inline sequence of instructions). We do this because
	     * commands that were compiled inline will never result in
	     * a command trace being called.
	     */

	    iPtr->compileEpoch++;
	    iPtr->flags |= DONT_COMPILE_CMDS_INLINE;
	}
	iPtr->tracesForbiddingInline++;
    }
    
    tracePtr = (Trace *) ckalloc(sizeof(Trace));
    tracePtr->level		= level;
    tracePtr->proc		= proc;
    tracePtr->clientData	= clientData;
    tracePtr->delProc           = delProc;
    tracePtr->nextPtr		= iPtr->tracePtr;
    tracePtr->flags		= flags;
    iPtr->tracePtr		= tracePtr;

    return (Tcl_Trace) tracePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateTrace --
 *
 *	Arrange for a procedure to be called to trace command execution.
 *
 * Results:
 *	The return value is a token for the trace, which may be passed
 *	to Tcl_DeleteTrace to eliminate the trace.
 *
 * Side effects:
 *	From now on, proc will be called just before a command procedure
 *	is called to execute a Tcl command.  Calls to proc will have the
 *	following form:
 *
 *	void
 *	proc(clientData, interp, level, command, cmdProc, cmdClientData,
 *		argc, argv)
 *	    ClientData clientData;
 *	    Tcl_Interp *interp;
 *	    int level;
 *	    char *command;
 *	    int (*cmdProc)();
 *	    ClientData cmdClientData;
 *	    int argc;
 *	    char **argv;
 *	{
 *	}
 *
 *	The clientData and interp arguments to proc will be the same
 *	as the corresponding arguments to this procedure.  Level gives
 *	the nesting level of command interpretation for this interpreter
 *	(0 corresponds to top level).  Command gives the ASCII text of
 *	the raw command, cmdProc and cmdClientData give the procedure that
 *	will be called to process the command and the ClientData value it
 *	will receive, and argc and argv give the arguments to the
 *	command, after any argument parsing and substitution.  Proc
 *	does not return a value.
 *
 *----------------------------------------------------------------------
 */

Tcl_Trace
Tcl_CreateTrace(interp, level, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter in which to create trace. */
    int level;			/* Only call proc for commands at nesting
				 * level<=argument level (1=>top level). */
    Tcl_CmdTraceProc *proc;	/* Procedure to call before executing each
				 * command. */
    ClientData clientData;	/* Arbitrary value word to pass to proc. */
{
    StringTraceData* data;
    data = (StringTraceData*) ckalloc( sizeof( *data ));
    data->clientData = clientData;
    data->proc = proc;
    return Tcl_CreateObjTrace( interp, level, 0, StringTraceProc,
			       (ClientData) data, StringTraceDeleteProc );
}

/*
 *----------------------------------------------------------------------
 *
 * StringTraceProc --
 *
 *	Invoke a string-based trace procedure from an object-based
 *	callback.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Whatever the string-based trace procedure does.
 *
 *----------------------------------------------------------------------
 */

static int
StringTraceProc( clientData, interp, level, command, commandInfo, objc, objv )
    ClientData clientData;
    Tcl_Interp* interp;
    int level;
    CONST char* command;
    Tcl_Command commandInfo;
    int objc;
    Tcl_Obj *CONST *objv;
{
    StringTraceData* data = (StringTraceData*) clientData;
    Command* cmdPtr = (Command*) commandInfo;

    CONST char** argv;		/* Args to pass to string trace proc */

    int i;

    /*
     * This is a bit messy because we have to emulate the old trace
     * interface, which uses strings for everything.
     */
	    
    argv = (CONST char **) ckalloc((unsigned) ( (objc + 1)
						* sizeof(CONST char *) ));
    for (i = 0; i < objc; i++) {
	argv[i] = Tcl_GetString(objv[i]);
    }
    argv[objc] = 0;

    /*
     * Invoke the command procedure.  Note that we cast away const-ness
     * on two parameters for compatibility with legacy code; the code
     * MUST NOT modify either command or argv.
     */
          
    ( data->proc )( data->clientData, interp, level,
		    (char*) command, cmdPtr->proc, cmdPtr->clientData,
		    objc, argv );
    ckfree( (char*) argv );

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * StringTraceDeleteProc --
 *
 *	Clean up memory when a string-based trace is deleted.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Allocated memory is returned to the system.
 *
 *----------------------------------------------------------------------
 */

static void
StringTraceDeleteProc( clientData )
    ClientData clientData;
{
    ckfree( (char*) clientData );
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteTrace --
 *
 *	Remove a trace.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	From now on there will be no more calls to the procedure given
 *	in trace.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DeleteTrace(interp, trace)
    Tcl_Interp *interp;		/* Interpreter that contains trace. */
    Tcl_Trace trace;		/* Token for trace (returned previously by
				 * Tcl_CreateTrace). */
{
    Interp *iPtr = (Interp *) interp;
    Trace *tracePtr = (Trace *) trace;
    register Trace **tracePtr2 = &(iPtr->tracePtr);

    /*
     * Locate the trace entry in the interpreter's trace list,
     * and remove it from the list.
     */

    while ((*tracePtr2) != NULL && (*tracePtr2) != tracePtr) {
	tracePtr2 = &((*tracePtr2)->nextPtr);
    }
    if (*tracePtr2 == NULL) {
	return;
    }
    (*tracePtr2) = (*tracePtr2)->nextPtr;

    /*
     * If the trace forbids bytecode compilation, change the interpreter's
     * state.  If bytecode compilation is now permitted, flag the fact and
     * advance the compilation epoch so that procs will be recompiled to
     * take advantage of it.
     */

    if (!(tracePtr->flags & TCL_ALLOW_INLINE_COMPILATION)) {
	iPtr->tracesForbiddingInline--;
	if (iPtr->tracesForbiddingInline == 0) {
	    iPtr->flags &= ~DONT_COMPILE_CMDS_INLINE;
	    iPtr->compileEpoch++;
	}
    }

    /*
     * Execute any delete callback.
     */

    if (tracePtr->delProc != NULL) {
	(tracePtr->delProc)(tracePtr->clientData);
    }

    /* Delete the trace object */

    Tcl_EventuallyFree((char*)tracePtr, TCL_DYNAMIC);
}

/*
 *----------------------------------------------------------------------
 *
 * TclTraceVarExists --
 *
 *	This is called from info exists.  We need to trigger read
 *	and/or array traces because they may end up creating a
 *	variable that doesn't currently exist.
 *
 * Results:
 *	A pointer to the Var structure, or NULL.
 *
 * Side effects:
 *	May fill in error messages in the interp.
 *
 *----------------------------------------------------------------------
 */

Var *
TclVarTraceExists(interp, varName)
    Tcl_Interp *interp;		/* The interpreter */
    CONST char *varName;	/* The variable name */
{
    Var *varPtr;
    Var *arrayPtr;

    /*
     * The choice of "create" flag values is delicate here, and
     * matches the semantics of GetVar.  Things are still not perfect,
     * however, because if you do "info exists x" you get a varPtr
     * and therefore trigger traces.  However, if you do 
     * "info exists x(i)", then you only get a varPtr if x is already
     * known to be an array.  Otherwise you get NULL, and no trace
     * is triggered.  This matches Tcl 7.6 semantics.
     */

    varPtr = TclLookupVar(interp, varName, (char *) NULL,
            0, "access", /*createPart1*/ 0, /*createPart2*/ 1, &arrayPtr);

    if (varPtr == NULL) {
	return NULL;
    }

    if ((varPtr->tracePtr != NULL)
	    || ((arrayPtr != NULL) && (arrayPtr->tracePtr != NULL))) {
	TclCallVarTraces((Interp *)interp, arrayPtr, varPtr, varName, NULL,
		TCL_TRACE_READS, /* leaveErrMsg */ 0);
    }

    /*
     * If the variable doesn't exist anymore and no-one's using
     * it, then free up the relevant structures and hash table entries.
     */

    if (TclIsVarUndefined(varPtr)) {
	TclCleanupVar(varPtr, arrayPtr);
	return NULL;
    }

    return varPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCallVarTraces --
 *
 *	This procedure is invoked to find and invoke relevant
 *	trace procedures associated with a particular operation on
 *	a variable. This procedure invokes traces both on the
 *	variable and on its containing array (where relevant).
 *
 * Results:
 *      Returns TCL_OK to indicate normal operation.  Returns TCL_ERROR
 *      if invocation of a trace procedure indicated an error.  When
 *      TCL_ERROR is returned and leaveErrMsg is true, then the
 *      errorInfo field of iPtr has information about the error
 *      placed in it.
 *
 * Side effects:
 *	Almost anything can happen, depending on trace; this procedure
 *	itself doesn't have any side effects.
 *
 *----------------------------------------------------------------------
 */

int
TclCallVarTraces(iPtr, arrayPtr, varPtr, part1, part2, flags, leaveErrMsg)
    Interp *iPtr;		/* Interpreter containing variable. */
    register Var *arrayPtr;	/* Pointer to array variable that contains
				 * the variable, or NULL if the variable
				 * isn't an element of an array. */
    Var *varPtr;		/* Variable whose traces are to be
				 * invoked. */
    CONST char *part1;
    CONST char *part2;		/* Variable's two-part name. */
    int flags;			/* Flags passed to trace procedures:
				 * indicates what's happening to variable,
				 * plus other stuff like TCL_GLOBAL_ONLY,
				 * TCL_NAMESPACE_ONLY, and
				 * TCL_INTERP_DESTROYED. */
    int leaveErrMsg;	        /* If true, and one of the traces indicates an
				 * error, then leave an error message and stack
				 * trace information in *iPTr. */
{
    register VarTrace *tracePtr;
    ActiveVarTrace active;
    char *result;
    CONST char *openParen, *p;
    Tcl_DString nameCopy;
    int copiedName;
    int code = TCL_OK;
    int disposeFlags = 0;
    Tcl_InterpState state = NULL;

    /*
     * If there are already similar trace procedures active for the
     * variable, don't call them again.
     */

    if (TclIsVarTraceActive(varPtr)) {
	return code;
    }
    TclSetVarTraceActive(varPtr);
    varPtr->refCount++;
    if (arrayPtr != NULL) {
	arrayPtr->refCount++;
    }

    /*
     * If the variable name hasn't been parsed into array name and
     * element, do it here.  If there really is an array element,
     * make a copy of the original name so that NULLs can be
     * inserted into it to separate the names (can't modify the name
     * string in place, because the string might get used by the
     * callbacks we invoke).
     */

    copiedName = 0;
    if (part2 == NULL) {
	for (p = part1; *p ; p++) {
	    if (*p == '(') {
		openParen = p;
		do {
		    p++;
		} while (*p != '\0');
		p--;
		if (*p == ')') {
		    int offset = (openParen - part1);
		    char *newPart1;
		    Tcl_DStringInit(&nameCopy);
		    Tcl_DStringAppend(&nameCopy, part1, (p-part1));
		    newPart1 = Tcl_DStringValue(&nameCopy);
		    newPart1[offset] = 0;
		    part1 = newPart1;
		    part2 = newPart1 + offset + 1;
		    copiedName = 1;
		}
		break;
	    }
	}
    }

    /*
     * Invoke traces on the array containing the variable, if relevant.
     */

    result = NULL;
    active.nextPtr = iPtr->activeVarTracePtr;
    iPtr->activeVarTracePtr = &active;
    Tcl_Preserve((ClientData) iPtr);
    if (arrayPtr != NULL && !TclIsVarTraceActive(arrayPtr)) {
	active.varPtr = arrayPtr;
	for (tracePtr = arrayPtr->tracePtr;  tracePtr != NULL;
	     tracePtr = active.nextTracePtr) {
	    active.nextTracePtr = tracePtr->nextPtr;
	    if (!(tracePtr->flags & flags)) {
		continue;
	    }
	    Tcl_Preserve((ClientData) tracePtr);
	    if (state == NULL) {
		state = Tcl_SaveInterpState((Tcl_Interp *)iPtr, code);
	    }
	    result = (*tracePtr->traceProc)(tracePtr->clientData,
		    (Tcl_Interp *) iPtr, part1, part2, flags);
	    if (result != NULL) {
		if (flags & TCL_TRACE_UNSETS) {
		    /* Ignore errors in unset traces */
		    DisposeTraceResult(tracePtr->flags, result);
		} else {
	            disposeFlags = tracePtr->flags;
		    code = TCL_ERROR;
		}
	    }
	    Tcl_Release((ClientData) tracePtr);
	    if (code == TCL_ERROR) {
		goto done;
	    }
	}
    }

    /*
     * Invoke traces on the variable itself.
     */

    if (flags & TCL_TRACE_UNSETS) {
	flags |= TCL_TRACE_DESTROYED;
    }
    active.varPtr = varPtr;
    for (tracePtr = varPtr->tracePtr; tracePtr != NULL;
	 tracePtr = active.nextTracePtr) {
	active.nextTracePtr = tracePtr->nextPtr;
	if (!(tracePtr->flags & flags)) {
	    continue;
	}
	Tcl_Preserve((ClientData) tracePtr);
	if (state == NULL) {
	    state = Tcl_SaveInterpState((Tcl_Interp *)iPtr, code);
	}
	result = (*tracePtr->traceProc)(tracePtr->clientData,
		(Tcl_Interp *) iPtr, part1, part2, flags);
	if (result != NULL) {
	    if (flags & TCL_TRACE_UNSETS) {
		/* Ignore errors in unset traces */
		DisposeTraceResult(tracePtr->flags, result);
	    } else {
		disposeFlags = tracePtr->flags;
		code = TCL_ERROR;
	    }
	}
	Tcl_Release((ClientData) tracePtr);
	if (code == TCL_ERROR) {
	    goto done;
	}
    }

    /*
     * Restore the variable's flags, remove the record of our active
     * traces, and then return.
     */

    done:
    if (code == TCL_ERROR) {
	if (leaveErrMsg) {
	    CONST char *type = "";
	    Tcl_Obj *options = Tcl_GetReturnOptions((Tcl_Interp *)iPtr, code);
	    Tcl_Obj *errorInfoKey = Tcl_NewStringObj("-errorinfo", -1);
	    Tcl_Obj *errorInfo;

	    Tcl_IncrRefCount(errorInfoKey);
	    Tcl_DictObjGet(NULL, options, errorInfoKey, &errorInfo);
	    Tcl_IncrRefCount(errorInfo);
	    Tcl_DictObjRemove(NULL, options, errorInfoKey);
	    if (Tcl_IsShared(errorInfo)) {
		Tcl_DecrRefCount(errorInfo);
		errorInfo = Tcl_DuplicateObj(errorInfo);
		Tcl_IncrRefCount(errorInfo);
	    }
	    Tcl_AppendToObj(errorInfo, "\n    (", -1);
	    switch (flags&(TCL_TRACE_READS|TCL_TRACE_WRITES|TCL_TRACE_ARRAY)) {
		case TCL_TRACE_READS:
		    type = "read";
		    Tcl_AppendToObj(errorInfo, type, -1);
		    break;
		case TCL_TRACE_WRITES:
		    type = "set";
		    Tcl_AppendToObj(errorInfo, "write", -1);
		    break;
		case TCL_TRACE_ARRAY:
		    type = "trace array";
		    Tcl_AppendToObj(errorInfo, "array", -1);
		    break;
	    }
	    if (disposeFlags & TCL_TRACE_RESULT_OBJECT) {
		TclVarErrMsg((Tcl_Interp *) iPtr, part1, part2, type,
			Tcl_GetString((Tcl_Obj *) result));
	    } else {
		TclVarErrMsg((Tcl_Interp *) iPtr, part1, part2, type, result);
	    }
	    Tcl_AppendToObj(errorInfo, " trace on \"", -1);
	    Tcl_AppendToObj(errorInfo, part1, -1);
	    if (part2 != NULL) {
		Tcl_AppendToObj(errorInfo, "(", -1);
		Tcl_AppendToObj(errorInfo, part1, -1);
		Tcl_AppendToObj(errorInfo, ")", -1);
	    }
	    Tcl_AppendToObj(errorInfo, "\")", -1);
	    Tcl_DictObjPut(NULL, options, errorInfoKey, errorInfo);
	    Tcl_DecrRefCount(errorInfoKey);
	    Tcl_DecrRefCount(errorInfo);
	    code = Tcl_SetReturnOptions((Tcl_Interp *)iPtr, options);
	    iPtr->flags &= ~(ERR_ALREADY_LOGGED);
	    Tcl_DiscardInterpState(state);
	} else {
	    (void) Tcl_RestoreInterpState((Tcl_Interp *)iPtr, state);
	}
	DisposeTraceResult(disposeFlags,result);
    } else if (state) {
	if (code == TCL_OK) {
	    code = Tcl_RestoreInterpState((Tcl_Interp *)iPtr, state);
	} else {
	    Tcl_DiscardInterpState(state);
	}
    }

    if (arrayPtr != NULL) {
	arrayPtr->refCount--;
    }
    if (copiedName) {
	Tcl_DStringFree(&nameCopy);
    }
    TclClearVarTraceActive(varPtr);
    varPtr->refCount--;
    iPtr->activeVarTracePtr = active.nextPtr;
    Tcl_Release((ClientData) iPtr);
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * DisposeTraceResult--
 *
 *	This procedure is called to dispose of the result returned from
 *	a trace procedure.  The disposal method appropriate to the type
 *	of result is determined by flags.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The memory allocated for the trace result may be freed.
 *
 *----------------------------------------------------------------------
 */

static void
DisposeTraceResult(flags, result)
    int flags;			/* Indicates type of result to determine
				 * proper disposal method */
    char *result;		/* The result returned from a trace
				 * procedure to be disposed */
{
    if (flags & TCL_TRACE_RESULT_DYNAMIC) {
	ckfree(result);
    } else if (flags & TCL_TRACE_RESULT_OBJECT) {
	Tcl_DecrRefCount((Tcl_Obj *) result);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UntraceVar --
 *
 *	Remove a previously-created trace for a variable.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If there exists a trace for the variable given by varName
 *	with the given flags, proc, and clientData, then that trace
 *	is removed.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_UntraceVar(interp, varName, flags, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter containing variable. */
    CONST char *varName;	/* Name of variable; may end with "(index)"
				 * to signify an array reference. */
    int flags;			/* OR-ed collection of bits describing
				 * current trace, including any of
				 * TCL_TRACE_READS, TCL_TRACE_WRITES,
				 * TCL_TRACE_UNSETS, TCL_GLOBAL_ONLY
				 * and TCL_NAMESPACE_ONLY. */
    Tcl_VarTraceProc *proc;	/* Procedure assocated with trace. */
    ClientData clientData;	/* Arbitrary argument to pass to proc. */
{
    Tcl_UntraceVar2(interp, varName, (char *) NULL, flags, proc, clientData);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UntraceVar2 --
 *
 *	Remove a previously-created trace for a variable.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If there exists a trace for the variable given by part1
 *	and part2 with the given flags, proc, and clientData, then
 *	that trace is removed.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_UntraceVar2(interp, part1, part2, flags, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter containing variable. */
    CONST char *part1;		/* Name of variable or array. */
    CONST char *part2;		/* Name of element within array;  NULL means
				 * trace applies to scalar variable or array
				 * as-a-whole. */
    int flags;			/* OR-ed collection of bits describing
				 * current trace, including any of
				 * TCL_TRACE_READS, TCL_TRACE_WRITES,
				 * TCL_TRACE_UNSETS, TCL_GLOBAL_ONLY,
				 * and TCL_NAMESPACE_ONLY. */
    Tcl_VarTraceProc *proc;	/* Procedure assocated with trace. */
    ClientData clientData;	/* Arbitrary argument to pass to proc. */
{
    register VarTrace *tracePtr;
    VarTrace *prevPtr;
    Var *varPtr, *arrayPtr;
    Interp *iPtr = (Interp *) interp;
    ActiveVarTrace *activePtr;
    int flagMask;
    
    /*
     * Set up a mask to mask out the parts of the flags that we are not
     * interested in now.
     */
    flagMask = TCL_GLOBAL_ONLY | TCL_NAMESPACE_ONLY;
    varPtr = TclLookupVar(interp, part1, part2, flags & flagMask,
	    /*msg*/ (char *) NULL,
	    /*createPart1*/ 0, /*createPart2*/ 0, &arrayPtr);
    if (varPtr == NULL) {
	return;
    }

    /*
     * Set up a mask to mask out the parts of the flags that we are not
     * interested in now.
     */
    flagMask = TCL_TRACE_READS | TCL_TRACE_WRITES | TCL_TRACE_UNSETS |
	TCL_TRACE_ARRAY | TCL_TRACE_RESULT_DYNAMIC | TCL_TRACE_RESULT_OBJECT; 
#ifndef TCL_REMOVE_OBSOLETE_TRACES
    flagMask |= TCL_TRACE_OLD_STYLE;
#endif
    flags &= flagMask;
    for (tracePtr = varPtr->tracePtr, prevPtr = NULL;  ;
	 prevPtr = tracePtr, tracePtr = tracePtr->nextPtr) {
	if (tracePtr == NULL) {
	    return;
	}
	if ((tracePtr->traceProc == proc) && (tracePtr->flags == flags)
		&& (tracePtr->clientData == clientData)) {
	    break;
	}
    }

    /*
     * The code below makes it possible to delete traces while traces
     * are active: it makes sure that the deleted trace won't be
     * processed by TclCallVarTraces.
     */

    for (activePtr = iPtr->activeVarTracePtr;  activePtr != NULL;
	 activePtr = activePtr->nextPtr) {
	if (activePtr->nextTracePtr == tracePtr) {
	    activePtr->nextTracePtr = tracePtr->nextPtr;
	}
    }
    if (prevPtr == NULL) {
	varPtr->tracePtr = tracePtr->nextPtr;
    } else {
	prevPtr->nextPtr = tracePtr->nextPtr;
    }
    Tcl_EventuallyFree((ClientData) tracePtr, TCL_DYNAMIC);

    /*
     * If this is the last trace on the variable, and the variable is
     * unset and unused, then free up the variable.
     */

    if (TclIsVarUndefined(varPtr)) {
	TclCleanupVar(varPtr, (Var *) NULL);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_VarTraceInfo --
 *
 *	Return the clientData value associated with a trace on a
 *	variable.  This procedure can also be used to step through
 *	all of the traces on a particular variable that have the
 *	same trace procedure.
 *
 * Results:
 *	The return value is the clientData value associated with
 *	a trace on the given variable.  Information will only be
 *	returned for a trace with proc as trace procedure.  If
 *	the clientData argument is NULL then the first such trace is
 *	returned;  otherwise, the next relevant one after the one
 *	given by clientData will be returned.  If the variable
 *	doesn't exist, or if there are no (more) traces for it,
 *	then NULL is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ClientData
Tcl_VarTraceInfo(interp, varName, flags, proc, prevClientData)
    Tcl_Interp *interp;		/* Interpreter containing variable. */
    CONST char *varName;	/* Name of variable;  may end with "(index)"
				 * to signify an array reference. */
    int flags;			/* OR-ed combo or TCL_GLOBAL_ONLY,
				 * TCL_NAMESPACE_ONLY (can be 0). */
    Tcl_VarTraceProc *proc;	/* Procedure assocated with trace. */
    ClientData prevClientData;	/* If non-NULL, gives last value returned
				 * by this procedure, so this call will
				 * return the next trace after that one.
				 * If NULL, this call will return the
				 * first trace. */
{
    return Tcl_VarTraceInfo2(interp, varName, (char *) NULL,
	    flags, proc, prevClientData);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_VarTraceInfo2 --
 *
 *	Same as Tcl_VarTraceInfo, except takes name in two pieces
 *	instead of one.
 *
 * Results:
 *	Same as Tcl_VarTraceInfo.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ClientData
Tcl_VarTraceInfo2(interp, part1, part2, flags, proc, prevClientData)
    Tcl_Interp *interp;		/* Interpreter containing variable. */
    CONST char *part1;		/* Name of variable or array. */
    CONST char *part2;		/* Name of element within array;  NULL means
				 * trace applies to scalar variable or array
				 * as-a-whole. */
    int flags;			/* OR-ed combination of TCL_GLOBAL_ONLY,
				 * TCL_NAMESPACE_ONLY. */
    Tcl_VarTraceProc *proc;	/* Procedure assocated with trace. */
    ClientData prevClientData;	/* If non-NULL, gives last value returned
				 * by this procedure, so this call will
				 * return the next trace after that one.
				 * If NULL, this call will return the
				 * first trace. */
{
    register VarTrace *tracePtr;
    Var *varPtr, *arrayPtr;

    varPtr = TclLookupVar(interp, part1, part2,
	    flags & (TCL_GLOBAL_ONLY|TCL_NAMESPACE_ONLY),
	    /*msg*/ (char *) NULL,
	    /*createPart1*/ 0, /*createPart2*/ 0, &arrayPtr);
    if (varPtr == NULL) {
	return NULL;
    }

    /*
     * Find the relevant trace, if any, and return its clientData.
     */

    tracePtr = varPtr->tracePtr;
    if (prevClientData != NULL) {
	for ( ;  tracePtr != NULL;  tracePtr = tracePtr->nextPtr) {
	    if ((tracePtr->clientData == prevClientData)
		    && (tracePtr->traceProc == proc)) {
		tracePtr = tracePtr->nextPtr;
		break;
	    }
	}
    }
    for ( ;  tracePtr != NULL;  tracePtr = tracePtr->nextPtr) {
	if (tracePtr->traceProc == proc) {
	    return tracePtr->clientData;
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_TraceVar --
 *
 *	Arrange for reads and/or writes to a variable to cause a
 *	procedure to be invoked, which can monitor the operations
 *	and/or change their actions.
 *
 * Results:
 *	A standard Tcl return value.
 *
 * Side effects:
 *	A trace is set up on the variable given by varName, such that
 *	future references to the variable will be intermediated by
 *	proc.  See the manual entry for complete details on the calling
 *	sequence for proc.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_TraceVar(interp, varName, flags, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter in which variable is
				 * to be traced. */
    CONST char *varName;	/* Name of variable;  may end with "(index)"
				 * to signify an array reference. */
    int flags;			/* OR-ed collection of bits, including any
				 * of TCL_TRACE_READS, TCL_TRACE_WRITES,
				 * TCL_TRACE_UNSETS, TCL_GLOBAL_ONLY, and
				 * TCL_NAMESPACE_ONLY. */
    Tcl_VarTraceProc *proc;	/* Procedure to call when specified ops are
				 * invoked upon varName. */
    ClientData clientData;	/* Arbitrary argument to pass to proc. */
{
    return Tcl_TraceVar2(interp, varName, (char *) NULL, 
	    flags, proc, clientData);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_TraceVar2 --
 *
 *	Arrange for reads and/or writes to a variable to cause a
 *	procedure to be invoked, which can monitor the operations
 *	and/or change their actions.
 *
 * Results:
 *	A standard Tcl return value.
 *
 * Side effects:
 *	A trace is set up on the variable given by part1 and part2, such
 *	that future references to the variable will be intermediated by
 *	proc.  See the manual entry for complete details on the calling
 *	sequence for proc.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_TraceVar2(interp, part1, part2, flags, proc, clientData)
    Tcl_Interp *interp;		/* Interpreter in which variable is
				 * to be traced. */
    CONST char *part1;		/* Name of scalar variable or array. */
    CONST char *part2;		/* Name of element within array;  NULL means
				 * trace applies to scalar variable or array
				 * as-a-whole. */
    int flags;			/* OR-ed collection of bits, including any
				 * of TCL_TRACE_READS, TCL_TRACE_WRITES,
				 * TCL_TRACE_UNSETS, TCL_GLOBAL_ONLY,
				 * and TCL_NAMESPACE_ONLY. */
    Tcl_VarTraceProc *proc;	/* Procedure to call when specified ops are
				 * invoked upon varName. */
    ClientData clientData;	/* Arbitrary argument to pass to proc. */
{
    Var *varPtr, *arrayPtr;
    register VarTrace *tracePtr;
    int flagMask;
    
    /* 
     * We strip 'flags' down to just the parts which are relevant to
     * TclLookupVar, to avoid conflicts between trace flags and
     * internal namespace flags such as 'TCL_FIND_ONLY_NS'.  This can
     * now occur since we have trace flags with values 0x1000 and higher.
     */
    flagMask = TCL_GLOBAL_ONLY | TCL_NAMESPACE_ONLY;
    varPtr = TclLookupVar(interp, part1, part2,
	    (flags & flagMask) | TCL_LEAVE_ERR_MSG,
	    "trace", /*createPart1*/ 1, /*createPart2*/ 1, &arrayPtr);
    if (varPtr == NULL) {
	return TCL_ERROR;
    }

    /*
     * Check for a nonsense flag combination.  Note that this is a
     * Tcl_Panic() because there should be no code path that ever sets
     * both flags.
     */
    if ((flags&TCL_TRACE_RESULT_DYNAMIC) && (flags&TCL_TRACE_RESULT_OBJECT)) {
	Tcl_Panic("bad result flag combination");
    }

    /*
     * Set up trace information.
     */

    flagMask = TCL_TRACE_READS | TCL_TRACE_WRITES | TCL_TRACE_UNSETS | 
	TCL_TRACE_ARRAY | TCL_TRACE_RESULT_DYNAMIC | TCL_TRACE_RESULT_OBJECT;
#ifndef TCL_REMOVE_OBSOLETE_TRACES
    flagMask |= TCL_TRACE_OLD_STYLE;
#endif
    tracePtr = (VarTrace *) ckalloc(sizeof(VarTrace));
    tracePtr->traceProc		= proc;
    tracePtr->clientData	= clientData;
    tracePtr->flags		= flags & flagMask;
    tracePtr->nextPtr		= varPtr->tracePtr;
    varPtr->tracePtr		= tracePtr;
    return TCL_OK;
}
