cmake_minimum_required(VERSION 3.12)

# try to set the best C++ language level
set(CMAKE_CXX_STANDARD 20)
# let it take the lowest version, we need some precursor of C++14x
#set(CMAKE_CXX_STANDARD_REQUIRED on)

# don't care for now... just expecting POSIX
set(CMAKE_LEGACY_CYGWIN_WIN32 0)

set(PACKAGE "apt-cacher-ng")

PROJECT(${PACKAGE} CXX C)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})
#set(CMAKE_SKIP_BUILD_RPATH on) # no -rdynamic needed ever

INCLUDE(CheckIncludeFiles) 
INCLUDE(CheckCXXSourceCompiles)
INCLUDE(CheckCXXCompilerFlag)
INCLUDE(CheckTypeSize)
INCLUDE(TestBigEndian)
INCLUDE(CheckFunctionExists)
INCLUDE(CheckLibraryExists)
INCLUDE(CheckSymbolExists)
INCLUDE(FindPkgConfig)
INCLUDE(GNUInstallDirs)
INCLUDE(CheckLinkerFlag OPTIONAL RESULT_VARIABLE HaveLinkerCheck)

IF(NOT DEFINED(CMAKE_INSTALL_PREFIX))
    set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE STRING "Target file space")
ENDIF()
IF(NOT DEFINED(LIBDIR))
    set(LIBDIR "${CMAKE_INSTALL_PREFIX}/lib/${PACKAGE}" CACHE STRING "Location of ${PACKAGE} extra files")
ENDIF()
if(NOT DEFINED(CFGDIR))
    set(CFGDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/${PACKAGE}")
endif()
if(NOT DEFINED(RUNDIR))
    set(RUNDIR "/run")
endif()
set(SOCKET_PATH "${RUNDIR}/${PACKAGE}/socket")


IF(NOT DEFINED(ACNG_CACHE_DIR))
    set(ACNG_CACHE_DIR "/var/tmp" CACHE STRING "Cache folder for examples and default configuration")
ENDIF()

IF(NOT DEFINED(ACNG_LOG_DIR))
    set(ACNG_LOG_DIR "/var/tmp" CACHE STRING "Log file folder for examples and default configuration")
ENDIF()

INCLUDE_DIRECTORIES(${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR} "include")

IF(NOT DEFINED(ACVERSION))
    FILE(READ "${CMAKE_SOURCE_DIR}/VERSION" ACVERSION)
    string(REPLACE "\n" "" ACVERSION ${ACVERSION})
    #MESSAGE(STATUS "Version: ${ACVERSION}")
ENDIF()

# Various feature checks
CHECK_INCLUDE_FILES ("sys/param.h;sys/mount.h" HAVE_SYS_MOUNT_H)
CHECK_INCLUDE_FILES ("sys/vfs.h" HAVE_SYS_VFS_H)
CHECK_TYPE_SIZE(int SIZE_INT)
CHECK_TYPE_SIZE(long SIZE_LONG)
CHECK_TYPE_SIZE(char* SIZE_PTR)
TEST_BIG_ENDIAN(WORDS_BIGENDIAN)

CHECK_FUNCTION_EXISTS(strlcpy HAVE_STRLCPY)

# collectors of build flags
foreach(tok acng_common acng_network acng_crypto acng_server acng_compression)
    add_library(${tok} INTERFACE)
endforeach()

macro(_sethave _pcpkg)
    if(TARGET PkgConfig::${_pcpkg})
        set("HAVE_${_pcpkg}" TRUE)
        set("${_pcpkg}_FOUND" TRUE)
        foreach(arg ${ARGN})
            target_link_libraries(${arg} INTERFACE PkgConfig::${_pcpkg})
        endforeach()
    else()
        set("HAVE_${_pcpkg}" FALSE)
        set("${_pcpkg}_FOUND" FALSE)
    endif()
endmacro()

target_compile_definitions(acng_common INTERFACE -D_FILE_OFFSET_BITS=64)
target_include_directories(acng_common INTERFACE ${CMAKE_SOURCE_DIR}/src)

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)
if(Threads_FOUND)
    target_link_libraries(acng_common INTERFACE Threads::Threads)
endif()

foreach(cxxarg -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fvisibility-inlines-hidden)
    STRING(REGEX REPLACE "=|-|," "" optname "${cxxarg}")
    CHECK_CXX_COMPILER_FLAG("${cxxarg}" "COPT_${optname}")
    if(COPT_${optname})
        target_compile_options(acng_common INTERFACE ${cxxarg})
    endif()
endforeach()

foreach(linkarg --as-needed -O1 --discard-all --no-undefined --build-id=sha1 -fuse-ld=gold -fuse-ld=mold)
    STRING(REGEX REPLACE "=|-|," "" optname "${linkarg}")
    # XXX: using the legacy method, not check_linker_flag due to cmake 3.18 requirement
    set(CMAKE_REQUIRED_FLAGS "-Wl,${linkarg}")
    CHECK_CXX_COMPILER_FLAG("" "LD_${optname}")
    set(CMAKE_REQUIRED_FLAGS "")
    if(LD_${optname})
        target_link_options(acng_common INTERFACE "-Wl,${linkarg}")
    endif()
endforeach(linkarg)

#set(CMAKE_REQUIRED_FLAGS "-Wl,-fuse-ld=gold -Wl,--threads")
#CHECK_CXX_COMPILER_FLAG("" LD_MULTITHREADED)
#if(LD_MULTITHREADED)
#	_append(CMAKE_EXE_LINKER_FLAGS "-Wl,-fuse-ld=gold -Wl,--threads")
#endif()
#set(CMAKE_REQUIRED_FLAGS "")

option(USE_SSL "Use OpenSSL library for TLS and other crypto functionality" on)
option(ENABLE_TOOL "Build the management tool (acngtool)" on)
option(ENABLE_EXTRA_TOOLS "Build additional command line tools (acngfs, inetd client)" off)
option(ENABLE_TESTS "Build tests (for development only)" off)
option(EXTRA_DEBUG "Additional logging and tracing (for development only)" off)

IF(CMAKE_SYSTEM MATCHES "Darwin")
    target_compile_definitions(acng_common INTERFACE -D_DARWIN_C_SOURCE)
ENDIF()

IF(CMAKE_SYSTEM MATCHES "CYGWIN")
    set(USE_LTO_DEFAULT off)
    target_compile_definitions(acng_common INTERFACE -DNOMINMAX)
    target_compile_options(acng_common INTERFACE -U__STRICT_ANSI__)
ELSEIF(CMAKE_COMPILER_IS_GNUCXX AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER "11")
    set(USE_LTO_DEFAULT on)
ELSE()
    set(USE_LTO_DEFAULT off)
ENDIF()

if(NOT CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_REQUIRED_FLAGS "-Wl,--gc-sections")
    CHECK_CXX_COMPILER_FLAG("-Os -fdata-sections -ffunction-sections -Wl,--gc-sections" GC_SECTIONS)
    set(CMAKE_REQUIRED_FLAGS "")
    if(GC_SECTIONS)
        target_compile_options(acng_common INTERFACE -Wl,-fdata-sections -Wl,-ffunction-sections)
        target_link_options(acng_common INTERFACE -Wl,--gc-sections)
    endif()
endif()

option(USE_LTO "Enable Link Time Optimization (requires modern compilers)" ${USE_LTO_DEFAULT})

if(USE_LTO AND HaveLinkerCheck)
    check_linker_flag(CXX -flto=auto HAVE_LTO_AUTO)
    if(HAVE_LTO_AUTO)
        target_link_options(acng_common INTERFACE -flto=auto)
    else()
        check_linker_flag(CXX -flto HAVE_LD_LTO)
        if(HAVE_LD_LTO)
            target_link_options(acng_common INTERFACE -flto)
        endif()
    endif()
    if(HAVE_LD_LTO OR HAVE_LTO_AUTO)
        CHECK_CXX_COMPILER_FLAG("-flto" HAVE_CXX_LTO)
        if(HAVE_CXX_LTO)
            target_compile_options(acng_common INTERFACE -flto)
        endif()
    endif()
endif()

FIND_LIBRARY(HAVE_SOCKETLIB socket) # separate socket lib looks like Solaris-like environment
if(HAVE_SOCKETLIB)
    set(BaseNetworkLibs socket nsl)
    target_link_libraries(acng_network INTERFACE ${BaseNetworkLibs})
endif(HAVE_SOCKETLIB)

set(TESTKITDIR "tests/build")
SET(CMAKE_REQUIRED_LIBRARIES wrap ${BaseNetworkLibs})
FILE(READ ${TESTKITDIR}/HAVE_LIBWRAP.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LIBWRAP)
SET(CMAKE_REQUIRED_LIBRARIES "")
IF(HAVE_LIBWRAP)
    target_link_libraries(acng_server INTERFACE wrap)
ELSE(HAVE_LIBWRAP)
    MESSAGE("!! libwrap development files not usable, disabling support")
ENDIF(HAVE_LIBWRAP)

# lib is not in the standard path so FIND_LIBRARY is not reliable
set(CMAKE_REQUIRED_FLAGS "-latomic")
CHECK_CXX_COMPILER_FLAG("" LD_ATOMIC)
if(LD_ATOMIC)
    target_link_libraries(acng_network INTERFACE atomic)
endif()
set(CMAKE_REQUIRED_FLAGS "-lstdc++fs")
CHECK_CXX_COMPILER_FLAG("" HAVE_FSLIB)
if(HAVE_FSLIB)
    target_link_libraries(acng_network INTERFACE stdc++fs)
endif()
set(CMAKE_REQUIRED_FLAGS "")


if(CYGWIN)
    message("!! Not using wordexp on Cygwin, not reliable")
    set(HAVE_WORDEXP off)
else()
    FILE(READ ${TESTKITDIR}/HAVE_WORDEXP.cc TESTSRC)
    CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_WORDEXP)
endif()

# test some methods explicitly where we want to be sure about the signatures
# and linkability

FILE(READ ${TESTKITDIR}/HAVE_GLOB.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_GLOB)

FILE(READ ${TESTKITDIR}/HAVE_FADVISE.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_FADVISE)

FILE(READ ${TESTKITDIR}/HAVE_MADVISE.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_MADVISE)

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    FILE(READ ${TESTKITDIR}/HAVE_LINUX_FALLOCATE.cc TESTSRC)
    CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_FALLOCATE)

    FILE(READ ${TESTKITDIR}/HAVE_LINUX_SENDFILE.cc TESTSRC)
    CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_SENDFILE)

    FILE(READ ${TESTKITDIR}/HAVE_LINUX_EVENTFD.cc TESTSRC)
    CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_EVENTFD)
endif()

FILE(READ ${TESTKITDIR}/HAVE_PREAD.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_PREAD)

FILE(READ ${TESTKITDIR}/HAVE_DAEMON.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_DAEMON)

pkg_check_modules(lsd "libsystemd>=209" IMPORTED_TARGET)
_sethave(lsd acng_server)
# either part of the big library nowadays or in the helper library on older systems
if(NOT lsd_FOUND)
    pkg_check_modules(lsd libsystemd-daemon IMPORTED_TARGET)
    _sethave(lsd acng_server)
endif()
if(lsd_FOUND)
    target_compile_definitions(acng_server INTERFACE -DHAVE_SD_NOTIFY)
    set(HAVE_SD_NOTIFY TRUE)
endif()

pkg_check_modules(zlib zlib IMPORTED_TARGET)
_sethave(zlib acng_compression)
if(NOT zlib_FOUND)
    message(FATAL_ERROR "!! apt-cacher-ng requires gzip library and development files")
endif()

INCLUDE(FindBZip2)
SET(HAVE_LIBBZ2 ${BZIP2_FOUND})
if (BZIP2_FOUND)
    target_link_libraries(acng_compression INTERFACE BZip2::BZip2)
else()
    message("!! apt-cacher-ng requires bzip2 library and development files for bz2 format support")
endif()

pkg_check_modules(lzma liblzma IMPORTED_TARGET)
_sethave(lzma acng_compression)
IF(NOT HAVE_LZMA)
    MESSAGE(WARNING "!! XZ (liblzma) not found or not working, disabling support")
ENDIF()

pkg_check_modules(libevent "libevent>=2.1.2" IMPORTED_TARGET)
_sethave(libevent acng_network)
if(NOT libevent_FOUND)
    message(FATAL_ERROR "!! Error: proper libevent installation is required")
endif()

pkg_check_modules(libevent_pthreads libevent_pthreads IMPORTED_TARGET)
_sethave(libevent_pthreads acng_network)
if(NOT libevent_pthreads_FOUND)
    message(FATAL_ERROR "!! Error: proper libevent_pthreads installation is required")
endif()

pkg_check_modules(cares REQUIRED "libcares>=1.17" IMPORTED_TARGET)
_sethave(cares acng_network)

if(USE_SSL)
    pkg_check_modules(openssl "openssl>=1.1" IMPORTED_TARGET)
    pkg_check_modules(libevossl "libevent_openssl>=2.0.21" IMPORTED_TARGET)
    _sethave(openssl acng_crypto)
    _sethave(libevossl acng_crypto)
    if(openssl_FOUND AND libevossl_FOUND)
        set(HAVE_SSL on)
    else()
        set(HAVE_SSL off)
        message(WARNING "OpenSSL or libevent_openssl not found, please enable TLS support in libevent installation; TLS support will be disabled")
    endif()
endif()

if(NOT HAVE_SSL)
    if(EXISTS "$ENV{TOMCRYPT_HOME}")
        message("!! TOMCRYPT_HOME folder found, will use the library from there")
        target_include_directories(acng_crypto INTERFACE "$ENV{TOMCRYPT_HOME}/src/headers")
        target_link_libraries(acng_crypto INTERFACE tomcrypt)
        target_link_options(acng_crypto INTERFACE -L"$ENV{TOMCRYPT_HOME}")
        set(HAVE_TOMCRYPT on)
    else()
        pkg_check_modules(tomcrypt "libtomcrypt")
        _sethave(tomcrypt)
        # with workaround, http://clang.debian.net/logs/2015-03-25/libtomcrypt_1.17-6_unstable_clang.log
        target_compile_definitions(acng_crypto INTERFACE -DLTC_NO_ROLC -DLTC_BASE64)
    endif()
endif()

if(HAVE_SSL OR HAVE_TOMCRYPT)
    set(HAVE_CHECKSUM on)
else()
    message(WARNING "Could not find LibTomCrypt or use OpenSSL. Some important functionality will be disabled.")
endif()


# -DEXTRA_LIBS_INETD=-lsupc++
# funny hack, link with gcc and avoid libstdc++/libm (since STL is not used
# there). However, it needs to be tested - the alternative linking makes
# the binary ~40kb larger, might include higher relocation costs and bigger
# chunks of unique memory while libstdc++ needs to be loaded anyway for the
# server process.
# Needs HAVE_WL_AS_NEEDED!

#message("Build settings:")
#message("Compiler flags (common): ${ACNG_COMPFLAGS}")
#message("Compiler flags (C++ only): ${ACNG_CXXFLAGS}")
#message("Compiler flags (environment): ${CMAKE_CXX_FLAGS}")
#message("Linker flags: ${CMAKE_EXE_LINKER_FLAGS}")
#message("LTO use: ${USE_LTO}
#")

# unset everything, only use as needed
SET(CMAKE_REQUIRED_LIBRARIES )
SET(CMAKE_REQUIRED_FLAGS )

# I don't need -rdynamic, thanks!
SET(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")
SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

#######################################
# all checks done, save configuration #
#######################################

set(DEBUG ${EXTRA_DEBUG})
CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/src/acsyscap.h.in" "${CMAKE_BINARY_DIR}/acsyscap.h")

if(ENABLE_EXTRA_TOOLS)
    add_subdirectory(client)
    add_subdirectory(fs)
endif()
add_subdirectory(src)
add_subdirectory(conf)
add_subdirectory(systemd)

if(ENABLE_TESTS)
    add_subdirectory(tests)
endif()

###
### Extra install rules for static files
###
install(FILES doc/README doc/${PACKAGE}.pdf DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY doc/html/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/html
    FILES_MATCHING PATTERN "*.*")
install(DIRECTORY doc/man/ DESTINATION ${CMAKE_INSTALL_MANDIR}/man8
    FILES_MATCHING PATTERN "*.8")
if(NOT DEFINED(AVAHIDIR))
    set(AVAHIDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/avahi/services")
endif()
install(FILES contrib/apt-cacher-ng.service DESTINATION ${AVAHIDIR})

message("Installation settings:
PACKAGE: ${PACKAGE}
VERSION: ${ACVERSION}
CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}
CMAKE_INSTALL_FULL_SYSCONFDIR: ${CMAKE_INSTALL_FULL_SYSCONFDIR}
LIBDIR: ${LIBDIR}
AVAHIDIR: ${AVAHIDIR}
EXTRA_DEBUG: ${EXTRA_DEBUG}
CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}
CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}
")

# vim: set sw=4 ts=4 et:
