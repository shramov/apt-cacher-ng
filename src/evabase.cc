#include "evabase.h"
#include "meta.h"
#include "debug.h"
#include "acfg.h"
#include "fileio.h"

#include <mutex>
#include <condition_variable>
#include <future>

#include <event2/util.h>

#ifdef HAVE_SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ares.h>

using namespace std;

#define DNS_ABORT_RETURNING_ERROR 1

//XXX: add an extra task once per hour or so, optimizing all caches

namespace acng
{

event_base* evabase::base = nullptr;

using tResolvConfStamp = Cstat::tID;
tResolvConfStamp cachedDnsFingerprint { { 0, 1 }, 0, 0 };

struct event *handover_wakeup;
const struct timeval timeout_asap{0,0};
#warning add unlocked queues which are only used when installing thread is the main thread
deque<tAction> temp_simple_q, local_simple_q;
void RejectPendingDnsRequests();
std::atomic_bool g_shutdownHint = false;

CDnsBase::~CDnsBase()
{
	shutdown();
}

void cb_sync_ares(evutil_socket_t, short, void* arg)
{
	// who knows what it has done with its FDs, simply recreating them all to be safe
	auto p=(CDnsBase*) arg;
	p->dropEvents();
	p->setupEvents();
}

void cb_ares_action(evutil_socket_t fd, short what, void* arg)
{
	auto p=(CDnsBase*) arg;
	if (what&EV_TIMEOUT)
		ares_process_fd(p->get(), ARES_SOCKET_BAD, ARES_SOCKET_BAD);
	else
	{
		auto toread = (what&EV_READ) ? fd : ARES_SOCKET_BAD;
		auto towrite = (what&EV_WRITE) ? fd : ARES_SOCKET_BAD;
		ares_process_fd(p->get(), toread, towrite);
	}
	// need to run another cycle asap
	p->sync();
}

void CDnsBase::sync()
{
	if (!m_aresSyncEvent)
		m_aresSyncEvent = evtimer_new(evabase::base, cb_sync_ares, this);
	event_add(m_aresSyncEvent, &timeout_asap);
}

void CDnsBase::dropEvents()
{
	for (auto& el: m_aresEvents)
	{
		if (el)
			event_free(el);
	}
	m_aresEvents.clear();
}

void CDnsBase::setupEvents()
{
	ASSERT(m_channel);
	if (!m_channel)
		return;
	ares_socket_t socks[ARES_GETSOCK_MAXNUM];
	auto bitfield = ares_getsock(m_channel, socks, _countof(socks));
	struct timeval tvbuf;
	auto tmout = ares_timeout(m_channel, nullptr, &tvbuf);
	for(unsigned i = 0; i < ARES_GETSOCK_MAXNUM; ++i)
	{
		short what(0);
		if (ARES_GETSOCK_READABLE(bitfield, i))
			what = EV_READ;
		else if (ARES_GETSOCK_WRITABLE(bitfield, i))
			what = EV_WRITE;
		else
			continue;
		m_aresEvents.emplace_back(event_new(evabase::base, socks[i], what, cb_ares_action, this));
		event_add(m_aresEvents.back(), tmout);
	}
}

void CDnsBase::shutdown()
{
	if (m_channel)
	{
		// forceful DNS resolver shutdown
		ares_destroy(m_channel);
	}
	dropEvents();
	if (m_aresSyncEvent)
		event_free(m_aresSyncEvent), m_aresSyncEvent = nullptr;

	m_channel = nullptr;
}

CDnsBase* evabase::GetDnsBase()
{
	InitDnsOrCheckCfgChange();
	return m_cachedDnsBase;
}

void evabase::InitDnsOrCheckCfgChange()
{
	Cstat info(cfg::dnsresconf);
	if (!info) // file is missing anyway?
		return;
	// still the same?
	auto fpr(info.fpr());
	if (m_cachedDnsBase && cachedDnsFingerprint == fpr)
		return;

	ares_channel newDnsBase;
	switch(ares_init(&newDnsBase))
	{
	case ARES_SUCCESS:
		break;
	case ARES_EFILE:
		log::err("DNS system error, cannot read config file"sv);
		return;
	case ARES_ENOMEM:
		log::err("DNS system error, out of memory"sv);
		return;
	case ARES_ENOTINITIALIZED:
		log::err("DNS system error, faulty initialization sequence"sv);
		return;
	default:
		log::err("DNS system error, internal error"sv);
		return;
	}
	// ok, found new configuration and it can be applied
	if (m_cachedDnsBase)
		delete m_cachedDnsBase;
	m_cachedDnsBase = new CDnsBase(newDnsBase);
	cachedDnsFingerprint = fpr;
}

evabase* g_eventBase;

evabase &evabase::GetGlobal()
{
	return *g_eventBase;
}

std::thread::id g_main_thread;

thread::id evabase::GetMainThreadId()
{
	return g_main_thread;
}

ACNG_API int evabase::MainLoop()
{
	LOGSTARTFUNCs;

	InitDnsOrCheckCfgChange(); // init DNS base

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=1");
#endif

	int r = event_base_loop(evabase::base, EVLOOP_NO_EXIT_ON_EMPTY);

	notify();

	// make sure that there are no actions from abandoned DNS bases blocking the futures
	RejectPendingDnsRequests();
	PushLoop();

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=0");
#endif

	// this might cause trouble with sloppy written tests
#ifndef UNDER_TEST
	// nothing but our owner should reference it now
//	ASSERT(__ref_cnt() == 1);
#endif

	return r;
}

void evabase::SignalStop()
{
	g_shutdownHint = true;

	Post([]()
	{
		if(evabase::base)
			event_base_loopbreak(evabase::base);
	});
}

void cb_handover(evutil_socket_t, short, void*)
{
	temp_simple_q.swap(local_simple_q);

	if (temp_simple_q.empty())
		return;

	for (const auto& ac: temp_simple_q)
			ac();
	temp_simple_q.clear();
}

void evabase::Post(tAction&& act)
{
	if (!act)
		return;

	ASSERT(IsMainThread());

	local_simple_q.emplace_back(move(act));
	ASSERT(handover_wakeup);
	event_add(handover_wakeup, &timeout_asap);
}


evabase::evabase()
{
	g_main_thread = std::this_thread::get_id();
	evabase::base = event_base_new();
	handover_wakeup = evtimer_new(base, cb_handover, nullptr);
	g_eventBase = this;
}

evabase::~evabase()
{
	delete m_cachedDnsBase;
	m_cachedDnsBase = nullptr;

	if(evabase::base)
	{
		event_base_free(evabase::base);
		evabase::base = nullptr;
	}
}

void evabase::PushLoop()
{
	// push the loop a few times to make sure that the state change
	// is propagated to the background threads
	for (int i = 10; i >= 0; --i)
	{
		// if error or nothing more to do...
		if (0 != event_base_loop(base, EVLOOP_NONBLOCK))
			break;
	}
}

uintptr_t evabase::SyncRunOnMainThread(std::function<uintptr_t ()> act, uintptr_t onRejection)
{
	if (!act)
		return 0;

	if(std::this_thread::get_id() == GetMainThreadId())
		return act();
	acpromise<uintptr_t> pro;
	auto fut = pro.get_future();

#if 1

	try
	{
		Post([&]() -> void
		{
			try
			{
				pro.set_value(act());
			}
			catch (...)
			{
				pro.set_exception(std::current_exception());
			}
		});
	}
	catch (...)
	{
		return onRejection;
	}
	return fut.get();
#endif
#if 0
	void *a = &pro, *b = &act;
	try
	{
		tAction todo = [/*pa = &act, p = &pro*/ a, b]() mutable
		{
				auto p = ((decltype(pro)*)a);
				auto pa = ((decltype(act)*)b);
			try
			{
				p->set_value((*pa)());

			}
			catch (...)
			{
				p->set_exception(std::current_exception());
			}
		};
		Post(move(todo));
	}
	catch (...)
	{
		return onRejection;
	}
	return fut.get();
#endif
#if 0
	bool done = false;
	exception_ptr bad;
	std::mutex mx;
	std::condition_variable cond;
	uintptr_t res;
	try
	{
		tAction todo = [&]() mutable
		{
			try
			{
				auto x = act();
				lguard g(mx);
				res = x;
				done = true;
				cond.notify_all();
			}
			catch (...)
			{
				lguard g(mx);
				bad = std::current_exception();
				done = true;
				cond.notify_all();
			}
		};
		Post(move(todo));
	}
	catch (...)
	{
		return onRejection;
	}
	ulock g(mx);
	while(!done)
	{
		cond.wait(g);
	}
	if (bad)
		std::rethrow_exception(bad);
	return res;
#endif
}


}
