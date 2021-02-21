#include "evabase.h"
#include "meta.h"
#include "debug.h"
#include "lockable.h"

#include <event2/dns.h>
#include <event2/util.h>
#include <event2/thread.h>

#ifdef HAVE_SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

#define DNS_ABORT_RETURNING_ERROR 1

//XXX: add an extra task once per hour or so, optimizing all caches

namespace acng
{

event_base* evabase::base = nullptr;
evdns_base* evabase::dnsbase = nullptr;
std::atomic<bool> evabase::in_shutdown = ATOMIC_VAR_INIT(false);

struct event *handover_wakeup;
const struct timeval timeout_asap{0,0};
deque<evabase::tCancelableAction> incoming_q, processing_q;
mutex handover_mx;

namespace conserver
{
// forward declarations for the pointer checks
//void cb_resume(evutil_socket_t fd, short what, void* arg);
void do_accept(evutil_socket_t server_fd, short what, void* arg);
}

struct t_event_desctor {
	evutil_socket_t fd;
	event_callback_fn callback;
	void *arg;
};

/**
 * Forcibly run each callback and signal shutdown.
 */
int teardown_event_activity(const event_base*, const event* ev, void* ret)
{
	t_event_desctor r;
	event_base *nix;
	short what;
	auto lret((deque<t_event_desctor>*)ret);
	event_get_assignment(ev, &nix, &r.fd, &what, &r.callback, &r.arg);
#ifdef DEBUG
	if(r.callback == conserver::do_accept)
		cout << "stop accept: " << r.arg << endl;
#endif
	if(r.callback == conserver::do_accept)
		lret->emplace_back(move(r));
	return 0;
}

ACNG_API int evabase::MainLoop()
{
	LOGSTARTFUNCs;

	dnsbase = evdns_base_new(base, cfg::dnsopts ? 0 :
			EVDNS_BASE_INITIALIZE_NAMESERVERS);

	if (!dnsbase && ! cfg::dnsopts)
	{
		// this might be a libevent bug. If it does not find nameservers in
		// resolv.conf, it starts doing strange things all over the place.
		// And even if custom nameserver is added afterwards manually,
		// and it's seems to communicate with it fine,
		// libevent still throws EAI_FAIL to user code for no f* reason.
		//
		// also, the fallback to localhost DNS is apparently sometimes
		// added to the server list but not always, needs further
		// investigation.

		// Initialise w/o servers and add the fallback manually
		dnsbase = evdns_base_new(base, 0);
		if (dnsbase)
		{
			auto err = evdns_base_resolv_conf_parse(dnsbase, DNS_OPTIONS_ALL,
							cfg::dnsresconf.c_str());
			(void) err;
			// in might be in error state now but it seems to be at least
			// operational enough to get an additional NS added

			auto backup = "127.0.0.1";
			// backup = "8.8.8.8";
			struct sockaddr_in localdns;
			localdns.sin_addr.s_addr = inet_addr(backup);
			localdns.sin_family = PF_INET;
			localdns.sin_port = htons(53);

			if (0 != evdns_base_nameserver_sockaddr_add(dnsbase,
							(sockaddr*) &localdns, sizeof(localdns), 0))
			{
				log::err("ERROR: cannot add fallback DNS server!");
			}
		}
	}

	if (!dnsbase)
	{
		log::err("ERROR: Failed to setup default DNS service!");
	}
	else if (cfg::dnsopts)
	{
		// in any case set a sensible timeout!
		// XXX: this is not effective without having a nameserver
		// evdns_base_set_option(evabase::dnsbase, "timeout", "8");

		// XXX: might also make that path configurable, and also allow to pass custom hosts file
		auto err = evdns_base_resolv_conf_parse(dnsbase, cfg::dnsopts,
				cfg::dnsresconf.c_str());
		if (err)
		{
			log::err(mstring("ERROR: Failed to initialize custom DNS! ") +
					evdns_err_to_string(err));
			// this is not totally broken, can still act as local web server
			// cfg::DegradedMode(true);
		}
	}

#if 0
	// XXX: early attempts to work around the evdns bug w/o name servers
	auto nResolvers = evdns_base_count_nameservers(dnsbase);
	if (nResolvers < 2)
	{
		/*
		struct evutil_addrinfo hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_flags = EVUTIL_AI_NUMERICHOST | EVUTIL_AI_NUMERICSERV;
		hints.ai_protocol = PF_INET;
		hints.ai_socktype = SOCK_STREAM;
		static const evutil_addrinfo hints =
		{
			// we provide plain port numbers, no resolution needed
			// also return only probably working addresses
			AI_NUMERICSERV | AI_ADDRCONFIG,
			PF_UNSPEC,
			SOCK_STREAM, IPPROTO_TCP,
			0, nullptr, nullptr, nullptr
		};

	    struct evutil_addrinfo *answer = nullptr;
	    auto res = evutil_getaddrinfo(backup, "53", &hints, &answer);
		if (0 == res)
		{
			evdns_base_nameserver_sockaddr_add(dnsbase, answer->ai_addr, answer->ai_addrlen, 0);
		}

		if (answer)
			evutil_freeaddrinfo(answer);
			*/


	}
	nResolvers = evdns_base_count_nameservers(dnsbase);
#endif

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=1");
#endif

	int r = event_base_loop(evabase::base, EVLOOP_NO_EXIT_ON_EMPTY);

	in_shutdown = true;
	if (dnsbase)
	{
		// graceful DNS resolver shutdown
		evdns_base_free(dnsbase, DNS_ABORT_RETURNING_ERROR);
		dnsbase = nullptr;
	}

	// push the loop a few times to make sure that the state change
	// is propagated to the background threads
	for (int i = 5; i >= 0; --i)
	{
		// if error or nothing more to do...
		if (0 != event_base_loop(base, EVLOOP_NONBLOCK))
			break;
	}

	// send teardown hint to all event callbacks
	deque<t_event_desctor> todo;
	event_base_foreach_event(evabase::base, teardown_event_activity, &todo);
	for (const auto &ptr : todo)
	{
		DBGQLOG("Notifying event on " << ptr.fd);
		ptr.callback(ptr.fd, EV_TIMEOUT, ptr.arg);
	}
	event_base_loop(base, EVLOOP_NONBLOCK);

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=0");
#endif
	return r;
}

void evabase::SignalStop()
{
	if(evabase::base)
		event_base_loopbreak(evabase::base);
}

void cb_handover(evutil_socket_t sock, short what, void* arg)
{
	{
		lockguard g(handover_mx);
		processing_q.swap(incoming_q);
	}
	for(const auto& ac: processing_q)
		ac(evabase::in_shutdown);
	processing_q.clear();
}

void evabase::Post(tCancelableAction&& act)
{
	{
		lockguard g(handover_mx);
		incoming_q.emplace_back(move(act));
	}
	ASSERT(handover_wakeup);
	event_add(handover_wakeup, &timeout_asap);
}

evabase::evabase()
{
	evthread_use_pthreads();
	evabase::base = event_base_new();
	handover_wakeup = evtimer_new(base, cb_handover, nullptr);
}

evabase::~evabase()
{
	if(evabase::base)
	{
		event_base_free(evabase::base);
		evabase::base = nullptr;
	}
}


}
