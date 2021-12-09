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

#include <future>
#include <list>

#include <sys/types.h>
#include <sys/socket.h>

#include <event.h>

using namespace std;

namespace acng
{
string dnsError("Unknown DNS error");

struct tConnRqData : public tLintRefcounted
{
	time_t exTime;
	mstring target;
	uint16_t port;
	aconnector::tCallback m_cbReport;
	std::deque<acng_addrinfo> m_targets;
	// linear search is sufficient for this amount of elements
	struct tProbeInfo
	{
		unique_fd fd;
		unique_event ev;
	};
	std::list<tProbeInfo> m_eventFds;
	unsigned m_pending = 0;
	time_t m_timeNextCand;
	mstring m_error2report;
	decltype (m_targets)::iterator m_cursor;
	void processDnsResult(std::shared_ptr<CAddrInfo>);
	void step(int fd, short what);
	static void cbStep(int fd, short what, void* arg)
	{
		((tConnRqData*)arg)->step(fd, what);
	}
	void retError(mstring, bool fromDns);
	void retSuccess(int fd);
	void disable(int fd, int ec);
	void abort()
	{
		// stop all event interaction ASAP and (maybe) self-destruct
		m_cbReport = aconnector::tCallback();
		m_eventFds.clear();
	}
};

TFinalAction aconnector::Connect(cmstring& target, uint16_t port, tCallback cbReport, int timeout)
{
	auto ctx = make_lptr<tConnRqData>();
	if (timeout < 0)
		ctx->exTime = GetTime() + cfg::GetNetworkTimeout()->tv_sec;
	else if (timeout == 0)
		ctx->exTime = MAX_VAL(time_t);
	else
		ctx->exTime = GetTime() + timeout;

	ctx->target = target;
	ctx->port = port;
	ctx->m_cbReport = move(cbReport);

	CAddrInfo::Resolve(ctx->target, ctx->port, [ctx](std::shared_ptr<CAddrInfo> res)
	{
		ctx->processDnsResult(move(res));
	});
	return TFinalAction([ctx](){ ctx->abort(); });
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

void tConnRqData::processDnsResult(std::shared_ptr<CAddrInfo> res)
{
	LOGSTARTFUNCs;
	if (!m_cbReport)
		return; // this was cancelled by the caller already!
	if (!res)
		return retError(dnsError, true);
	auto errDns = res->getError();
	if (!errDns.empty())
		return retError(errDns, true);
	dbgline;
	m_targets = res->getTargets();
	if (m_targets.empty())
		return retError(dnsError, true);
	step(-1, 0);
}

void tConnRqData::step(int fd, short what)
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
	if (now > exTime)
		return retError("Connection timeout", false);

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
		unique_event pe;

		nextFd.m_p = ::socket(m_cursor->ai_family, SOCK_STREAM, 0);
		if (!nextFd.valid())
		{
			if (m_error2report.empty())
				m_error2report = tErrnoFmter();
			continue;
		}
		set_connect_sock_flags(nextFd.get());
		for (unsigned i=0; i < 50; ++i)
		{
			auto res = connect(nextFd.get(), (sockaddr*) & m_cursor->ai_addr, m_cursor->ai_addrlen);
			if (res == 0)
				return retSuccess(nextFd.release());
			if (errno != EINTR)
				break;
		}
		if (errno != EINPROGRESS)
		{
			setIfNotEmpty(m_error2report, tErrnoFmter(errno))
					continue;
		}
		auto tmout = isFirst ? cfg::GetFirstConTimeout() : cfg::GetFurtherConTimeout();
		pe.m_p = event_new(evabase::base, nextFd.get(), EV_WRITE | EV_PERSIST, cbStep, this);
		if (AC_LIKELY(pe.valid() && 0 == event_add(pe.get(), tmout)))
		{
			m_eventFds.push_back({move(nextFd), move(pe)});
			m_pending++;
			// advance to the next after timeout
			m_cursor++;
			dbgline;
			return;
		}
		setIfNotEmpty(m_error2report, "Out of memory"sv);
	}
	// not success if got here, any active connection pending?
	if (m_pending == 0)
		return retError(m_error2report.empty() ? tErrnoFmter(EAFNOSUPPORT) : m_error2report, false);
	LOG("pending connections: " << m_pending);
}

void tConnRqData::retError(mstring msg, bool isDnsError)
{
	LOGSTARTFUNCx(msg);
	if (m_cbReport)
		m_cbReport({unique_fd(), move(msg), isDnsError});
	return abort();
}

void tConnRqData::retSuccess(int fd)
{
	// doing destructors work already since we need to visit nodes anyway to find and extract the winner
	auto it = find_if(m_eventFds.begin(), m_eventFds.end(), [fd](auto& el){return el.fd.get() == fd;});
	if (it == m_eventFds.end())
	{
		if (m_cbReport)
			m_cbReport({unique_fd(), "Internal error", false});
	}
	else
	{
		it->ev.reset();
		if (m_cbReport)
			m_cbReport({move(it->fd), se, false});
	}
	return abort();
}

void tConnRqData::disable(int fd, int ec)
{
	LOGSTARTFUNCx(fd);
	ASSERT(fd != -1);
	auto it = find_if(m_eventFds.begin(), m_eventFds.end(), [fd](auto& el){return el.fd.get() == fd;});
	if (it == m_eventFds.end())
		return;
	// error from primary always wins, grab before closing it
	if (it == m_eventFds.begin())
		setIfNotEmpty(m_error2report, tErrnoFmter(ec));
	// stop observing and release resources
	if (it->ev.get())
		m_pending--;
	m_eventFds.erase(it);
}
}
