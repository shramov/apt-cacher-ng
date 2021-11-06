
#include "atransport.h"
#include "ahttpurl.h"
#include "portutils.h"
#include "acfg.h"
#include "evabase.h"
#include "aconnect.h"
#include "fileio.h"
#include "meta.h"

#include <map>

#include <event.h>
#include <event2/bufferevent.h>

using namespace std;

//#define REUSE_TARGET_CONN

namespace acng
{

class atcpstreamImpl;
#ifdef REUSE_TARGET_CONN
//multimap<string, lint_ptr<atcpstreamImpl>> g_con_cache;
#define CACHE_SIZE_MAX 42
void cbCachedKill(struct bufferevent *bev, short what, void *ctx);
#endif

time_t g_proxyBackOffUntil = 0;

class atcpstreamImpl : public atransport
{
	bufferevent* m_buf = nullptr;
	mstring sHost;
	uint16_t nPort;
	bool m_bPeerIsProxy;
#ifdef REUSE_TARGET_CONN
	decltype (g_con_cache)::iterator m_cleanIt;
#endif

public:
	atcpstreamImpl(string host, uint16_t port, bool isProxy) :
		sHost(move(host)),
		nPort(port),
		m_bPeerIsProxy(isProxy)
	{
	}
	~atcpstreamImpl()
	{
		if (m_buf)
			bufferevent_free(m_buf);
	}
#ifdef REUSE_TARGET_CONN
	// mooth-ball operations for storing in the cache
	void Hibernate(decltype (m_cleanIt) cacheRef)
	{
		if (!m_buf)
			return;
		m_cleanIt = cacheRef;
		bufferevent_setcb(m_buf, nullptr, nullptr, cbCachedKill, this);
		bufferevent_set_timeouts(m_buf, cfg::GetNetworkTimeout(), nullptr);
		bufferevent_enable(m_buf, EV_READ);
	}
	// restore operation after hibernation
	void Reuse()
	{
		bufferevent_disable(m_buf, EV_READ | EV_WRITE);
		bufferevent_setcb(m_buf, nullptr, nullptr, nullptr, this);
		m_cleanIt = g_con_cache.end();
	}
#endif
	void SetEvent(bufferevent *be) { m_buf = be; }

	// atcpstream interface
        bufferevent *GetBufferEvent() override { return m_buf; }
	const string &GetHost() override { return sHost; }
	uint16_t GetPort() override { return nPort; }
	bool PeerIsProxy() override { return m_bPeerIsProxy; }
};


struct tConnContext : public tLintRefcounted
{
	atransport::TConnectParms m_hints;
	atransport::tCallBack m_reporter;
	tHttpUrl m_target;
	enum class eState
	{
		NEW,
		NEW_CON_PROXY
	} m_state = eState::NEW;
	int m_fd = -1;

	void Step(short what)
	{

	}

	tConnContext(const tHttpUrl &url, const atransport::tCallBack &cback, const atransport::TConnectParms &extHints)
		: m_hints(extHints), m_reporter(cback), m_target(url)
	{
	}
	~tConnContext()
	{
		checkforceclose(m_fd);
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

mstring atransport::TConnectParms::AddFingerprint(mstring &&prefix) const
{
	prefix += '/';
	prefix += char(directConnection);
	prefix += char(noTlsOnTarget);
	prefix += char(proxyStrategy);
	return move(prefix);
}



void atransport::Create(const tHttpUrl &url, const tCallBack &cback, const TConnectParms &extHints)
{
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
			return cback({static_lptr_cast<atransport>(ret), se, false});
		}
	}
#endif
	(new tConnContext(url, cback, extHints))->Step(0);
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
