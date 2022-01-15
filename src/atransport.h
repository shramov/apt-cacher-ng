#ifndef ATCPSTREAM_H
#define ATCPSTREAM_H

#include "acsmartptr.h"
#include "actemplates.h"
#include "aevutil.h"
#include "ahttpurl.h"
#include "acomcommon.h"

namespace acng
{

class tHttpUrl;
struct tConnContext;
class acres;

/**
  * A transport represents an established stream which transports plain HTTP protocol (proxy-style or not),
  * it handles the proxy selection, connect-through scheme if needed and TLS handshake and internal crypto overlay setup
  * (unless specified otherwise).
  *
  * "host" and "port" are the main target parameters.
  */
class ACNG_API atransport : public acng::tLintRefcounted
{
protected:
	unique_bufferevent m_buf;
	bool m_bIsSslStream = false;
	bool m_bCanceled = false;
	tHttpUrl m_url;
	const tHttpUrl* m_pProxy = nullptr;
	friend struct tConnContext;

public:
	atransport() =default;
	virtual ~atransport() =default;
	struct tResult
	{
		lint_ptr<atransport> strm;
		mstring err;
		tComError flags;
		tResult(tComError flags, string_view errMsg);
		tResult(tComError flags, mstring&& errMsg);
		tResult(tComError flags, lint_ptr<atransport>);
	};
	using tCallBack = std::function<void(tResult)>;

	struct TConnectParms
	{
		bool noCache = false; // always build a new connection
		bool directConnection = false; // establish a direct stream even through proxy
		bool noTlsOnTarget = false; // (even for HTTPS urls), don't do the final switch to TLS layer
		int timeoutSeconds = -1; // timeout value, -1 for config default, 0 to disable timeout

		const tHttpUrl* forcedProxy = nullptr;

		TConnectParms& SetTimeout(int n) { timeoutSeconds = n; return *this; }
		TConnectParms& SetDirectConnection(bool val) { directConnection = val; return *this; }
		TConnectParms& SetNoTlsOnTarget(bool val) { noTlsOnTarget = val; return *this; }
		TConnectParms& SetProxy(const tHttpUrl* forcedProxy_) { forcedProxy = forcedProxy_; return *this; }

		TConnectParms() {};
	};

	/**
	 * @brief Create a new stream, optionally through proxy
	 * @param cback
	 */
	static TFinalAction Create(tHttpUrl, const tCallBack&, acres& res, TConnectParms extHints = TConnectParms());

	/**
	 * @brief Return an item to cache for reuse by others, or destroy if not cacheable
	 * @param stream
	 */
	static void Return(lint_ptr<atransport>& stream);

	bufferevent *GetBufferEvent() { return *m_buf; }
	const tHttpUrl& GetTargetHost() { return m_url; }
	const tHttpUrl* GetUsedProxy() { return m_pProxy; }

	/**
	 * Check whether the proxy was blocked temporarily; this is static for
	 * now because this failure timeout is a global option, might be changed
	 * in future to transport specific blocking
	 */
	static bool IsProxyNowBroken();

private:
	static TFinalAction CreateUds(tHttpUrl, const tCallBack&);

};

}

#endif // ATCPSTREAM_H
