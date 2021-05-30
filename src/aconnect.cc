#include "aconnect.h"
#include "meta.h"
#include "acfg.h"
#include "sockio.h"
#include "evabase.h"
#include "debug.h"
//#include "acsmartptr.h"

#include <future>

#include <sys/types.h>
#include <sys/socket.h>

using namespace std;

namespace acng
{

string dnsError("Unknown DNS error");

aconnector::~aconnector()
{
	for(auto& el: m_eventFds)
	{
		if (el.second)
			event_free(el.second);
		checkforceclose(el.first);
	}
}

void aconnector::Connect(cmstring& target, uint16_t port, unsigned timeout, std::function<void (unique_fd, std::string)> cbReport)
{
	auto exTime = GetTime() + timeout;
	evabase::Post([exTime, target, port, cbReport](bool canceled)
	{
		if (canceled)
			return cbReport(unique_fd(), "Canceled");
		auto o = new aconnector;
		o->m_tmoutTotal = exTime;
		o->m_cback = move(cbReport);
		// capture the object there
		CAddrInfo::Resolve(target, port, [o](std::shared_ptr<CAddrInfo> res)
		{
			o->processDnsResult(move(res));
		});
	});
}

pair<unique_fd, std::string> aconnector::Connect(cmstring &target, uint16_t port, unsigned timeout)
{
	std::promise<pair<unique_fd, std::string>> reppro;
	Connect(target, port, timeout, [&](unique_fd ufd, mstring serr)
	{
		reppro.set_value(make_pair(move(ufd), move(serr)));
	});
	return reppro.get_future().get();
}

void aconnector::processDnsResult(std::shared_ptr<CAddrInfo> res)
{
	if (!res)
		return retError(dnsError);
	auto errDns = res->getError();
	if (!errDns.empty())
		return retError(errDns);
	m_targets = res->getTargets();
	if (m_targets.empty())
		return retError(dnsError);

	step(-1, 0);
}

void aconnector::cbStep(int fd, short what, void *arg) {
	((aconnector*)arg)->step(fd, what);
}

void aconnector::step(int fd, short what)
{
	//LOGSTARTFUNCs(fd, what);
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
	auto isFirst = fd == -1;

	if (isFirst)
	{
		m_timeNextCand = now + cfg::fasttimeout;
	}
	else if (now < m_timeNextCand)
	{
		// not yet finished with this socket, try again
		return;
	}
	else
	{
		m_timeNextCand = now + cfg::GetFirstConTimeout()->tv_sec;

		if (now > m_tmoutTotal)
			disable(fd, ETIMEDOUT);

		// otherwise, try to find a candidate and invoke another connect
	}
	// open attempt for the selected next candidate, or any valid which comes after
	for (; !m_targets.empty(); m_targets.pop_front())
	{
		// to move into m_eventFds on success
		unique_fd nextFd;
		unique_event pe;

		nextFd.m_p = ::socket(m_targets.front().ai_family, SOCK_STREAM, 0);
		if (!nextFd.valid())
		{
			if (m_error2report.empty()) m_error2report = tErrnoFmter();
			continue;
		}

		set_connect_sock_flags(nextFd.get());

		for (unsigned i=0; i < 50; ++i)
		{
			auto res = connect(nextFd.get(), (sockaddr*) & m_targets.front().ai_addr, m_targets.front().ai_addrlen);
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
			m_eventFds.push_back({nextFd.release(), pe.release()});
			m_pending++;
			// advance to the next after timeout
			m_targets.pop_front();
			return;
		}
		setIfNotEmpty(m_error2report, "Out of memory"sv);
	}
	// not success if got here, any active connection pending?
	if (m_pending == 0)
		return retError(m_error2report.empty() ? tErrnoFmter(EAFNOSUPPORT) : m_error2report);
}

void aconnector::retError(mstring msg)
{
	m_cback(unique_fd(), move(msg));
	delete this;
}

void aconnector::retSuccess(int fd)
{
	// doing destructors work already since we need to visit nodes anyway to find and extract the winner
	for(auto& el: m_eventFds)
	{
		if (el.second)
			event_free(el.second);
		if (el.first == fd)
			continue;
		checkforceclose(el.first);
	}
	m_eventFds.clear();
	m_cback(unique_fd(fd), sEmptyString);
	delete this;
}

void aconnector::disable(int fd, int ec)
{
	ASSERT(fd != -1);
	for(auto& el: m_eventFds)
	{
		if (el.first == fd)
		{
			// error from primary always wins, grab before closing it
			if (&el == &m_eventFds.front())
			{
				setIfNotEmpty(m_error2report, tErrnoFmter(ec));
			}

			// stop observing and release resources
			if (el.second)
			{
				m_pending --;
				event_free(el.second);
				el.second = nullptr;
			}

			checkforceclose(el.first);
		}
	}
}

}
