#
# Generated Makefile - do not edit!
#
# Edit the Makefile in the project folder instead (../Makefile). Each target
# has a -pre and a -post target defined where you can add customized code.
#
# This makefile implements configuration specific macros and targets.


# Environment
MKDIR=mkdir
CP=cp
GREP=grep
NM=nm
CCADMIN=CCadmin
RANLIB=ranlib
CC=gcc
CCC=g++
CXX=g++
FC=gfortran
AS=as

# Macros
CND_PLATFORM=MinGW64-Windows
CND_DLIB_EXT=dll
CND_CONF=Debug
CND_DISTDIR=dist
CND_BUILDDIR=build

# Include project Makefile
include Makefile

# Object Directory
OBJECTDIR=${CND_BUILDDIR}/${CND_CONF}/${CND_PLATFORM}

# Object Files
OBJECTFILES= \
	${OBJECTDIR}/tutorial07.o


# C Compiler Flags
CFLAGS=-fpermissive

# CC Compiler Flags
CCFLAGS=-fpermissive
CXXFLAGS=-fpermissive

# Fortran Compiler Flags
FFLAGS=

# Assembler Flags
ASFLAGS=

# Link Libraries and Options
LDLIBSOPTIONS=-L../../ExternalLibs/ffmpeg/lib -L../../ExternalLibs/SDL2-2.0.5/x86_64-w64-mingw32/lib -L../../ExternalLibs/SDL/libs/lib/x64 -L../../ExternalLibs/SDL/libs/lib -lmingw32 -lSDLmain -lSDL -lavutil -lavformat -lavcodec -lswscale -lz -lm

# Build Targets
.build-conf: ${BUILD_SUBPROJECTS}
	"${MAKE}"  -f nbproject/Makefile-${CND_CONF}.mk ${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}/videojaw.exe

${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}/videojaw.exe: ${OBJECTFILES}
	${MKDIR} -p ${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}
	${LINK.cc} -o ${CND_DISTDIR}/${CND_CONF}/${CND_PLATFORM}/videojaw ${OBJECTFILES} ${LDLIBSOPTIONS}

${OBJECTDIR}/tutorial07.o: tutorial07.cpp
	${MKDIR} -p ${OBJECTDIR}
	${RM} "$@.d"
	$(COMPILE.cc) -g -w -I../../ExternalLibs/ffmpeg/include -I../../ExternalLibs/SDL2-2.0.5/x86_64-w64-mingw32/include -I../../ExternalLibs/SDL/libs/include -MMD -MP -MF "$@.d" -o ${OBJECTDIR}/tutorial07.o tutorial07.cpp

# Subprojects
.build-subprojects:

# Clean Targets
.clean-conf: ${CLEAN_SUBPROJECTS}
	${RM} -r ${CND_BUILDDIR}/${CND_CONF}

# Subprojects
.clean-subprojects:

# Enable dependency checking
.dep.inc: .depcheck-impl

include .dep.inc