#ifndef ATCPSTREAM_H
#define ATCPSTREAM_H

#include "acsmartptr.h"

#include <functional>

extern "C"
{
struct bufferevent;
}

namespace acng
{

class tHttpUrl;

/**
  * This implement second level connection establishment, which serves
  * various purposes and automates the setup flow.
  */
class atransport : public acng::tLintRefcounted
{
public:
	atransport() =default;
	struct tResult
	{
		lint_ptr<atransport> strm;
		string_view err;
		bool isFresh;
	};
	using tCallBack = std::function<void(tResult)>;

	enum class EProxyType
	{
		AUTO, // force use proxy if known
		AUTO_TIMEOUT_FALLBACK, // try proxy, stop using it after timeout
		AUTO_TIMEOUT_FALLBACK_STICKY, // like AUTO_TIMEOUT_FALLBACK but remember that proxy is faulty for a certain interval
		NONE // not using proxy, even if configured
	};

	struct TConnectParms
	{
		bool noCache = false; // always build a new connection
		bool directConnection = false; // establish a direct stream even through proxy
		bool noTlsOnTarget = false; // (even for HTTPS urls), don't do the final switch to TLS layer
		int timeoutSeconds = -1; // timeout value, -1 for config default, 0 to disable timeout
		EProxyType proxyStrategy = EProxyType::AUTO;
		TConnectParms& setTimeout(int n) { timeoutSeconds = n; return *this;}
		TConnectParms() {}
	};

	/**
	 * @brief Create a new stream, optionally through proxy
	 * @param forceFresh Don't use a cached connection
	 * @param forceTimeout If positive value, override the regular timeout
	 * @param connectThrough If using a proxy, do a CONNECT to establish a virtual connection to the actual host
	 * @param sslUpgrade Overlay the stream with TLS stream after connecting
	 * @param cback
	 */
	static void Create(const tHttpUrl&, const tCallBack& cback, const TConnectParms& extHints = TConnectParms());
	//static void Create(mstring host, uint16_t port, const tCallBack& cback);
	static void Return(lint_ptr<atransport>& stream);
	virtual bufferevent* GetBufferEvent() =0;
	virtual const std::string& GetHost() =0;
	virtual uint16_t GetPort() =0;
	virtual bool PeerIsProxy() =0;
	/**
	 * @brief GetConnKey creates an identifier which describes the connection.
	 * @return
	 */
	virtual std::string GetConnKey() =0;
};
}

#endif // ATCPSTREAM_H
