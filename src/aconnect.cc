#include "aconnect.h"
#include "meta.h"
#include "acfg.h"
#include "sockio.h"
#include "evabase.h"
#include "debug.h"
#include "portutils.h"
#include "aevutil.h"
#include "acsmartptr.h"
#include "caddrinfo.h"
#include "atransport.h"

#include <future>
#include <list>

#include <sys/types.h>
#include <sys/socket.h>

#include <event.h>

using namespace std;

#define CONNECT_SYSCALL_RETRY_LIMIT 50

namespace acng
{
string dnsError("Unknown DNS error");

struct ConnProbingContext : public tLintRefcounted
{
	time_t m_deadline;
	mstring target;
	uint16_t port;
	aconnector::tCallback m_cbReport;
	std::deque<acng_addrinfo> m_targets;
	// linear search is sufficient for this amount of elements
	std::list<unique_fdevent> m_eventFds;
	time_t m_timeNextCand;
	mstring m_error2report;
	decltype (m_targets)::iterator m_cursor;
	void processDnsResult(std::shared_ptr<CAddrInfo>);
	void step(int fd, short what);
	static void cbStep(int fd, short what, void* arg)
	{
		((ConnProbingContext*)arg)->step(fd, what);
	}
	void retError(mstring, tComError flags);
	void retSuccess(int fd);
	void disable(int fd, int ec);
	void stop()
	{
		m_cbReport = aconnector::tCallback();
		// this should stop all events
		m_eventFds.clear();
	}
	~ConnProbingContext()
	{
		DBGQLOG("FIXME: deleting from dtor: " << uintptr_t(this));
	}
};

TFinalAction aconnector::Connect(cmstring& target, uint16_t port, tCallback cbReport, int timeout)
{
	auto ctx = make_lptr<ConnProbingContext>();
	if (timeout < 0)
		ctx->m_deadline = GetTime() + cfg::GetNetworkTimeout()->tv_sec;
	else if (timeout == 0)
		ctx->m_deadline = MAX_VAL(time_t);
	else
		ctx->m_deadline = GetTime() + timeout;

	ctx->target = target;
	ctx->port = port;
	ctx->m_cbReport = move(cbReport);

	CAddrInfo::Resolve(ctx->target, ctx->port, [ctx](std::shared_ptr<CAddrInfo> res)
	{
		ctx->processDnsResult(move(res));
	});
	return TFinalAction([ctx]()
	{
		ctx->stop();
	});
}

aconnector::tConnResult aconnector::Connect(cmstring &target, uint16_t port, int timeout)
{
	std::promise<aconnector::tConnResult> reppro;
	evabase::Post([&]()
	{
		Connect(target, port, [&](aconnector::tConnResult res)
		{
			reppro.set_value(move(res));
		}, timeout);
	});
	return reppro.get_future().get();
}

void ConnProbingContext::processDnsResult(std::shared_ptr<CAddrInfo> res)
{
	LOGSTARTFUNCs;
	if (!m_cbReport)
		return; // this was cancelled by the caller already!
	if (!res)
		return retError(dnsError, TRANS_DNS_NOTFOUND);
	auto errDns = res->getError();
	if (!errDns.empty())
		return retError(errDns, TRANS_DNS_NOTFOUND);
	dbgline;
	m_targets = res->getTargets();
	if (m_targets.empty())
		return retError(dnsError, TRANS_DNS_NOTFOUND);
	step(-1, 0);
}

void ConnProbingContext::step(int fd, short what)
{
	LOGSTARTFUNCx(fd, what);
	if (!m_cbReport)
		return; // this was cancelled by the caller already!
	if (what & EV_WRITE)
	{
		// ok, some real socket became ready on connecting, let's doublecheck it
		int err;
		socklen_t optlen = sizeof(err);
		if(getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*) &err, &optlen) == 0)
		{
			if (!err)
				return retSuccess(fd);
			disable(fd, err);
		}
		else
		{
			disable(fd, errno);
		}
	}
	// okay, socket not usable, create next candicate connection?
	time_t now = GetTime();
	if (now > m_deadline)
		return retError("Connection timeout", TRANS_TIMEOUT);

	auto isFirst = fd == -1;

	if (isFirst)
	{
		m_timeNextCand = now + cfg::fasttimeout;
		m_cursor = m_targets.begin();
	}
	else if (now >= m_timeNextCand)
	{
		// okay, arming the next candidate
		m_timeNextCand = now + cfg::GetFirstConTimeout()->tv_sec;
		dbgline;
	}

	// open attempt for the selected next candidate, or any valid which comes after
	for (; m_cursor != m_targets.end(); m_cursor++)
	{
		// to move into m_eventFds on success
		unique_fd nextFd;
		unique_fdevent pe;

		nextFd.m_p = ::socket(m_cursor->ai_family, SOCK_STREAM, 0);
		if (!nextFd.valid())
		{
			if (m_error2report.empty())
				m_error2report = tErrnoFmter();
			continue;
		}

		set_connect_sock_flags(nextFd.get());

		unsigned i = CONNECT_SYSCALL_RETRY_LIMIT;
		do
		{
			auto res = connect(nextFd.get(), (sockaddr*) & m_cursor->ai_addr, m_cursor->ai_addrlen);
			// can we get the connection immediately? Unlikely, but who knows
			if (res == 0)
				return retSuccess(nextFd.release());
			if (errno != EINTR)
				break;
		} while(i--);

		if (errno != EINPROGRESS)
		{
			// probably the first error, remember the reason
			setIfNotEmpty(m_error2report, tErrnoFmter(errno));
			continue;
		}
		auto tmout = isFirst ? cfg::GetFirstConTimeout() : cfg::GetFurtherConTimeout();
		pe.m_p = event_new(evabase::base, nextFd.get(), EV_WRITE | EV_PERSIST, cbStep, this);
		if (AC_LIKELY(pe.valid() && 0 == event_add(pe.get(), tmout)))
		{
			m_eventFds.emplace_back(move(pe));
			nextFd.release(); // eventfd helper will care

			// advance to the next after timeout
			m_cursor++;
			dbgline;
			return;
		}
		setIfNotEmpty(m_error2report, "Out of memory"sv);
	}
	// not success if got here, any active connection pending? consider this a timeout?
	if (m_eventFds.empty())
		return retError(m_error2report.empty() ? tErrnoFmter(EAFNOSUPPORT) : m_error2report, TRANS_TIMEOUT);
//	LOG("pending connections: " << m_pending);
}

void ConnProbingContext::retError(mstring msg, tComError errHints)
{
	LOGSTARTFUNCx(msg);
	stop();
	if (m_cbReport)
		m_cbReport({unique_fd(), move(msg), errHints});
}

void ConnProbingContext::retSuccess(int fd)
{
	auto rep = move(m_cbReport);
	m_cbReport = decltype (m_cbReport)();

	// doing destructors work already since we need to visit nodes anyway to find and extract the winner
	for(auto it = m_eventFds.begin(); it != m_eventFds.end(); ++it)
	{
		auto elfd = event_get_fd(**it);
		if (fd == elfd)
		{
			// get a naked event pointer without guard influence and defuse the outside
			auto el = it->release();
			m_eventFds.erase(it);
			event_free(el);
			DBGQLOG("FIXME: stoping on success: " << uintptr_t(this));
			stop();
			if (rep)
				rep({unique_fd(fd), se, 0});
			return;
		}
	}
	ASSERT(!"Unreachable");
	return stop();
	if (rep)
		rep({unique_fd(), "Internal error", TRANS_INTERNAL_ERROR});
}

void ConnProbingContext::disable(int fd, int ec)
{
	LOGSTARTFUNCx(fd);
	ASSERT(fd != -1);
	auto it = find_if(m_eventFds.begin(), m_eventFds.end(), [fd](auto& el)
	{
		return event_get_fd(*el) == fd;
	});
	if (it == m_eventFds.end())
		return;
	// error from primary always wins, grab before closing it
	if (it == m_eventFds.begin())
		setIfNotEmpty(m_error2report, tErrnoFmter(ec));
	// stop observing and release resources
	m_eventFds.erase(it);
}
}
