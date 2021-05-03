
############################################
## Attempt to build acngfs where possible ##
############################################

pkg_check_modules(fuse "fuse>=2.9.4")

_append(acngfs_cflags ${fuse_CFLAGS} ${CFLAGS_PTHREAD})

if(fuse_FOUND)
        # not linking acngstuff lib, too many dependencies, so building a custom variant with MINIBUILD flag
        list(APPEND fsSRCS httpfs.cc)

        ADD_EXECUTABLE(acngfs ${fsSRCS})
        SET_TARGET_PROPERTIES(acngfs PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${acngfs_cflags}")
        INSTALL(TARGETS acngfs DESTINATION ${LIBDIR})
        TARGET_LINK_LIBRARIES(acngfs supacng ${fuse_LDFLAGS} ${BaseNetworkLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGFS} ${CMAKE_THREAD_LIBS_INIT})
endif()


