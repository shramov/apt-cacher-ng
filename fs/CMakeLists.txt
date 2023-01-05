
############################################
## Attempt to build acngfs where possible ##
############################################

pkg_check_modules(fuse fuse IMPORTED_TARGET)

#_append(acngfs_cflags ${fuse_CFLAGS} ${CFLAGS_PTHREAD})

# double-check and make sure it compiles
FILE(READ ${PROJECT_SOURCE_DIR}/${TESTKITDIR}/HAVE_FUSE_25.cc TESTSRC)
SET(CMAKE_REQUIRED_FLAGS "${ACNG_COMPFLAGS} ${acngfs_cflags}")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_FUSE_25)
#FILE(READ ${TESTKITDIR}/HAVE_DLOPEN.cc TESTSRC)
#CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_DLOPEN)


if(TARGET PkgConfig::fuse AND HAVE_FUSE_25)
        add_executable(acngfs httpfs.cc)
        #SET_TARGET_PROPERTIES(acngfs PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${acngfs_cflags}")
        install(TARGETS acngfs DESTINATION ${LIBDIR})
        target_link_libraries(acngfs acng_common acng_network PkgConfig::fuse acng_crypto) # supacng ${fuse_LDFLAGS} ${BaseNetworkLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGFS} ${CMAKE_THREAD_LIBS_INIT})

        #if(HAVE_DLOPEN)
        #        _append(EXTRA_LIBS_ACNGFS dl)
        #endif()
else()
        message("- FUSE not found or not compatible, not building acngfs")
endif()


