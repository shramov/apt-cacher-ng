
include(GoogleTest)

add_subdirectory(/usr/src/googletest gtest)
add_definitions(-DUNDER_TEST)
include_directories(src ${CMAKE_SOURCE_DIR}/src)
#set(TEST_LIB_SET gtest_main gmock_main supacngTEST SQLiteCpp sqlite3 ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
set(TEST_LIB_SET gtest_main gmock_main supacngTEST ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})

#file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false src/*.h src/*.cc)
#add_executable(ut_acng_misc
#	src/main.cc
#	)
        #	src/ut_mandb.cc
        #target_link_libraries(ut_acng_misc ${TEST_LIB_SET})

#add_executable(ut_cfg_remap
#	src/ut_cfg_remap.cc
#	src/main.cc
#	)
#target_link_libraries(ut_cfg_remap ${TEST_LIB_SET})

#target_compile_definitions(ut_cfg_remap PUBLIC -DNOCOMP -DMINIBUILD)


add_executable(ut_algos	src/ut_algos.cc	src/main.cc	)
target_link_libraries(ut_algos ${TEST_LIB_SET})

add_executable(ut_cacheman
        src/ut_cacheman.cc
        src/main.cc
        ../src/cacheman.cc
        )

target_link_libraries(ut_cacheman ${TEST_LIB_SET})

add_executable(ut_http
	src/ut_http.cc
	src/main.cc
        src/ut_io.cpp
	)
target_link_libraries(ut_http ${TEST_LIB_SET})

add_executable(ut_dns
        src/ut_dns.cc
        src/main.cc
        )
target_link_libraries(ut_dns ${TEST_LIB_SET})

add_executable(ut_server
        src/ut_server.cc
        src/main.cc
        src/ut_server_misc.cc
        # ../src/conserver.cc
        )
target_link_libraries(ut_server ${TEST_LIB_SET})

add_executable(ut_dlcon
        src/ut_dlcon.cc
        src/main.cc
        #src/ut_server_misc.cc
        #../src/dlcon.cc
        )
target_link_libraries(ut_dlcon ${TEST_LIB_SET})

add_executable(it_job
        src/it_job.cc
        src/main.cc
        ../src/job.cc
        )
target_link_libraries(it_job ${TEST_LIB_SET})

add_executable(it_transport
        src/main.cc
        src/it_transport.cc
        ../src/conserver.cc
        src/ut_server_misc.cc
        )
target_link_libraries(it_transport ${TEST_LIB_SET})

# disable for now until tests become fully stable
#gtest_discover_tests(ut_http)
#gtest_discover_tests(ut_algos)
#gtest_discover_tests(ut_cacheman)
