/* 
 * tclCompile.c --
 *
 *	This file contains procedures that compile Tcl commands or parts
 *	of commands (like quoted strings or nested sub-commands) into a
 *	sequence of instructions ("bytecodes"). 
 *
 * Copyright (c) 1996-1998 Sun Microsystems, Inc.
 * Copyright (c) 2001 by Kevin B. Kenny.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclCompile.c,v 1.78 2004/10/08 15:39:52 dkf Exp $
 */

#include "tclInt.h"
#include "tclCompile.h"

/*
 * Table of all AuxData types.
 */
 
static Tcl_HashTable auxDataTypeTable;
static int auxDataTypeTableInitialized; /* 0 means not yet initialized. */

TCL_DECLARE_MUTEX(tableMutex)

/*
 * Variable that controls whether compilation tracing is enabled and, if so,
 * what level of tracing is desired:
 *    0: no compilation tracing
 *    1: summarize compilation of top level cmds and proc bodies
 *    2: display all instructions of each ByteCode compiled
 * This variable is linked to the Tcl variable "tcl_traceCompile".
 */

#ifdef TCL_COMPILE_DEBUG
int tclTraceCompile = 0;
static int traceInitialized = 0;
#endif

/*
 * A table describing the Tcl bytecode instructions. Entries in this table
 * must correspond to the instruction opcode definitions in tclCompile.h.
 * The names "op1" and "op4" refer to an instruction's one or four byte
 * first operand. Similarly, "stktop" and "stknext" refer to the topmost
 * and next to topmost stack elements.
 *
 * Note that the load, store, and incr instructions do not distinguish local
 * from global variables; the bytecode interpreter at runtime uses the
 * existence of a procedure call frame to distinguish these.
 */

InstructionDesc tclInstructionTable[] = {
   /* Name	      Bytes stackEffect #Opnds Operand types	Stack top, next	  */
    {"done",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Finish ByteCode execution and return stktop (top stack item) */
    {"push1",		  2,   +1,         1,   {OPERAND_UINT1}},
	/* Push object at ByteCode objArray[op1] */
    {"push4",		  5,   +1,         1,   {OPERAND_UINT4}},
	/* Push object at ByteCode objArray[op4] */
    {"pop",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Pop the topmost stack object */
    {"dup",		  1,   +1,         0,   {OPERAND_NONE}},
	/* Duplicate the topmost stack object and push the result */
    {"concat1",		  2,   INT_MIN,    1,   {OPERAND_UINT1}},
	/* Concatenate the top op1 items and push result */
    {"invokeStk1",	  2,   INT_MIN,    1,   {OPERAND_UINT1}},
	/* Invoke command named objv[0]; <objc,objv> = <op1,top op1> */
    {"invokeStk4",	  5,   INT_MIN,    1,   {OPERAND_UINT4}},
	/* Invoke command named objv[0]; <objc,objv> = <op4,top op4> */
    {"evalStk",		  1,   0,          0,   {OPERAND_NONE}},
	/* Evaluate command in stktop using Tcl_EvalObj. */
    {"exprStk",		  1,   0,          0,   {OPERAND_NONE}},
	/* Execute expression in stktop using Tcl_ExprStringObj. */
    
    {"loadScalar1",	  2,   1,          1,   {OPERAND_UINT1}},
	/* Load scalar variable at index op1 <= 255 in call frame */
    {"loadScalar4",	  5,   1,          1,   {OPERAND_UINT4}},
	/* Load scalar variable at index op1 >= 256 in call frame */
    {"loadScalarStk",	  1,   0,          0,   {OPERAND_NONE}},
	/* Load scalar variable; scalar's name is stktop */
    {"loadArray1",	  2,   0,          1,   {OPERAND_UINT1}},
	/* Load array element; array at slot op1<=255, element is stktop */
    {"loadArray4",	  5,   0,          1,   {OPERAND_UINT4}},
	/* Load array element; array at slot op1 > 255, element is stktop */
    {"loadArrayStk",	  1,   -1,         0,   {OPERAND_NONE}},
	/* Load array element; element is stktop, array name is stknext */
    {"loadStk",		  1,   0,          0,   {OPERAND_NONE}},
	/* Load general variable; unparsed variable name is stktop */
    {"storeScalar1",	  2,   0,          1,   {OPERAND_UINT1}},
	/* Store scalar variable at op1<=255 in frame; value is stktop */
    {"storeScalar4",	  5,   0,          1,   {OPERAND_UINT4}},
	/* Store scalar variable at op1 > 255 in frame; value is stktop */
    {"storeScalarStk",	  1,   -1,         0,   {OPERAND_NONE}},
	/* Store scalar; value is stktop, scalar name is stknext */
    {"storeArray1",	  2,   -1,         1,   {OPERAND_UINT1}},
	/* Store array element; array at op1<=255, value is top then elem */
    {"storeArray4",	  5,   -1,         1,   {OPERAND_UINT4}},
	/* Store array element; array at op1>=256, value is top then elem */
    {"storeArrayStk",	  1,   -2,         0,   {OPERAND_NONE}},
	/* Store array element; value is stktop, then elem, array names */
    {"storeStk",	  1,   -1,         0,   {OPERAND_NONE}},
	/* Store general variable; value is stktop, then unparsed name */
    
    {"incrScalar1",	  2,   0,          1,   {OPERAND_UINT1}},
	/* Incr scalar at index op1<=255 in frame; incr amount is stktop */
    {"incrScalarStk",	  1,   -1,         0,   {OPERAND_NONE}},
	/* Incr scalar; incr amount is stktop, scalar's name is stknext */
    {"incrArray1",	  2,   -1,         1,   {OPERAND_UINT1}},
	/* Incr array elem; arr at slot op1<=255, amount is top then elem */
    {"incrArrayStk",	  1,   -2,         0,   {OPERAND_NONE}},
	/* Incr array element; amount is top then elem then array names */
    {"incrStk",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Incr general variable; amount is stktop then unparsed var name */
    {"incrScalar1Imm",	  3,   +1,         2,   {OPERAND_UINT1, OPERAND_INT1}},
	/* Incr scalar at slot op1 <= 255; amount is 2nd operand byte */
    {"incrScalarStkImm",  2,   0,          1,   {OPERAND_INT1}},
	/* Incr scalar; scalar name is stktop; incr amount is op1 */
    {"incrArray1Imm",	  3,   0,          2,   {OPERAND_UINT1, OPERAND_INT1}},
	/* Incr array elem; array at slot op1 <= 255, elem is stktop,
	 * amount is 2nd operand byte */
    {"incrArrayStkImm",	  2,   -1,         1,   {OPERAND_INT1}},
	/* Incr array element; elem is top then array name, amount is op1 */
    {"incrStkImm",	  2,   0,	   1,   {OPERAND_INT1}},
	/* Incr general variable; unparsed name is top, amount is op1 */
    
    {"jump1",		  2,   0,          1,   {OPERAND_INT1}},
	/* Jump relative to (pc + op1) */
    {"jump4",		  5,   0,          1,   {OPERAND_INT4}},
	/* Jump relative to (pc + op4) */
    {"jumpTrue1",	  2,   -1,         1,   {OPERAND_INT1}},
	/* Jump relative to (pc + op1) if stktop expr object is true */
    {"jumpTrue4",	  5,   -1,         1,   {OPERAND_INT4}},
	/* Jump relative to (pc + op4) if stktop expr object is true */
    {"jumpFalse1",	  2,   -1,         1,   {OPERAND_INT1}},
	/* Jump relative to (pc + op1) if stktop expr object is false */
    {"jumpFalse4",	  5,   -1,         1,   {OPERAND_INT4}},
	/* Jump relative to (pc + op4) if stktop expr object is false */

    {"lor",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Logical or:	push (stknext || stktop) */
    {"land",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Logical and:	push (stknext && stktop) */
    {"bitor",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Bitwise or:	push (stknext | stktop) */
    {"bitxor",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Bitwise xor	push (stknext ^ stktop) */
    {"bitand",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Bitwise and:	push (stknext & stktop) */
    {"eq",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Equal:	push (stknext == stktop) */
    {"neq",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Not equal:	push (stknext != stktop) */
    {"lt",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Less:	push (stknext < stktop) */
    {"gt",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Greater:	push (stknext || stktop) */
    {"le",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Logical or:	push (stknext || stktop) */
    {"ge",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Logical or:	push (stknext || stktop) */
    {"lshift",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Left shift:	push (stknext << stktop) */
    {"rshift",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Right shift:	push (stknext >> stktop) */
    {"add",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Add:		push (stknext + stktop) */
    {"sub",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Sub:		push (stkext - stktop) */
    {"mult",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Multiply:	push (stknext * stktop) */
    {"div",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Divide:	push (stknext / stktop) */
    {"mod",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Mod:		push (stknext % stktop) */
    {"uplus",		  1,   0,          0,   {OPERAND_NONE}},
	/* Unary plus:	push +stktop */
    {"uminus",		  1,   0,          0,   {OPERAND_NONE}},
	/* Unary minus:	push -stktop */
    {"bitnot",		  1,   0,          0,   {OPERAND_NONE}},
	/* Bitwise not:	push ~stktop */
    {"not",		  1,   0,          0,   {OPERAND_NONE}},
	/* Logical not:	push !stktop */
    {"callBuiltinFunc1",  2,   1,          1,   {OPERAND_UINT1}},
	/* Call builtin math function with index op1; any args are on stk */
    {"callFunc1",	  2,   INT_MIN,    1,   {OPERAND_UINT1}},
	/* Call non-builtin func objv[0]; <objc,objv>=<op1,top op1>  */
    {"tryCvtToNumeric",	  1,   0,          0,   {OPERAND_NONE}},
	/* Try converting stktop to first int then double if possible. */

    {"break",		  1,   0,          0,   {OPERAND_NONE}},
	/* Abort closest enclosing loop; if none, return TCL_BREAK code. */
    {"continue",	  1,   0,          0,   {OPERAND_NONE}},
	/* Skip to next iteration of closest enclosing loop; if none,
	 * return TCL_CONTINUE code. */

    {"foreach_start4",	  5,   0,          1,   {OPERAND_UINT4}},
	/* Initialize execution of a foreach loop. Operand is aux data index
	 * of the ForeachInfo structure for the foreach command. */
    {"foreach_step4",	  5,   +1,         1,   {OPERAND_UINT4}},
	/* "Step" or begin next iteration of foreach loop. Push 0 if to
	 *  terminate loop, else push 1. */

    {"beginCatch4",	  5,   0,          1,   {OPERAND_UINT4}},
	/* Record start of catch with the operand's exception index.
	 * Push the current stack depth onto a special catch stack. */
    {"endCatch",	  1,   0,          0,   {OPERAND_NONE}},
	/* End of last catch. Pop the bytecode interpreter's catch stack. */
    {"pushResult",	  1,   +1,         0,   {OPERAND_NONE}},
	/* Push the interpreter's object result onto the stack. */
    {"pushReturnCode",	  1,   +1,         0,   {OPERAND_NONE}},
	/* Push interpreter's return code (e.g. TCL_OK or TCL_ERROR) as
	 * a new object onto the stack. */
    {"streq",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Str Equal:	push (stknext eq stktop) */
    {"strneq",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Str !Equal:	push (stknext neq stktop) */
    {"strcmp",		  1,   -1,         0,   {OPERAND_NONE}},
	/* Str Compare:	push (stknext cmp stktop) */
    {"strlen",		  1,   0,          0,   {OPERAND_NONE}},
	/* Str Length:	push (strlen stktop) */
    {"strindex",	  1,   -1,         0,   {OPERAND_NONE}},
	/* Str Index:	push (strindex stknext stktop) */
    {"strmatch",	  2,   -1,         1,   {OPERAND_INT1}},
	/* Str Match:	push (strmatch stknext stktop) opnd == nocase */
    {"list",		  5,   INT_MIN,    1,   {OPERAND_UINT4}},
	/* List:	push (stk1 stk2 ... stktop) */
    {"listIndex",	  1,   -1,         0,   {OPERAND_NONE}},
	/* List Index:	push (listindex stknext stktop) */
    {"listLength",	  1,   0,          0,   {OPERAND_NONE}},
	/* List Len:	push (listlength stktop) */
    {"appendScalar1",	  2,   0,          1,   {OPERAND_UINT1}},
	/* Append scalar variable at op1<=255 in frame; value is stktop */
    {"appendScalar4",	  5,   0,          1,   {OPERAND_UINT4}},
	/* Append scalar variable at op1 > 255 in frame; value is stktop */
    {"appendArray1",	  2,   -1,         1,   {OPERAND_UINT1}},
	/* Append array element; array at op1<=255, value is top then elem */
    {"appendArray4",	  5,   -1,         1,   {OPERAND_UINT4}},
	/* Append array element; array at op1>=256, value is top then elem */
    {"appendArrayStk",	  1,   -2,         0,   {OPERAND_NONE}},
	/* Append array element; value is stktop, then elem, array names */
    {"appendStk",	  1,   -1,         0,   {OPERAND_NONE}},
	/* Append general variable; value is stktop, then unparsed name */
    {"lappendScalar1",	  2,   0,          1,   {OPERAND_UINT1}},
	/* Lappend scalar variable at op1<=255 in frame; value is stktop */
    {"lappendScalar4",	  5,   0,          1,   {OPERAND_UINT4}},
	/* Lappend scalar variable at op1 > 255 in frame; value is stktop */
    {"lappendArray1",	  2,   -1,         1,   {OPERAND_UINT1}},
	/* Lappend array element; array at op1<=255, value is top then elem */
    {"lappendArray4",	  5,   -1,         1,   {OPERAND_UINT4}},
	/* Lappend array element; array at op1>=256, value is top then elem */
    {"lappendArrayStk",	  1,   -2,         0,   {OPERAND_NONE}},
	/* Lappend array element; value is stktop, then elem, array names */
    {"lappendStk",	  1,   -1,         0,   {OPERAND_NONE}},
	/* Lappend general variable; value is stktop, then unparsed name */
    {"lindexMulti",	  5,   INT_MIN,    1,   {OPERAND_UINT4}},
        /* Lindex with generalized args, operand is number of stacked objs 
	 * used: (operand-1) entries from stktop are the indices; then list 
	 * to process. */
    {"over",		  5,   +1,         1,   {OPERAND_UINT4}},
        /* Duplicate the arg-th element from top of stack (TOS=0) */
    {"lsetList",          1,   -2,         0,   {OPERAND_NONE}},
        /* Four-arg version of 'lset'. stktop is old value; next is
         * new element value, next is the index list; pushes new value */
    {"lsetFlat",          5,   INT_MIN,    1,   {OPERAND_UINT4}},
        /* Three- or >=5-arg version of 'lset', operand is number of 
	 * stacked objs: stktop is old value, next is new element value, next 
	 * come (operand-2) indices; pushes the new value.
	 */
    {"return",		  9,   -2,         2,   {OPERAND_INT4, OPERAND_UINT4}},
	/* Compiled [return], code, level are operands; options and result
	 * are on the stack. */
    {"expon",		  1,   -1,	   0,	{OPERAND_NONE}},
	/* Binary exponentiation operator: push (stknext ** stktop) */
     /* 
      * NOTE: the stack effects of expandStkTop and invokeExpanded
      * are wrong - but it cannot be done right at compile time, the stack
      * effect is only known at run time. The value for invokeExpanded
      * is estimated better at compile time.
      * See the comments further down in this file, where INST_INVOKE_EXPANDED 
      * is emitted.
      */
     {"expandStart",       1,    0,          0,   {OPERAND_NONE}},
         /* Start of command with {expand}ed arguments */
     {"expandStkTop",      5,    0,          1,   {OPERAND_INT4}},
         /* Expand the list at stacktop: push its elements on the stack */
     {"invokeExpanded",    1,    0,          0,   {OPERAND_NONE}},
         /* Invoke the command marked by the last 'expandStart' */
    {"listIndexImm",	  5,	0,	   1,	{OPERAND_IDX4}},
	/* List Index:	push (lindex stktop op4) */
    {"listRangeImm",	  9,	0,	   2,	{OPERAND_IDX4, OPERAND_IDX4}},
	/* List Range:	push (lrange stktop op4 op4) */

    {"startCommand",      5,    0,         1,   {OPERAND_UINT4}},
        /* Start of bytecoded command: op is the length of the cmd's code */ 

    {"listIn",		  1,	-1,	   0,	{OPERAND_NONE}},
	/* List containment: push [lsearch stktop stknext]>=0) */
    {"listNotIn",	  1,	-1,	   0,	{OPERAND_NONE}},
	/* List negated containment: push [lsearch stktop stknext]<0) */
    {0}
};

/*
 * Prototypes for procedures defined later in this file:
 */

static void		DupByteCodeInternalRep _ANSI_ARGS_((Tcl_Obj *srcPtr,
			    Tcl_Obj *copyPtr));
static unsigned char *	EncodeCmdLocMap _ANSI_ARGS_((
			    CompileEnv *envPtr, ByteCode *codePtr,
			    unsigned char *startPtr));
static void		EnterCmdExtentData _ANSI_ARGS_((
    			    CompileEnv *envPtr, int cmdNumber,
			    int numSrcBytes, int numCodeBytes));
static void		EnterCmdStartData _ANSI_ARGS_((
    			    CompileEnv *envPtr, int cmdNumber,
			    int srcOffset, int codeOffset));
static void		FreeByteCodeInternalRep _ANSI_ARGS_((
    			    Tcl_Obj *objPtr));
static int		GetCmdLocEncodingSize _ANSI_ARGS_((
			    CompileEnv *envPtr));
#ifdef TCL_COMPILE_STATS
static void		RecordByteCodeStats _ANSI_ARGS_((
			    ByteCode *codePtr));
#endif /* TCL_COMPILE_STATS */
static int		SetByteCodeFromAny _ANSI_ARGS_((Tcl_Interp *interp,
			    Tcl_Obj *objPtr));

/*
 * The structure below defines the bytecode Tcl object type by
 * means of procedures that can be invoked by generic object code.
 */

Tcl_ObjType tclByteCodeType = {
    "bytecode",				/* name */
    FreeByteCodeInternalRep,		/* freeIntRepProc */
    DupByteCodeInternalRep,		/* dupIntRepProc */
    (Tcl_UpdateStringProc *) NULL,	/* updateStringProc */
    SetByteCodeFromAny			/* setFromAnyProc */
};

/*
 *----------------------------------------------------------------------
 *
 * TclSetByteCodeFromAny --
 *
 *	Part of the bytecode Tcl object type implementation. Attempts to
 *	generate an byte code internal form for the Tcl object "objPtr" by
 *	compiling its string representation.  This function also takes
 *	a hook procedure that will be invoked to perform any needed post
 *	processing on the compilation results before generating byte
 *	codes.
 *
 * Results:
 *	The return value is a standard Tcl object result. If an error occurs
 *	during compilation, an error message is left in the interpreter's
 *	result unless "interp" is NULL.
 *
 * Side effects:
 *	Frees the old internal representation. If no error occurs, then the
 *	compiled code is stored as "objPtr"s bytecode representation.
 *	Also, if debugging, initializes the "tcl_traceCompile" Tcl variable
 *	used to trace compilations.
 *
 *----------------------------------------------------------------------
 */

int
TclSetByteCodeFromAny(interp, objPtr, hookProc, clientData)
    Tcl_Interp *interp;		/* The interpreter for which the code is
				 * being compiled.  Must not be NULL. */
    Tcl_Obj *objPtr;		/* The object to make a ByteCode object. */
    CompileHookProc *hookProc;	/* Procedure to invoke after compilation. */
    ClientData clientData;	/* Hook procedure private data. */
{
#ifdef TCL_COMPILE_DEBUG
    Interp *iPtr = (Interp *) interp;
#endif /*TCL_COMPILE_DEBUG*/
    CompileEnv compEnv;		/* Compilation environment structure
				 * allocated in frame. */
    LiteralTable *localTablePtr = &(compEnv.localLitTable);
    register AuxData *auxDataPtr;
    LiteralEntry *entryPtr;
    register int i;
    int length, result = TCL_OK;
    char *stringPtr;

#ifdef TCL_COMPILE_DEBUG
    if (!traceInitialized) {
        if (Tcl_LinkVar(interp, "tcl_traceCompile",
	            (char *) &tclTraceCompile,  TCL_LINK_INT) != TCL_OK) {
            Tcl_Panic("SetByteCodeFromAny: unable to create link for tcl_traceCompile variable");
        }
        traceInitialized = 1;
    }
#endif

    stringPtr = Tcl_GetStringFromObj(objPtr, &length);
    TclInitCompileEnv(interp, &compEnv, stringPtr, length);
    TclCompileScript(interp, stringPtr, length, &compEnv);

    /*
     * Successful compilation. Add a "done" instruction at the end.
     */

    TclEmitOpcode(INST_DONE, &compEnv);

    /*
     * Invoke the compilation hook procedure if one exists.
     */

    if (hookProc) {
        result = (*hookProc)(interp, &compEnv, clientData);
    }

    /*
     * Change the object into a ByteCode object. Ownership of the literal
     * objects and aux data items is given to the ByteCode object.
     */
    
#ifdef TCL_COMPILE_DEBUG
    TclVerifyLocalLiteralTable(&compEnv);
#endif /*TCL_COMPILE_DEBUG*/

    TclInitByteCodeObj(objPtr, &compEnv);
#ifdef TCL_COMPILE_DEBUG
    if (tclTraceCompile >= 2) {
        TclPrintByteCodeObj(interp, objPtr);
    }
#endif /* TCL_COMPILE_DEBUG */
	
    if (result != TCL_OK) {
	/*
	 * Handle any error from the hookProc
	 */

	entryPtr = compEnv.literalArrayPtr;
	for (i = 0;  i < compEnv.literalArrayNext;  i++) {
	    TclReleaseLiteral(interp, entryPtr->objPtr);
	    entryPtr++;
	}
#ifdef TCL_COMPILE_DEBUG
	TclVerifyGlobalLiteralTable(iPtr);
#endif /*TCL_COMPILE_DEBUG*/

	auxDataPtr = compEnv.auxDataArrayPtr;
	for (i = 0;  i < compEnv.auxDataArrayNext;  i++) {
	    if (auxDataPtr->type->freeProc != NULL) {
		auxDataPtr->type->freeProc(auxDataPtr->clientData);
	    }
	    auxDataPtr++;
	}
    }


    /*
     * Free storage allocated during compilation.
     */
    
    if (localTablePtr->buckets != localTablePtr->staticBuckets) {
	ckfree((char *) localTablePtr->buckets);
    }
    TclFreeCompileEnv(&compEnv);
    return result;
}

/*
 *-----------------------------------------------------------------------
 *
 * SetByteCodeFromAny --
 *
 *	Part of the bytecode Tcl object type implementation. Attempts to
 *	generate an byte code internal form for the Tcl object "objPtr" by
 *	compiling its string representation.
 *
 * Results:
 *	The return value is a standard Tcl object result. If an error occurs
 *	during compilation, an error message is left in the interpreter's
 *	result unless "interp" is NULL.
 *
 * Side effects:
 *	Frees the old internal representation. If no error occurs, then the
 *	compiled code is stored as "objPtr"s bytecode representation.
 *	Also, if debugging, initializes the "tcl_traceCompile" Tcl variable
 *	used to trace compilations.
 *
 *----------------------------------------------------------------------
 */

static int
SetByteCodeFromAny(interp, objPtr)
    Tcl_Interp *interp;		/* The interpreter for which the code is
				 * being compiled.  Must not be NULL. */
    Tcl_Obj *objPtr;		/* The object to make a ByteCode object. */
{
    return TclSetByteCodeFromAny(interp, objPtr,
	    (CompileHookProc *) NULL, (ClientData) NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * DupByteCodeInternalRep --
 *
 *	Part of the bytecode Tcl object type implementation. However, it
 *	does not copy the internal representation of a bytecode Tcl_Obj, but
 *	instead leaves the new object untyped (with a NULL type pointer).
 *	Code will be compiled for the new object only if necessary.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
DupByteCodeInternalRep(srcPtr, copyPtr)
    Tcl_Obj *srcPtr;		/* Object with internal rep to copy. */
    Tcl_Obj *copyPtr;		/* Object with internal rep to set. */
{
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeByteCodeInternalRep --
 *
 *	Part of the bytecode Tcl object type implementation. Frees the
 *	storage associated with a bytecode object's internal representation
 *	unless its code is actively being executed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The bytecode object's internal rep is marked invalid and its
 *	code gets freed unless the code is actively being executed.
 *	In that case the cleanup is delayed until the last execution
 *	of the code completes.
 *
 *----------------------------------------------------------------------
 */

static void
FreeByteCodeInternalRep(objPtr)
    register Tcl_Obj *objPtr;	/* Object whose internal rep to free. */
{
    register ByteCode *codePtr =
	    (ByteCode *) objPtr->internalRep.otherValuePtr;

    codePtr->refCount--;
    if (codePtr->refCount <= 0) {
	TclCleanupByteCode(codePtr);
    }
    objPtr->typePtr = NULL;
    objPtr->internalRep.otherValuePtr = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCleanupByteCode --
 *
 *	This procedure does all the real work of freeing up a bytecode
 *	object's ByteCode structure. It's called only when the structure's
 *	reference count becomes zero.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees objPtr's bytecode internal representation and sets its type
 *	and objPtr->internalRep.otherValuePtr NULL. Also releases its
 *	literals and frees its auxiliary data items.
 *
 *----------------------------------------------------------------------
 */

void
TclCleanupByteCode(codePtr)
    register ByteCode *codePtr;	/* Points to the ByteCode to free. */
{
    Tcl_Interp *interp = (Tcl_Interp *) *codePtr->interpHandle;
    int numLitObjects = codePtr->numLitObjects;
    int numAuxDataItems = codePtr->numAuxDataItems;
    register Tcl_Obj **objArrayPtr, *objPtr;
    register AuxData *auxDataPtr;
    int i;
#ifdef TCL_COMPILE_STATS

    if (interp != NULL) {
	ByteCodeStats *statsPtr;
	Tcl_Time destroyTime;
	int lifetimeSec, lifetimeMicroSec, log2;

	statsPtr = &((Interp *) interp)->stats;

	statsPtr->numByteCodesFreed++;
	statsPtr->currentSrcBytes -= (double) codePtr->numSrcBytes;
	statsPtr->currentByteCodeBytes -= (double) codePtr->structureSize;

	statsPtr->currentInstBytes   -= (double) codePtr->numCodeBytes;
	statsPtr->currentLitBytes    -=
		(double) (codePtr->numLitObjects * sizeof(Tcl_Obj *)); 
	statsPtr->currentExceptBytes -=
		(double) (codePtr->numExceptRanges * sizeof(ExceptionRange));
	statsPtr->currentAuxBytes    -=
		(double) (codePtr->numAuxDataItems * sizeof(AuxData));
	statsPtr->currentCmdMapBytes -= (double) codePtr->numCmdLocBytes;

	Tcl_GetTime(&destroyTime);
	lifetimeSec = destroyTime.sec - codePtr->createTime.sec;
	if (lifetimeSec > 2000) {	/* avoid overflow */
	    lifetimeSec = 2000;
	}
	lifetimeMicroSec =
	    1000000*lifetimeSec + (destroyTime.usec - codePtr->createTime.usec);
	
	log2 = TclLog2(lifetimeMicroSec);
	if (log2 > 31) {
	    log2 = 31;
	}
	statsPtr->lifetimeCount[log2]++;
    }
#endif /* TCL_COMPILE_STATS */

    /*
     * A single heap object holds the ByteCode structure and its code,
     * object, command location, and auxiliary data arrays. This means we
     * only need to 1) decrement the ref counts of the LiteralEntry's in
     * its literal array, 2) call the free procs for the auxiliary data
     * items, and 3) free the ByteCode structure's heap object.
     *
     * The case for TCL_BYTECODE_PRECOMPILED (precompiled ByteCodes,
     * like those generated from tbcload) is special, as they doesn't
     * make use of the global literal table.  They instead maintain
     * private references to their literals which must be decremented.
     *
     * In order to insure a proper and efficient cleanup of the literal
     * array when it contains non-shared literals [Bug 983660], we also
     * distinguish the case of an interpreter being deleted (signaled by
     * interp == NULL). Also, as the interp deletion will remove the global
     * literal table anyway, we avoid the extra cost of updating it for each
     * literal being released.
     */

    if ((codePtr->flags & TCL_BYTECODE_PRECOMPILED)
	    || (interp == NULL)) {
 
	objArrayPtr = codePtr->objArrayPtr;
	for (i = 0;  i < numLitObjects;  i++) {
	    objPtr = *objArrayPtr;
	    if (objPtr) {
		Tcl_DecrRefCount(objPtr);
	    }
	    objArrayPtr++;
	}
	codePtr->numLitObjects = 0;
    } else {
	objArrayPtr = codePtr->objArrayPtr;
	for (i = 0;  i < numLitObjects;  i++) {
	    /*
	     * TclReleaseLiteral sets a ByteCode's object array entry NULL to
	     * indicate that it has already freed the literal.
	     */
	    
	    objPtr = *objArrayPtr;
	    if (objPtr != NULL) {
		TclReleaseLiteral(interp, objPtr);
	    }
	    objArrayPtr++;
	}
    }
    
    auxDataPtr = codePtr->auxDataArrayPtr;
    for (i = 0;  i < numAuxDataItems;  i++) {
	if (auxDataPtr->type->freeProc != NULL) {
	    (*auxDataPtr->type->freeProc)(auxDataPtr->clientData);
	}
	auxDataPtr++;
    }

    TclHandleRelease(codePtr->interpHandle);
    ckfree((char *) codePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TclInitCompileEnv --
 *
 *	Initializes a CompileEnv compilation environment structure for the
 *	compilation of a string in an interpreter.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The CompileEnv structure is initialized.
 *
 *----------------------------------------------------------------------
 */

void
TclInitCompileEnv(interp, envPtr, stringPtr, numBytes)
    Tcl_Interp *interp;		 /* The interpreter for which a CompileEnv
				  * structure is initialized. */
    register CompileEnv *envPtr; /* Points to the CompileEnv structure to
				  * initialize. */
    char *stringPtr;		 /* The source string to be compiled. */
    int numBytes;		 /* Number of bytes in source string. */
{
    Interp *iPtr = (Interp *) interp;
    
    envPtr->iPtr = iPtr;
    envPtr->source = stringPtr;
    envPtr->numSrcBytes = numBytes;
    envPtr->procPtr = iPtr->compiledProcPtr;
    envPtr->numCommands = 0;
    envPtr->exceptDepth = 0;
    envPtr->maxExceptDepth = 0;
    envPtr->maxStackDepth = 0;
    envPtr->currStackDepth = 0;
    TclInitLiteralTable(&(envPtr->localLitTable));

    envPtr->codeStart = envPtr->staticCodeSpace;
    envPtr->codeNext = envPtr->codeStart;
    envPtr->codeEnd = (envPtr->codeStart + COMPILEENV_INIT_CODE_BYTES);
    envPtr->mallocedCodeArray = 0;

    envPtr->literalArrayPtr = envPtr->staticLiteralSpace;
    envPtr->literalArrayNext = 0;
    envPtr->literalArrayEnd = COMPILEENV_INIT_NUM_OBJECTS;
    envPtr->mallocedLiteralArray = 0;
    
    envPtr->exceptArrayPtr = envPtr->staticExceptArraySpace;
    envPtr->exceptArrayNext = 0;
    envPtr->exceptArrayEnd = COMPILEENV_INIT_EXCEPT_RANGES;
    envPtr->mallocedExceptArray = 0;
    
    envPtr->cmdMapPtr = envPtr->staticCmdMapSpace;
    envPtr->cmdMapEnd = COMPILEENV_INIT_CMD_MAP_SIZE;
    envPtr->mallocedCmdMap = 0;
    
    envPtr->auxDataArrayPtr = envPtr->staticAuxDataArraySpace;
    envPtr->auxDataArrayNext = 0;
    envPtr->auxDataArrayEnd = COMPILEENV_INIT_AUX_DATA_SIZE;
    envPtr->mallocedAuxDataArray = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TclFreeCompileEnv --
 *
 *	Free the storage allocated in a CompileEnv compilation environment
 *	structure.
 *
 * Results:
 *	None.
 * 
 * Side effects:
 *	Allocated storage in the CompileEnv structure is freed. Note that
 *	its local literal table is not deleted and its literal objects are
 *	not released. In addition, storage referenced by its auxiliary data
 *	items is not freed. This is done so that, when compilation is
 *	successful, "ownership" of these objects and aux data items is
 *	handed over to the corresponding ByteCode structure.
 *
 *----------------------------------------------------------------------
 */

void
TclFreeCompileEnv(envPtr)
    register CompileEnv *envPtr; /* Points to the CompileEnv structure. */
{
    if (envPtr->mallocedCodeArray) {
	ckfree((char *) envPtr->codeStart);
    }
    if (envPtr->mallocedLiteralArray) {
	ckfree((char *) envPtr->literalArrayPtr);
    }
    if (envPtr->mallocedExceptArray) {
	ckfree((char *) envPtr->exceptArrayPtr);
    }
    if (envPtr->mallocedCmdMap) {
	ckfree((char *) envPtr->cmdMapPtr);
    }
    if (envPtr->mallocedAuxDataArray) {
	ckfree((char *) envPtr->auxDataArrayPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclWordKnownAtCompileTime --
 *
 *	Test whether the value of a token is completely known at compile
 *	time.
 *
 * Results:
 *	Returns true if the tokenPtr argument points to a word value that
 *	is completely known at compile time.  Generally, values that are
 *	known at compile time can be compiled to their values, while values
 *	that cannot be	known until substitution at runtime must be compiled
 *	to bytecode instructions that perform that substitution.  For several
 *	commands, whether or not arguments are known at compile time determine
 *	whether it is worthwhile to compile at all.
 *
 * Side effects:
 *	When returning true, appends the known value of the word to
 *	the unshared Tcl_Obj (*valuePtr), unless valuePtr is NULL.
 *
 *----------------------------------------------------------------------
 */

int
TclWordKnownAtCompileTime(tokenPtr, valuePtr)
    Tcl_Token *tokenPtr;	/* Points to Tcl_Token we should check */
    Tcl_Obj *valuePtr;		/* If not NULL, points to an unshared Tcl_Obj
				 * to which we should append the known value
				 * of the word. */
{
    int numComponents = tokenPtr->numComponents;
    Tcl_Obj *tempPtr = NULL;

    if (tokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	if (valuePtr != NULL) {
	    Tcl_AppendToObj(valuePtr, tokenPtr[1].start, tokenPtr[1].size);
	}
	return 1;
    }
    if (tokenPtr->type != TCL_TOKEN_WORD) {
	return 0;
    }
    tokenPtr++;
    if (valuePtr != NULL) {
	tempPtr = Tcl_NewObj();
	Tcl_IncrRefCount(tempPtr);
    }
    while (numComponents--) {
	switch (tokenPtr->type) {
	    case TCL_TOKEN_TEXT:
		if (tempPtr != NULL) {
		    Tcl_AppendToObj(tempPtr, tokenPtr->start, tokenPtr->size);
		}
		break;

	    case TCL_TOKEN_BS:
		if (tempPtr != NULL) {
		    char utfBuf[TCL_UTF_MAX];
		    int length = 
			    Tcl_UtfBackslash(tokenPtr->start, NULL, utfBuf);
		    Tcl_AppendToObj(tempPtr, utfBuf, length);
		}
		break;
	    
	    default:
		if (tempPtr != NULL) {
		    Tcl_DecrRefCount(tempPtr);
		}
		return 0;
	}
	tokenPtr++;
    }
    if (valuePtr != NULL) {
	Tcl_AppendObjToObj(valuePtr, tempPtr);
	Tcl_DecrRefCount(tempPtr);
    }
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileScript --
 *
 *	Compile a Tcl script in a string.
 *
 * Results:
 *	The return value is TCL_OK on a successful compilation and TCL_ERROR
 *	on failure. If TCL_ERROR is returned, then the interpreter's result
 *	contains an error message.
 *
 * Side effects:
 *	Adds instructions to envPtr to evaluate the script at runtime.
 *
 *----------------------------------------------------------------------
 */

void
TclCompileScript(interp, script, numBytes, envPtr)
    Tcl_Interp *interp;		/* Used for error and status reporting.
				 * Also serves as context for finding and
				 * compiling commands.  May not be NULL. */
    CONST char *script;		/* The source script to compile. */
    int numBytes;		/* Number of bytes in script. If < 0, the
				 * script consists of all bytes up to the
				 * first null character. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Interp *iPtr = (Interp *) interp;
    Tcl_Parse parse;
    int lastTopLevelCmdIndex = -1;
    				/* Index of most recent toplevel command in
 				 * the command location table. Initialized
				 * to avoid compiler warning. */
    int startCodeOffset = -1;	/* Offset of first byte of current command's
                                 * code. Init. to avoid compiler warning. */
    unsigned char *entryCodeNext = envPtr->codeNext;
    CONST char *p, *next;
    Namespace *cmdNsPtr;
    Command *cmdPtr;
    Tcl_Token *tokenPtr;
    int bytesLeft, isFirstCmd, gotParse, wordIdx, currCmdIndex;
    int commandLength, objIndex, code;
    Tcl_DString ds;

    Tcl_DStringInit(&ds);

    if (numBytes < 0) {
	numBytes = strlen(script);
    }
    Tcl_ResetResult(interp);
    isFirstCmd = 1;

    if (envPtr->procPtr != NULL) {
	cmdNsPtr = envPtr->procPtr->cmdPtr->nsPtr;
    } else {
	cmdNsPtr = NULL; /* use current NS */
    }

    /*
     * Each iteration through the following loop compiles the next
     * command from the script.
     */

    p = script;
    bytesLeft = numBytes;
    gotParse = 0;
    do {
	if (Tcl_ParseCommand(interp, p, bytesLeft, 0, &parse) != TCL_OK) {
	    /* Compile bytecodes to report the parse error at runtime */
	    Tcl_Obj *returnCmd = Tcl_NewStringObj(
		    "return -code 1 -level 0 -errorinfo", -1);
	    Tcl_Obj *errMsg = Tcl_GetObjResult(interp);
	    Tcl_Obj *errInfo = Tcl_DuplicateObj(errMsg);
	    char *cmdString;
	    int cmdLength;
	    Tcl_Parse subParse;
	    int errorLine = 1;

	    Tcl_IncrRefCount(returnCmd);
	    Tcl_IncrRefCount(errInfo);
	    Tcl_AppendToObj(errInfo, "\n    while executing\n\"", -1);
	    TclAppendLimitedToObj(errInfo, parse.commandStart,
		/* Drop the command terminator (";" or "]") if appropriate */
		(parse.term == parse.commandStart + parse.commandSize - 1) ?
			parse.commandSize - 1 : parse.commandSize, 153, NULL);
	    Tcl_AppendToObj(errInfo, "\"", -1);

	    Tcl_ListObjAppendElement(NULL, returnCmd, errInfo);

	    for (p = envPtr->source; p != parse.commandStart; p++) {
		if (*p == '\n') {
		    errorLine++;
		}
	    }
	    Tcl_ListObjAppendElement(NULL, returnCmd,
		    Tcl_NewStringObj("-errorline", -1));
	    Tcl_ListObjAppendElement(NULL, returnCmd,
		    Tcl_NewIntObj(errorLine));

	    Tcl_ListObjAppendElement(NULL, returnCmd, errMsg);
	    Tcl_DecrRefCount(errInfo);

	    cmdString = Tcl_GetStringFromObj(returnCmd, &cmdLength);
	    Tcl_ParseCommand(interp, cmdString, cmdLength, 0, &subParse);
	    TclCompileReturnCmd(interp, &subParse, envPtr);
	    Tcl_DecrRefCount(returnCmd);
	    Tcl_FreeParse(&subParse);
	    return;
	}
	gotParse = 1;
	if (parse.numWords > 0) {
	    int expand = 0;

	    /*
	     * If not the first command, pop the previous command's result
	     * and, if we're compiling a top level command, update the last
	     * command's code size to account for the pop instruction.
	     */

	    if (!isFirstCmd) {
		TclEmitOpcode(INST_POP, envPtr);
		envPtr->cmdMapPtr[lastTopLevelCmdIndex].numCodeBytes =
			(envPtr->codeNext - envPtr->codeStart)
			- startCodeOffset;
	    }

	    /*
	     * Determine the actual length of the command.
	     */

	    commandLength = parse.commandSize;
	    if (parse.term == parse.commandStart + commandLength - 1) {
		/*
		 * The command terminator character (such as ; or ]) is
		 * the last character in the parsed command.  Reduce the
		 * length by one so that the trace message doesn't include
		 * the terminator character.
		 */
		
		commandLength -= 1;
	    }

#ifdef TCL_COMPILE_DEBUG
	    /*
             * If tracing, print a line for each top level command compiled.
             */

	    if ((tclTraceCompile >= 1) && (envPtr->procPtr == NULL)) {
		fprintf(stdout, "  Compiling: ");
		TclPrintSource(stdout, parse.commandStart,
			TclMin(commandLength, 55));
		fprintf(stdout, "\n");
	    }
#endif

	    /*
	     * Check whether expansion has been requested for any of
	     * the words
	     */

	    for (wordIdx = 0, tokenPtr = parse.tokenPtr;
		    wordIdx < parse.numWords;
		    wordIdx++, tokenPtr += (tokenPtr->numComponents + 1)) {
		if (tokenPtr->type == TCL_TOKEN_EXPAND_WORD) {
		    expand = 1;
		    TclEmitOpcode(INST_EXPAND_START, envPtr);		    
		    break;
		}
	    }

	    envPtr->numCommands++;
	    currCmdIndex = (envPtr->numCommands - 1);
	    lastTopLevelCmdIndex = currCmdIndex;
	    startCodeOffset = (envPtr->codeNext - envPtr->codeStart);
	    EnterCmdStartData(envPtr, currCmdIndex,
	            (parse.commandStart - envPtr->source), startCodeOffset);

	    /*
	     * Each iteration of the following loop compiles one word
	     * from the command.
	     */
	    
	    for (wordIdx = 0, tokenPtr = parse.tokenPtr;
		    wordIdx < parse.numWords; wordIdx++,
		    tokenPtr += (tokenPtr->numComponents + 1)) {

		if (tokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
		    /*
		     * If this is the first word and the command has a
		     * compile procedure, let it compile the command.
		     */

		    if ((wordIdx == 0) && !expand) {
			/*
			 * We copy the string before trying to find the command
			 * by name.  We used to modify the string in place, but
			 * this is not safe because the name resolution
			 * handlers could have side effects that rely on the
			 * unmodified string.
			 */

			Tcl_DStringSetLength(&ds, 0);
			Tcl_DStringAppend(&ds, tokenPtr[1].start,
				tokenPtr[1].size);

			cmdPtr = (Command *) Tcl_FindCommand(interp,
				Tcl_DStringValue(&ds),
			        (Tcl_Namespace *) cmdNsPtr, /*flags*/ 0);

			if ((cmdPtr != NULL)
			        && (cmdPtr->compileProc != NULL)
			        && !(cmdPtr->flags & CMD_HAS_EXEC_TRACES)
			        && !(iPtr->flags & DONT_COMPILE_CMDS_INLINE)) {
			    int savedNumCmds = envPtr->numCommands;
			    unsigned int savedCodeNext =
				    envPtr->codeNext - envPtr->codeStart;			    

			    /*
			     * Mark the start of the command; the proper
			     * bytecode length will be updated later. There
			     * is no need to do this for the first command
			     * in the compile env, as the check is done before
			     * calling TclExecuteByteCode(). Remark that we
			     * are compiling the first cmd in the environment
			     * exactly when (savedCodeNext == 0)
			     */

			    if (savedCodeNext != 0) {
				TclEmitInstInt4(INST_START_CMD, 0, envPtr);				
			    }
			    
			    code = (*(cmdPtr->compileProc))(interp, &parse,
			            envPtr);
			    
			    if (code == TCL_OK) {
				if (savedCodeNext != 0) {
				    /*
				     * Fix the bytecode length.
				     */
				    unsigned char *fixPtr = envPtr->codeStart
					    + savedCodeNext + 1;
				    unsigned int fixLen = envPtr->codeNext
					    - envPtr->codeStart
					    - savedCodeNext;
				
				    TclStoreInt4AtPtr(fixLen, fixPtr);
				}				
				goto finishCommand;
			    } else if (code == TCL_OUT_LINE_COMPILE) {
				/*
				 * Restore numCommands and codeNext to their
				 * correct values, removing any commands
				 * compiled before TCL_OUT_LINE_COMPILE
				 * [Bugs 705406 and 735055]
				 */
				envPtr->numCommands = savedNumCmds;
				envPtr->codeNext = envPtr->codeStart
					+ savedCodeNext;
			    } else { /* an error */
				Tcl_Panic("TclCompileScript: compileProc returned TCL_ERROR\n");
			    }
			}

			/*
			 * No compile procedure so push the word. If the
			 * command was found, push a CmdName object to
			 * reduce runtime lookups.
			 */

			objIndex = TclRegisterNewLiteral(envPtr,
				tokenPtr[1].start, tokenPtr[1].size);
			if (cmdPtr != NULL) {
			    TclSetCmdNameObj(interp,
			           envPtr->literalArrayPtr[objIndex].objPtr,
				   cmdPtr);
			}
			if ((wordIdx == 0) && (parse.numWords == 1)) {
			    /*
			     * Single word script: unshare the command name to
			     * avoid shimmering between bytecode and cmdName
			     * representations [Bug 458361]
			     */

			    TclHideLiteral(interp, envPtr, objIndex);
			}
		    } else {
			objIndex = TclRegisterNewLiteral(envPtr,
				tokenPtr[1].start, tokenPtr[1].size);
		    }
		    TclEmitPush(objIndex, envPtr);
		} else {
		    /*
		     * The word is not a simple string of characters.
		     */
		    
		    TclCompileTokens(interp, tokenPtr+1,
			    tokenPtr->numComponents, envPtr);
		}
		if (tokenPtr->type == TCL_TOKEN_EXPAND_WORD) {
		    TclEmitInstInt4(INST_EXPAND_STKTOP, 
		            envPtr->currStackDepth, envPtr);
		}
	    }

	    /*
	     * Emit an invoke instruction for the command. We skip this
	     * if a compile procedure was found for the command.
	     */

	    if (expand) {
		/*
		 * The stack depth during argument expansion can only be
		 * managed at runtime, as the number of elements in the
		 * expanded lists is not known at compile time.
		 * We adjust here the stack depth estimate so that it is
		 * correct after the command with expanded arguments
		 * returns.
		 * The end effect of this command's invocation is that 
		 * all the words of the command are popped from the stack, 
		 * and the result is pushed: the stack top changes by
		 * (1-wordIdx).
		 * Note that the estimates are not correct while the 
		 * command is being prepared and run, INST_EXPAND_STKTOP 
		 * is not stack-neutral in general. 
		 */

		TclEmitOpcode(INST_INVOKE_EXPANDED, envPtr);
		TclAdjustStackDepth((1-wordIdx), envPtr);
	    } else if (wordIdx > 0) {
		if (wordIdx <= 255) {
		    TclEmitInstInt1(INST_INVOKE_STK1, wordIdx, envPtr);
		} else {
		    TclEmitInstInt4(INST_INVOKE_STK4, wordIdx, envPtr);
		}
	    } 

	    /*
	     * Update the compilation environment structure and record the
	     * offsets of the source and code for the command.
	     */

	    finishCommand:
	    EnterCmdExtentData(envPtr, currCmdIndex, commandLength,
		    (envPtr->codeNext-envPtr->codeStart) - startCodeOffset);
	    isFirstCmd = 0;
	} /* end if parse.numWords > 0 */

	/*
	 * Advance to the next command in the script.
	 */
	
	next = parse.commandStart + parse.commandSize;
	bytesLeft -= (next - p);
	p = next;
	Tcl_FreeParse(&parse);
	gotParse = 0;
    } while (bytesLeft > 0);

    /*
     * If the source script yielded no instructions (e.g., if it was empty),
     * push an empty string as the command's result.
     *
     * WARNING: push an unshared object! If the script being compiled is a
     * shared empty string, it will otherwise be self-referential and cause
     * difficulties with literal management [Bugs 467523, 983660]. We used to
     * have special code in TclReleaseLiteral to handle this particular
     * self-reference, but now opt for avoiding its creation altogether.
     */
    
    if (envPtr->codeNext == entryCodeNext) {
	TclEmitPush(TclAddLiteralObj(envPtr, Tcl_NewObj(), NULL), envPtr);
    }
    
    envPtr->numSrcBytes = (p - script);
    Tcl_DStringFree(&ds);
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileTokens --
 *
 *	Given an array of tokens parsed from a Tcl command (e.g., the tokens
 *	that make up a word) this procedure emits instructions to evaluate
 *	the tokens and concatenate their values to form a single result
 *	value on the interpreter's runtime evaluation stack.
 *
 * Results:
 *	The return value is a standard Tcl result. If an error occurs, an
 *	error message is left in the interpreter's result.
 *	
 * Side effects:
 *	Instructions are added to envPtr to push and evaluate the tokens
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

void
TclCompileTokens(interp, tokenPtr, count, envPtr)
    Tcl_Interp *interp;		/* Used for error and status reporting. */
    Tcl_Token *tokenPtr;	/* Pointer to first in an array of tokens
				 * to compile. */
    int count;			/* Number of tokens to consider at tokenPtr.
				 * Must be at least 1. */
    CompileEnv *envPtr;		/* Holds the resulting instructions. */
{
    Tcl_DString textBuffer;	/* Holds concatenated chars from adjacent
				 * TCL_TOKEN_TEXT, TCL_TOKEN_BS tokens. */
    char buffer[TCL_UTF_MAX];
    CONST char *name, *p;
    int numObjsToConcat, nameBytes, localVarName, localVar;
    int length, i;
    unsigned char *entryCodeNext = envPtr->codeNext;

    Tcl_DStringInit(&textBuffer);
    numObjsToConcat = 0;
    for ( ;  count > 0;  count--, tokenPtr++) {
	switch (tokenPtr->type) {
	    case TCL_TOKEN_TEXT:
		Tcl_DStringAppend(&textBuffer, tokenPtr->start,
			tokenPtr->size);
		break;

	    case TCL_TOKEN_BS:
		length = Tcl_UtfBackslash(tokenPtr->start, (int *) NULL,
			buffer);
		Tcl_DStringAppend(&textBuffer, buffer, length);
		break;

	    case TCL_TOKEN_COMMAND:
		/*
		 * Push any accumulated chars appearing before the command.
		 */
		
		if (Tcl_DStringLength(&textBuffer) > 0) {
		    int literal;
		    
		    literal = TclRegisterLiteral(envPtr,
			    Tcl_DStringValue(&textBuffer),
			    Tcl_DStringLength(&textBuffer), /*onHeap*/ 0);
		    TclEmitPush(literal, envPtr);
		    numObjsToConcat++;
		    Tcl_DStringFree(&textBuffer);
		}
		
		TclCompileScript(interp, tokenPtr->start+1,
			tokenPtr->size-2, envPtr);
		numObjsToConcat++;
		break;

	    case TCL_TOKEN_VARIABLE:
		/*
		 * Push any accumulated chars appearing before the $<var>.
		 */
		
		if (Tcl_DStringLength(&textBuffer) > 0) {
		    int literal;
		    
		    literal = TclRegisterLiteral(envPtr,
			    Tcl_DStringValue(&textBuffer),
			    Tcl_DStringLength(&textBuffer), /*onHeap*/ 0);
		    TclEmitPush(literal, envPtr);
		    numObjsToConcat++;
		    Tcl_DStringFree(&textBuffer);
		}
		
		/*
		 * Determine how the variable name should be handled: if it contains 
		 * any namespace qualifiers it is not a local variable (localVarName=-1);
		 * if it looks like an array element and the token has a single component, 
		 * it should not be created here [Bug 569438] (localVarName=0); otherwise, 
		 * the local variable can safely be created (localVarName=1).
		 */
		
		name = tokenPtr[1].start;
		nameBytes = tokenPtr[1].size;
		localVarName = -1;
		if (envPtr->procPtr != NULL) {
		    localVarName = 1;
		    for (i = 0, p = name;  i < nameBytes;  i++, p++) {
			if ((*p == ':') && (i < (nameBytes-1))
			        && (*(p+1) == ':')) {
			    localVarName = -1;
			    break;
			} else if ((*p == '(')
			        && (tokenPtr->numComponents == 1) 
				&& (*(name + nameBytes - 1) == ')')) {
			    localVarName = 0;
			    break;
			}
		    }
		}

		/*
		 * Either push the variable's name, or find its index in
		 * the array of local variables in a procedure frame. 
		 */

		localVar = -1;
		if (localVarName != -1) {
		    localVar = TclFindCompiledLocal(name, nameBytes, 
			        localVarName, /*flags*/ 0, envPtr->procPtr);
		}
		if (localVar < 0) {
		    TclEmitPush(TclRegisterNewLiteral(envPtr, name, nameBytes),
			    envPtr); 
		}

		/*
		 * Emit instructions to load the variable.
		 */
		
		if (tokenPtr->numComponents == 1) {
		    if (localVar < 0) {
			TclEmitOpcode(INST_LOAD_SCALAR_STK, envPtr);
		    } else if (localVar <= 255) {
			TclEmitInstInt1(INST_LOAD_SCALAR1, localVar,
			        envPtr);
		    } else {
			TclEmitInstInt4(INST_LOAD_SCALAR4, localVar,
				envPtr);
		    }
		} else {
		    TclCompileTokens(interp, tokenPtr+2,
			    tokenPtr->numComponents-1, envPtr);
		    if (localVar < 0) {
			TclEmitOpcode(INST_LOAD_ARRAY_STK, envPtr);
		    } else if (localVar <= 255) {
			TclEmitInstInt1(INST_LOAD_ARRAY1, localVar,
			        envPtr);
		    } else {
			TclEmitInstInt4(INST_LOAD_ARRAY4, localVar,
			        envPtr);
		    }
		}
		numObjsToConcat++;
		count -= tokenPtr->numComponents;
		tokenPtr += tokenPtr->numComponents;
		break;

	    default:
		Tcl_Panic("Unexpected token type in TclCompileTokens");
	}
    }

    /*
     * Push any accumulated characters appearing at the end.
     */

    if (Tcl_DStringLength(&textBuffer) > 0) {
	int literal;

	literal = TclRegisterLiteral(envPtr, Tcl_DStringValue(&textBuffer),
	        Tcl_DStringLength(&textBuffer), /*onHeap*/ 0);
	TclEmitPush(literal, envPtr);
	numObjsToConcat++;
    }

    /*
     * If necessary, concatenate the parts of the word.
     */

    while (numObjsToConcat > 255) {
	TclEmitInstInt1(INST_CONCAT1, 255, envPtr);
	numObjsToConcat -= 254;	/* concat pushes 1 obj, the result */
    }
    if (numObjsToConcat > 1) {
	TclEmitInstInt1(INST_CONCAT1, numObjsToConcat, envPtr);
    }

    /*
     * If the tokens yielded no instructions, push an empty string.
     */
    
    if (envPtr->codeNext == entryCodeNext) {
	TclEmitPush(TclRegisterLiteral(envPtr, "", 0, /*onHeap*/ 0),
	        envPtr);
    }
    Tcl_DStringFree(&textBuffer);
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileCmdWord --
 *
 *	Given an array of parse tokens for a word containing one or more Tcl
 *	commands, emit inline instructions to execute them. This procedure
 *	differs from TclCompileTokens in that a simple word such as a loop
 *	body enclosed in braces is not just pushed as a string, but is
 *	itself parsed into tokens and compiled.
 *
 * Results:
 *	The return value is a standard Tcl result. If an error occurs, an
 *	error message is left in the interpreter's result.
 *	
 * Side effects:
 *	Instructions are added to envPtr to execute the tokens at runtime.
 *
 *----------------------------------------------------------------------
 */

void
TclCompileCmdWord(interp, tokenPtr, count, envPtr)
    Tcl_Interp *interp;		/* Used for error and status reporting. */
    Tcl_Token *tokenPtr;	/* Pointer to first in an array of tokens
				 * for a command word to compile inline. */
    int count;			/* Number of tokens to consider at tokenPtr.
				 * Must be at least 1. */
    CompileEnv *envPtr;		/* Holds the resulting instructions. */
{
    if ((count == 1) && (tokenPtr->type == TCL_TOKEN_TEXT)) {
	/*
	 * Handle the common case: if there is a single text token,
	 * compile it into an inline sequence of instructions.
	 */
    
	TclCompileScript(interp, tokenPtr->start, tokenPtr->size, envPtr);
    } else {
	/*
	 * Multiple tokens or the single token involves substitutions.
	 * Emit instructions to invoke the eval command procedure at
	 * runtime on the result of evaluating the tokens.
	 */

	TclCompileTokens(interp, tokenPtr, count, envPtr);
	TclEmitOpcode(INST_EVAL_STK, envPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileExprWords --
 *
 *	Given an array of parse tokens representing one or more words that
 *	contain a Tcl expression, emit inline instructions to execute the
 *	expression. This procedure differs from TclCompileExpr in that it
 *	supports Tcl's two-level substitution semantics for expressions that
 *	appear as command words.
 *
 * Results:
 *	The return value is a standard Tcl result. If an error occurs, an
 *	error message is left in the interpreter's result.
 *	
 * Side effects:
 *	Instructions are added to envPtr to execute the expression.
 *
 *----------------------------------------------------------------------
 */

void
TclCompileExprWords(interp, tokenPtr, numWords, envPtr)
    Tcl_Interp *interp;		/* Used for error and status reporting. */
    Tcl_Token *tokenPtr;	/* Points to first in an array of word
				 * tokens tokens for the expression to
				 * compile inline. */
    int numWords;		/* Number of word tokens starting at
				 * tokenPtr. Must be at least 1. Each word
				 * token contains one or more subtokens. */
    CompileEnv *envPtr;		/* Holds the resulting instructions. */
{
    Tcl_Token *wordPtr;
    int i, concatItems;

    /*
     * If the expression is a single word that doesn't require
     * substitutions, just compile its string into inline instructions.
     */

    if ((numWords == 1) && (tokenPtr->type == TCL_TOKEN_SIMPLE_WORD)) {
	CONST char *script = tokenPtr[1].start;
	int numBytes = tokenPtr[1].size;
	int savedNumCmds = envPtr->numCommands;
	unsigned int savedCodeNext = envPtr->codeNext - envPtr->codeStart;

	if (TclCompileExpr(interp, script, numBytes, envPtr) == TCL_OK) {
	    return;
	}
	envPtr->numCommands = savedNumCmds;
	envPtr->codeNext = envPtr->codeStart + savedCodeNext;
    }
   
    /*
     * Emit code to call the expr command proc at runtime. Concatenate the
     * (already substituted once) expr tokens with a space between each.
     */

    wordPtr = tokenPtr;
    for (i = 0;  i < numWords;  i++) {
	TclCompileTokens(interp, wordPtr+1, wordPtr->numComponents, envPtr);
	if (i < (numWords - 1)) {
	    TclEmitPush(TclRegisterLiteral(envPtr, " ", 1, /*onHeap*/ 0),
	            envPtr);
	}
	wordPtr += (wordPtr->numComponents + 1);
    }
    concatItems = 2*numWords - 1;
    while (concatItems > 255) {
	TclEmitInstInt1(INST_CONCAT1, 255, envPtr);
	concatItems -= 254;
    }
    if (concatItems > 1) {
	TclEmitInstInt1(INST_CONCAT1, concatItems, envPtr);
    }
    TclEmitOpcode(INST_EXPR_STK, envPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TclInitByteCodeObj --
 *
 *	Create a ByteCode structure and initialize it from a CompileEnv
 *	compilation environment structure. The ByteCode structure is
 *	smaller and contains just that information needed to execute
 *	the bytecode instructions resulting from compiling a Tcl script.
 *	The resulting structure is placed in the specified object.
 *
 * Results:
 *	A newly constructed ByteCode object is stored in the internal
 *	representation of the objPtr.
 *
 * Side effects:
 *	A single heap object is allocated to hold the new ByteCode structure
 *	and its code, object, command location, and aux data arrays. Note
 *	that "ownership" (i.e., the pointers to) the Tcl objects and aux
 *	data items will be handed over to the new ByteCode structure from
 *	the CompileEnv structure.
 *
 *----------------------------------------------------------------------
 */

void
TclInitByteCodeObj(objPtr, envPtr)
    Tcl_Obj *objPtr;		 /* Points object that should be
				  * initialized, and whose string rep
				  * contains the source code. */
    register CompileEnv *envPtr; /* Points to the CompileEnv structure from
				  * which to create a ByteCode structure. */
{
    register ByteCode *codePtr;
    size_t codeBytes, objArrayBytes, exceptArrayBytes, cmdLocBytes;
    size_t auxDataArrayBytes, structureSize;
    register unsigned char *p;
#ifdef TCL_COMPILE_DEBUG
    unsigned char *nextPtr;
#endif
    int numLitObjects = envPtr->literalArrayNext;
    Namespace *namespacePtr;
    int i;
    Interp *iPtr;

    iPtr = envPtr->iPtr;

    codeBytes = (envPtr->codeNext - envPtr->codeStart);
    objArrayBytes = (envPtr->literalArrayNext * sizeof(Tcl_Obj *));
    exceptArrayBytes = (envPtr->exceptArrayNext * sizeof(ExceptionRange));
    auxDataArrayBytes = (envPtr->auxDataArrayNext * sizeof(AuxData));
    cmdLocBytes = GetCmdLocEncodingSize(envPtr);
    
    /*
     * Compute the total number of bytes needed for this bytecode.
     */

    structureSize = sizeof(ByteCode);
    structureSize += TCL_ALIGN(codeBytes);        /* align object array */
    structureSize += TCL_ALIGN(objArrayBytes);    /* align exc range arr */
    structureSize += TCL_ALIGN(exceptArrayBytes); /* align AuxData array */
    structureSize += auxDataArrayBytes;
    structureSize += cmdLocBytes;

    if (envPtr->iPtr->varFramePtr != NULL) {
        namespacePtr = envPtr->iPtr->varFramePtr->nsPtr;
    } else {
        namespacePtr = envPtr->iPtr->globalNsPtr;
    }
    
    p = (unsigned char *) ckalloc((size_t) structureSize);
    codePtr = (ByteCode *) p;
    codePtr->interpHandle = TclHandlePreserve(iPtr->handle);
    codePtr->compileEpoch = iPtr->compileEpoch;
    codePtr->nsPtr = namespacePtr;
    codePtr->nsEpoch = namespacePtr->resolverEpoch;
    codePtr->refCount = 1;
    codePtr->flags = 0;
    codePtr->source = envPtr->source;
    codePtr->procPtr = envPtr->procPtr;

    codePtr->numCommands = envPtr->numCommands;
    codePtr->numSrcBytes = envPtr->numSrcBytes;
    codePtr->numCodeBytes = codeBytes;
    codePtr->numLitObjects = numLitObjects;
    codePtr->numExceptRanges = envPtr->exceptArrayNext;
    codePtr->numAuxDataItems = envPtr->auxDataArrayNext;
    codePtr->numCmdLocBytes = cmdLocBytes;
    codePtr->maxExceptDepth = envPtr->maxExceptDepth;
    codePtr->maxStackDepth = envPtr->maxStackDepth;

    p += sizeof(ByteCode);
    codePtr->codeStart = p;
    memcpy((VOID *) p, (VOID *) envPtr->codeStart, (size_t) codeBytes);
    
    p += TCL_ALIGN(codeBytes);	      /* align object array */
    codePtr->objArrayPtr = (Tcl_Obj **) p;
    for (i = 0;  i < numLitObjects;  i++) {
	codePtr->objArrayPtr[i] = envPtr->literalArrayPtr[i].objPtr;
    }

    p += TCL_ALIGN(objArrayBytes);    /* align exception range array */
    if (exceptArrayBytes > 0) {
	codePtr->exceptArrayPtr = (ExceptionRange *) p;
	memcpy((VOID *) p, (VOID *) envPtr->exceptArrayPtr,
	        (size_t) exceptArrayBytes);
    } else {
	codePtr->exceptArrayPtr = NULL;
    }
    
    p += TCL_ALIGN(exceptArrayBytes); /* align AuxData array */
    if (auxDataArrayBytes > 0) {
	codePtr->auxDataArrayPtr = (AuxData *) p;
	memcpy((VOID *) p, (VOID *) envPtr->auxDataArrayPtr,
	        (size_t) auxDataArrayBytes);
    } else {
	codePtr->auxDataArrayPtr = NULL;
    }

    p += auxDataArrayBytes;
#ifndef TCL_COMPILE_DEBUG
    EncodeCmdLocMap(envPtr, codePtr, (unsigned char *) p);
#else
    nextPtr = EncodeCmdLocMap(envPtr, codePtr, (unsigned char *) p);
    if (((size_t)(nextPtr - p)) != cmdLocBytes) {	
	Tcl_Panic("TclInitByteCodeObj: encoded cmd location bytes %d != expected size %d\n", (nextPtr - p), cmdLocBytes);
    }
#endif
    
    /*
     * Record various compilation-related statistics about the new ByteCode
     * structure. Don't include overhead for statistics-related fields.
     */

#ifdef TCL_COMPILE_STATS
    codePtr->structureSize = structureSize
	    - (sizeof(size_t) + sizeof(Tcl_Time));
    Tcl_GetTime(&(codePtr->createTime));
    
    RecordByteCodeStats(codePtr);
#endif /* TCL_COMPILE_STATS */
    
    /*
     * Free the old internal rep then convert the object to a
     * bytecode object by making its internal rep point to the just
     * compiled ByteCode.
     */
	    
    TclFreeIntRep(objPtr);
    objPtr->internalRep.otherValuePtr = (VOID *) codePtr;
    objPtr->typePtr = &tclByteCodeType;
}

/*
 *----------------------------------------------------------------------
 *
 * TclFindCompiledLocal --
 *
 *	This procedure is called at compile time to look up and optionally
 *	allocate an entry ("slot") for a variable in a procedure's array of
 *	local variables. If the variable's name is NULL, a new temporary
 *	variable is always created. (Such temporary variables can only be
 *	referenced using their slot index.)
 *
 * Results:
 *	If create is 0 and the name is non-NULL, then if the variable is
 *	found, the index of its entry in the procedure's array of local
 *	variables is returned; otherwise -1 is returned. If name is NULL,
 *	the index of a new temporary variable is returned. Finally, if
 *	create is 1 and name is non-NULL, the index of a new entry is
 *	returned.
 *
 * Side effects:
 *	Creates and registers a new local variable if create is 1 and
 *	the variable is unknown, or if the name is NULL.
 *
 *----------------------------------------------------------------------
 */

int
TclFindCompiledLocal(name, nameBytes, create, flags, procPtr)
    register CONST char *name;	/* Points to first character of the name of
				 * a scalar or array variable. If NULL, a
				 * temporary var should be created. */
    int nameBytes;		/* Number of bytes in the name. */
    int create;			/* If 1, allocate a local frame entry for
				 * the variable if it is new. */
    int flags;			/* Flag bits for the compiled local if
				 * created. Only VAR_SCALAR, VAR_ARRAY, and
				 * VAR_LINK make sense. */
    register Proc *procPtr;	/* Points to structure describing procedure
				 * containing the variable reference. */
{
    register CompiledLocal *localPtr;
    int localVar = -1;
    register int i;

    /*
     * If not creating a temporary, does a local variable of the specified
     * name already exist?
     */

    if (name != NULL) {	
	int localCt = procPtr->numCompiledLocals;
	localPtr = procPtr->firstLocalPtr;
	for (i = 0;  i < localCt;  i++) {
	    if (!TclIsVarTemporary(localPtr)) {
		char *localName = localPtr->name;
		if ((nameBytes == localPtr->nameLength)
	                && (strncmp(name, localName, (unsigned) nameBytes) == 0)) {
		    return i;
		}
	    }
	    localPtr = localPtr->nextPtr;
	}
    }

    /*
     * Create a new variable if appropriate.
     */
    
    if (create || (name == NULL)) {
	localVar = procPtr->numCompiledLocals;
	localPtr = (CompiledLocal *) ckalloc((unsigned) 
	        (sizeof(CompiledLocal) - sizeof(localPtr->name)
		+ nameBytes+1));
	if (procPtr->firstLocalPtr == NULL) {
	    procPtr->firstLocalPtr = procPtr->lastLocalPtr = localPtr;
	} else {
	    procPtr->lastLocalPtr->nextPtr = localPtr;
	    procPtr->lastLocalPtr = localPtr;
	}
	localPtr->nextPtr = NULL;
	localPtr->nameLength = nameBytes;
	localPtr->frameIndex = localVar;
	localPtr->flags = flags | VAR_UNDEFINED;
	if (name == NULL) {
	    localPtr->flags |= VAR_TEMPORARY;
	}
	localPtr->defValuePtr = NULL;
	localPtr->resolveInfo = NULL;

	if (name != NULL) {
	    memcpy((VOID *) localPtr->name, (VOID *) name,
	            (size_t) nameBytes);
	}
	localPtr->name[nameBytes] = '\0';
	procPtr->numCompiledLocals++;
    }
    return localVar;
}

/*
 *----------------------------------------------------------------------
 *
 * TclInitCompiledLocals --
 *
 *	This routine is invoked in order to initialize the compiled
 *	locals table for a new call frame.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May invoke various name resolvers in order to determine which
 *	variables are being referenced at runtime.
 *
 *----------------------------------------------------------------------
 */

void
TclInitCompiledLocals(interp, framePtr, nsPtr)
    Tcl_Interp *interp;		/* Current interpreter. */
    CallFrame *framePtr;	/* Call frame to initialize. */
    Namespace *nsPtr;		/* Pointer to current namespace. */
{
    register CompiledLocal *localPtr;
    Interp *iPtr = (Interp*) interp;
    Tcl_ResolvedVarInfo *vinfo, *resVarInfo;
    Var *varPtr = framePtr->compiledLocals;
    Var *resolvedVarPtr;
    ResolverScheme *resPtr;
    int result;

    /*
     * Initialize the array of local variables stored in the call frame.
     * Some variables may have special resolution rules.  In that case,
     * we call their "resolver" procs to get our hands on the variable,
     * and we make the compiled local a link to the real variable.
     */

    for (localPtr = framePtr->procPtr->firstLocalPtr;
	 localPtr != NULL;
	 localPtr = localPtr->nextPtr) {

	/*
	 * Check to see if this local is affected by namespace or
	 * interp resolvers.  The resolver to use is cached for the
	 * next invocation of the procedure.
	 */

	if (!(localPtr->flags & (VAR_ARGUMENT|VAR_TEMPORARY|VAR_RESOLVED))
		&& (nsPtr->compiledVarResProc || iPtr->resolverPtr)) {
	    resPtr = iPtr->resolverPtr;

	    if (nsPtr->compiledVarResProc) {
		result = (*nsPtr->compiledVarResProc)(nsPtr->interp,
			localPtr->name, localPtr->nameLength,
			(Tcl_Namespace *) nsPtr, &vinfo);
	    } else {
		result = TCL_CONTINUE;
	    }

	    while ((result == TCL_CONTINUE) && resPtr) {
		if (resPtr->compiledVarResProc) {
		    result = (*resPtr->compiledVarResProc)(nsPtr->interp,
			    localPtr->name, localPtr->nameLength,
			    (Tcl_Namespace *) nsPtr, &vinfo);
		}
		resPtr = resPtr->nextPtr;
	    }
	    if (result == TCL_OK) {
		localPtr->resolveInfo = vinfo;
		localPtr->flags |= VAR_RESOLVED;
	    }
	}

	/*
	 * Now invoke the resolvers to determine the exact variables that
	 * should be used.
	 */

        resVarInfo = localPtr->resolveInfo;
        resolvedVarPtr = NULL;

        if (resVarInfo && resVarInfo->fetchProc) {
            resolvedVarPtr = (Var*) (*resVarInfo->fetchProc)(interp,
		    resVarInfo);
        }

        if (resolvedVarPtr) {
	    varPtr->name = localPtr->name; /* will be just '\0' if temp var */
	    varPtr->nsPtr = NULL;
	    varPtr->hPtr = NULL;
	    varPtr->refCount = 0;
	    varPtr->tracePtr = NULL;
	    varPtr->searchPtr = NULL;
	    varPtr->flags = 0;
            TclSetVarLink(varPtr);
            varPtr->value.linkPtr = resolvedVarPtr;
            resolvedVarPtr->refCount++;
        } else {
	    varPtr->value.objPtr = NULL;
	    varPtr->name = localPtr->name; /* will be just '\0' if temp var */
	    varPtr->nsPtr = NULL;
	    varPtr->hPtr = NULL;
	    varPtr->refCount = 0;
	    varPtr->tracePtr = NULL;
	    varPtr->searchPtr = NULL;
	    varPtr->flags = localPtr->flags;
        }
	varPtr++;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclExpandCodeArray --
 *
 *	Procedure that uses malloc to allocate more storage for a
 *	CompileEnv's code array.
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	The byte code array in *envPtr is reallocated to a new array of
 *	double the size, and if envPtr->mallocedCodeArray is non-zero the
 *	old array is freed. Byte codes are copied from the old array to the
 *	new one.
 *
 *----------------------------------------------------------------------
 */

void
TclExpandCodeArray(envArgPtr)
    void *envArgPtr;		/* Points to the CompileEnv whose code array
				 * must be enlarged. */
{
    CompileEnv *envPtr = (CompileEnv*) envArgPtr;	/* Points to the CompileEnv whose code array
							 * must be enlarged. */

    /*
     * envPtr->codeNext is equal to envPtr->codeEnd. The currently defined
     * code bytes are stored between envPtr->codeStart and
     * (envPtr->codeNext - 1) [inclusive].
     */
    
    size_t currBytes = (envPtr->codeNext - envPtr->codeStart);
    size_t newBytes  = 2*(envPtr->codeEnd  - envPtr->codeStart);
    unsigned char *newPtr = (unsigned char *) ckalloc((unsigned) newBytes);

    /*
     * Copy from old code array to new, free old code array if needed, and
     * mark new code array as malloced.
     */
 
    memcpy((VOID *) newPtr, (VOID *) envPtr->codeStart, currBytes);
    if (envPtr->mallocedCodeArray) {
        ckfree((char *) envPtr->codeStart);
    }
    envPtr->codeStart = newPtr;
    envPtr->codeNext = (newPtr + currBytes);
    envPtr->codeEnd  = (newPtr + newBytes);
    envPtr->mallocedCodeArray = 1;
}

/*
 *----------------------------------------------------------------------
 *
 * EnterCmdStartData --
 *
 *	Registers the starting source and bytecode location of a
 *	command. This information is used at runtime to map between
 *	instruction pc and source locations.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Inserts source and code location information into the compilation
 *	environment envPtr for the command at index cmdIndex. The
 *	compilation environment's CmdLocation array is grown if necessary.
 *
 *----------------------------------------------------------------------
 */

static void
EnterCmdStartData(envPtr, cmdIndex, srcOffset, codeOffset)
    CompileEnv *envPtr;		/* Points to the compilation environment
				 * structure in which to enter command
				 * location information. */
    int cmdIndex;		/* Index of the command whose start data
				 * is being set. */
    int srcOffset;		/* Offset of first char of the command. */
    int codeOffset;		/* Offset of first byte of command code. */
{
    CmdLocation *cmdLocPtr;
    
    if ((cmdIndex < 0) || (cmdIndex >= envPtr->numCommands)) {
	Tcl_Panic("EnterCmdStartData: bad command index %d\n", cmdIndex);
    }
    
    if (cmdIndex >= envPtr->cmdMapEnd) {
	/*
	 * Expand the command location array by allocating more storage from
	 * the heap. The currently allocated CmdLocation entries are stored
	 * from cmdMapPtr[0] up to cmdMapPtr[envPtr->cmdMapEnd] (inclusive).
	 */

	size_t currElems = envPtr->cmdMapEnd;
	size_t newElems  = 2*currElems;
	size_t currBytes = currElems * sizeof(CmdLocation);
	size_t newBytes  = newElems  * sizeof(CmdLocation);
	CmdLocation *newPtr = (CmdLocation *) ckalloc((unsigned) newBytes);
	
	/*
	 * Copy from old command location array to new, free old command
	 * location array if needed, and mark new array as malloced.
	 */
	
	memcpy((VOID *) newPtr, (VOID *) envPtr->cmdMapPtr, currBytes);
	if (envPtr->mallocedCmdMap) {
	    ckfree((char *) envPtr->cmdMapPtr);
	}
	envPtr->cmdMapPtr = (CmdLocation *) newPtr;
	envPtr->cmdMapEnd = newElems;
	envPtr->mallocedCmdMap = 1;
    }

    if (cmdIndex > 0) {
	if (codeOffset < envPtr->cmdMapPtr[cmdIndex-1].codeOffset) {
	    Tcl_Panic("EnterCmdStartData: cmd map not sorted by code offset");
	}
    }

    cmdLocPtr = &(envPtr->cmdMapPtr[cmdIndex]);
    cmdLocPtr->codeOffset = codeOffset;
    cmdLocPtr->srcOffset = srcOffset;
    cmdLocPtr->numSrcBytes = -1;
    cmdLocPtr->numCodeBytes = -1;
}

/*
 *----------------------------------------------------------------------
 *
 * EnterCmdExtentData --
 *
 *	Registers the source and bytecode length for a command. This
 *	information is used at runtime to map between instruction pc and
 *	source locations.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Inserts source and code length information into the compilation
 *	environment envPtr for the command at index cmdIndex. Starting
 *	source and bytecode information for the command must already
 *	have been registered.
 *
 *----------------------------------------------------------------------
 */

static void
EnterCmdExtentData(envPtr, cmdIndex, numSrcBytes, numCodeBytes)
    CompileEnv *envPtr;		/* Points to the compilation environment
				 * structure in which to enter command
				 * location information. */
    int cmdIndex;		/* Index of the command whose source and
				 * code length data is being set. */
    int numSrcBytes;		/* Number of command source chars. */
    int numCodeBytes;		/* Offset of last byte of command code. */
{
    CmdLocation *cmdLocPtr;

    if ((cmdIndex < 0) || (cmdIndex >= envPtr->numCommands)) {
	Tcl_Panic("EnterCmdExtentData: bad command index %d\n", cmdIndex);
    }
    
    if (cmdIndex > envPtr->cmdMapEnd) {
	Tcl_Panic("EnterCmdExtentData: missing start data for command %d\n",
	        cmdIndex);
    }

    cmdLocPtr = &(envPtr->cmdMapPtr[cmdIndex]);
    cmdLocPtr->numSrcBytes = numSrcBytes;
    cmdLocPtr->numCodeBytes = numCodeBytes;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCreateExceptRange --
 *
 *	Procedure that allocates and initializes a new ExceptionRange
 *	structure of the specified kind in a CompileEnv.
 *
 * Results:
 *	Returns the index for the newly created ExceptionRange.
 *
 * Side effects:
 *	If there is not enough room in the CompileEnv's ExceptionRange
 *	array, the array in expanded: a new array of double the size is
 *	allocated, if envPtr->mallocedExceptArray is non-zero the old
 *	array is freed, and ExceptionRange entries are copied from the old
 *	array to the new one.
 *
 *----------------------------------------------------------------------
 */

int
TclCreateExceptRange(type, envPtr)
    ExceptionRangeType type;	/* The kind of ExceptionRange desired. */
    register CompileEnv *envPtr;/* Points to CompileEnv for which to
				 * create a new ExceptionRange structure. */
{
    register ExceptionRange *rangePtr;
    int index = envPtr->exceptArrayNext;
    
    if (index >= envPtr->exceptArrayEnd) {
        /*
	 * Expand the ExceptionRange array. The currently allocated entries
	 * are stored between elements 0 and (envPtr->exceptArrayNext - 1)
	 * [inclusive].
	 */
	
	size_t currBytes =
	        envPtr->exceptArrayNext * sizeof(ExceptionRange);
	int newElems = 2*envPtr->exceptArrayEnd;
	size_t newBytes = newElems * sizeof(ExceptionRange);
	ExceptionRange *newPtr = (ExceptionRange *)
	        ckalloc((unsigned) newBytes);
	
	/*
	 * Copy from old ExceptionRange array to new, free old
	 * ExceptionRange array if needed, and mark the new ExceptionRange
	 * array as malloced.
	 */
	
	memcpy((VOID *) newPtr, (VOID *) envPtr->exceptArrayPtr,
	        currBytes);
	if (envPtr->mallocedExceptArray) {
	    ckfree((char *) envPtr->exceptArrayPtr);
	}
	envPtr->exceptArrayPtr = (ExceptionRange *) newPtr;
	envPtr->exceptArrayEnd = newElems;
	envPtr->mallocedExceptArray = 1;
    }
    envPtr->exceptArrayNext++;
    
    rangePtr = &(envPtr->exceptArrayPtr[index]);
    rangePtr->type = type;
    rangePtr->nestingLevel = envPtr->exceptDepth;
    rangePtr->codeOffset = -1;
    rangePtr->numCodeBytes = -1;
    rangePtr->breakOffset = -1;
    rangePtr->continueOffset = -1;
    rangePtr->catchOffset = -1;
    return index;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCreateAuxData --
 *
 *	Procedure that allocates and initializes a new AuxData structure in
 *	a CompileEnv's array of compilation auxiliary data records. These
 *	AuxData records hold information created during compilation by
 *	CompileProcs and used by instructions during execution.
 *
 * Results:
 *	Returns the index for the newly created AuxData structure.
 *
 * Side effects:
 *	If there is not enough room in the CompileEnv's AuxData array,
 *	the AuxData array in expanded: a new array of double the size
 *	is allocated, if envPtr->mallocedAuxDataArray is non-zero
 *	the old array is freed, and AuxData entries are copied from
 *	the old array to the new one.
 *
 *----------------------------------------------------------------------
 */

int
TclCreateAuxData(clientData, typePtr, envPtr)
    ClientData clientData;	/* The compilation auxiliary data to store
				 * in the new aux data record. */
    AuxDataType *typePtr;	/* Pointer to the type to attach to this AuxData */
    register CompileEnv *envPtr;/* Points to the CompileEnv for which a new
				 * aux data structure is to be allocated. */
{
    int index;			/* Index for the new AuxData structure. */
    register AuxData *auxDataPtr;
    				/* Points to the new AuxData structure */
    
    index = envPtr->auxDataArrayNext;
    if (index >= envPtr->auxDataArrayEnd) {
        /*
	 * Expand the AuxData array. The currently allocated entries are
	 * stored between elements 0 and (envPtr->auxDataArrayNext - 1)
	 * [inclusive].
	 */
	
	size_t currBytes = envPtr->auxDataArrayNext * sizeof(AuxData);
	int newElems = 2*envPtr->auxDataArrayEnd;
	size_t newBytes = newElems * sizeof(AuxData);
	AuxData *newPtr = (AuxData *) ckalloc((unsigned) newBytes);
	
	/*
	 * Copy from old AuxData array to new, free old AuxData array if
	 * needed, and mark the new AuxData array as malloced.
	 */
	
	memcpy((VOID *) newPtr, (VOID *) envPtr->auxDataArrayPtr,
	        currBytes);
	if (envPtr->mallocedAuxDataArray) {
	    ckfree((char *) envPtr->auxDataArrayPtr);
	}
	envPtr->auxDataArrayPtr = newPtr;
	envPtr->auxDataArrayEnd = newElems;
	envPtr->mallocedAuxDataArray = 1;
    }
    envPtr->auxDataArrayNext++;
    
    auxDataPtr = &(envPtr->auxDataArrayPtr[index]);
    auxDataPtr->clientData = clientData;
    auxDataPtr->type = typePtr;
    return index;
}

/*
 *----------------------------------------------------------------------
 *
 * TclInitJumpFixupArray --
 *
 *	Initializes a JumpFixupArray structure to hold some number of
 *	jump fixup entries.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The JumpFixupArray structure is initialized.
 *
 *----------------------------------------------------------------------
 */

void
TclInitJumpFixupArray(fixupArrayPtr)
    register JumpFixupArray *fixupArrayPtr;
				 /* Points to the JumpFixupArray structure
				  * to initialize. */
{
    fixupArrayPtr->fixup = fixupArrayPtr->staticFixupSpace;
    fixupArrayPtr->next = 0;
    fixupArrayPtr->end = (JUMPFIXUP_INIT_ENTRIES - 1);
    fixupArrayPtr->mallocedArray = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TclExpandJumpFixupArray --
 *
 *	Procedure that uses malloc to allocate more storage for a
 *      jump fixup array.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The jump fixup array in *fixupArrayPtr is reallocated to a new array
 *	of double the size, and if fixupArrayPtr->mallocedArray is non-zero
 *	the old array is freed. Jump fixup structures are copied from the
 *	old array to the new one.
 *
 *----------------------------------------------------------------------
 */

void
TclExpandJumpFixupArray(fixupArrayPtr)
    register JumpFixupArray *fixupArrayPtr;
				 /* Points to the JumpFixupArray structure
				  * to enlarge. */
{
    /*
     * The currently allocated jump fixup entries are stored from fixup[0]
     * up to fixup[fixupArrayPtr->fixupNext] (*not* inclusive). We assume
     * fixupArrayPtr->fixupNext is equal to fixupArrayPtr->fixupEnd.
     */

    size_t currBytes = fixupArrayPtr->next * sizeof(JumpFixup);
    int newElems = 2*(fixupArrayPtr->end + 1);
    size_t newBytes = newElems * sizeof(JumpFixup);
    JumpFixup *newPtr = (JumpFixup *) ckalloc((unsigned) newBytes);

    /*
     * Copy from the old array to new, free the old array if needed,
     * and mark the new array as malloced.
     */
 
    memcpy((VOID *) newPtr, (VOID *) fixupArrayPtr->fixup, currBytes);
    if (fixupArrayPtr->mallocedArray) {
	ckfree((char *) fixupArrayPtr->fixup);
    }
    fixupArrayPtr->fixup = (JumpFixup *) newPtr;
    fixupArrayPtr->end = newElems;
    fixupArrayPtr->mallocedArray = 1;
}

/*
 *----------------------------------------------------------------------
 *
 * TclFreeJumpFixupArray --
 *
 *	Free any storage allocated in a jump fixup array structure.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Allocated storage in the JumpFixupArray structure is freed.
 *
 *----------------------------------------------------------------------
 */

void
TclFreeJumpFixupArray(fixupArrayPtr)
    register JumpFixupArray *fixupArrayPtr;
				 /* Points to the JumpFixupArray structure
				  * to free. */
{
    if (fixupArrayPtr->mallocedArray) {
	ckfree((char *) fixupArrayPtr->fixup);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclEmitForwardJump --
 *
 *	Procedure to emit a two-byte forward jump of kind "jumpType". Since
 *	the jump may later have to be grown to five bytes if the jump target
 *	is more than, say, 127 bytes away, this procedure also initializes a
 *	JumpFixup record with information about the jump. 
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The JumpFixup record pointed to by "jumpFixupPtr" is initialized
 *	with information needed later if the jump is to be grown. Also,
 *	a two byte jump of the designated type is emitted at the current
 *	point in the bytecode stream.
 *
 *----------------------------------------------------------------------
 */

void
TclEmitForwardJump(envPtr, jumpType, jumpFixupPtr)
    CompileEnv *envPtr;		/* Points to the CompileEnv structure that
				 * holds the resulting instruction. */
    TclJumpType jumpType;	/* Indicates the kind of jump: if true or
				 * false or unconditional. */
    JumpFixup *jumpFixupPtr;	/* Points to the JumpFixup structure to
				 * initialize with information about this
				 * forward jump. */
{
    /*
     * Initialize the JumpFixup structure:
     *    - codeOffset is offset of first byte of jump below
     *    - cmdIndex is index of the command after the current one
     *    - exceptIndex is the index of the first ExceptionRange after
     *      the current one.
     */
    
    jumpFixupPtr->jumpType = jumpType;
    jumpFixupPtr->codeOffset = (envPtr->codeNext - envPtr->codeStart);
    jumpFixupPtr->cmdIndex = envPtr->numCommands;
    jumpFixupPtr->exceptIndex = envPtr->exceptArrayNext;
    
    switch (jumpType) {
    case TCL_UNCONDITIONAL_JUMP:
	TclEmitInstInt1(INST_JUMP1, 0, envPtr);
	break;
    case TCL_TRUE_JUMP:
	TclEmitInstInt1(INST_JUMP_TRUE1, 0, envPtr);
	break;
    default:
	TclEmitInstInt1(INST_JUMP_FALSE1, 0, envPtr);
	break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclFixupForwardJump --
 *
 *	Procedure that updates a previously-emitted forward jump to jump
 *	a specified number of bytes, "jumpDist". If necessary, the jump is
 *      grown from two to five bytes; this is done if the jump distance is
 *	greater than "distThreshold" (normally 127 bytes). The jump is
 *	described by a JumpFixup record previously initialized by
 *	TclEmitForwardJump.
 *
 * Results:
 *	1 if the jump was grown and subsequent instructions had to be moved;
 *	otherwise 0. This result is returned to allow callers to update
 *	any additional code offsets they may hold.
 *
 * Side effects:
 *	The jump may be grown and subsequent instructions moved. If this
 *	happens, the code offsets for any commands and any ExceptionRange
 *	records	between the jump and the current code address will be
 *	updated to reflect the moved code. Also, the bytecode instruction
 *	array in the CompileEnv structure may be grown and reallocated.
 *
 *----------------------------------------------------------------------
 */

int
TclFixupForwardJump(envPtr, jumpFixupPtr, jumpDist, distThreshold)
    CompileEnv *envPtr;		/* Points to the CompileEnv structure that
				 * holds the resulting instruction. */
    JumpFixup *jumpFixupPtr;    /* Points to the JumpFixup structure that
				 * describes the forward jump. */
    int jumpDist;		/* Jump distance to set in jump
				 * instruction. */
    int distThreshold;		/* Maximum distance before the two byte
				 * jump is grown to five bytes. */
{
    unsigned char *jumpPc, *p;
    int firstCmd, lastCmd, firstRange, lastRange, k;
    unsigned int numBytes;
    
    if (jumpDist <= distThreshold) {
	jumpPc = (envPtr->codeStart + jumpFixupPtr->codeOffset);
	switch (jumpFixupPtr->jumpType) {
	case TCL_UNCONDITIONAL_JUMP:
	    TclUpdateInstInt1AtPc(INST_JUMP1, jumpDist, jumpPc);
	    break;
	case TCL_TRUE_JUMP:
	    TclUpdateInstInt1AtPc(INST_JUMP_TRUE1, jumpDist, jumpPc);
	    break;
	default:
	    TclUpdateInstInt1AtPc(INST_JUMP_FALSE1, jumpDist, jumpPc);
	    break;
	}
	return 0;
    }

    /*
     * We must grow the jump then move subsequent instructions down.
     * Note that if we expand the space for generated instructions,
     * code addresses might change; be careful about updating any of
     * these addresses held in variables.
     */
    
    if ((envPtr->codeNext + 3) > envPtr->codeEnd) {
        TclExpandCodeArray(envPtr);
    }
    jumpPc = (envPtr->codeStart + jumpFixupPtr->codeOffset);
    for (numBytes = envPtr->codeNext-jumpPc-2, p = jumpPc+2+numBytes-1;
	    numBytes > 0;  numBytes--, p--) {
	p[3] = p[0];
    }
    envPtr->codeNext += 3;
    jumpDist += 3;
    switch (jumpFixupPtr->jumpType) {
    case TCL_UNCONDITIONAL_JUMP:
	TclUpdateInstInt4AtPc(INST_JUMP4, jumpDist, jumpPc);
	break;
    case TCL_TRUE_JUMP:
	TclUpdateInstInt4AtPc(INST_JUMP_TRUE4, jumpDist, jumpPc);
	break;
    default:
	TclUpdateInstInt4AtPc(INST_JUMP_FALSE4, jumpDist, jumpPc);
	break;
    }
    
    /*
     * Adjust the code offsets for any commands and any ExceptionRange
     * records between the jump and the current code address.
     */
    
    firstCmd = jumpFixupPtr->cmdIndex;
    lastCmd  = (envPtr->numCommands - 1);
    if (firstCmd < lastCmd) {
	for (k = firstCmd;  k <= lastCmd;  k++) {
	    (envPtr->cmdMapPtr[k]).codeOffset += 3;
	}
    }
    
    firstRange = jumpFixupPtr->exceptIndex;
    lastRange  = (envPtr->exceptArrayNext - 1);
    for (k = firstRange;  k <= lastRange;  k++) {
	ExceptionRange *rangePtr = &(envPtr->exceptArrayPtr[k]);
	rangePtr->codeOffset += 3;
	
	switch (rangePtr->type) {
	case LOOP_EXCEPTION_RANGE:
	    rangePtr->breakOffset += 3;
	    if (rangePtr->continueOffset != -1) {
		rangePtr->continueOffset += 3;
	    }
	    break;
	case CATCH_EXCEPTION_RANGE:
	    rangePtr->catchOffset += 3;
	    break;
	default:
	    Tcl_Panic("TclFixupForwardJump: bad ExceptionRange type %d\n",
	            rangePtr->type);
	}
    }
    return 1;			/* the jump was grown */
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetInstructionTable --
 *
 *  Returns a pointer to the table describing Tcl bytecode instructions.
 *  This procedure is defined so that clients can access the pointer from
 *  outside the TCL DLLs.
 *
 * Results:
 *	Returns a pointer to the global instruction table, same as the
 *	expression (&tclInstructionTable[0]).
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void * /* == InstructionDesc* == */
TclGetInstructionTable()
{
    return &tclInstructionTable[0];
}

/*
 *--------------------------------------------------------------
 *
 * TclRegisterAuxDataType --
 *
 *	This procedure is called to register a new AuxData type
 *	in the table of all AuxData types supported by Tcl.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The type is registered in the AuxData type table. If there was already
 *	a type with the same name as in typePtr, it is replaced with the
 *	new type.
 *
 *--------------------------------------------------------------
 */

void
TclRegisterAuxDataType(typePtr)
    AuxDataType *typePtr;	/* Information about object type;
                             * storage must be statically
                             * allocated (must live forever). */
{
    register Tcl_HashEntry *hPtr;
    int new;

    Tcl_MutexLock(&tableMutex);
    if (!auxDataTypeTableInitialized) {
        TclInitAuxDataTypeTable();
    }

    /*
     * If there's already a type with the given name, remove it.
     */

    hPtr = Tcl_FindHashEntry(&auxDataTypeTable, typePtr->name);
    if (hPtr != (Tcl_HashEntry *) NULL) {
        Tcl_DeleteHashEntry(hPtr);
    }

    /*
     * Now insert the new object type.
     */

    hPtr = Tcl_CreateHashEntry(&auxDataTypeTable, typePtr->name, &new);
    if (new) {
        Tcl_SetHashValue(hPtr, typePtr);
    }
    Tcl_MutexUnlock(&tableMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetAuxDataType --
 *
 *	This procedure looks up an Auxdata type by name.
 *
 * Results:
 *	If an AuxData type with name matching "typeName" is found, a pointer
 *	to its AuxDataType structure is returned; otherwise, NULL is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

AuxDataType *
TclGetAuxDataType(typeName)
    char *typeName;		/* Name of AuxData type to look up. */
{
    register Tcl_HashEntry *hPtr;
    AuxDataType *typePtr = NULL;

    Tcl_MutexLock(&tableMutex);
    if (!auxDataTypeTableInitialized) {
        TclInitAuxDataTypeTable();
    }

    hPtr = Tcl_FindHashEntry(&auxDataTypeTable, typeName);
    if (hPtr != (Tcl_HashEntry *) NULL) {
        typePtr = (AuxDataType *) Tcl_GetHashValue(hPtr);
    }
    Tcl_MutexUnlock(&tableMutex);

    return typePtr;
}

/*
 *--------------------------------------------------------------
 *
 * TclInitAuxDataTypeTable --
 *
 *	This procedure is invoked to perform once-only initialization of
 *	the AuxData type table. It also registers the AuxData types defined in 
 *	this file.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Initializes the table of defined AuxData types "auxDataTypeTable" with
 *	builtin AuxData types defined in this file.
 *
 *--------------------------------------------------------------
 */

void
TclInitAuxDataTypeTable()
{
    /*
     * The table mutex must already be held before this routine is invoked.
     */

    auxDataTypeTableInitialized = 1;
    Tcl_InitHashTable(&auxDataTypeTable, TCL_STRING_KEYS);

    /*
     * There is only one AuxData type at this time, so register it here.
     */

    TclRegisterAuxDataType(&tclForeachInfoType);
}

/*
 *----------------------------------------------------------------------
 *
 * TclFinalizeAuxDataTypeTable --
 *
 *	This procedure is called by Tcl_Finalize after all exit handlers
 *	have been run to free up storage associated with the table of AuxData
 *	types.  This procedure is called by TclFinalizeExecution() which
 *	is called by Tcl_Finalize().
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Deletes all entries in the hash table of AuxData types.
 *
 *----------------------------------------------------------------------
 */

void
TclFinalizeAuxDataTypeTable()
{
    Tcl_MutexLock(&tableMutex);
    if (auxDataTypeTableInitialized) {
        Tcl_DeleteHashTable(&auxDataTypeTable);
        auxDataTypeTableInitialized = 0;
    }
    Tcl_MutexUnlock(&tableMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * GetCmdLocEncodingSize --
 *
 *	Computes the total number of bytes needed to encode the command
 *	location information for some compiled code.
 *
 * Results:
 *	The byte count needed to encode the compiled location information.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
GetCmdLocEncodingSize(envPtr)
     CompileEnv *envPtr;	/* Points to compilation environment
				 * structure containing the CmdLocation
				 * structure to encode. */
{
    register CmdLocation *mapPtr = envPtr->cmdMapPtr;
    int numCmds = envPtr->numCommands;
    int codeDelta, codeLen, srcDelta, srcLen;
    int codeDeltaNext, codeLengthNext, srcDeltaNext, srcLengthNext;
				/* The offsets in their respective byte
				 * sequences where the next encoded offset
				 * or length should go. */
    int prevCodeOffset, prevSrcOffset, i;

    codeDeltaNext = codeLengthNext = srcDeltaNext = srcLengthNext = 0;
    prevCodeOffset = prevSrcOffset = 0;
    for (i = 0;  i < numCmds;  i++) {
	codeDelta = (mapPtr[i].codeOffset - prevCodeOffset);
	if (codeDelta < 0) {
	    Tcl_Panic("GetCmdLocEncodingSize: bad code offset");
	} else if (codeDelta <= 127) {
	    codeDeltaNext++;
	} else {
	    codeDeltaNext += 5;	 /* 1 byte for 0xFF, 4 for positive delta */
	}
	prevCodeOffset = mapPtr[i].codeOffset;

	codeLen = mapPtr[i].numCodeBytes;
	if (codeLen < 0) {
	    Tcl_Panic("GetCmdLocEncodingSize: bad code length");
	} else if (codeLen <= 127) {
	    codeLengthNext++;
	} else {
	    codeLengthNext += 5; /* 1 byte for 0xFF, 4 for length */
	}

	srcDelta = (mapPtr[i].srcOffset - prevSrcOffset);
	if ((-127 <= srcDelta) && (srcDelta <= 127)) {
	    srcDeltaNext++;
	} else {
	    srcDeltaNext += 5;	 /* 1 byte for 0xFF, 4 for delta */
	}
	prevSrcOffset = mapPtr[i].srcOffset;

	srcLen = mapPtr[i].numSrcBytes;
	if (srcLen < 0) {
	    Tcl_Panic("GetCmdLocEncodingSize: bad source length");
	} else if (srcLen <= 127) {
	    srcLengthNext++;
	} else {
	    srcLengthNext += 5;	 /* 1 byte for 0xFF, 4 for length */
	}
    }

    return (codeDeltaNext + codeLengthNext + srcDeltaNext + srcLengthNext);
}

/*
 *----------------------------------------------------------------------
 *
 * EncodeCmdLocMap --
 *
 *	Encode the command location information for some compiled code into
 *	a ByteCode structure. The encoded command location map is stored as
 *	three adjacent byte sequences.
 *
 * Results:
 *	Pointer to the first byte after the encoded command location
 *	information.
 *
 * Side effects:
 *	The encoded information is stored into the block of memory headed
 *	by codePtr. Also records pointers to the start of the four byte
 *	sequences in fields in codePtr's ByteCode header structure.
 *
 *----------------------------------------------------------------------
 */

static unsigned char *
EncodeCmdLocMap(envPtr, codePtr, startPtr)
     CompileEnv *envPtr;	/* Points to compilation environment
				 * structure containing the CmdLocation
				 * structure to encode. */
     ByteCode *codePtr;		/* ByteCode in which to encode envPtr's
				 * command location information. */
     unsigned char *startPtr;	/* Points to the first byte in codePtr's
				 * memory block where the location
				 * information is to be stored. */
{
    register CmdLocation *mapPtr = envPtr->cmdMapPtr;
    int numCmds = envPtr->numCommands;
    register unsigned char *p = startPtr;
    int codeDelta, codeLen, srcDelta, srcLen, prevOffset;
    register int i;
    
    /*
     * Encode the code offset for each command as a sequence of deltas.
     */

    codePtr->codeDeltaStart = p;
    prevOffset = 0;
    for (i = 0;  i < numCmds;  i++) {
	codeDelta = (mapPtr[i].codeOffset - prevOffset);
	if (codeDelta < 0) {
	    Tcl_Panic("EncodeCmdLocMap: bad code offset");
	} else if (codeDelta <= 127) {
	    TclStoreInt1AtPtr(codeDelta, p);
	    p++;
	} else {
	    TclStoreInt1AtPtr(0xFF, p);
	    p++;
	    TclStoreInt4AtPtr(codeDelta, p);
	    p += 4;
	}
	prevOffset = mapPtr[i].codeOffset;
    }

    /*
     * Encode the code length for each command.
     */

    codePtr->codeLengthStart = p;
    for (i = 0;  i < numCmds;  i++) {
	codeLen = mapPtr[i].numCodeBytes;
	if (codeLen < 0) {
	    Tcl_Panic("EncodeCmdLocMap: bad code length");
	} else if (codeLen <= 127) {
	    TclStoreInt1AtPtr(codeLen, p);
	    p++;
	} else {
	    TclStoreInt1AtPtr(0xFF, p);
	    p++;
	    TclStoreInt4AtPtr(codeLen, p);
	    p += 4;
	}
    }

    /*
     * Encode the source offset for each command as a sequence of deltas.
     */

    codePtr->srcDeltaStart = p;
    prevOffset = 0;
    for (i = 0;  i < numCmds;  i++) {
	srcDelta = (mapPtr[i].srcOffset - prevOffset);
	if ((-127 <= srcDelta) && (srcDelta <= 127)) {
	    TclStoreInt1AtPtr(srcDelta, p);
	    p++;
	} else {
	    TclStoreInt1AtPtr(0xFF, p);
	    p++;
	    TclStoreInt4AtPtr(srcDelta, p);
	    p += 4;
	}
	prevOffset = mapPtr[i].srcOffset;
    }

    /*
     * Encode the source length for each command.
     */

    codePtr->srcLengthStart = p;
    for (i = 0;  i < numCmds;  i++) {
	srcLen = mapPtr[i].numSrcBytes;
	if (srcLen < 0) {
	    Tcl_Panic("EncodeCmdLocMap: bad source length");
	} else if (srcLen <= 127) {
	    TclStoreInt1AtPtr(srcLen, p);
	    p++;
	} else {
	    TclStoreInt1AtPtr(0xFF, p);
	    p++;
	    TclStoreInt4AtPtr(srcLen, p);
	    p += 4;
	}
    }
    
    return p;
}

#ifdef TCL_COMPILE_DEBUG
/*
 *----------------------------------------------------------------------
 *
 * TclPrintByteCodeObj --
 *
 *	This procedure prints ("disassembles") the instructions of a
 *	bytecode object to stdout.
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
TclPrintByteCodeObj(interp, objPtr)
    Tcl_Interp *interp;		/* Used only for Tcl_GetStringFromObj. */
    Tcl_Obj *objPtr;		/* The bytecode object to disassemble. */
{
    ByteCode* codePtr = (ByteCode *) objPtr->internalRep.otherValuePtr;
    unsigned char *codeStart, *codeLimit, *pc;
    unsigned char *codeDeltaNext, *codeLengthNext;
    unsigned char *srcDeltaNext, *srcLengthNext;
    int codeOffset, codeLen, srcOffset, srcLen, numCmds, delta, i;
    Interp *iPtr = (Interp *) *codePtr->interpHandle;

    if (codePtr->refCount <= 0) {
	return;			/* already freed */
    }

    codeStart = codePtr->codeStart;
    codeLimit = (codeStart + codePtr->numCodeBytes);
    numCmds = codePtr->numCommands;

    /*
     * Print header lines describing the ByteCode.
     */

    fprintf(stdout, "\nByteCode 0x%x, refCt %u, epoch %u, interp 0x%x (epoch %u)\n",
	    (unsigned int) codePtr, codePtr->refCount,
	    codePtr->compileEpoch, (unsigned int) iPtr,
	    iPtr->compileEpoch);
    fprintf(stdout, "  Source ");
    TclPrintSource(stdout, codePtr->source,
	    TclMin(codePtr->numSrcBytes, 55));
    fprintf(stdout, "\n  Cmds %d, src %d, inst %d, litObjs %u, aux %d, stkDepth %u, code/src %.2f\n",
	    numCmds, codePtr->numSrcBytes, codePtr->numCodeBytes,
	    codePtr->numLitObjects, codePtr->numAuxDataItems,
	    codePtr->maxStackDepth,
#ifdef TCL_COMPILE_STATS
	    (codePtr->numSrcBytes?
	            ((float)codePtr->structureSize)/((float)codePtr->numSrcBytes) : 0.0));
#else
	    0.0);
#endif
#ifdef TCL_COMPILE_STATS
    fprintf(stdout,
	    "  Code %d = header %d+inst %d+litObj %d+exc %d+aux %d+cmdMap %d\n",
	    codePtr->structureSize,
	    (sizeof(ByteCode) - (sizeof(size_t) + sizeof(Tcl_Time))),
	    codePtr->numCodeBytes,
	    (codePtr->numLitObjects * sizeof(Tcl_Obj *)),
	    (codePtr->numExceptRanges * sizeof(ExceptionRange)),
	    (codePtr->numAuxDataItems * sizeof(AuxData)),
	    codePtr->numCmdLocBytes);
#endif /* TCL_COMPILE_STATS */
    
    /*
     * If the ByteCode is the compiled body of a Tcl procedure, print
     * information about that procedure. Note that we don't know the
     * procedure's name since ByteCode's can be shared among procedures.
     */
    
    if (codePtr->procPtr != NULL) {
	Proc *procPtr = codePtr->procPtr;
	int numCompiledLocals = procPtr->numCompiledLocals;
	fprintf(stdout,
	        "  Proc 0x%x, refCt %d, args %d, compiled locals %d\n",
		(unsigned int) procPtr, procPtr->refCount, procPtr->numArgs,
		numCompiledLocals);
	if (numCompiledLocals > 0) {
	    CompiledLocal *localPtr = procPtr->firstLocalPtr;
	    for (i = 0;  i < numCompiledLocals;  i++) {
		fprintf(stdout, "      slot %d%s%s%s%s%s%s", i, 
			((localPtr->flags & VAR_SCALAR)?  ", scalar"  : ""),
			((localPtr->flags & VAR_ARRAY)?  ", array"  : ""),
			((localPtr->flags & VAR_LINK)?  ", link"  : ""),
			((localPtr->flags & VAR_ARGUMENT)?  ", arg"  : ""),
			((localPtr->flags & VAR_TEMPORARY)? ", temp" : ""),
			((localPtr->flags & VAR_RESOLVED)? ", resolved" : ""));
		if (TclIsVarTemporary(localPtr)) {
		    fprintf(stdout,	"\n");
		} else {
		    fprintf(stdout,	", \"%s\"\n", localPtr->name);
		}
		localPtr = localPtr->nextPtr;
	    }
	}
    }

    /*
     * Print the ExceptionRange array.
     */

    if (codePtr->numExceptRanges > 0) {
	fprintf(stdout, "  Exception ranges %d, depth %d:\n",
	        codePtr->numExceptRanges, codePtr->maxExceptDepth);
	for (i = 0;  i < codePtr->numExceptRanges;  i++) {
	    ExceptionRange *rangePtr = &(codePtr->exceptArrayPtr[i]);
	    fprintf(stdout, "      %d: level %d, %s, pc %d-%d, ",
		    i, rangePtr->nestingLevel,
		    ((rangePtr->type == LOOP_EXCEPTION_RANGE)
			    ? "loop" : "catch"),
		    rangePtr->codeOffset,
		    (rangePtr->codeOffset + rangePtr->numCodeBytes - 1));
	    switch (rangePtr->type) {
	    case LOOP_EXCEPTION_RANGE:
		fprintf(stdout,	"continue %d, break %d\n",
		        rangePtr->continueOffset, rangePtr->breakOffset);
		break;
	    case CATCH_EXCEPTION_RANGE:
		fprintf(stdout,	"catch %d\n", rangePtr->catchOffset);
		break;
	    default:
		Tcl_Panic("TclPrintByteCodeObj: bad ExceptionRange type %d\n",
		        rangePtr->type);
	    }
	}
    }
    
    /*
     * If there were no commands (e.g., an expression or an empty string
     * was compiled), just print all instructions and return.
     */

    if (numCmds == 0) {
	pc = codeStart;
	while (pc < codeLimit) {
	    fprintf(stdout, "    ");
	    pc += TclPrintInstruction(codePtr, pc);
	}
	return;
    }
    
    /*
     * Print table showing the code offset, source offset, and source
     * length for each command. These are encoded as a sequence of bytes.
     */

    fprintf(stdout, "  Commands %d:", numCmds);
    codeDeltaNext = codePtr->codeDeltaStart;
    codeLengthNext = codePtr->codeLengthStart;
    srcDeltaNext  = codePtr->srcDeltaStart;
    srcLengthNext = codePtr->srcLengthStart;
    codeOffset = srcOffset = 0;
    for (i = 0;  i < numCmds;  i++) {
	if ((unsigned int) (*codeDeltaNext) == (unsigned int) 0xFF) {
	    codeDeltaNext++;
	    delta = TclGetInt4AtPtr(codeDeltaNext);
	    codeDeltaNext += 4;
	} else {
	    delta = TclGetInt1AtPtr(codeDeltaNext);
	    codeDeltaNext++;
	}
	codeOffset += delta;

	if ((unsigned int) (*codeLengthNext) == (unsigned int) 0xFF) {
	    codeLengthNext++;
	    codeLen = TclGetInt4AtPtr(codeLengthNext);
	    codeLengthNext += 4;
	} else {
	    codeLen = TclGetInt1AtPtr(codeLengthNext);
	    codeLengthNext++;
	}
	
	if ((unsigned int) (*srcDeltaNext) == (unsigned int) 0xFF) {
	    srcDeltaNext++;
	    delta = TclGetInt4AtPtr(srcDeltaNext);
	    srcDeltaNext += 4;
	} else {
	    delta = TclGetInt1AtPtr(srcDeltaNext);
	    srcDeltaNext++;
	}
	srcOffset += delta;

	if ((unsigned int) (*srcLengthNext) == (unsigned int) 0xFF) {
	    srcLengthNext++;
	    srcLen = TclGetInt4AtPtr(srcLengthNext);
	    srcLengthNext += 4;
	} else {
	    srcLen = TclGetInt1AtPtr(srcLengthNext);
	    srcLengthNext++;
	}
	
	fprintf(stdout,	"%s%4d: pc %d-%d, src %d-%d",
		((i % 2)? "   	" : "\n   "),
		(i+1), codeOffset, (codeOffset + codeLen - 1),
		srcOffset, (srcOffset + srcLen - 1));
    }
    if (numCmds > 0) {
	fprintf(stdout,	"\n");
    }
    
    /*
     * Print each instruction. If the instruction corresponds to the start
     * of a command, print the command's source. Note that we don't need
     * the code length here.
     */

    codeDeltaNext = codePtr->codeDeltaStart;
    srcDeltaNext  = codePtr->srcDeltaStart;
    srcLengthNext = codePtr->srcLengthStart;
    codeOffset = srcOffset = 0;
    pc = codeStart;
    for (i = 0;  i < numCmds;  i++) {
	if ((unsigned int) (*codeDeltaNext) == (unsigned int) 0xFF) {
	    codeDeltaNext++;
	    delta = TclGetInt4AtPtr(codeDeltaNext);
	    codeDeltaNext += 4;
	} else {
	    delta = TclGetInt1AtPtr(codeDeltaNext);
	    codeDeltaNext++;
	}
	codeOffset += delta;

	if ((unsigned int) (*srcDeltaNext) == (unsigned int) 0xFF) {
	    srcDeltaNext++;
	    delta = TclGetInt4AtPtr(srcDeltaNext);
	    srcDeltaNext += 4;
	} else {
	    delta = TclGetInt1AtPtr(srcDeltaNext);
	    srcDeltaNext++;
	}
	srcOffset += delta;

	if ((unsigned int) (*srcLengthNext) == (unsigned int) 0xFF) {
	    srcLengthNext++;
	    srcLen = TclGetInt4AtPtr(srcLengthNext);
	    srcLengthNext += 4;
	} else {
	    srcLen = TclGetInt1AtPtr(srcLengthNext);
	    srcLengthNext++;
	}

	/*
	 * Print instructions before command i.
	 */
	
	while ((pc-codeStart) < codeOffset) {
	    fprintf(stdout, "    ");
	    pc += TclPrintInstruction(codePtr, pc);
	}

	fprintf(stdout, "  Command %d: ", (i+1));
	TclPrintSource(stdout, (codePtr->source + srcOffset),
	        TclMin(srcLen, 55));
	fprintf(stdout, "\n");
    }
    if (pc < codeLimit) {
	/*
	 * Print instructions after the last command.
	 */

	while (pc < codeLimit) {
	    fprintf(stdout, "    ");
	    pc += TclPrintInstruction(codePtr, pc);
	}
    }
}
#endif /* TCL_COMPILE_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * TclPrintInstruction --
 *
 *	This procedure prints ("disassembles") one instruction from a
 *	bytecode object to stdout.
 *
 * Results:
 *	Returns the length in bytes of the current instruiction.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TclPrintInstruction(codePtr, pc)
    ByteCode* codePtr;		/* Bytecode containing the instruction. */
    unsigned char *pc;		/* Points to first byte of instruction. */
{
    Proc *procPtr = codePtr->procPtr;
    unsigned char opCode = *pc;
    register InstructionDesc *instDesc = &tclInstructionTable[opCode];
    unsigned char *codeStart = codePtr->codeStart;
    unsigned int pcOffset = (pc - codeStart);
    int opnd, i, j, numBytes = 1;
    
    fprintf(stdout, "(%u) %s ", pcOffset, instDesc->name);
    for (i = 0;  i < instDesc->numOperands;  i++) {
	switch (instDesc->opTypes[i]) {
	case OPERAND_INT1:
	    opnd = TclGetInt1AtPtr(pc+numBytes); numBytes++;
	    if ((i == 0) && ((opCode == INST_JUMP1)
			     || (opCode == INST_JUMP_TRUE1)
		             || (opCode == INST_JUMP_FALSE1))) {
		fprintf(stdout, "%d  	# pc %u", opnd, (pcOffset + opnd));
	    } else {
		fprintf(stdout, "%d ", opnd);
	    }
	    break;
	case OPERAND_INT4:
	    opnd = TclGetInt4AtPtr(pc+numBytes); numBytes += 4;
	    if ((i == 0) && ((opCode == INST_JUMP4)
			     || (opCode == INST_JUMP_TRUE4)
		             || (opCode == INST_JUMP_FALSE4))) {
		fprintf(stdout, "%d  	# pc %u", opnd, (pcOffset + opnd));
	    } else {
		fprintf(stdout, "%d ", opnd);
	    }
	    break;
	case OPERAND_UINT1:
	    opnd = TclGetUInt1AtPtr(pc+numBytes); numBytes++;
	    if ((i == 0) && (opCode == INST_PUSH1)) {
		fprintf(stdout, "%u  	# ", (unsigned int) opnd);
		TclPrintObject(stdout, codePtr->objArrayPtr[opnd], 40);
	    } else if ((i == 0) && ((opCode == INST_LOAD_SCALAR1)
				    || (opCode == INST_LOAD_ARRAY1)
				    || (opCode == INST_STORE_SCALAR1)
				    || (opCode == INST_STORE_ARRAY1))) {
		int localCt = procPtr->numCompiledLocals;
		CompiledLocal *localPtr = procPtr->firstLocalPtr;
		if (opnd >= localCt) {
		    Tcl_Panic("TclPrintInstruction: bad local var index %u (%u locals)\n",
			     (unsigned int) opnd, localCt);
		}
		for (j = 0;  j < opnd;  j++) {
		    localPtr = localPtr->nextPtr;
		}
		if (TclIsVarTemporary(localPtr)) {
		    fprintf(stdout, "%u	# temp var %u",
			    (unsigned int) opnd, (unsigned int) opnd);
		} else {
		    fprintf(stdout, "%u	# var ", (unsigned int) opnd);
		    TclPrintSource(stdout, localPtr->name, 40);
		}
	    } else {
		fprintf(stdout, "%u ", (unsigned int) opnd);
	    }
	    break;
	case OPERAND_UINT4:
	    opnd = TclGetUInt4AtPtr(pc+numBytes); numBytes += 4;
	    if (opCode == INST_PUSH4) {
		fprintf(stdout, "%u  	# ", opnd);
		TclPrintObject(stdout, codePtr->objArrayPtr[opnd], 40);
	    } else if ((i == 0) && ((opCode == INST_LOAD_SCALAR4)
				    || (opCode == INST_LOAD_ARRAY4)
				    || (opCode == INST_STORE_SCALAR4)
				    || (opCode == INST_STORE_ARRAY4))) {
		int localCt = procPtr->numCompiledLocals;
		CompiledLocal *localPtr = procPtr->firstLocalPtr;
		if (opnd >= localCt) {
		    Tcl_Panic("TclPrintInstruction: bad local var index %u (%u locals)\n",
			     (unsigned int) opnd, localCt);
		}
		for (j = 0;  j < opnd;  j++) {
		    localPtr = localPtr->nextPtr;
		}
		if (TclIsVarTemporary(localPtr)) {
		    fprintf(stdout, "%u	# temp var %u",
			    (unsigned int) opnd, (unsigned int) opnd);
		} else {
		    fprintf(stdout, "%u	# var ", (unsigned int) opnd);
		    TclPrintSource(stdout, localPtr->name, 40);
		}
	    } else {
		fprintf(stdout, "%u ", (unsigned int) opnd);
	    }
	    break;

	case OPERAND_IDX4:
	    opnd = TclGetInt4AtPtr(pc+numBytes); numBytes += 4;
	    if (opnd >= -1) {
		fprintf(stdout, "%d ", opnd);
	    } else if (opnd == -2) {
		fprintf(stdout, "end ");
	    } else {
		fprintf(stdout, "end-%d ", -2-opnd);
            }
	    break;

	case OPERAND_NONE:
	default:
	    break;
	}
    }
    fprintf(stdout, "\n");
    return numBytes;
}

/*
 *----------------------------------------------------------------------
 *
 * TclPrintObject --
 *
 *	This procedure prints up to a specified number of characters from
 *	the argument Tcl object's string representation to a specified file.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Outputs characters to the specified file.
 *
 *----------------------------------------------------------------------
 */

void
TclPrintObject(outFile, objPtr, maxChars)
    FILE *outFile;		/* The file to print the source to. */
    Tcl_Obj *objPtr;		/* Points to the Tcl object whose string
				 * representation should be printed. */
    int maxChars;		/* Maximum number of chars to print. */
{
    char *bytes;
    int length;
    
    bytes = Tcl_GetStringFromObj(objPtr, &length);
    TclPrintSource(outFile, bytes, TclMin(length, maxChars));
}

/*
 *----------------------------------------------------------------------
 *
 * TclPrintSource --
 *
 *	This procedure prints up to a specified number of characters from
 *	the argument string to a specified file. It tries to produce legible
 *	output by adding backslashes as necessary.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Outputs characters to the specified file.
 *
 *----------------------------------------------------------------------
 */

void
TclPrintSource(outFile, stringPtr, maxChars)
    FILE *outFile;		/* The file to print the source to. */
    CONST char *stringPtr;	/* The string to print. */
    int maxChars;		/* Maximum number of chars to print. */
{
    register CONST char *p;
    register int i = 0;

    if (stringPtr == NULL) {
	fprintf(outFile, "\"\"");
	return;
    }

    fprintf(outFile, "\"");
    p = stringPtr;
    for (;  (*p != '\0') && (i < maxChars);  p++, i++) {
	switch (*p) {
	    case '"':
		fprintf(outFile, "\\\"");
		continue;
	    case '\f':
		fprintf(outFile, "\\f");
		continue;
	    case '\n':
		fprintf(outFile, "\\n");
		continue;
            case '\r':
		fprintf(outFile, "\\r");
		continue;
	    case '\t':
		fprintf(outFile, "\\t");
		continue;
            case '\v':
		fprintf(outFile, "\\v");
		continue;
	    default:
		fprintf(outFile, "%c", *p);
		continue;
	}
    }
    fprintf(outFile, "\"");
}

#ifdef TCL_COMPILE_STATS
/*
 *----------------------------------------------------------------------
 *
 * RecordByteCodeStats --
 *
 *	Accumulates various compilation-related statistics for each newly
 *	compiled ByteCode. Called by the TclInitByteCodeObj when Tcl is
 *	compiled with the -DTCL_COMPILE_STATS flag
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Accumulates aggregate code-related statistics in the interpreter's
 *	ByteCodeStats structure. Records statistics specific to a ByteCode
 *	in its ByteCode structure.
 *
 *----------------------------------------------------------------------
 */

void
RecordByteCodeStats(codePtr)
    ByteCode *codePtr;		/* Points to ByteCode structure with info
				 * to add to accumulated statistics. */
{
    Interp *iPtr = (Interp *) *codePtr->interpHandle;
    register ByteCodeStats *statsPtr = &(iPtr->stats);

    statsPtr->numCompilations++;
    statsPtr->totalSrcBytes        += (double) codePtr->numSrcBytes;
    statsPtr->totalByteCodeBytes   += (double) codePtr->structureSize;
    statsPtr->currentSrcBytes      += (double) codePtr->numSrcBytes;
    statsPtr->currentByteCodeBytes += (double) codePtr->structureSize;
    
    statsPtr->srcCount[TclLog2(codePtr->numSrcBytes)]++;
    statsPtr->byteCodeCount[TclLog2((int)(codePtr->structureSize))]++;

    statsPtr->currentInstBytes   += (double) codePtr->numCodeBytes;
    statsPtr->currentLitBytes    +=
	    (double) (codePtr->numLitObjects * sizeof(Tcl_Obj *)); 
    statsPtr->currentExceptBytes +=
	    (double) (codePtr->numExceptRanges * sizeof(ExceptionRange));
    statsPtr->currentAuxBytes    +=
            (double) (codePtr->numAuxDataItems * sizeof(AuxData));
    statsPtr->currentCmdMapBytes += (double) codePtr->numCmdLocBytes;
}
#endif /* TCL_COMPILE_STATS */
