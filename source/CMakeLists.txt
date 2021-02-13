set(SHAREDSRCS sockio.cc acbuf.cc acfg.cc acfg_defaults.cc aclogger.cc caddrinfo.cc dirwalk.cc dlcon.cc fileio.cc fileitem.cc filereader.cc header.cc meta.cc tcpconnect.cc cleaner.cc job.cc bgtask.cc cacheman.cc conn.cc conserver.cc expiration.cc lockable.cc maintenance.cc mirror.cc pkgimport.cc showinfo.cc evabase.cc dnsiter.cc ebrunner.cc)
set(ACNG_SRCS apt-cacher.cc)

if(SUP_STATIC)
ADD_LIBRARY(supacng STATIC ${SHAREDSRCS})
else()
ADD_LIBRARY(supacng SHARED ${SHAREDSRCS})
add_definitions(-DACNG_CORE_IN_SO)
endif()

SET_TARGET_PROPERTIES(supacng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")

# this brings about 15% faster builds
# OTOH it ruins ccache functionality so keep it off until ccache is improved, or this function
#if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.16")
#        target_precompile_headers(supacng PUBLIC ../include/meta.h)
#endif()

TARGET_LINK_LIBRARIES(supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
INSTALL(TARGETS supacng LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

ADD_EXECUTABLE(apt-cacher-ng ${ACNG_SRCS} apt-cacher.cc)
TARGET_LINK_LIBRARIES(apt-cacher-ng supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")
INSTALL(TARGETS apt-cacher-ng DESTINATION ${CMAKE_INSTALL_SBINDIR})

ADD_EXECUTABLE(acngtool acngtool.cc)
SET_TARGET_PROPERTIES(acngtool PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_PTHREAD}")
TARGET_LINK_LIBRARIES(acngtool supacng ${BaseNetworkLibs} ${CompLibs} ${SSL_LIB_LIST} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNGTOOL})
INSTALL(TARGETS acngtool DESTINATION ${LIBDIR})

# build a special version for test with full symbol visibility
if(ENABLE_TESTS)
	ADD_LIBRARY(supacngTEST STATIC ${SHAREDSRCS})
  SET_TARGET_PROPERTIES(supacng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD} -fvisibility=default -DUNDER_TEST")
endif()

