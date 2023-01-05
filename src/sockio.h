#ifndef SOCKIO_H_
#define SOCKIO_H_

#include "actypes.h"
#include "fileio.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <unistd.h>
#include <cstddef>

#include <event.h>

#ifndef AI_NUMERICSERV
#define AI_NUMERICSERV 0
#endif
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef MSG_MORE
#define MSG_MORE 0
#endif

#ifndef SO_MAXCONN
#define SO_MAXCONN 250
#endif

//! Time after which the pooled sockets are considered EOLed
#define TIME_SOCKET_EXPIRE_CLOSE 33

namespace acng
{

void termsocket_async(int, event_base*);

void termsocket_quick(int& fd);

bool check_read_state(int fd);

struct select_set_t
{
	int m_max = -1;
	fd_set fds;
	void add(int n) { if(m_max == -1 ) FD_ZERO(&fds); if(n>m_max) m_max = n; FD_SET(n, &fds); }
	bool is_set(int fd) { return FD_ISSET(fd, &fds); }
	int nfds() { return m_max + 1; }
	operator bool() const { return m_max != -1; }
};

// common flags for a CONNECTING socket
void set_connect_sock_flags(evutil_socket_t fd);
void set_serving_sock_flags(evutil_socket_t fd);

void FinishConnection(int fd);

void TuneSendWindow(bufferevent* bev);

}

#endif /*SOCKIO_H_*/
