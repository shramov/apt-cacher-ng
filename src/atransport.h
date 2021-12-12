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
class acres;

/**
  * This implement second level connection establishment, which serves
  * various purposes and automates the setup flow.
  */
class ACNG_API atransport : public acng::tLintRefcounted
{
protected:
	unique_bufferevent m_buf;
	bool m_bPeerIsProxy = false;
	bool m_bIsSslStream = false;
	tHttpUrl m_url;
	friend struct tConnContext;

public:
	atransport() =default;
	virtual ~atransport() =default;
	struct tResult
	{
		lint_ptr<atransport> strm;
		mstring err;
		uint_fast16_t flags;
		tResult(uint_fast16_t flags, string_view errMsg);
		tResult(uint_fast16_t flags, mstring&& errMsg);
		tResult(uint_fast16_t flags, lint_ptr<atransport>);
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
	static TFinalAction Create(tHttpUrl, const tCallBack&, acres& res, TConnectParms extHints = TConnectParms());
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

#define TRANS_DNS_NOTFOUND 0x1
#define TRANS_TIMEOUT 0x2
#define TRANS_WAS_USED 0x4
#define TRANS_INTERNAL_ERROR 0x8
#define TRANS_FAULTY_SSL_PEER 0x10
// codes which mean that the connection shall not be retried for that peer
#define TRANS_CODE_FATAL(n) (n & (TRANS_INTERNAL_ERROR|TRANS_DNS_NOTFOUND|TRANS_TIMEOUT))

}

#endif // ATCPSTREAM_H
