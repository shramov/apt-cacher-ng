add_executable(in.acng client.cc ../src/acerrno.cc)
target_link_libraries(in.acng acng_common acng_network)
install(TARGETS in.acng DESTINATION ${LIBDIR})
