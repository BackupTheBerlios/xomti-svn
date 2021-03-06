/*
 * tkUndo.h --
 *
 *	Declarations shared among the files that implement an undo
 *	stack.
 *
 * Copyright (c) 2002 Ludwig Callewaert.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tkUndo.h,v 1.3 2004/09/10 12:13:42 vincentdarley Exp $
 */

#ifndef _TKUNDO
#define _TKUNDO

#ifndef _TK
#include "tk.h"
#endif

#ifdef BUILD_tk
# undef TCL_STORAGE_CLASS
# define TCL_STORAGE_CLASS DLLEXPORT
#endif

/* Enum definining the types used in an undo stack */

typedef enum {
    TK_UNDO_SEPARATOR,			/* Marker */
    TK_UNDO_ACTION			/* Command */
} TkUndoAtomType;

/* 
 * Callback proc type to carry out an undo or redo action
 * via C code.  (Actions can also be defined by Tcl scripts).
 */

typedef int (TkUndoProc) _ANSI_ARGS_((Tcl_Interp *interp, 
				      ClientData clientData, 
				      Tcl_Obj *objPtr));

/* 
 * Struct defining a single action, one or more of which may
 * be defined (and stored in a linked list) separately for each 
 * undo and redo action of an undo atom.
 */

typedef struct TkUndoSubAtom {
    Tcl_Command command;        /* Tcl token used to get the current
				 * Tcl command name which will be used
				 * to execute apply/revert scripts.  If 
				 * NULL then it is assumed the
				 * apply/revert scripts already contain
				 * everything. */
    TkUndoProc *funcPtr;        /* Function pointer for callback to
				 * perform undo/redo actions. */
    ClientData clientData;      /* data for 'funcPtr' */
    Tcl_Obj *action;		/* Command to apply the action that 
				 * was taken */
    struct TkUndoSubAtom *next; /* Pointer to the next element in the
				 * linked list */
} TkUndoSubAtom;

/* 
 * Struct representing a single undo+redo atom to be placed in
 * the stack.
 */

typedef struct TkUndoAtom {
    TkUndoAtomType type;	/* The type that will trigger the
				 * required action*/
    TkUndoSubAtom *apply;       /* Linked list of 'apply' actions to
                                 * perform for this operation */
    TkUndoSubAtom *revert;      /* Linked list of 'revert' actions to
                                 * perform for this operation */
    struct TkUndoAtom *next;	/* Pointer to the next element in the
				 * stack */
} TkUndoAtom;

/* 
 * Struct defining a single undo+redo stack.
 */

typedef struct TkUndoRedoStack {
    TkUndoAtom * undoStack;	  /* The undo stack */
    TkUndoAtom * redoStack;	  /* The redo stack */
    Tcl_Interp * interp   ;       /* The interpreter in which to execute 
                                   * the revert and apply scripts */
    int          maxdepth;
    int          depth;
} TkUndoRedoStack;

/* Basic functions */

EXTERN void TkUndoPushStack         _ANSI_ARGS_((TkUndoAtom **stack,
						TkUndoAtom *elem));
EXTERN TkUndoAtom * TkUndoPopStack  _ANSI_ARGS_((TkUndoAtom **stack));
EXTERN int TkUndoInsertSeparator    _ANSI_ARGS_((TkUndoAtom **stack));
EXTERN void TkUndoClearStack        _ANSI_ARGS_((TkUndoAtom **stack));

/* Functions for working on an undo/redo stack */

EXTERN TkUndoRedoStack * TkUndoInitStack    _ANSI_ARGS_((Tcl_Interp *interp,
						int maxdepth));
EXTERN void TkUndoSetDepth                  _ANSI_ARGS_((TkUndoRedoStack *stack,
						int maxdepth));
EXTERN void TkUndoClearStacks               _ANSI_ARGS_((TkUndoRedoStack *stack));
EXTERN void TkUndoFreeStack                 _ANSI_ARGS_((TkUndoRedoStack *stack));
EXTERN void TkUndoInsertUndoSeparator       _ANSI_ARGS_((TkUndoRedoStack *stack));
EXTERN TkUndoSubAtom * TkUndoMakeCmdSubAtom _ANSI_ARGS_((
						 Tcl_Command command, 
						 Tcl_Obj *actionScript, 
						 TkUndoSubAtom *subAtomList));
EXTERN TkUndoSubAtom * TkUndoMakeSubAtom    _ANSI_ARGS_((
						 TkUndoProc *funcPtr, 
						 ClientData clientData,
						 Tcl_Obj *actionScript, 
						 TkUndoSubAtom *subAtomList));
EXTERN void TkUndoPushAction                _ANSI_ARGS_((TkUndoRedoStack *stack,
						 TkUndoSubAtom *apply, 
						 TkUndoSubAtom *revert));
EXTERN int TkUndoRevert                     _ANSI_ARGS_((TkUndoRedoStack *stack));
EXTERN int TkUndoApply                      _ANSI_ARGS_((TkUndoRedoStack *stack));

# undef TCL_STORAGE_CLASS
# define TCL_STORAGE_CLASS DLLIMPORT

#endif /* _TKUNDO */
