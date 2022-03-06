# FIXMEs

INVESTIGATE! Inf.loop in automated expiratin run.

[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123) = 123
[pid 64946] newfstatat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", {st_mode=S_IFREG|0644, st_size=174, ...}, 0) = 0
[pid 64946] openat(AT_FDCWD, "/var/cache/apt-cacher-ng/debrep/dists/experimental/contrib/binary-i386/Packages.diff/T-2022-03-27-2003.09-F-2022-03-15-0203.29.gz.head", O_RDONLY) = 55
[pid 64946] read(55, "HTTP/1.1 200 OK\r\nX-Original-Sour"..., 174) = 174
[pid 64946] close(55)                   = 0
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 187) = 187
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 101) = 101
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 191) = 191
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|      -"..., 81) = 81
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 123) = 123
[pid 64946] writev(15, [{iov_base="Error downloading debrep/dists/e"..., iov_len=124}], 1) = 124
[pid 64946] write(15, "<br>\n", 5)      = 5
[pid 64946] write(15, "\n<br>\n", 6)    = 6
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|       "..., 282) = 282
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    << "..., 123) = 123
[pid 64946] write(9, "Wed Apr 13 22:33:12 2022|    >> "..., 123^C) = 123







Review maint stream parser in acngtool

Hanging in shutdown, at:  0x00007f0385fce2ec in acng::tpoolImpl::stop (this=0x87edd0)

## Debug logging

### Dbg logfile with limited size, written in ring-buffer fashion

Starting with jump hints in the beginning (line and byte position)

### Better state dumping also from maint jobs, including download activity

### Maybe use binary logging? journalctl?

## Data management

### Better reporting on configuration loading errors

### Review init sequence of **filePattern** regexps

Order or evaluation must be consistent with documentation and previous behavior.

## Maint stuff

### Restore some optional functionality

Flag name to search: DISABLED\_SUGAR

- PA trace: probably not needed, move to some script which analyzes logs

### Add reliable auto-bottom-scrolling to maint pages

### Merge delconfirm.html template into the regular maint.html template

Making the buttons visibility more conditional with dedicated properties.

### Change the exclusive page handling:
- opening while active -> new info page about activity, options: cancel-and-replace or attach
- cancel function -> abort the active job immediately, wait for it to finish, start a new job

### Fix slow download processing, seems to get stuck and continue on timeouts?

### Maybe add some sequencing of download jobs

### Add Control bubble in JS, content requirements:
- Authenticate link if not authenticated (and until authenticated, disable the action buttons on the control page!)
- "Cancel Activivity" if BG activity is detected (and report presense of activity, visualize with something simple like 3-dot-gauge)
- or "View Activity" button to attach
- if errors are detected: buttons to jump up/down between the errors, and jump to the control bar
- small icons to jump to the particular links of the control bar

### Cosmetics
- use dark page theme (or adaptive theme depending on Browser settings)

### Add a dedicated cleanup page

(or just a special mode of Expiration task which does not abort on errors and does not remove stuff, focusing on the distro expiration issue)

### Add a dedicated stats analysis page page

- move the advanced log analysis to a dedicated page, and use Chart.js or similar to render it nicely.

### Expire-Trade-Off
Change expire-trade-off setting and make it adaptive, learning the cost of metdata fetching from the last time?

## Transport

### Fix and reconsider proxy use

- Add dead proxy detection (disabling and restoration) handling
- AND: tdljob::getpeerhost not sufficient, needs a pre-calculation method which decides on proxy use, something like "tdljob::ReconsiderTargetHost". That would set a flag to remember that this item actually wanted to use the proxy, and this hint shall also be checked later when establishing connection, so if a new connection turned out to be a non-proxy access in the end -> reject with a hint to redispatch the job!
- AND: for that, add a method to aconnect which gives an early clue whether a proxy is considered for a given host!! (either because of non-proxy-fallback or because we might have item-specific proxies!)

Alternative strategy: move the dead-proxy-detection into a shared class (reuse the existing shaper agent!). If proxy loss was detected somewhere, send a signal which makes all jobs be re-evaluated. And add a smaller version of the validation check into the request sending part (lighter than SetupSource).

### Move dnsbase into custom source, access through resources!

### Add SSL protected access

### Implement https-Download over https-accessed proxy

Too many newlines in downloadIO output, also spam from retries, simplifies this whole mess and print result summary after the job was processed,
and apply whitespace:nowrap (HOWEVER: too many dots might overflow it all the time and garble the page -> more analysis required)

## Interaction stuff

Maybe... restore data collector, or maybe drop it and add a log scanner script instead?

## Tests

## Stress tests

- reduce number of connection per user and feed with URL mix

## To perform
- import of files
- delivery of imported files
- sanity check of the headers of imported files
- CONNECT through proxy handling
- accessing https through a https proxy (fully authorized, no authorization, denied access)
- Storm of requests on different targets, sample: tests/mixed.list
- UTs for local file delivery and various sizes (1gb, 2gb, 2.5gb, 5gb)

# MISC

## Complexity

Check ExpandFilePattern with all related costs (copying filenames to internal
allocated buffers, copying again out of them into strdeq, processing that
strdeq). Maybe cheaper to use DirWalk with fnmatch for most cases, using a
little convenience facade on top?

## Move functionality

Move guided precaching functionality to acngtool.

Drop "curl" functionality from acngtool. As transitional solution, call
curl/wget/http(from-apt) as client instead. Then drop DL code usage from UDS
communication.

acngtool curl mode shall print error messages (from error logging) to STDERR

Rework acfg namespace to use a builder object.

Redesign the config file format to add sections for remap-stuff instead of
pushing everything into the same config line.  Basic idea: git-config extended
ini format.

Add better collaboration with a frontend HTTP server like nginx. Maybe offer
special paramters for specific server ports, so X-Forwarded-For is only
accepted from those ports. Better configuration file would be needed here, as
mentioned above.

Better cleanup for orphaned .gpg files

IDEA: create a special control socket, ACLed by local permissions. Requests
coming from there would be considered as ultimately thrusted and go directly to
the maintenance handlers (i.e. alternative reportpage access only for local
administrator even when report page is disabled).

IDEA: a special control command for acng/acngtool to rebind local TCP interfaces.
For https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=786717

Raw pass-throough of Location: addresses as long as they follow the minimum
safety rules, for web services that apply strange rules WRT validation of such
redirections (https://bugs.launchpad.net/bugs/1625928)

Add custom job trigger with a command file plus fifo file (for output). This
might be triggered by SIGUSR2.

Fix URL parser to cope with more combinations of encoded chars (specially in user:pass part)

Make better secure mechanism to trigger maint jobs. The current one requires faked website authentication, etc.
Should use local methods instead, maybe passing a config file with commands via
filesystem which is only readable/writtable to apt-cacher-ng and push it via
signal?

Local storage of admin password shall use hashing, maybe PBKDF2 from OpenSSL

Direct NTLM proxy authentication (user can use cntlm but internal solution would be nice)

Investigate more on Fedora mirror management, improve mirror list generation

No cancel button in attached mode of maint pages

[for s.] root startup and EUID changing for privileged port use
[maybe, what was the urgent reason again...?!] Don't die if at leat one socket was open on start. Plus, retry to open sockets on HUP. document this... ifup can send hups?!
[debian] don't install acngfs manpage on hurd... or finally fix and build acngfs for hurd

> And after a code review yesterday I think the html log is still not a
> bad idea. It still needs some cosmetical fixes.
It has advantages.  Would be neat if it could be served by
http://<server ID>:3142/logs.html (or something like that).  It would
have to generate a pick-list of the
/var/log/apt-cacher-ng/maint_*.log.html files ...

 - consider creating Debian and Ubuntu security mirror setup, like:
 Remap-ubusec: file:ubuntu_security /ubuntu-security ; http://security.ubuntu.com/ubuntu

 - Document all options in the manpage (Prio: low)

 - (maybe) for import: smart mirror structure discovery... if _$ARCH.deb found but no
   binary-$ARCH data for them, try to locate binary-$ARCH folder positions in
   the cache

 - dynamic update/scrolling of the log pages

