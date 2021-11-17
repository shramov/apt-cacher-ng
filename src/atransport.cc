
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

//#define REUSE_TARGET_CONN

namespace acng
{

cmstring siErr("Internal Error"sv);

#ifdef REUSE_TARGET_CONN
//multimap<string, lint_ptr<atcpstreamImpl>> g_con_cache;
#define CACHE_SIZE_MAX 42
void cbCachedKill(struct bufferevent *bev, short what, void *ctx);
#endif

time_t g_proxyBackOffUntil = 0;

class atransportEx : public atransport
{
public:
#ifdef REUSE_TARGET_CONN
	decltype (g_con_cache)::iterator m_cleanIt;
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
/*
	enum class eState
	{
		ABORTED,
		PLAINCON,
		PLAINCON_PROXY
	} m_state = eState::PLAINCON;
*/
	void Abort()
	{
//		m_state = eState::ABORTED;
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

#define ASTRANSPORT static_lptr_cast<atransport>(m_result)
			if (!res.sError.empty())
			{
#warning if AUTO_TIMEOUT_FALLBACK_STICKY then setup condition and restart
				return m_reporter({lint_ptr<atransport>(), res.sError, true});
			}

			m_result->m_buf.reset(bufferevent_socket_new(evabase::base, res.fd.release(), BEV_OPT_CLOSE_ON_FREE));
			m_result->m_bPeerIsProxy = prInfo;

			if (AC_UNLIKELY(!m_result->m_buf.get()))
				return m_reporter({lint_ptr<atransport>(), "Internal Error w/o message", true});

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

			return m_reporter({ASTRANSPORT, se, true});
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
#ifdef REUSE_TARGET_CONN
void cbCachedKill(struct bufferevent *, short , void *ctx)
{
	auto pin = as_lptr((atcpstreamImpl*) ctx);
	auto key = makeHostPortKey(pin->GetHost(), pin->GetPort());
	// XXX: this is not really efficient, maybe store the iterator in the class?
	auto hit = g_con_cache.equal_range(key);
	for (auto it = hit.first; it != hit.second; ++it)
	{
		if (it->second.get() == pin.get())
		{
			g_con_cache.erase(it);
			return;
		}
	}
}
#endif

void atransport::TConnectParms::AppendFingerprint(mstring &prefix) const
{
	prefix += '/';
	prefix += char(directConnection);
	prefix += char(noTlsOnTarget);
}

#ifdef REUSE_TARGET_CONN
	// mooth-ball operations for storing in the cache
	void atransportEx::Moothball(decltype (m_cleanIt) cacheRef)
	{
		if (!m_buf)
			return;
		m_cleanIt = cacheRef;
		bufferevent_setcb(m_buf, nullptr, nullptr, cbCachedKill, this);
		bufferevent_set_timeouts(m_buf, cfg::GetNetworkTimeout(), nullptr);
		bufferevent_enable(m_buf, EV_READ);
	}
	// restore operation after hibernation
	void atransportEx::Reuse()
	{
		bufferevent_disable(m_buf, EV_READ | EV_WRITE);
		bufferevent_setcb(m_buf, nullptr, nullptr, nullptr, this);
		m_cleanIt = g_con_cache.end();
	}
#endif

TFinalAction atransport::Create(tHttpUrl url, const tCallBack &cback, TConnectParms extHints)
{
	ASSERT(cback);

#ifdef REUSE_TARGET_CONN
	auto cacheKey = extHints.AddFingerprint(url.GetHostPortKey());
	if (!extHints.noCache)
	{
		auto anyIt = g_con_cache.find(cacheKey);
		if (anyIt != g_con_cache.end())
		{
			auto ret = anyIt->second;
			g_con_cache.erase(anyIt);
			ret->Reuse();
#error nope, run through post queue
			return cback({static_lptr_cast<atransport>(ret), se, false});
		}
	}
#endif

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
	if (g_con_cache.size() < CACHE_SIZE_MAX)
	{
		auto ptr = static_lptr_cast<atcpstreamImpl>(stream);
		auto it = g_con_cache.insert(make_pair(makeHostPortKey(ptr->GetHost(), ptr->GetPort()), ptr));
		ptr->Hibernate(it);
	}
#endif
	stream.reset();
}

#warning fixme
#if 0

void atransport::Create(const tHttpUrl& url, int forceTimeout, bool forceFresh, bool connectThrough,
					 bool sslUpgrade, bool noProxy, const tCallBack& cback)
{
get from cache here


	evabase::Post([host = url.sHost, port = url.GetPort(), doSsl = url.bSSL,
				  forceTimeout, cback]
				  (bool cancled)
	{
		if (cancled)
			return cback({lint_ptr<atransport>(), "Operation canceled", true});

		auto proxy = cfg::GetProxyInfo();
                auto timeout = forceTimeout > 0 ? forceTimeout : ( (cfg::GetProxyInfo() && cfg::optproxytimeout > 0)
                                          ? cfg::optproxytimeout : cfg::nettimeout);

#warning implement fallback to no-proxy

                /*
                 * 		aconnector::Connect(pin->url.sHost, pin->url.GetPort(),
                                                        cfg::optproxytimeout > 0
                                                        ? cfg::optproxytimeout
                                                        : cfg::nettimeout,
                                                        [pin] (aconnector::tConnResult res)


                                                        ...

                else if(cfg::optproxytimeout > 0) // ok... try without
                {
                        cfg::MarkProxyFailure();
                        goto direct_connect;
                }
                 * */

		aconnector::Connect(proxy ? proxy->sHost : host,
							proxy ? proxy->GetPort() : port,
							timeout,
							[cback, host, port, proxy, doSsl]
							(aconnector::tConnResult res) mutable
		{
			if (!res.sError.empty())
				return cback(lint_ptr<atransport>(), res.sError, true);
			int nFd = res.fd.release();
			atcpstreamImpl *p(nullptr);
			try
			{
				auto p = make_lptr<atcpstreamImpl>(host, port, proxy);
				if (proxy && doSsl)
				{
					// XXX: tPostConnOps setup and set callbacks for CONNECT call
					return;
				}
				else if (doSsl)
				{
					// XXX: tPostConnOps setup and set callbacks for SSL setup
					return;
				}
				else
				{
					p->SetEvent(bufferevent_new(nFd, nullptr, nullptr, nullptr, nullptr));
					return cback(static_lptr_cast<atransport>(p), string_view(), true);
				}
			}
			catch (...)
			{
			}

			if (p)
				delete(p);
			checkforceclose(nFd);

			//auto pin = make_lptr<streamConnectCtx>( );
		});
	});
}

#endif

}
