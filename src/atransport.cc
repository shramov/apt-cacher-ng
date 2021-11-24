
#include "atransport.h"
#include "ahttpurl.h"
#include "portutils.h"
#include "acfg.h"
#include "evabase.h"
#include "aconnect.h"
#include "fileio.h"
#include "meta.h"
#include "debug.h"
#include "aevutil.h"

#include <map>

#include <event.h>
#include <event2/bufferevent.h>

using namespace std;

#define REUSE_TARGET_CONN

namespace acng
{

cmstring siErr("Internal Error"sv);
time_t g_proxyBackOffUntil = 0;

#ifdef REUSE_TARGET_CONN
multimap<string, lint_ptr<atransport>> g_con_cache;
#define CACHE_SIZE_MAX 42
#define REUSE_TIMEOUT_LIMIT 31
static timeval expirationTimeout;
static const timeval* g_pExpTimeout = nullptr;
static const timeval* GetKeepTimeout()
{
	if (g_pExpTimeout)
		return g_pExpTimeout;

	// find or guess something sensible
	if (cfg::maxtempdelay <= 0)
	{
		if (cfg::GetNetworkTimeout() && cfg::GetNetworkTimeout()->tv_sec <= REUSE_TIMEOUT_LIMIT)
		g_pExpTimeout = cfg::GetNetworkTimeout();
	}
	else
	{
		if (cfg::maxtempdelay <= REUSE_TIMEOUT_LIMIT)
		{
			expirationTimeout = { cfg::maxtempdelay, 1};
			g_pExpTimeout = &expirationTimeout;
		}
	}
	if (!g_pExpTimeout)
	{
		expirationTimeout = { REUSE_TIMEOUT_LIMIT / 2, 1};
		g_pExpTimeout = &expirationTimeout;
	}
	return g_pExpTimeout;
}
#endif

class atransportEx : public atransport
{
public:
#ifdef REUSE_TARGET_CONN
	decltype (g_con_cache)::iterator m_cleanIt;

	// stop operations for storing in the cache, respond to timeout only
	void Moothball()
	{
		if (!m_buf.valid() || g_con_cache.size() > CACHE_SIZE_MAX)
			return;
		bufferevent_setcb(*m_buf, nullptr, nullptr, cbCachedKill, this);
		bufferevent_set_timeouts(*m_buf, GetKeepTimeout(), nullptr);
		bufferevent_enable(*m_buf, EV_READ);
		m_cleanIt = g_con_cache.emplace(makeHostPortKey(GetHost(), GetPort()), lint_ptr<atransport>(this));
	}
	// restore operation after hibernation
	void Reused()
	{
		bufferevent_disable(*m_buf, EV_READ | EV_WRITE);
		bufferevent_setcb(*m_buf, nullptr, nullptr, nullptr, this);
		m_cleanIt = g_con_cache.end();
	}
	static void cbCachedKill(struct bufferevent *, short , void *ctx)
	{
		auto delIfLast = as_lptr((atransportEx*) ctx);
		g_con_cache.erase(delIfLast->m_cleanIt);
	}
#endif
};

time_t g_proxyDeadUntil = 0;

struct tConnContext : public tLintRefcounted
{
	atransport::TConnectParms m_hints;
	atransport::tCallBack m_reporter;
#warning use a derived class with iterator selfref
	lint_ptr<atransportEx> m_result;
	TFinalAction m_connBuilder;

	void Abort()
	{
		m_result.reset();
	}

	void Start()
	{
		const auto* tgt = &m_result->m_url;

		auto prInfo = cfg::GetProxyInfo();
		if (prInfo)
		{
			if (cfg::optproxytimeout > 0)
			{
				if (GetTime() < g_proxyDeadUntil)
					prInfo = nullptr; // unusable for now
			}
			if (prInfo)
				tgt = prInfo;
		}

		m_connBuilder = aconnector::Connect(tgt->sHost, tgt->GetPort(),
											[pin = as_lptr(this), this, prInfo, tgt](aconnector::tConnResult res)
		{
			if (!m_result || !m_reporter)
				return;

			if (!res.sError.empty())
			{
#warning if AUTO_TIMEOUT_FALLBACK_STICKY then setup condition and restart
				return m_reporter({lint_ptr<atransport>(), res.sError, true, res.isDnsError});
			}

			m_result->m_buf.reset(bufferevent_socket_new(evabase::base, res.fd.release(), BEV_OPT_CLOSE_ON_FREE));
			m_result->m_bPeerIsProxy = prInfo;

			if (!m_result->m_buf.get())
				return m_reporter({lint_ptr<atransport>(), "Internal Error w/o message", true, res.isDnsError});

			bool doAskConnect = prInfo && ( m_hints.directConnection || m_result->m_url.m_schema == tHttpUrl::EProtoType::HTTPS);

			if (doAskConnect)
			{
				ASSERT(!"implementme");
				// return DoConnectNegotiation();
			}

			// switch to SSL, either for the proxy or for the target?
			bool doSslUpgrade = tgt->m_schema == tHttpUrl::EProtoType::HTTPS;

			if (doSslUpgrade)
			{
				ASSERT(!"implementme");
				//return DoTlsSwitch(isProxy);
			}

			return m_reporter({static_lptr_cast<atransport>(m_result), se, true, res.isDnsError});
		}
		, m_hints.timeoutSeconds);
	}

	tConnContext(tHttpUrl &&url, const atransport::tCallBack &cback, atransport::TConnectParms&& extHints)
		: m_hints(extHints), m_reporter(cback)
	{
		m_result = make_lptr<atransportEx>();
		m_result->m_url = move(url);
	}
};

TFinalAction atransport::Create(tHttpUrl url, const tCallBack &cback, TConnectParms extHints)
{
	ASSERT(cback);

	if (!extHints.noCache && !extHints.noTlsOnTarget && !extHints.directConnection)
	{
#ifdef REUSE_TARGET_CONN
		auto cacheKey = url.GetHostPortKey();
		auto anyIt = g_con_cache.find(cacheKey);
		if (anyIt != g_con_cache.end())
		{
			auto ret = anyIt->second;
			if (AC_LIKELY(ret->m_buf.valid()))
			{
				g_con_cache.erase(anyIt);
				static_lptr_cast<atransportEx>(ret)->Reused();
				evabase::Post([ret, cback](){ if (ret->m_buf.valid()) cback({ret, se, false, false});});
				return TFinalAction([ret](){ ret->m_buf.reset(); });
			}
		}
#endif
	}

	try
	{
		lint_ptr<tConnContext> tr;
		tr.reset(new tConnContext(move(url), cback, move(extHints)));
		tr->Start();
		return TFinalAction([tr](){ tr->Abort(); });
	}
	catch (...)
	{
		return TFinalAction();
	}
}

void atransport::Return(lint_ptr<atransport> &stream)
{
#ifdef REUSE_TARGET_CONN
	static_lptr_cast<atransportEx>(stream)->Moothball();
#endif
	stream.reset();
}

}
