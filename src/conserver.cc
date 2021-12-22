#include <memory>
#include "conserver.h"
#include "meta.h"
#include "acfg.h"
#include "caddrinfo.h"
#include "ahttpurl.h"
#include "sockio.h"
#include "fileio.h"
#include "evabase.h"
#include "acregistry.h"
#include "portutils.h"
#include "aclock.h"

#include <signal.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>

#include <cstdio>
#include <map>
#include <unordered_set>
#include <iostream>
#include <algorithm>    // std::min_element, std::max_element
#include <thread>

#ifdef HAVE_LIBWRAP
#include <tcpd.h>
#endif

#include "debug.h"

using namespace std;

// for cygwin, incomplete ipv6 support
#ifndef AF_INET6
#define AF_INET6        23              /* IP version 6 */
#endif

namespace acng
{

int yes(1);

const struct timeval g_resumeTimeout { 2, 11 };

// from conn.cc
void StartServing(unique_fd&& fd, string clientName, acres& res);

class conserverImpl : public conserver
{
	std::list<unique_fdevent> m_listeners;
	acres& m_res;
	aobservable::subscription m_shutdownSub, m_resumeSub;

	//	auto nSockets = conserver::Setup([](unique_fd&& fd, std::string name) { StartServing(move(fd), name, *sharedResources); });

	/**
* Perform a gracefull connection shutdown.
*/
	ACNG_API void static FinishConnection(int fd);

public:

	void SetupConAndGo(unique_fd&& man_fd, string clientName, LPCSTR clientPort)
	{
		USRDBG("Client name: " << clientName << ":" << clientPort);
		try
		{
			StartServing(move(man_fd), move(clientName), m_res);
		}
		catch (const std::bad_alloc&)
		{
			// ignored
		}
	}

	static void do_accept(evutil_socket_t server_fd, short, void* arg)
	{
		LOGSTARTFUNCxs(server_fd);

		// lazy trigger of DNS configuration change check
		evabase::GetGlobal().GetDnsBase();
		auto& srv(*(conserverImpl*)arg);

		struct sockaddr_storage addr;
		socklen_t addrlen = sizeof(addr);

		// pick all incoming connections in one run
		while(true)
		{
			auto fd = accept(server_fd, (struct sockaddr*) &addr, &addrlen);

			if (fd == -1)
			{
				switch (errno)
				{
				case EMFILE:
				case ENFILE:
				case ENOBUFS:
				case ENOMEM:
					// OOM condition? Suspend all accepting for a while and hope that it will recover then
					for(auto& el: srv.m_listeners)
						event_del(el.m_p);
					srv.m_resumeSub = srv.m_res.GetIdleCheckBeat().AddListener([&]()
					{
						bool failed = false;
						for(auto& el: srv.m_listeners)
							failed |= event_add(el.m_p, nullptr);
						ASSERT(!failed);
						if (!failed)
							srv.m_resumeSub.reset();
					});
					return;
				case EAGAIN:
				case EINTR:
				default:
					// listener will notify
					return;
				}
			}
			unique_fd man_fd(fd);

			evutil_make_socket_nonblocking(fd);
			if (addr.ss_family == AF_UNIX)
			{
				USRDBG("Detected incoming connection from the UNIX socket");
				((conserverImpl*) arg)->SetupConAndGo(move(man_fd), "<UNIX>", "unix");
			}
			else
			{
				USRDBG("Detected incoming connection from the TCP socket");
				char hbuf[NI_MAXHOST];
				char pbuf[11];
				if (getnameinfo((struct sockaddr*) &addr, addrlen, hbuf, sizeof(hbuf),
								pbuf, sizeof(pbuf), NI_NUMERICHOST|NI_NUMERICSERV))
				{
					USRERR("ERROR: could not resolve hostname for incoming TCP host");
					return;
				}

				if (cfg::usewrap)
				{
#ifdef HAVE_LIBWRAP
					// libwrap is non-reentrant stuff, call it from here only
					request_info req;
					request_init(&req, RQ_DAEMON, "apt-cacher-ng", RQ_FILE, fd, 0);
					fromhost(&req);
					if (!hosts_access(&req))
					{
						log::err(string(hbuf) + "|ERROR: access not permitted by hosts files");
						return;
					}
#else
					log::err(
								"WARNING: attempted to use libwrap which was not enabled at build time");
#endif
				}
				((conserverImpl*) arg)->SetupConAndGo(move(man_fd), hbuf, pbuf);
			}
		}
	}

	void bind_and_listen(evutil_socket_t mSock, const addrinfo *pAddrInfo, uint16_t port)
	{
		LOGSTARTFUNCs;
		USRDBG("Binding " << acng_addrinfo::formatIpPort(pAddrInfo->ai_addr, pAddrInfo->ai_addrlen, pAddrInfo->ai_family));
		if ( ::bind(mSock, pAddrInfo->ai_addr, pAddrInfo->ai_addrlen))
		{
			log::flush();
			perror("Couldn't bind socket");
			cerr.flush();
			if(EADDRINUSE == errno)
			{
				if(pAddrInfo->ai_family == PF_UNIX)
					cerr << "Error creating or binding the UNIX domain socket - please check permissions!" <<endl;
				else
					cerr << "Port " << port << " is busy, see the manual (Troubleshooting chapter) for details." <<endl;
				cerr.flush();
			}
			close(mSock);
			return;
		}
		if (listen(mSock, SO_MAXCONN))
		{
			perror("Couldn't listen on socket");
			close(mSock);
			return;
		}

		set_nb(mSock);
		auto ev = event_new(evabase::base, mSock, EV_READ|EV_PERSIST, do_accept, this);
		if(!ev)
		{
			cerr << "Socket creation error" << endl;
			return;
		}
		event_add(ev, nullptr);
		m_listeners.emplace_back(ev);
		return;
	};

	std::string scratchBuf;

	void setup_tcp_listeners(LPCSTR addi, uint16_t port)
	{
		LOGSTARTFUNCxs(addi, port);
		USRDBG("Binding on host: " << addi << ", port: " << port);

		auto hints = addrinfo();
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		hints.ai_family = PF_UNSPEC;

		addrinfo* dnsret;
		int r = getaddrinfo(addi, tPortFmter().fmt(port), &hints, &dnsret);
		if(r)
		{
			log::flush();
			perror("Error resolving address for binding");
			return;
		}
		TFinalAction dnsclean([dnsret]()
		{
			if(dnsret)
				freeaddrinfo(dnsret);
		});
		std::unordered_set<std::string> dedup;
		for(auto p = dnsret; p; p = p->ai_next)
		{
			// no fit or or seen before?
			if(!dedup.emplace((const char*) p->ai_addr, p->ai_addrlen).second)
				continue;
			int nSockFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
			if (nSockFd == -1)
			{
				// STFU on lack of IPv6?
				switch(errno)
				{
				case EAFNOSUPPORT:
				case EPFNOSUPPORT:
				case ESOCKTNOSUPPORT:
				case EPROTONOSUPPORT:
					continue;
				default:
					perror("Error creating socket");
					continue;
				}
			}
			// if we have a dual-stack IP implementation (like on Linux) then
			// explicitly disable the shadow v4 listener. Otherwise it might be
			// bound or maybe not, and then just sometimes because of configurable
			// dual-behavior, or maybe because of real errors;
			// we just cannot know for sure but we need to.
#if defined(IPV6_V6ONLY) && defined(SOL_IPV6)
			if(p->ai_family==AF_INET6)
				setsockopt(nSockFd, SOL_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
#endif
			setsockopt(nSockFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
			bind_and_listen(nSockFd, p, port);
		}
	};

	bool Setup() override
	{
		LOGSTARTFUNCs;

		if (cfg::udspath.empty() && (!cfg::port && cfg::bindaddr.empty()))
		{
			cerr << "Neither TCP nor UNIX interface configured, cannot proceed.\n";
			exit(EXIT_FAILURE);
		}

		if (cfg::udspath.empty())
			cerr << "Not creating Unix Domain Socket, fifo_path not specified";
		else
		{
			string & sPath = cfg::udspath;
			auto addr_unx = sockaddr_un();

			size_t size = sPath.length() + 1 + offsetof(struct sockaddr_un, sun_path);

			auto die = []()
			{
				cerr << "Error creating Unix Domain Socket, ";
				cerr.flush();
				perror(cfg::udspath.c_str());
				cerr << "Check socket file and directory permissions" <<endl;
				exit(EXIT_FAILURE);
			};

			if (sPath.length() > sizeof(addr_unx.sun_path))
			{
				errno = ENAMETOOLONG;
				die();
			}

			addr_unx.sun_family = AF_UNIX;
			strncpy(addr_unx.sun_path, sPath.c_str(), sPath.length());

			mkbasedir(sPath);
			unlink(sPath.c_str());

			auto sockFd = socket(PF_UNIX, SOCK_STREAM, 0);
			if(sockFd < 0) die();

			addrinfo ai;
			ai.ai_addr =(struct sockaddr *) &addr_unx;
			ai.ai_addrlen = size;
			ai.ai_family = PF_UNIX;

			bind_and_listen(sockFd, &ai, 0);
		}

		bool custom_listen_ip = false;
		tHttpUrl url;
		for(const auto& sp: tSplitWalk(cfg::bindaddr))
		{
			mstring token(sp);
			auto isUrl = url.SetHttpUrl(token, false);
			if(!isUrl && !cfg::port)
			{
				USRDBG("Not creating TCP listening socket for " <<  sp
					   << ", no custom nor default port specified!");
				continue;
			}
			//	XXX: uri parser accepts anything wihtout shema, good for this situation but maybe bad for strict validation...
			//			USRDBG("Binding as host:port URI? " << isUrl << ", addr: " << url.ToURI(false));
			setup_tcp_listeners(isUrl ? url.sHost.c_str() : token.c_str(),
								isUrl ? url.GetPort(cfg::port) : cfg::port);
			custom_listen_ip = true;
		}
		// just TCP_ANY if none was specified
		if(!custom_listen_ip)
			setup_tcp_listeners(nullptr, cfg::port);

		if (m_listeners.empty())
			return false;

		m_shutdownSub = evabase::GetGlobal().subscribe([&]()
		{
			m_listeners.clear();
			m_resumeSub.reset();
			m_shutdownSub.reset();
		});
		return true;
	}

	conserverImpl(acres& res) : m_res(res) {}
	~conserverImpl()
	{
		// actually it's all self-releasing
	}
};

conserver *conserver::Create(acres &res)
{
	return new conserverImpl(res);
}

}
