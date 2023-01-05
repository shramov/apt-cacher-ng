cmake_minimum_required(VERSION 3.1)

if(HAVE_SD_NOTIFY)
	set(SDTYPE notify)
else()
	set(SDTYPE simple)
endif()
configure_file(apt-cacher-ng.service.in apt-cacher-ng.service)
configure_file(apt-cacher-ng.conf.in apt-cacher-ng.conf)

# this is experimental and is supposed to match LSB
if(SDINSTALL)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/apt-cacher-ng.conf DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/tmpfiles.d/)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/apt-cacher-ng.service DESTINATION /lib/systemd/system)
endif()
