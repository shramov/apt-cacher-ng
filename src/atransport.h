#ifndef ATCPSTREAM_H
#define ATCPSTREAM_H

#include "acsmartptr.h"
#include "actemplates.h"
#include "aevutil.h"
#include "ahttpurl.h"

namespace acng
{

class tHttpUrl;
struct tConnContext;

/**
  * This implement second level connection establishment, which serves
  * various purposes and automates the setup flow.
  */
class ACNG_API atransport : public acng::tLintRefcounted
{
protected:
	unique_bufferevent m_buf;
	bool m_bPeerIsProxy = false;
	tHttpUrl m_url;
	friend struct tConnContext;

public:
	atransport() =default;
	struct tResult
	{
		lint_ptr<atransport> strm;
		mstring err;
		bool isFresh;
	};
	using tCallBack = std::function<void(tResult)>;

	struct TConnectParms
	{
		bool noCache = false; // always build a new connection
		bool directConnection = false; // establish a direct stream even through proxy
		bool noTlsOnTarget = false; // (even for HTTPS urls), don't do the final switch to TLS layer
		int timeoutSeconds = -1; // timeout value, -1 for config default, 0 to disable timeout

		TConnectParms& SetTimeout(int n) { timeoutSeconds = n; return *this; }
		TConnectParms& SetDirectConnection(bool val) { directConnection = val ; return *this; }
		TConnectParms& SetNoTlsOnTarget(bool val) { noTlsOnTarget = val ; return *this; }

		TConnectParms() {};
		//void AppendFingerprint(mstring& prefix) const;
	};

	/**
	 * @brief Create a new stream, optionally through proxy
	 * @param forceFresh Don't use a cached connection
	 * @param forceTimeout If positive value, override the regular timeout
	 * @param connectThrough If using a proxy, do a CONNECT to establish a virtual connection to the actual host
	 * @param sslUpgrade Overlay the stream with TLS stream after connecting
	 * @param cback
	 */
	static TFinalAction Create(tHttpUrl, const tCallBack&, TConnectParms extHints = TConnectParms());
	/**
	 * @brief Return an item to cache for reuse by others, or destroy if not cacheable
	 * @param stream
	 */
	static void Return(lint_ptr<atransport>& stream);

	bufferevent *GetBufferEvent() { return *m_buf; }
	cmstring& GetHost() { return m_url.sHost; }
	uint16_t GetPort() { return m_url.GetPort(); }
	bool PeerIsProxy() { return m_bPeerIsProxy; }

	/**
	 * @brief GetConnKey creates an identifier which describes the connection.
	 * @return
	 */
	std::string GetConnKey();
};

}

#endif // ATCPSTREAM_H
