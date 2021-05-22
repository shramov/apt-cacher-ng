#include "meta.h"

#include <deque>
#include <memory>
#include <list>
#include <unordered_map>
#include <future>

#include "caddrinfo.h"
#include "sockio.h"
#include "acfg.h"
#include "debug.h"
#include "lockable.h"
#include "evabase.h"

#include <ares.h>

using namespace std;

/**
 * The CAddrInfo has multiple jobs:
 * - cache the resolution results for a specific time span
 * - in case of parallel ongoing resolution requests, coordinate the wishes so that the result is reused (and all callers are notified)
 */

namespace acng
{
static const unsigned DNS_CACHE_MAX = 255;
static const unsigned DNS_ERROR_KEEP_MAX_TIME = 10;
static const unsigned MAX_ADDR = 10;
static const string dns_error_status_prefix("DNS error, ");
#define MOVE_FRONT_THERE_TO_BACK_HERE(from, to) to.emplace_back(from.front()), from.pop_front()

std::string acng_addrinfo::formatIpPort(const sockaddr *pAddr, socklen_t addrLen, int ipFamily)
{
	char buf[300], pbuf[30];
	getnameinfo(pAddr, addrLen, buf, sizeof(buf), pbuf, sizeof(pbuf),
			NI_NUMERICHOST | NI_NUMERICSERV);
	return string(ipFamily == PF_INET6 ? "[" : "") +
			buf +
			(ipFamily == PF_INET6 ? "]" : "") +
			":" + pbuf;
}

bool acng_addrinfo::operator==(const acng_addrinfo &other) const
{
	return this == &other ||
			(other.ai_addrlen == ai_addrlen &&
			 other.ai_family == ai_family &&
			 memcmp(&ai_addr, &other.ai_addr, ai_addrlen) == 0);
}

acng::acng_addrinfo::operator mstring() const {
	return formatIpPort((const sockaddr *) &ai_addr, ai_addrlen, ai_family);
}

static string make_dns_key(const string & sHostname, const string &sPort)
{
	return sHostname + ":" + sPort;
}

// using non-ordered map because of iterator stability, needed for expiration queue;
// something like boost multiindex would be more appropriate if there was complicated
// ordering on expiration date but that's not the case; OTOH could also use a multi-key
// index instead of g_active_resolver_index but then again, when the resolution is finished,
// that key data becomes worthless.
map<string,CAddrInfoPtr> dns_cache;
deque<decltype(dns_cache)::iterator> dns_exp_q;

// descriptor of a running DNS lookup, passed around with libevent callbacks
struct tDnsResContext
{
	string sHost, sPort;
	std::shared_ptr<CDnsBase> resolver;
	list<CAddrInfo::tDnsResultReporter> cbs;
};
unordered_map<string,tDnsResContext*> g_active_resolver_index;

// this shall remain global and forever, for last-resort notifications
LPCSTR sGenericErrorStatus = "Fatal system error within apt-cacher-ng processing";
auto fail_hint = make_shared<CAddrInfo>(sGenericErrorStatus);


/**
 * Trash old entries and keep purging until there is enough space for at least one new entry.
 */
void CAddrInfo::clean_dns_cache()
{
	if(cfg::dnscachetime <= 0)
		return;
	auto now=GetTime();
	ASSERT(dns_cache.size() == dns_exp_q.size());
	while(dns_cache.size() >= DNS_CACHE_MAX-1
			|| (!dns_exp_q.empty() && dns_exp_q.front()->second->m_expTime >= now))
	{
		dns_cache.erase(dns_exp_q.front());
		dns_exp_q.pop_front();
	}
}

void CAddrInfo::cb_dns(void *arg,
					   int status,
					   int /* timeouts */,
					   struct ares_addrinfo *results)
{
	// take ownership
	unique_ptr<tDnsResContext> args((tDnsResContext*)arg);
	arg = nullptr;
	try
	{
		g_active_resolver_index.erase(make_dns_key(args->sHost, args->sPort));
		auto ret = std::shared_ptr<CAddrInfo>(new CAddrInfo);
		tDtorEx invoke_cbs([&args, &ret, results]() {
			for(auto& it: args->cbs)
				it(ret);
			if (results)
				ares_freeaddrinfo(results);
		});

		switch (status)
		{
		case ARES_SUCCESS:
			break;
		case ARES_ENOTIMP:
			ret->m_expTime = 0; // expire this ASAP and retry
			ret->m_sError = "Unsupported address family";
			return;
		case ARES_ENOTFOUND:
			ret->m_expTime = 0; // expire this ASAP and retry
			ret->m_sError = "Host not found";
			return;
		case ARES_ECANCELLED:
		case ARES_EDESTRUCTION:
			ret->m_expTime = 0; // expire this ASAP and retry
			ret->m_sError = "Temporary DNS resolution error";
			return;
		case ARES_ENOMEM: // similar but cache it for some time so things might improve
			ret->m_expTime = GetTime() + std::min(cfg::dnscachetime, (int) DNS_ERROR_KEEP_MAX_TIME);
			ret->m_sError = "Out of memory";
			break;
		default:
			ret->m_expTime = GetTime() + std::min(cfg::dnscachetime, (int) DNS_ERROR_KEEP_MAX_TIME);
			ret->m_sError = tErrnoFmter(status);
			return;
		}
#ifdef DEBUG
		for (auto p = results->nodes; p; p = p->ai_next)
			DBGQLOG("Resolved: " << acng_addrinfo::formatIpPort(p->ai_addr, p->ai_addrlen, p->ai_family));
#endif
		auto out = ret->m_orderedInfos;
		auto takeV4 = cfg::conprotos[0] == PF_INET || cfg::conprotos[0] == PF_UNSPEC
					  || cfg::conprotos[1] == PF_INET || cfg::conprotos[1] == PF_UNSPEC;
		auto takeV6 = cfg::conprotos[0] == PF_INET6 || cfg::conprotos[0] == PF_UNSPEC
					  || cfg::conprotos[1] == PF_INET6 || cfg::conprotos[1] == PF_UNSPEC;

		// strange things, something (localhost) goes resolved twice for each family, however there is apparently subtle difference in the ai_addr bits (padding issues in ares?)
		std::deque<acng_addrinfo> dedup, q4, q6;

		for (auto pCur = results->nodes; pCur; pCur = pCur->ai_next)
		{
			if (pCur->ai_socktype != SOCK_STREAM || pCur->ai_protocol != IPPROTO_TCP)
				continue;

			acng_addrinfo svAddr(pCur);
			auto itExist = find(dedup.begin(), dedup.end(), svAddr);
			if (itExist != dedup.end())
				continue;
			dedup.emplace_back(svAddr);
		}
#ifdef DEBUG
		for (auto& it: ret->m_orderedInfos)
			DBGQLOG("Refined: " << it);
#endif
		while (!dedup.empty() && (q4.size() + q6.size()) < MAX_ADDR)
		{
			if(takeV4 && dedup.front().ai_family == PF_INET)
				MOVE_FRONT_THERE_TO_BACK_HERE(dedup, q4);
			else if (takeV6 && dedup.front().ai_family == PF_INET6)
				MOVE_FRONT_THERE_TO_BACK_HERE(dedup, q6);
			else
				dedup.pop_front();
		}

		for(bool sel6 = cfg::conprotos[0] == PF_INET6 || cfg::conprotos[0] == PF_UNSPEC;
			q4.size() + q6.size() > 0;
			sel6 = !sel6)
		{
			if(sel6 && !q6.empty())
				MOVE_FRONT_THERE_TO_BACK_HERE(q6, ret->m_orderedInfos);
			else if (!sel6 && !q4.empty())
				MOVE_FRONT_THERE_TO_BACK_HERE(q4, ret->m_orderedInfos);
		}
		ASSERT(q4.empty() && q6.empty());

		if (!ret->m_orderedInfos.empty())
			ret->m_expTime = GetTime() + cfg::dnscachetime;
		else
		{
			// nothing found? Report a common error then.
			ret->m_expTime = GetTime() + std::min(cfg::dnscachetime, (int) DNS_ERROR_KEEP_MAX_TIME);
			ret->m_sError = dns_error_status_prefix + "system error";
		}

		if (cfg::dnscachetime > 0) // keep a copy for other users
		{
			clean_dns_cache();
			auto newIt = dns_cache.emplace(make_dns_key(args->sHost, args->sPort), ret);
			dns_exp_q.push_back(newIt.first);
		}
	}
	catch(...)
	{
		// nothing above should actually throw, but if it does, make sure to not keep wild pointers
		g_active_resolver_index.clear();
	}
}

SHARED_PTR<CAddrInfo> CAddrInfo::Resolve(cmstring & sHostname, cmstring &sPort)
{
	promise<CAddrInfoPtr> reppro;
	auto reporter = [&reppro](CAddrInfoPtr result) { reppro.set_value(result); };
	Resolve(sHostname, sPort, move(reporter));
	auto res(reppro.get_future().get());
	return res ? res : fail_hint;
}
void CAddrInfo::Resolve(cmstring & sHostname, cmstring &sPort, tDnsResultReporter rep)
{
	auto temp_ctx = new tDnsResContext {
		sHostname,
		sPort,
		std::shared_ptr<CDnsBase>(),
		list<CAddrInfo::tDnsResultReporter> {move(rep)}
	};
	// keep a reference on the dns base to extend its lifetime
	auto cb_invoke_dns_res = [temp_ctx](bool canceled)
	{
		evabase::GetDnsBase()->sync();

		auto args = unique_ptr<tDnsResContext>(temp_ctx); //temporarily owned here
		if(!args || args->cbs.empty() || !(args->cbs.front())) return; // heh?
		LOGSTARTFUNCsx(temp_ctx->sHost);

		if (AC_UNLIKELY(canceled || evabase::in_shutdown))
		{
			args->cbs.front()(make_shared<CAddrInfo>(
					"system error"));
			return;
		}

		auto key=make_dns_key(args->sHost, args->sPort);
		if(cfg::dnscachetime > 0)
		{
			auto caIt = dns_cache.find(key);
			if(caIt != dns_cache.end())
			{
				args->cbs.front()(caIt->second);
				return;
			}
		}
		auto resIt = g_active_resolver_index.find(key);
		// join the waiting crowd, move all callbacks to there...
		if(resIt != g_active_resolver_index.end())
		{
			resIt->second->cbs.splice(resIt->second->cbs.end(), args->cbs);
			return;
		}

		// ok, this is fresh, invoke a completely new DNS lookup operation
		temp_ctx->resolver = evabase::GetDnsBase();
		if (AC_UNLIKELY(!temp_ctx->resolver || !temp_ctx->resolver->get()))
		{
			args->cbs.front()(make_shared<CAddrInfo>("503 Bad DNS configuration"));
			return;
		}

		g_active_resolver_index[key] = args.get();
		static bool filter_specific = (cfg::conprotos[0] != PF_UNSPEC && cfg::conprotos[1] == PF_UNSPEC);

		static const ares_addrinfo_hints default_connect_hints =
		{
			// we provide plain port numbers, no resolution needed
			// also return only probably working addresses
			AI_NUMERICSERV | AI_ADDRCONFIG,
			filter_specific ? cfg::conprotos[0] : PF_UNSPEC,
			SOCK_STREAM, IPPROTO_TCP
		};
		auto pRaw = args.release(); // to be owned by the operation
		ares_getaddrinfo(temp_ctx->resolver->get(),
						 pRaw->sHost.empty() ? nullptr : pRaw->sHost.c_str(),
						 pRaw->sPort.empty() ? nullptr : pRaw->sPort.c_str(),
						 &default_connect_hints, CAddrInfo::cb_dns, pRaw);

		evabase::GetDnsBase()->sync();
	};

	evabase::Post(move(cb_invoke_dns_res));
}

void RejectPendingDnsRequests()
{
	for (auto& el: g_active_resolver_index)
	{
		if (!el.second)
			continue;

		for (const auto& action: el.second->cbs)
		{
			action(make_shared<CAddrInfo>("System shutting down"));
		}
		el.second->cbs.clear();
	}
}

acng_addrinfo::acng_addrinfo(ares_addrinfo_node *src)
	: ai_family(src->ai_family), ai_addrlen(src->ai_addrlen)
{
	memcpy(&ai_addr, src->ai_addr, ai_addrlen);
}

}
