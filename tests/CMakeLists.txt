
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


add_executable(ut_algos
	src/ut_algos.cc
	src/main.cc
	)
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


add_executable(it_job
        src/it_job.cc
        src/main.cc
        ../src/job.cc
        )
target_link_libraries(it_job ${TEST_LIB_SET})

gtest_discover_tests(ut_http)
gtest_discover_tests(ut_algos)
gtest_discover_tests(ut_cacheman)
