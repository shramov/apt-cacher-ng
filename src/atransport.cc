
#include "atransport.h"
#include "ahttpurl.h"
#include "portutils.h"
#include "acfg.h"
#include "evabase.h"
#include "aconnect.h"
#include "fileio.h"
#include "meta.h"
#include "acres.h"
#include "debug.h"
#include "aevutil.h"
#include "ac3rdparty.h"
#include "header.h"
#include "acomcommon.h"

#include <map>

#include <event.h>
#include <event2/bufferevent.h>

#ifdef HAVE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <event2/bufferevent_ssl.h>
#endif

using namespace std;

#define REUSE_TARGET_CONN

#warning Do we need BEV_OPT_DEFER_CALLBACKS?

namespace acng
{

const mstring pfxSslError("Fatal TLS error: "sv);

cmstring siErr("Internal Error"sv);
time_t g_proxyBackOffUntil = 0;

using unique_ssl = auto_raii<SSL*,SSL_free,nullptr>;

#ifdef REUSE_TARGET_CONN

#define CACHE_SIZE_MAX 42
#define REUSE_TIMEOUT_LIMIT 31

static timeval expirationTimeout;
static const timeval* g_pExpTimeout = nullptr;

using tConnCache = multimap<string, lint_ptr<atransport>>;

/**
 * @brief Basic container with on-demand initialization and controlled shutdown.
 */
class ConnCacher
{
	unique_ptr<tConnCache> m_cache;
	TFinalAction sub;
public:
	multimap<string, lint_ptr<atransport>>& get()
	{
		if (!m_cache)
		{
			m_cache = make_unique<tConnCache>();
			sub = evabase::GetGlobal().subscribe([&](){m_cache.reset();});
		}
		return *m_cache;
	}
	bool Full() {return get().size() > CACHE_SIZE_MAX; }
} g_con_cache;


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
	tConnCache::iterator m_cleanIt;

	// stop operations for storing in the cache, respond to timeout only
	void Moothball()
	{
		if (!m_buf.valid() || g_con_cache.Full())
			return;
		bufferevent_setcb(*m_buf, nullptr, nullptr, cbCachedKill, this);
		bufferevent_set_timeouts(*m_buf, GetKeepTimeout(), nullptr);
		bufferevent_enable(*m_buf, EV_READ);
		m_cleanIt = g_con_cache.get().emplace(HostPortKeyMaker(GetHost(), GetPort()),
											  lint_ptr<atransport>(this));
	}
	// restore operation after hibernation
	void GotReused()
	{
		bufferevent_disable(*m_buf, EV_READ | EV_WRITE);
		bufferevent_setcb(*m_buf, nullptr, nullptr, nullptr, this);
		m_cleanIt = g_con_cache.get().end();
	}
	/**
	 * @brief cbCachedKill can only be triggered by a timeout or other error
	 */
	static void cbCachedKill(struct bufferevent *, short , void *ctx)
	{
		auto delIfLast = as_lptr((atransportEx*) ctx);
		g_con_cache.get().erase(delIfLast->m_cleanIt);
	}
#endif
	~atransportEx()
	{
		if (GetBufferEvent() && m_bIsSslStream)
		{
			auto ssl = bufferevent_openssl_get_ssl(GetBufferEvent());
			if (ssl)
			{
				SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
				SSL_shutdown(ssl);
			}
		}
	}
};

time_t g_proxyDeadUntil = 0;

struct tConnContext : public tLintRefcounted
{
	atransport::TConnectParms m_hints;
	atransport::tCallBack m_reporter;
	lint_ptr<atransportEx> m_result;
#define ABORT_IF_CANCELED if (!m_result || !m_reporter) return;
	TFinalAction m_connBuilder;
	acres& m_res;

	unique_bufferevent& bev() { return m_result->m_buf; }

	// XXX: reenable specific insecure modes?
	bool m_disableNameValidation = cfg::nsafriendly == 1;// || (bGuessedTls * cfg::nsafriendly == 2);
	bool m_disableAllValidation = cfg::nsafriendly == 1; // || (bGuessedTls * (cfg::nsafriendly == 2 || cfg::nsafriendly == 3));

	// temp vars to keep the lambda object small enough for optimization
	const tHttpUrl *m_currentTarget, *m_prInfo;

	tConnContext(tHttpUrl &&url, const atransport::tCallBack &cback,
				 atransport::TConnectParms&& extHints, acres& res)
		: m_hints(extHints), m_reporter(cback), m_res(res)
	{
		m_result = make_lptr<atransportEx>();
		m_result->m_url = move(url);
	}

	void Abort()
	{
		m_result.reset();
	}

	void Start()
	{
		m_currentTarget = &m_result->m_url;
		m_prInfo = cfg::GetProxyInfo();
		if (m_prInfo)
		{
			if (cfg::optproxytimeout > 0 && GetTime() < g_proxyDeadUntil)
				m_prInfo = nullptr; // unusable for now
			if (m_prInfo)
			{
				m_result->m_bPeerIsProxy = true;
				m_currentTarget = m_prInfo;
			}
		}
		m_connBuilder = aconnector::Connect(m_currentTarget->sHost, m_currentTarget->GetPort(),
											[pin = as_lptr(this)](auto res)
		{
			pin->OnFirstConnect(move(res));
		}, m_hints.timeoutSeconds);
	}

	void OnFirstConnect(aconnector::tConnResult &&res)
	{
		ABORT_IF_CANCELED;

		if (!res.sError.empty())
		{
#warning if AUTO_TIMEOUT_FALLBACK_STICKY then setup condition and restart
			return m_reporter({res.flags, res.sError});
		}

		// switch to SSL, either for the proxy or for the target?
		bool doSslUpgrade = m_currentTarget->m_schema == tHttpUrl::EProtoType::HTTPS;

		if (doSslUpgrade)
			return DoTlsSwitch(move(res.fd));

		m_result->m_buf.reset(bufferevent_socket_new(evabase::base, res.fd.get(), BEV_OPT_CLOSE_ON_FREE));
		if (!m_result->m_buf.get())
			return m_reporter({TRANS_INTERNAL_ERROR, "Internal Error w/o message"sv});

		// freed by BEV_OPT_CLOSE_ON_FREE, not by us
		res.fd.release();

		bool doAskConnect = m_prInfo && ( m_hints.directConnection || m_result->m_url.m_schema == tHttpUrl::EProtoType::HTTPS);

		if (doAskConnect)
			return DoConnectNegotiation();

		return m_reporter({0, static_lptr_cast<atransport>(m_result)});
	}

	void DoConnectNegotiation()
	{
		ABORT_IF_CANCELED;
		ASSERT(m_result.get());
		auto bev = m_result->GetBufferEvent();
		ASSERT(bev);

		bufferevent_setcb(bev, cbReadProxy, nullptr, cbStatusProxy, this);
		ebstream(bev) << "CONNECT "sv << m_result->m_url.sHost << ":" << m_result->m_url.sPath << " HTTP/1.1\r\n\r\n"sv;
		bufferevent_enable(bev, EV_READ|EV_WRITE);
	}

	static void cbReadProxy(struct bufferevent *bev, void *ctx)	{ ((tConnContext*)ctx)->onReadFromProxy(bev); }
	static void cbStatusProxy(struct bufferevent *bev, short what, void *ctx) { ((tConnContext*)ctx)->onStatusFromProxy(bev, what); }
	static void cbStatusSslCheck(struct bufferevent *bev, short what, void *ctx) { ((tConnContext*)ctx)->onStatusSslCheck(bev, what); }

	void onReadFromProxy(bufferevent* bev)
	{
		ABORT_IF_CANCELED;
		header h;
		auto r = h.Load(bereceiver(bev));
		if (!r)
			return;

		// are we good?
		if (r > 0)
		{
			evbuffer_drain(bereceiver(bev), r);

			auto status = h.getStatus();
			if (AC_UNLIKELY(h.type != header::eHeadType::ANSWER))
				status = { 503, to_string("Bad proxy response"sv)};

			if (h.getStatus().code == 200)
			{
				bufferevent_disable(bev, EV_READ | EV_WRITE);
				bufferevent_setcb(bev, nullptr, nullptr, nullptr, nullptr);
				return m_reporter({ 0, as_lptr<atransport>(m_result)});
			}
			return m_reporter({ eTransErrors::TRANS_STREAM_ERR_FATAL | eTransErrors::TRANS_FAULTY_SSL_PEER, status.msg });
		}
		return m_reporter({ eTransErrors::TRANS_STREAM_ERR_FATAL | eTransErrors::TRANS_FAULTY_SSL_PEER, "Bad SSL tunnel response"sv });
	}
	void onStatusFromProxy(struct bufferevent *bev, short what)
	{
		ABORT_IF_CANCELED;
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
			return m_reporter({ eTransErrors::TRANS_STREAM_ERR_FATAL | eTransErrors::TRANS_FAULTY_SSL_PEER, "Bad SSL tunnel response"sv });
	}

	void DoTlsSwitch(unique_fd ufd)
	{
		mstring sErr;

		auto withLastSslError = [&]() ->mstring
		{
			auto nErr = ERR_get_error();
			auto serr = ERR_reason_error_string(nErr);
			if (!serr)
				serr = "Internal error";
			return pfxSslError + serr;
		};
		auto ctx = m_res.GetSslConfig().GetContext();
		if (!ctx)
			return m_reporter({ TRANS_INTERNAL_ERROR, pfxSslError + m_res.GetSslConfig().GetContextError()});
		unique_ssl ssl(SSL_new(ctx));
		if (! *ssl)
			return m_reporter({ TRANS_INTERNAL_ERROR, withLastSslError() });

		// for SNI
		SSL_set_tlsext_host_name(*ssl, m_currentTarget->sHost.c_str());

		if (!m_disableNameValidation)
		{
			auto param = SSL_get0_param(*ssl);
			/* Enable automatic hostname checks */
			X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			X509_VERIFY_PARAM_set1_host(param, m_currentTarget->sHost.c_str(), 0);
			/* Configure a non-zero callback if desired */
			SSL_set_verify(*ssl, SSL_VERIFY_PEER, 0);
		}

		m_result->m_buf.reset(bufferevent_openssl_socket_new(evabase::base,
			ufd.get(),
			*ssl,
			BUFFEREVENT_SSL_CONNECTING,
			BEV_OPT_CLOSE_ON_FREE));

		if (AC_LIKELY(m_result->m_buf.valid()))
		{
			// those will eventually freed by BEV_OPT_CLOSE_ON_FREE
			m_result->m_bIsSslStream = true;
			ssl.release();
			ufd.release();
		}
		else
			return m_reporter({TRANS_INTERNAL_ERROR, withLastSslError()});

		if (m_disableAllValidation && m_disableNameValidation)
			return m_reporter({0, static_lptr_cast<atransport>(m_result)});

		// okay, let's verify the SSL state in the status callback
		bufferevent_setcb(m_result->GetBufferEvent(), nullptr, nullptr, cbStatusSslCheck, this);
		__inc_ref();
	}

	void onStatusSslCheck(struct bufferevent *bev, short what)
	{
		ABORT_IF_CANCELED;

		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
		{
			return m_reporter({TRANS_FAULTY_SSL_PEER, pfxSslError + "Handshake aborted"});
		}

		if (what & BEV_EVENT_CONNECTED)
		{
			auto hret = SSL_get_verify_result(bufferevent_openssl_get_ssl(bev));
			if( hret != X509_V_OK)
			{
				auto err = X509_verify_cert_error_string(hret);
				if (!err) err = "Handshake failed";
				return m_reporter({TRANS_FAULTY_SSL_PEER, pfxSslError + err});
			}
			auto remote_cert = SSL_get_peer_certificate(bufferevent_openssl_get_ssl(bev));
			if(remote_cert)
			{
				// XXX: maybe extract the real name to a buffer and report it in the log?
				// X509_NAME_oneline(X509_get_subject_name (server_cert), cert_str, sizeof (cert_str));
				X509_free(remote_cert);
			}
			else // The handshake was successful although the server did not provide a certificate
				return m_reporter({TRANS_FAULTY_SSL_PEER, pfxSslError + "Incompatible remote certificate"});

			bufferevent_disable(bev, EV_READ | EV_WRITE);
			bufferevent_setcb(bev, nullptr, nullptr, nullptr, nullptr);
			return m_reporter({ 0, static_lptr_cast<atransport>(m_result)});
		}
		ASSERT(!"unknown event?");
	}
};


TFinalAction atransport::Create(tHttpUrl url, const tCallBack &cback, acres& res, TConnectParms extHints)
{
	ASSERT(cback);

	if (!extHints.noCache && !extHints.noTlsOnTarget && !extHints.directConnection)
	{
#ifdef REUSE_TARGET_CONN
		auto cacheKey = url.GetHostPortKey();
		auto anyIt = g_con_cache.get().find(cacheKey);
		if (anyIt != g_con_cache.get().end())
		{
			auto ret = move(anyIt->second);
			g_con_cache.get().erase(anyIt);
			if (AC_LIKELY(ret->m_buf.valid()))
			{
				static_lptr_cast<atransportEx>(ret)->GotReused();
				evabase::Post([ret, cback]()
				{
					if (ret->m_buf.valid())
						cback({TRANS_WAS_USED, move(ret)});
				});
				return TFinalAction([ret](){ ret->m_buf.reset(); });
			}
		}
#endif
	}

	try
	{
		lint_ptr<tConnContext> tr;
		tr.reset(new tConnContext(move(url), cback, move(extHints), res));
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
	if (!evabase::GetGlobal().IsShuttingDown())
		static_lptr_cast<atransportEx>(stream)->Moothball();
#endif
	stream.reset();
}

inline atransport::tResult::tResult(tComError flags, string_view errMsg)
{
	this->flags = flags;
	this->err = to_string(errMsg);
}

atransport::tResult::tResult(tComError flags, mstring &&errMsg)
{
	this->flags = flags;
	this->err = move(errMsg);
}

inline atransport::tResult::tResult(tComError flags, lint_ptr<atransport> result)
{
	this->strm = move(result);
	this->flags = flags;
}

}
