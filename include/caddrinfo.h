#ifndef CADDRINFO_H_
#define CADDRINFO_H_

#include "actypes.h"

#include <memory>
#include <list>
#include <functional>
#include <arpa/inet.h>

extern "C"
{
struct ares_addrinfo;
struct ares_addrinfo_node;
}

namespace acng
{

// keep just the information we need here, everythign else can be freed ASAP
struct acng_addrinfo
{
	int ai_family;
	socklen_t ai_addrlen;
	sockaddr_storage ai_addr;
	acng_addrinfo(ares_addrinfo_node*);
	static std::string formatIpPort(const sockaddr *pAddr, socklen_t addrLen, int ipFamily);
	bool operator==(const acng_addrinfo& other) const;
	operator mstring() const;
};

class CAddrInfo
{
	CAddrInfo() = default;
	// not to be copied ever
	CAddrInfo(const CAddrInfo&) = delete;
	CAddrInfo operator=(const CAddrInfo&) = delete;

	// resolution results (or hint for caching)
	std::string m_sError;
	time_t m_expTime = MAX_VAL(time_t);

	// first entry selected by protocol preferences, others alternating
	std::list<acng_addrinfo> m_orderedInfos;

	void Reset();
	static void clean_dns_cache();

public:
	typedef std::function<void(std::shared_ptr<CAddrInfo>)> tDnsResultReporter;

	// async. DNS resolution on IO thread. Reports result through the reporter.
	static void Resolve(cmstring & sHostname, cmstring &sPort, tDnsResultReporter);
	// like above but blocking resolution
	static std::shared_ptr<CAddrInfo> Resolve(cmstring & sHostname, cmstring &sPort);

	const decltype (m_orderedInfos) & getTargets() const { return m_orderedInfos; }

	const std::string& getError() const { return m_sError; }

	// iih, just for building of a special element regardsless of private ctor
	static std::shared_ptr<CAddrInfo> make_fatal_failure_hint();

	// C-style callback for the resolver
	static void cb_dns(void *arg,
					   int status,
					   int timeouts,
					   struct ares_addrinfo *results);

	CAddrInfo(const char *szErrorMessage) : m_sError(szErrorMessage) {}
};

typedef std::shared_ptr<CAddrInfo> CAddrInfoPtr;
extern LPCSTR sGenericErrorStatus;

}

#endif /*CADDRINFO_H_*/
