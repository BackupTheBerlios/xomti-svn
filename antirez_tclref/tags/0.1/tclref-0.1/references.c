/* Tclref -- References support for Tcl.
 * 
 * Copyright (C) 2004 Salvatore Sanfilippo <antirez@invece.org>
 * All rights reserved.
 *
 * Optimizations may include:
 *
 * - Take the reference table and the visited table in
 *   a C hashtable, not visible from Tcl.
 * - Create an object type for references, mark it with
 *   some flag until the string representation for this
 *   object was NEVER generated. When the reference object
 *   gets killed, if still the string representation was
 *   never generated, we can *directly* collect this reference.
 * - Be smart if there is a lot of allocated memory. If the
 *   last collection takes more than MAXTIME to run, make the
 *   collection period longer. If the collection time is
 *   less than MINTIME make the collection time littler.
 *
 * This is a first attempt. More I work on it, more hard it seems
 * to get a solid implementation.
 *
 * See LICENSE for Copyright and License information. */

/* TODO:
 *
 * - Structures 'make' command should accept arguments to
 *   initialize fields, like the pure-Tcl implementation.
 * - clone and deepclone.
 * - Collect from fileevent scripts, proc arguments, Tk, ...
 *   most of this can be done in the Tcl helper included inside
 *   this C file.
 * - Provide a 'refunset' command to kill a reference by hand.
 *
 * OOP TODO: (note that OOP code is removed from current releases).
 * - Implement the rest of the OOP system.
 * - Should lookup the __unknown__ method, on unknown method.
 * - Export the basic method lookup system to let users to implement
 *   their specialized OOP systems.
 */

#if 0
#define TCLREF_DEBUG
#endif

/* #include <tcl.h> */

/* modified by FP #include <tcl-private/generic/tclInt.h>*/
#include <tclInt.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* Includes for setrlimit */
#if !defined(__WIN32__) && !defined(MAC_TCL) /* UNIX */
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#define VERSION "0.1"

/*------------------------------------------------------------------------------
 * Low level references.
 *
 * Exported commnads:
 *
 * ref
 * getbyref
 * setref
 * isref
 * emptyref
 * setfinalizer
 * collect
 *----------------------------------------------------------------------------*/

/* Default collection period. Run the GC every
 * TCL_REF_COLLPERIOD calls to [ref]. */
#define TCL_REF_COLLPERIOD 5000

/* Our per-interpreter private data */
struct RefData {
	int debug;
	long long nextid; /* FIXME: may overflow with very long running code */
	int calls;
	int collection_period;
	Tcl_Obj *emptyObj;
};

/* Types that can't contain a reference */
Tcl_ObjType *IntType;
Tcl_ObjType *DoubleType;
Tcl_ObjType *BooleanType;
Tcl_ObjType *WideIntType;
Tcl_ObjType *IndexType;
/* We also need to be able to sense the list and dict type. */
Tcl_ObjType *ListType;
Tcl_ObjType *DictType;

/* Tcl source code - The Gc helper is used to collect
 * global roots. Locals can't be collected from Tcl due
 * to Uplevel, so it's implemented in C. */
static char *Tcl_GcHelperText = \
"# Return all the namespaces currently defined, except for the ::ref\n"
"# namespace\n"
"proc ::ref::getnamespaces {{nslist ::}} {\n"
"    set res {}\n"
"    foreach ns $nslist {\n"
"	if {[string equal $ns ::ref]} continue\n"
"	lappend res $ns\n"
"	set res [concat $res [getnamespaces [namespace children $ns]]]\n"
"    }\n"
"    return $res\n"
"}\n"
"\n"
"# Return all the existing global variables (both in the root and in other namespaces)\n"
"proc ::ref::getallglobals {} {\n"
"    set res {}\n"
"    set namespaces [::ref::getnamespaces]\n"
"    foreach ns $namespaces {\n"
"	set res [concat $res [info vars $ns\\::*]]\n"
"    }\n"
"    return $res\n"
"}\n"
"\n"
"# Return a list of values of all the accessible local and global variables.\n"
"proc ::ref::getreachablevalues {} {\n"
"    set roots {}\n"
"\n"
"    # Get global values\n"
"    foreach g [::ref::getallglobals] {\n"
"	if {[array exists $g]} {\n"
"	    lappend roots [array get $g]\n"
"	} else {\n"
"	    if {[info exists $g]} {lappend roots [set $g]}\n"
"	}\n"
"    }\n"
"    # TODO: get filevent's callbacks\n"
"    # TODO: Tk bindings (using the bind command)\n"
"    return $roots\n"
"}\n"
"array set ::ref::visited {}\n"
"array set ::ref::finalizer {}\n"
;

/* ------------------------------- misc ------------------------------------- */
/* Return zero if the string pointed by 'p' is not a valid reference
 * represention. Otherwise return the length of the representation.
 * Try to be fast... */
static int strisref(char *p)
{
	char *savep = p;

	if (*p != '<' ||
	    *(p+1) != 'r' ||
	    *(p+2) != 'e' ||
	    *(p+3) != 'f' ||
	    *(p+4) != ':') goto invalid;
	p += 5;
	while(*p >= '0' && *p <= '9') p++;
	if (*p != '>') goto invalid;
	/* success */
	return (1+(p-savep));

invalid:
	return 0;
}

/*------------------------------------------------------------------------------
 * GC helpers to get the reachable objects in the Tcl stack frames
 *----------------------------------------------------------------------------*/

/* This is a GetLocalObjects() helper, used to append the
 * the array's keys/values objects. */
static void AppendArrayContent(Var *array, Tcl_Obj *listPtr)
{
	Tcl_HashSearch hSearch;
	Tcl_HashEntry *hPtr;
	Var *var;
	char *name;
	Tcl_Obj *auxPtr;

	hPtr = Tcl_FirstHashEntry(array->value.tablePtr, &hSearch);
	for (;hPtr != NULL; hPtr = Tcl_NextHashEntry(&hSearch)) {
		name = (char*) Tcl_GetHashKey(array->value.tablePtr, hPtr);
		auxPtr = Tcl_NewStringObj(name, -1);
		Tcl_ListObjAppendElement(NULL, listPtr, auxPtr);
		var = (Var*) Tcl_GetHashValue(hPtr);
		Tcl_ListObjAppendElement(NULL, listPtr, var->value.objPtr);
	}
}

/* Return a lit of all the reachable local objects.
 * A bit of black magic here, basically regardless of
 * the 'level' of a stack frame int the Tcl context,
 * in C it's possible to access the previous function
 * stack frame. So the function walks all the stack
 * frames generating a list with all the objects inside
 * compiled and non-compiled variables and arrays. Both
 * variable's name and content are used. */
static Tcl_Obj *GetLocalObjects(Tcl_Interp *interp)
{
	Interp *iPtr = (Interp*) interp;
	CallFrame *cf = iPtr->framePtr;
	Var *v;
	Tcl_Obj *listPtr, *auxPtr;

	listPtr = Tcl_NewListObj(0, NULL);
	Tcl_IncrRefCount(listPtr);

	while(cf) {
		int i;

		for (i = 0; i < cf->numCompiledLocals; i++) {
			v = &cf->compiledLocals[i];
			auxPtr = Tcl_NewStringObj(v->name, -1);
			Tcl_ListObjAppendElement(NULL, listPtr, auxPtr);
			if (v->flags & VAR_SCALAR) {
				if (v->value.objPtr) {
					Tcl_ListObjAppendElement(NULL, listPtr, v->value.objPtr);
				}
			} else {
				AppendArrayContent(v, listPtr);
			}
		}
		if (cf->varTablePtr) {
			Tcl_HashSearch hSearch;
			Tcl_HashEntry *hPtr;
			hPtr = Tcl_FirstHashEntry(cf->varTablePtr, &hSearch);
			for(;hPtr != NULL; hPtr = Tcl_NextHashEntry(&hSearch)) {
				char *name;

				v = (Var*) Tcl_GetHashValue(hPtr);
				name = (char*) Tcl_GetHashKey(cf->varTablePtr, hPtr);
				auxPtr = Tcl_NewStringObj(name, -1);
				Tcl_ListObjAppendElement(NULL, listPtr, auxPtr);
				if (v->flags & VAR_SCALAR) {
					if (v->value.objPtr) {
						Tcl_ListObjAppendElement(NULL, listPtr, v->value.objPtr);
					}
				} else {
					AppendArrayContent(v, listPtr);
				}
			}
		}
		cf = cf->callerPtr;
	}
	return listPtr;
}

/* ------------------------- The Garbage Collector -------------------------- */
/* The Garbage collector Scan phase */
static void GCScan(Tcl_Interp *interp, Tcl_Obj *obj)
{
	if (obj->typePtr == IntType ||
	    obj->typePtr == DoubleType ||
	    obj->typePtr == BooleanType ||
	    obj->typePtr == WideIntType ||
	    obj->typePtr == IndexType) {
		return;
	}
	if (obj->typePtr == ListType) {
		Tcl_Obj **objv;
		int objc, i;

		/* Scan all the objects inside the list */
		/* TODO: Need to do the same for dictionaries. */
		if (Tcl_ListObjGetElements(interp, obj, &objc, &objv) 
		    != TCL_OK)
			return;
		for (i = 0; i < objc; i++)
			GCScan(interp, objv[i]);
	} else {
		char *p = Tcl_GetStringFromObj(obj, NULL);
		
		/* Note that we don't care about \0 in the middle
		 * of the string... */
		while((p = strstr(p, "<ref:")) != NULL) {
			int len;
			char ref[64];
			Tcl_Obj *refObj;

			if (!(len = strisref(p))) {
				p += 5;
				continue;
			}
			/* We found a reference, get the string repr */
			memcpy(ref, p, len);
			ref[len] = '\0';
			p += len;
			/* Check if this was alreay visited */
			if (Tcl_GetVar2Ex(interp, "::ref::visited", ref, 0))
				continue;
			/* Set it as visited */
			Tcl_SetVar2(interp, "::ref::visited", ref, "", 0);
			/* Scan the pointed object */
			refObj = Tcl_GetVar2Ex(interp, "::ref::reftab", ref, 0);
			if (refObj) {
				GCScan(interp, refObj);
			}
		}
	}
}

/* The Garbage collector Sweep phase */
static int GCSweep(Tcl_Interp *interp)
{
	Tcl_Obj *refTabObj;
	Tcl_Obj **refObjv;
	int refObjc, i;

	if (Tcl_EvalEx(interp, "array names ::ref::reftab", -1, 0) != TCL_OK)
		return TCL_ERROR;
	refTabObj = Tcl_GetObjResult(interp);
	Tcl_IncrRefCount(refTabObj);
	if (Tcl_ListObjGetElements(interp, refTabObj, &refObjc, &refObjv)
	    != TCL_OK)
		return TCL_ERROR;
	/* Every reference not 'visited' by the mark phase gets collected */
	for (i = 0; i < refObjc; i++) {
		Tcl_Obj *finalizerCmd;

		if (Tcl_GetVar2Ex(interp, "::ref::visited", Tcl_GetStringFromObj(refObjv[i], NULL), 0)) {
			continue;
		}
#ifdef TCLREF_DEBUG
		printf("%s collected\n", Tcl_GetStringFromObj(refObjv[i], NULL));
#endif
		/* Call the finalizer command if any. */
		finalizerCmd = Tcl_GetVar2Ex(interp, "::ref::finalizer",
				Tcl_GetStringFromObj(refObjv[i], NULL), 0);
		if (finalizerCmd) {
			Tcl_Obj *ov[2];

			Tcl_IncrRefCount(finalizerCmd);
			Tcl_UnsetVar2(interp, "::ref::finalizer",
					Tcl_GetStringFromObj(refObjv[i], NULL),
					0);
			ov[0] = finalizerCmd;
			ov[1] = refObjv[i];
			if (Tcl_EvalObjv(interp, 2, ov, 0) != TCL_OK) {
				Tcl_DecrRefCount(refTabObj);
				Tcl_DecrRefCount(finalizerCmd);
				return TCL_ERROR;
			}
			Tcl_DecrRefCount(finalizerCmd);
		}
		/* Unset this reference */
		Tcl_UnsetVar2(interp, "::ref::reftab",
				Tcl_GetStringFromObj(refObjv[i], NULL), 0);
	}
	Tcl_DecrRefCount(refTabObj);
	return TCL_OK;
}

/* The GC main function */
static int RefGarbageCollector(Tcl_Interp *interp, struct RefData *rd)
{
	Tcl_Obj *roots, *locals;

	Tcl_EvalEx(interp, "::ref::getreachablevalues", -1, 0);
	roots = Tcl_GetObjResult(interp);
	Tcl_IncrRefCount(roots); /* Take it after the next eval call */
	locals = GetLocalObjects(interp);

	/* Scan the graph, save every visited reference in the
	 * ::ref::visited array. */
	Tcl_Eval(interp, "array unset ::ref::visited");
	GCScan(interp, roots);
	GCScan(interp, locals);
	/* The finalizer may raise an error. */
	if (GCSweep(interp) != TCL_OK) {
		Tcl_DecrRefCount(roots);
		Tcl_DecrRefCount(locals);
		return TCL_ERROR;
	}
	Tcl_ResetResult(interp);
	Tcl_DecrRefCount(roots);
	Tcl_DecrRefCount(locals);
	return 0;
}

/* Run the GC if needed. */
static int RefCollectIfNeeded(Tcl_Interp *interp, struct RefData *rd)
{
	rd->calls ++;
	if (rd->calls > rd->collection_period) {
		rd->calls = 0;
		return RefGarbageCollector(interp, rd);
	}
	return 0;
}

/* ----------------------- References C API --------------------------------- */
/* Return a reference set to the 'objPtr' object content.
 * The retured object (containing the reference) has refcount 0.
 * On error NULL is returned, and the result set to the error description.
 *
 * Refcount: the 'objPtr' object reference count is incremented. */
static Tcl_Obj *Tcl_NewRefObj(Tcl_Interp *interp,
		struct RefData *rd, Tcl_Obj *objPtr)
{
	char itoabuf[128]; /* stay safe about int->string convertion len */

	/* Very important: note that we only collect in this place.
	 * In theory, it may be better to collect in every basic
	 * reference operation, like getbyref, setref, and so on,
	 * but think about the following code:
	 *
	 * while 1 {
	 * 	getbyref [ref x]
	 * }
	 *
	 * What happens if getbyref run the collector? The [ref x]
	 * generated reference is only stored in the arguments Tcl_Obj
	 * vector of the getbyref C implementation, so it's not reachable
	 * by the collector (that collects in the Tcl environment, not
	 * the C one). So getbyref will destroy this valid reference
	 * and then will try to access to it!
	 *
	 * There are many solutions. For example to pass to
	 * CollectIfNeeded() the objv of the calling function
	 * in order to collect arguments objects.
	 *
	 * Our solution is just to collect *only* in the [ref] function.
	 * This is ok for most applications. If an application needs
	 * to free memory, it always allocates more objects with [ref].
	 * For the 0,000001% case, just it's possible to call [collect]
	 * by hand.
	 *
	 * Why it should be safe to call the collector here?
	 * Because the [ref] argument is always a value.
	 * So the code:
	 *
	 * while 1 {
	 * 	ref [ref x]
	 * }
	 *
	 * Will work even if the first [ref] command will run the
	 * collector and will destroy the reference just created
	 * by the [ref x] call. Anyway it's just a string (representing
	 * a reference, but that reference *can't* be rachable anyway).
	 *
	 * Ok... all this to say: DONT call RefCollectIfNeeded() in
	 * other possibly not safe places. Another example of non-safe
	 * place where to call the collector is code where you created
	 * references that you hold only in C variables where the
	 * collector is running.
	 */
	if (RefCollectIfNeeded(interp, rd) != TCL_OK)
		return NULL;

	/* Create the reference name and assign it in the array */
	sprintf(itoabuf, "<ref:%lld>", rd->nextid++);

	/* Set the reference in the references array.
	 * Note that SetVar2Ex increments the assigned object refcount. */
	if (!Tcl_SetVar2Ex(interp, "::ref::reftab", itoabuf, objPtr,
	     TCL_LEAVE_ERR_MSG))
	{
		return NULL;
	}
	return Tcl_NewStringObj(itoabuf, -1);
}

/* Set the reference object 'refObj' to the 'objPtr' object content.
 * On error returns TCL_ERROR and leaves an appropriate error string
 * as result.
 *
 * Refcount: the 'objPtr' reference count is incremented. */
static int Tcl_SetRefObj(Tcl_Interp *interp,
		struct RefData *rd, Tcl_Obj *refObj, Tcl_Obj *objPtr)
{
	Tcl_Obj *refContentObj; /* The object 'inside' the reference. */

	/* Try to get the reference in the references array */
	refContentObj = Tcl_GetVar2Ex(interp, "::ref::reftab",
			Tcl_GetStringFromObj(refObj, NULL), 0);
	/* Complain if it's not a valid reference */
	if (refContentObj == NULL) {
		Tcl_ResetResult(interp);
		Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			"Invalid reference '",
			Tcl_GetStringFromObj(refObj, NULL),
			"'", NULL);
		return TCL_ERROR;
	}
	/* Set to the new object */
	if (!Tcl_SetVar2Ex(interp, "::ref::reftab",
		Tcl_GetStringFromObj(refObj, NULL),
		objPtr, TCL_LEAVE_ERR_MSG))
	{
		return TCL_ERROR;
	}
	return TCL_OK;
}

/* set '*objPtrPtr' to the object inside the 'refObj' reference.
 * On error returns TCL_ERROR and leaves an appropriate error
 * string as result.
 *
 * The returned object reference count is unaltered. */
static int Tcl_GetObjByRef(Tcl_Interp *interp,
		struct RefData *rd, Tcl_Obj *refObj, Tcl_Obj **objPtrPtr)
{
	Tcl_Obj *refContentObj;

	/* Get the object in the references array */
	refContentObj = Tcl_GetVar2Ex(interp, "::ref::reftab",
			Tcl_GetStringFromObj(refObj, NULL), 0);
	if (refContentObj == NULL) {
		Tcl_ResetResult(interp);
		Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			"Invalid reference '",
			Tcl_GetStringFromObj(refObj, NULL),
			"'", NULL);
		return TCL_ERROR;
	}
	*objPtrPtr = refContentObj;
	return TCL_OK;
}

/* Set the content of the 'refObj' reference to a null string
 * and returns the previous object content.
 *
 * On error NULL is returned, and an error message set as
 * result.
 *
 * The refcount of the returned object is incremented. */
static Tcl_Obj *Tcl_EmtpyRefObj(Tcl_Interp *interp,
		struct RefData *rd, Tcl_Obj *refObj)
{
	Tcl_Obj *refContentObj;

	if (Tcl_GetObjByRef(interp, rd, refObj, &refContentObj) != TCL_OK)
		return NULL;
	/* We need to increment the object refcount
	 * to avoid it gets freed. */
	Tcl_IncrRefCount(refContentObj);
	if (Tcl_SetRefObj(interp, rd, refObj, rd->emptyObj) != TCL_OK) {
		/* Undo the refcount increment. */
		Tcl_DecrRefCount(refContentObj);
		return NULL;
	}
	return refContentObj;
}

/* ----------------------- Commands implementation -------------------------- */

/* Create a new reference that points to the passed object */
static int RefRefObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct RefData *rd = (struct RefData*) clientData;
	Tcl_Obj *refObj;

	if (objc != 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "value");
		return TCL_ERROR;
	}

	if (!(refObj = Tcl_NewRefObj(interp, rd, objv[1])))
		return TCL_ERROR;
	Tcl_SetObjResult(interp, refObj);
	return TCL_OK;
}

/* Create a new reference that points to the passed object */
static int RefGetByRefObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct RefData *rd = (struct RefData*) clientData;
	Tcl_Obj *refContentObj;

	if (objc != 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "value");
		return TCL_ERROR;
	}
	if (Tcl_GetObjByRef(interp, rd, objv[1], &refContentObj) != TCL_OK)
		return TCL_ERROR;
	Tcl_SetObjResult(interp, refContentObj);
	return TCL_OK;
}

/* Set a reference to a different object. */
static int RefSetRefObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct RefData *rd = (struct RefData*) clientData;
	if (objc != 3) {
		Tcl_WrongNumArgs(interp, 1, objv, "reference value");
		return TCL_ERROR;
	}
	if (Tcl_SetRefObj(interp, rd, objv[1], objv[2]) != TCL_OK)
		return TCL_ERROR;
	Tcl_SetObjResult(interp, objv[1]);
	return TCL_OK;
}

/* Set a reference to an empty string object, and return the
 * previously hold object.
 * This is very useful for efficiency reason as is able
 * to get the referenced object with a refcount == 1.
 * (similar to the K operator in other contexts) */
static int RefEmptyObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct RefData *rd = (struct RefData*) clientData;
	Tcl_Obj *oldContentObj;

	if (objc != 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "reference");
		return TCL_ERROR;
	}
	if (!(oldContentObj = Tcl_EmtpyRefObj(interp, rd, objv[1])))
		return TCL_ERROR;
	Tcl_SetObjResult(interp, oldContentObj);
	Tcl_DecrRefCount(oldContentObj); /* Tcl_SetObjResult() increments it */
	return TCL_OK;
}

/* Set the finalizer command for a reference */
static int RefSetFinalizerObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	Tcl_Obj *objPtr;

	if (objc != 3) {
		Tcl_WrongNumArgs(interp, 1, objv, "reference commandName");
		return TCL_ERROR;
	}

	/* Try to get the reference in the references array */
	objPtr = Tcl_GetVar2Ex(interp, "::ref::reftab",
			Tcl_GetStringFromObj(objv[1], NULL), 0);
	/* Complain if it's not a valid reference */
	if (objPtr == NULL) {
		Tcl_ResetResult(interp);
		Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			"Invalid reference '",
			Tcl_GetStringFromObj(objv[1], NULL),
			"'", NULL);
		return TCL_ERROR;
	}
	/* Set the finalizer to the given command */
	Tcl_SetVar2Ex(interp, "::ref::finalizer", Tcl_GetStringFromObj(objv[1], NULL), objv[2], 0);
	Tcl_SetObjResult(interp, objv[1]);
	return TCL_OK;
}

/* Return true if the object is a valid reference */
static int RefIsRefObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	Tcl_Obj *objPtr;

	if (objc != 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "reference");
		return TCL_ERROR;
	}

	/* Try to get the reference in the references array */
	objPtr = Tcl_GetVar2Ex(interp, "::ref::reftab",
			Tcl_GetStringFromObj(objv[1], NULL), 0);
	/* Return a boolean value */
	if (objPtr == NULL) {
		Tcl_SetIntObj(Tcl_GetObjResult(interp), 0);
	} else {
		Tcl_SetIntObj(Tcl_GetObjResult(interp), 1);
	}
	return TCL_OK;
}

/* Force a GC run */
static int RefCollectObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct RefData *rd = (struct RefData*) clientData;

	if (objc != 1) {
		Tcl_WrongNumArgs(interp, 1, objv, NULL);
		return TCL_ERROR;
	}

	RefGarbageCollector(interp, rd);
	return TCL_OK;
}

/*------------------------------------------------------------------------------
 * Structures.
 *
 * Exported commnads:
 *
 * struct
 * struct-type
 * struct-fields
 *----------------------------------------------------------------------------*/

/* ClientData for struct commands */
struct StructData {
	Tcl_Obj *template; /* An empty structure of this type */
	int index; /* Index of the field in the tcl list */
	struct RefData *rd; /* Pointer to the reference client data
				    for the same interpreter. */
};

/* Free the clientData of struct generated commands */
void StructDeleteProc(ClientData *clientData)
{
	struct StructData *sd = (struct StructData*) clientData;

	Tcl_DecrRefCount(sd->template);
	ckfree((char*)sd);
}

/* Generic 'make' command for structures */
static int RefStructMakeObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct StructData *sd = (struct StructData *) clientData;
	Tcl_CmdInfo cmdInfo;
	Tcl_Obj *fakeObjv[2];

	if (objc != 1) {
		Tcl_WrongNumArgs(interp, 1, objv, "");
		return TCL_ERROR;
	}

	/* Get the command info to call ::ref::ref with
	 * the right clientData argument. */
	if (Tcl_GetCommandInfo(interp, "::ref::ref", &cmdInfo) == 0) {
		Tcl_SetStringObj(Tcl_GetObjResult(interp),
			"Unknown command ::ref::ref", -1);
		return TCL_ERROR;
	}
	fakeObjv[1] = sd->template;
	return RefRefObjCmd(cmdInfo.objClientData, interp, 2, fakeObjv);
}

/* Generic 'get field' command for structures */
static int RefStructGetObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct StructData *sd = (struct StructData *) clientData;
	Tcl_Obj *cmdStructNameObj, *objStructNameObj, *listObj, *fieldObj;
	char *cmdStructName, *objStructName;

	if (objc != 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "structRef");
		return TCL_ERROR;
	}

	/* Get the list from the reference */
	if (Tcl_GetObjByRef(interp, sd->rd, objv[1], &listObj) != TCL_OK)
		return TCL_ERROR;

	/* Check if the struct name matches. Note that there are
	 * good changes that the name from the template and fromt
	 * the actual structure are =the same= Tcl_Obj. So we try
	 * to compare by pointer. This gives us a fast execution path
	 * in most cases. */
	if (Tcl_ListObjIndex(interp, listObj, 0, &objStructNameObj) != TCL_OK)
		return TCL_ERROR;
	if (Tcl_ListObjIndex(interp, sd->template, 0, &cmdStructNameObj) != TCL_OK)
		return TCL_ERROR;
	if (objStructNameObj != cmdStructNameObj) { /* compare by pointer */
		int l1, l2;
		/* Failed.. compare by value */
		cmdStructName = Tcl_GetStringFromObj(cmdStructNameObj, &l1);
		objStructName = Tcl_GetStringFromObj(objStructNameObj, &l2);
		if (l1 != l2 || memcmp(cmdStructName, objStructName, l1) != 0) {
			Tcl_ResetResult(interp);
			Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
				"Command for '", cmdStructName, "' structure "
				"called against '", objStructName,
				"' structure", NULL);
			return TCL_ERROR;
		}
	}
	/* Get the requested element */
	if (Tcl_ListObjIndex(interp, listObj, sd->index, &fieldObj) != TCL_OK)
		return TCL_ERROR;
	Tcl_SetObjResult(interp, fieldObj);
	return TCL_OK;
}

/* Function similar to TclListObjSetElement(), that is
 * currently not exported in tcl.h */
static int Tcl_ListObjSetIndex(Tcl_Interp *interp, Tcl_Obj *listPtr,
		int index, Tcl_Obj *objPtr)
{
	Tcl_Obj **objv, **objvcopy;
	int objc, i;

	if (Tcl_IsShared(listPtr)) {
		Tcl_Panic("Tcl_ListObjSetIndex called with a shared object");
	}
	if (Tcl_ListObjGetElements(interp, listPtr, &objc, &objv) != TCL_OK)
		return TCL_ERROR;
	if (index < 0 || index >= objc) {
		Tcl_SetStringObj(Tcl_GetObjResult(interp),
			"bad index passed to Tcl_LIstObjSetIndex()", -1);
		return TCL_ERROR;
	}
	objvcopy = (void*) ckalloc(sizeof(Tcl_Obj*)*objc);
	memcpy(objvcopy, objv, sizeof(Tcl_Obj*)*objc);
	objvcopy[index] = objPtr;
	/* We need to increment the refcount of the objects,
	 * we don't know if Tcl_SetListObj() will first increment
	 * or decrement the ref counts. */
	for (i = 0; i < objc; i++)
		Tcl_IncrRefCount(objvcopy[i]);
	Tcl_SetListObj(listPtr, objc, objvcopy);
	/* Undo the increment */
	for (i = 0; i < objc; i++)
		Tcl_DecrRefCount(objvcopy[i]);
	ckfree((void*)objvcopy);
	return TCL_OK;
}

/* Generic 'set field' command for structures */
static int RefStructSetObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct StructData *sd = (struct StructData *) clientData;
	Tcl_Obj *cmdStructNameObj, *objStructNameObj, *listObj;
	char *cmdStructName, *objStructName;

	if (objc != 3) {
		Tcl_WrongNumArgs(interp, 1, objv, "structRef newValue");
		return TCL_ERROR;
	}

	/* Get the list from the reference */
	if (Tcl_GetObjByRef(interp, sd->rd, objv[1], &listObj) != TCL_OK)
		return TCL_ERROR;

	/* Check if the struct name matches. Note that there are
	 * good changes that the name from the template and fromt
	 * the actual structure are =the same= Tcl_Obj. So we try
	 * to compare by pointer. This gives us a fast execution path
	 * in most cases. */
	if (Tcl_ListObjIndex(interp, listObj, 0, &objStructNameObj) != TCL_OK)
		return TCL_ERROR;
	if (Tcl_ListObjIndex(interp, sd->template, 0, &cmdStructNameObj) != TCL_OK)
		return TCL_ERROR;
	if (objStructNameObj != cmdStructNameObj) { /* compare by pointer */
		int l1, l2;
		/* Failed.. compare by value */
		cmdStructName = Tcl_GetStringFromObj(cmdStructNameObj, &l1);
		objStructName = Tcl_GetStringFromObj(objStructNameObj, &l2);
		if (l1 != l2 || memcmp(cmdStructName, objStructName, l1) != 0) {
			Tcl_ResetResult(interp);
			Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
				"Command for '", cmdStructName, "' structure "
				"called against '", objStructName,
				"' structure", NULL);
			return TCL_ERROR;
		}
	}
	/* Set the requested field */
	if (Tcl_IsShared(listObj)) {
		Tcl_Obj *dup;
		/* Copy it, and set the copy as reference content
		 * if we are not the only owners of the object. */
		dup = Tcl_DuplicateObj(listObj);
		if (Tcl_SetRefObj(interp, sd->rd, objv[1], dup) != TCL_OK) {
			/* Free the dup object */
			Tcl_IncrRefCount(dup);
			Tcl_DecrRefCount(dup);
			return TCL_ERROR;
		}
		listObj = dup;
	}
	if (Tcl_ListObjSetIndex(interp, listObj, sd->index, objv[2]) != TCL_OK)
		return TCL_ERROR;
	Tcl_SetObjResult(interp, objv[1]);
	return TCL_OK;
}

/* Return a copy of the 'struct data' structure.
 * Increments the referenced 'tempalte' Tcl Object. */
static struct StructData *StructDataCopy(struct StructData *sd)
{
	struct StructData *sdcopy;

	sdcopy = (void*) ckalloc(sizeof(*sd));
	memcpy(sdcopy, sd, sizeof(*sd));
	Tcl_IncrRefCount(sd->template);
	return sdcopy;
}

/* Return the type of the referenced structure */
static int RefStructTypeObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	struct RefData *rd = (struct RefData *) clientData;
	Tcl_Obj *listObj, *fieldObj;

	if (objc != 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "structRef");
		return TCL_ERROR;
	}

	/* Get the list from the reference */
	if (Tcl_GetObjByRef(interp, rd, objv[1], &listObj) != TCL_OK)
		return TCL_ERROR;

	/* Get the 'struct name' element */
	if (Tcl_ListObjIndex(interp, listObj, 0, &fieldObj) != TCL_OK)
		return TCL_ERROR;
	Tcl_SetObjResult(interp, fieldObj);
	return TCL_OK;
}

/* Create a new structure and all the commands needed to access it */
static int RefStructObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	char *structName, *aux;
	Tcl_Obj *templateObj, **tplObjv;
	int i, namelen, tplObjc, fields;
	struct StructData *sd;

	if (objc < 3) {
		Tcl_WrongNumArgs(interp, 1, objv, "structName fieldName ?...?");
		return TCL_ERROR;
	}

	structName = Tcl_GetStringFromObj(objv[1], &namelen);
	/* Create the template object. This is a list representing
	 * an empty struct. The 'make' command will just return
	 * this object, that will be automatically copied on write. */
	fields = objc - 2;
	tplObjc = 1+(fields*2);
	tplObjv = alloca(sizeof(Tcl_Obj*)*tplObjc);
	tplObjv[0] = Tcl_NewStringObj(structName, -1);
	for (i = 0; i < fields; i++) {
		int listlen;
		if (Tcl_ListObjLength(NULL, objv[i+2], &listlen) != TCL_OK ||
		    listlen != 2)
		{
			tplObjv[1+(i*2)] = objv[i+2];
			tplObjv[2+(i*2)] = Tcl_NewStringObj(NULL, 0);
		} else {
			Tcl_Obj *fieldNameObj, *fieldDefObj;

			Tcl_ListObjIndex(interp, objv[i+2], 0, &fieldNameObj);
			Tcl_ListObjIndex(interp, objv[i+2], 1, &fieldDefObj);
			tplObjv[1+(i*2)] = fieldNameObj;
			tplObjv[2+(i*2)] = fieldDefObj;
		}
	}
	templateObj = Tcl_NewListObj(tplObjc, tplObjv);

	/* Create the client data structure */
	sd = (void*) ckalloc(sizeof(*sd));
	sd->template = templateObj;
	sd->rd = (struct RefData*) clientData;
	sd->index = 0;
	Tcl_IncrRefCount(sd->template);

	/* Create the 'make-<structname>' command */
	aux = ckalloc(namelen+16);
	sprintf(aux, "make-%s", structName);
	Tcl_CreateObjCommand(interp, aux, RefStructMakeObjCmd,
			(ClientData)sd, (Tcl_CmdDeleteProc*)StructDeleteProc);
	ckfree(aux);
	/* Create the '<structname>.<field>' commands */
	for (i = 0; i < fields; i++) {
		char *fieldname = Tcl_GetStringFromObj(tplObjv[1+(i*2)], NULL);
		struct StructData *sdcopy;

		aux = ckalloc(namelen+strlen(fieldname)+16);
		sprintf(aux, "%s.%s", structName, fieldname);
		sdcopy = StructDataCopy(sd);
		sdcopy->index = 2+(i*2);
		/* Create the 'get' command for this field */
		Tcl_CreateObjCommand(interp, aux, RefStructGetObjCmd,
			(ClientData)sdcopy,
			(Tcl_CmdDeleteProc*)StructDeleteProc);
		/* Create the 'set' command for this field */
		sdcopy = StructDataCopy(sdcopy);
		/* the index is the same */
		sprintf(aux, "set-%s.%s", structName, fieldname);
		Tcl_CreateObjCommand(interp, aux, RefStructSetObjCmd,
			(ClientData)sdcopy,
			(Tcl_CmdDeleteProc*)StructDeleteProc);
		ckfree(aux);
	}
	return TCL_OK;
}

/*------------------------------------------------------------------------------
 * Debugging
 *----------------------------------------------------------------------------*/

static int DumpArrayContent(Var *array)
{
	Tcl_HashSearch hSearch;
	Tcl_HashEntry *hPtr;
	Var *var;
	char *name;

	printf("\n");
	hPtr = Tcl_FirstHashEntry(array->value.tablePtr, &hSearch);
	for (;hPtr != NULL; hPtr = Tcl_NextHashEntry(&hSearch)) {
		name = (char*) Tcl_GetHashKey(array->value.tablePtr, hPtr);
		var = (Var*) Tcl_GetHashValue(hPtr);
		printf("        %s --> %s\n", name, Tcl_GetStringFromObj(var->value.objPtr, NULL));
	}
	return 0;
}

static int RefDebugInterpInfoObjCmd(ClientData clientData, Tcl_Interp *interp,
		int objc, Tcl_Obj *CONST objv[])
{
	Interp *iPtr = (Interp*) interp;
	CallFrame *cf = iPtr->framePtr;
	char *strRepr;
	Var *v;

	while(cf) {
		int i;

		printf("CallFrame = %p\n", cf);
		printf("  numCompiledLocals = %d\n", cf->numCompiledLocals);
		for (i = 0; i < cf->numCompiledLocals; i++) {
			v = &cf->compiledLocals[i];
			printf("    %s ", v->name);
			if (v->flags & VAR_SCALAR) {
				printf("SCALAR");
				if (v->value.objPtr) {
					strRepr = Tcl_GetStringFromObj(v->value.objPtr, NULL);
					printf(" value(%s)", strRepr);
				} else {
					printf(" unset");
				}
			} else {
				printf("ARRAY");
				DumpArrayContent(v);
			}
			printf("\n");
		}
		printf("  varTable variables\n");
		if (!cf->varTablePtr) {
			printf("    empty\n");
		} else {
			Tcl_HashSearch hSearch;
			Tcl_HashEntry *hPtr;
			hPtr = Tcl_FirstHashEntry(cf->varTablePtr, &hSearch);
			for(;hPtr != NULL; hPtr = Tcl_NextHashEntry(&hSearch)) {
				char *name;

				v = (Var*) Tcl_GetHashValue(hPtr);
				name = (char*) Tcl_GetHashKey(cf->varTablePtr, hPtr);
				printf("    %s ", name);
				if (v->flags & VAR_SCALAR) {
					printf("SCALAR");
					if (v->value.objPtr) {
						strRepr = Tcl_GetStringFromObj(v->value.objPtr, NULL);
						printf(" value(%s)", strRepr);
					} else {
						printf(" unset");
					}
				} else {
					printf("ARRAY");
					DumpArrayContent(v);
				}
				printf("\n");
			}
		}
		printf("\n");
		cf = cf->callerPtr;
	}
	return TCL_OK;
}

/*------------------------------------------------------------------------------
 * Initialization
 *----------------------------------------------------------------------------*/
#if !defined(__WIN32__) && !defined(MAC_TCL) /* UNIX */
void EnlargeStackSize(void)
{
	struct rlimit rlim;

	/* Note that this doesn't completly fix the problem
	 * of Tcl not using lazy-freeing of objects, but just
	 * makes it able to recurse a bit more.
	 * For some reasons threaded Tcl interpreters crash
	 * anyway after some point. */
	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_STACK, &rlim);
}
#else
void EnlargeStackSize(void)
{
	return;
}
#endif

/* Ref_InitData - Initialize the private data structure */
void Ref_InitData(struct RefData *rd)
{
	rd->debug = 0;
	rd->nextid = 0;
	rd->calls = 0;
	rd->collection_period = TCL_REF_COLLPERIOD;
	rd->emptyObj = Tcl_NewStringObj(NULL, 0);
	Tcl_IncrRefCount(rd->emptyObj);
}

/* Initialization on loading */
int References_Init(Tcl_Interp *interp)
{
	struct RefData *rd;

	if (Tcl_InitStubs(interp, "8.0", 0) == NULL)
		return TCL_ERROR;
	if (Tcl_PkgRequire(interp, "Tcl", TCL_VERSION, 0) == NULL)
		return TCL_ERROR;
	if (Tcl_PkgProvide(interp, "references", VERSION) != TCL_OK)
		return TCL_ERROR;

	/* Types unable to hold references. Take it global.
	 * Can't create problems and is much
	 * faster in the GC scan phase. */
	IntType = Tcl_GetObjType("int");
	DoubleType = Tcl_GetObjType("double");
	BooleanType = Tcl_GetObjType("boolean");
	WideIntType = Tcl_GetObjType("wideInt");
	IndexType = Tcl_GetObjType("index");
	/* Other useful object types. */
	ListType = Tcl_GetObjType("list");
	DictType = Tcl_GetObjType("dict");

	/* Alloc private data */
	rd = (void*) ckalloc(sizeof(*rd));
	if (!rd) {
		Tcl_SetStringObj(Tcl_GetObjResult(interp),
			"Out of memory allocating 'References' "
			"extension private data", -1);
		return TCL_ERROR;
	}
	Ref_InitData(rd);

	/* Modify the stack limit. Does not perfectly work with threaded
	 * Tcl builds. But is able to enlarge a bit the max stack
	 * even in threaded Tcl a bit. */
	EnlargeStackSize();

	if (Tcl_Eval(interp,
	  "namespace eval ::ref {}; "
	  "namespace eval ::refdebug {}; "
	  "namespace eval ::struct {};") != TCL_OK) {
		ckfree((char*)rd);
		return TCL_ERROR;
	}
	/* Load the GC helper written in Tcl */
	if (Tcl_Eval(interp, Tcl_GcHelperText) != TCL_OK) {
		ckfree((char*)rd);
		return TCL_ERROR;
	}
	/* References commands */
	Tcl_CreateObjCommand(interp, "::ref::ref", RefRefObjCmd,
			(ClientData)rd, (Tcl_CmdDeleteProc*)NULL);
	Tcl_CreateObjCommand(interp, "::ref::getbyref", RefGetByRefObjCmd,
			(ClientData)rd, (Tcl_CmdDeleteProc*)NULL);
	Tcl_CreateObjCommand(interp, "::ref::setref", RefSetRefObjCmd,
			(ClientData)rd, (Tcl_CmdDeleteProc*)NULL);
	Tcl_CreateObjCommand(interp, "::ref::isref", RefIsRefObjCmd,
			(ClientData)rd, (Tcl_CmdDeleteProc*)NULL);
	Tcl_CreateObjCommand(interp, "::ref::collect",
			RefCollectObjCmd, (ClientData)rd,
			(Tcl_CmdDeleteProc*)NULL);
	Tcl_CreateObjCommand(interp, "::ref::emptyref", RefEmptyObjCmd,
			(ClientData)rd, (Tcl_CmdDeleteProc*)NULL);
	Tcl_CreateObjCommand(interp, "::ref::setfinalizer", RefSetFinalizerObjCmd,
			(ClientData)rd, (Tcl_CmdDeleteProc*)NULL);
	Tcl_Eval(interp, "namespace eval ::ref {namespace export *}; "
			 "namespace eval ::struct {namespace export *}");
	/* Debugging commands */
	Tcl_CreateObjCommand(interp, "::refdebug::interpinfo",
			RefDebugInterpInfoObjCmd,
			(ClientData)NULL, (Tcl_CmdDeleteProc*)NULL);
	/* Structures commands */
	Tcl_CreateObjCommand(interp, "::struct::struct", RefStructObjCmd,
			(ClientData)rd, (Tcl_CmdDeleteProc*)NULL);
	Tcl_CreateObjCommand(interp, "::struct::struct-type",
			RefStructTypeObjCmd,
			(ClientData)rd, (Tcl_CmdDeleteProc*)NULL);
	/* Private data initialization here */
	return TCL_OK;
}
