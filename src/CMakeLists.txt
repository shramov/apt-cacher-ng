FILE(GLOB ALL_HEADERS "*.h")

set(SERVER_SPECIFIC_SRCS job.cc conn.cc acregistry.cc )

# mirror.cc
set(MAINTSRCS cacheman.cc expiration.cc maintenance.cc mainthandler.cc pkgimport.cc showinfo.cc bgtask.cc)

set(SHAREDSRCS astrop.cc sockio.cc acbuf.cc acfg.cc acfg_defaults.cc aclogger.cc caddrinfo.cc dirwalk.cc dlcon.cc fileio.cc
    fileitem.cc filereader.cc header.cc meta.cc ptitem.cc evabase.cc httpdate.cc dlratelimiter.cc
    csmapping.cc acerrno.cc aconnect.cc ac3rdparty.cc remotedb.cc tpool.cc ahttpurl.cc aobservable.cc
    atransport.cc acutilev.cc acutilpath.cc rex.cc acworm.cc
    aclocal.cc acres.cc dumper.cc
    aclock.cc acpatcher.cc

#evil trade-off: keep them in the lib or move them to server and have to expose all the meta functionality they require?
     ${MAINTSRCS}

    ${SERVER_SPECIFIC_SRCS}
    ${ALL_HEADERS})

option(SUP_STATIC "Build and link core functionality as static code" off)

if(SUP_STATIC)
        message("-- Configuring shared code as STATIC lib")
        ADD_LIBRARY(supacng STATIC ${SHAREDSRCS})
else()
        message("-- Configuring shared code as DYNAMIC lib")
        ADD_LIBRARY(supacng SHARED ${SHAREDSRCS})
        target_compile_definitions(supacng PUBLIC -DACNG_CORE_IN_SO)
endif()

# XXX: acng_server is only there because of sd_notify; maybe factor that out
TARGET_LINK_LIBRARIES(supacng PUBLIC acng_common acng_network acng_compression acng_crypto acng_server)

option(ENABLE_PCH "Enable use of precompiled headers for faster builds" off)
if(ENABLE_PCH)
	# this brings about 15% faster builds
	# OTOH it ruins ccache functionality with default setting - needs ccache config change, see ccache(1) about PRECOMPILED HEADERS
	if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.16")
		target_precompile_headers(supacng PUBLIC meta.h debug.h filereader.h acfg.h)
		if(CMAKE_COMPILER_IS_CCACHE OR CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
			message(WARNING "Precompiled headers are enabled and ccache is in use. Make sure to configure it, see ccache(1) about PRECOMPILED HEADERS, or set ENABLE_PCH to off.")
		endif()
	endif()
endif()

TARGET_LINK_LIBRARIES(supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
INSTALL(TARGETS supacng LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

ADD_EXECUTABLE(apt-cacher-ng apt-cacher.cc conserver.cc)
TARGET_LINK_LIBRARIES(apt-cacher-ng supacng)
#SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")
INSTALL(TARGETS apt-cacher-ng DESTINATION ${CMAKE_INSTALL_SBINDIR})

if (ENABLE_TOOL)
    ADD_EXECUTABLE(acngtool acngtool.cc ebrunner.cc)
    TARGET_LINK_LIBRARIES(acngtool supacng)
    INSTALL(TARGETS acngtool DESTINATION ${LIBDIR})
endif()

# build a special version for test with full symbol visibility
if(ENABLE_TESTS)
	add_library(supacngTEST STATIC ${SHAREDSRCS})
  target_link_libraries(supacngTEST PUBLIC acng_common acng_network acng_compression acng_crypto acng_server)
  target_compile_options(supacngTEST PUBLIC -fvisibility=default)
  target_compile_definitions(supacngTEST PUBLIC -DUNDER_TEST)
endif()

