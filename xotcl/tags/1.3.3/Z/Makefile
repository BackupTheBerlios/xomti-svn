#XOTCL_VERSION 	= 1.3.3
XOTCL_VERSION 	= 1.3

PKG_SOURCES	=  xotcl.c xotclError.c xotclMetaData.c xotclObjectData.c xotclProfile.c xotclTrace.c xotclUtil.c xotclShadow.c xotclCompile.c aolstub.c xotclStubInit.c
OBJECTS	=  xotcl.o xotclError.o xotclMetaData.o xotclObjectData.o xotclProfile.o xotclTrace.o xotclUtil.o xotclShadow.o xotclCompile.o aolstub.o xotclStubInit.o
OBJECTS	=  xotcl.o xotclError.o                                                  xotclTrace.o xotclUtil.o xotclShadow.o

PACKAGE_NAME	= xotcl
PACKAGE_VERSION	= 1.3.3
CC		= gcc -pipe
CFLAGS_DEFAULT	= -g
CFLAGS_WARNING	= -Wall -Wconversion -Wno-implicit-int
CLEANFILES	= *.o *.a *.so *~ core gmon.out config.*
TCL_DEFS	= -DPACKAGE_NAME=\"tcl\" -DPACKAGE_TARNAME=\"tcl\" -DPACKAGE_VERSION=\"8.5\" -DPACKAGE_STRING=\"tcl\ 8.5\" -DPACKAGE_BUGREPORT=\"\" -DSTDC_HEADERS=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -DHAVE_MEMORY_H=1 -DHAVE_STRINGS_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_UNISTD_H=1 -DHAVE_LIMITS_H=1 -DHAVE_SYS_PARAM_H=1 -DTCL_CFGVAL_ENCODING=\"iso8859-1\" -DSTATIC_BUILD=1 -DPEEK_XCLOSEIM=1 -DTCL_SHLIB_EXT=\".so\" -DTCL_CFG_DEBUG=1 -DTCL_MEM_DEBUG=1 -DTCL_COMPILE_DEBUG=1 -DTCL_COMPILE_STATS=1 -D_LARGEFILE64_SOURCE=1 -DTCL_WIDE_INT_TYPE=long\ long -DHAVE_STRUCT_STAT64=1 -DHAVE_OPEN64=1 -DHAVE_LSEEK64=1 -DHAVE_TYPE_OFF64_T=1 -DHAVE_GETCWD=1 -DHAVE_OPENDIR=1 -DHAVE_STRSTR=1 -DHAVE_STRTOL=1 -DHAVE_STRTOLL=1 -DHAVE_STRTOULL=1 -DHAVE_TMPNAM=1 -DHAVE_WAITPID=1 -DUSE_TERMIOS=1 -DHAVE_SYS_TIME_H=1 -DTIME_WITH_SYS_TIME=1 -DHAVE_STRUCT_TM_TM_ZONE=1 -DHAVE_TM_ZONE=1 -DHAVE_GMTIME_R=1 -DHAVE_LOCALTIME_R=1 -DHAVE_MKTIME=1 -DHAVE_TM_GMTOFF=1 -DHAVE_TIMEZONE_VAR=1 -DHAVE_STRUCT_STAT_ST_BLKSIZE=1 -DHAVE_ST_BLKSIZE=1 -DHAVE_SIGNED_CHAR=1 -DHAVE_LANGINFO=1 -DHAVE_SYS_IOCTL_H=1 -DTCL_UNLOAD_DLLS=1 -DTCL_DBGX=g 
TCL_SRC_DIR	= /prog/Tcl/distrib/tcl8.5a2

SHARED_BUILD	= 0

INCLUDES	=  -I$(TCL_SRC_DIR)/generic -I$(TCL_SRC_DIR)/unix -I.
EXTRA_CFLAGS	=   -DXOTCLVERSION=\"1.3\" -DXOTCLPATCHLEVEL=\".3\" 	 -DHAVE_TCL_COMPILE_H=1


# supprimé = -DUSE_TCL_STUBS=1 -DCOMPILE_XOTCL_STUBS=1
DEFS		= -DPACKAGE_NAME=\"xotcl\" -DPACKAGE_TARNAME=\"xotcl\" -DPACKAGE_VERSION=\"1.3.3\" -DPACKAGE_STRING=\"xotcl\ 1.3.3\" -DPACKAGE_BUGREPORT=\"\" -DSTDC_HEADERS=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -DHAVE_MEMORY_H=1 -DHAVE_STRINGS_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_UNISTD_H=1 -D_LARGEFILE64_SOURCE=1 -DTCL_WIDE_INT_TYPE=long\ long -DHAVE_STRUCT_STAT64=1 -DHAVE_TYPE_OFF64_T=1 -DHAVE_LIMITS_H=1 -DHAVE_UNISTD_H=1 -DHAVE_SYS_PARAM_H=1 -DSTATIC_BUILD=1 -DTCL_MEM_DEBUG=1 $(EXTRA_CFLAGS) 

CFLAGS		=  ${CFLAGS_DEFAULT} ${CFLAGS_WARNING}
COMPILE		= $(CC) $(DEFS) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS)

TCL_LIB_SPEC    = -L/prog/Tcl/IntelLinuxDebugStatic/tcl8.5a2 -ltcl8.5g

all: xotclsh

VPATH = ../src

.c.o:
	$(COMPILE) -c `echo $<` -o $@

xotclsh:

xotclsh: tclAppInit.o $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ tclAppInit.o \
		$(OBJECTS) \
		$(TCL_LIB_SPEC) \
		-ldl -lm 

clean:  
	-rm -rf xotclsh $(CLEANFILES)

tests:
	./xotclsh ../tests/testx.xotcl
	./xotclsh ../tests/testo.xotcl
	./xotclsh ../tests/speedtest.xotcl
	./xotclsh ../tests/xocomm.test
	./xotclsh ../tests/persistence.test
	./xotclsh ../tests/xoRDF.test
