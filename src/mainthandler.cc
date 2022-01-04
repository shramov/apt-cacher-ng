#include "mainthandler.h"
#include "actemplates.h"
#include "expiration.h"
#include "pkgimport.h"
#include "showinfo.h"
#include "mirror.h"
#include "aclogger.h"
#include "filereader.h"
#include "acfg.h"
#include "acbuf.h"
#include "sockio.h"
#include "debug.h"
#include "ptitem.h"
#include "aclocal.h"
#include "aclock.h"

namespace acng
{
using namespace std;

mainthandler::mainthandler(tRunParms&& parms) :
		m_parms(move(parms))
{
	gettimeofday(&m_startTime, nullptr);
}

class authbounce : public mainthandler
{
public:
	using mainthandler::mainthandler;

	void Run() override
	{
		string_view msg = "Not Authorized. To start this action, an administrator password must be set and "
						  "you must be logged in.";
		item().ManualStart(403, "Access Forbidden", "text/plain", se, msg.size());
		Send(msg);
	}
};

class tAuthRequest : public mainthandler
{
public:
	using mainthandler::mainthandler;

	void Run() override
	{
		string_view msg = "Not Authorized. Please contact Apt-Cacher NG administrator for further questions.\n\n"
				   "For Admin: Check the AdminAuth option in one of the *.conf files in Apt-Cacher NG "
				   "configuration directory, probably " CFGDIR;
		item().ManualStart(401, "Not Authorized", "text/plain", "", msg.size());
		m_parms.owner->AddExtraHeaders("WWW-Authenticate: Basic realm=\"For login data, see AdminAuth in Apt-Cacher NG config files\"\r\n");
		Send(msg);
	}
};

#ifdef DEBUG
class sleeper : public mainthandler
{
public:
	using mainthandler::mainthandler;
	void Run() override
	{
		auto dpos = m_parms.cmd.find_first_of("0123456789");
		sleep(dpos == stmiss ? 100 : atoi(m_parms.cmd.data()+dpos));
		item().ManualStart(500, "DBG", "text/plain");
	}
};
#endif


namespace creators
{
#define CREAT(x) static mainthandler* x (mainthandler::tRunParms&& parms) { return new ::acng:: x (move(parms)); };
CREAT(aclocal);
CREAT(expiration);
CREAT(tShowInfo);
CREAT(tMaintOverview);
CREAT(tAuthRequest);
CREAT(authbounce);
CREAT(pkgimport);
CREAT(pkgmirror);
#ifdef DEBUG
CREAT(sleeper);
CREAT(tBgTester);
#endif
static mainthandler* deleter (mainthandler::tRunParms&& parms){ return new acng::tDeleter (move(parms), "Delet"); };
static mainthandler* truncator (mainthandler::tRunParms&& parms){ return new acng::tDeleter (move(parms), "Truncat"); };
}

array<tSpecialWorkDescription, (size_t) EWorkType::WORK_TYPE_MAX> workDescriptors;

const tSpecialWorkDescription& GetTaskInfo(EWorkType type)
{
	return AC_LIKELY(type < workDescriptors.max_size())
			? workDescriptors[type] : workDescriptors[0];
}

mainthandler* MakeMaintWorker(mainthandler::tRunParms&& parms)
{
	if (parms.type >= workDescriptors.size())
		parms.type = EWorkType::USER_INFO; // XXX: report as error in the log?
	if (workDescriptors[parms.type].creator)
		return workDescriptors[parms.type].creator(move(parms));
	return workDescriptors[EWorkType::USER_INFO].creator(move(parms));
}

void ACNG_API InitSpecialWorkDescriptors()
{
	workDescriptors[EWorkType::REGULAR] = {"UNKNOWNTYPE"sv, "Unknown Type"sv, se, nullptr, 0 }; // noop for regular jobs, also a dummy entry for wrong calls
	workDescriptors[EWorkType::LOCALITEM] = {"LOCALITEM"sv, "Local File Server"sv, se, &creators::aclocal, BLOCKING };
	workDescriptors[EWorkType::EXPIRE] = {"EXPIRE"sv, "Expiration"sv, "doExpire="sv, &creators::expiration, BLOCKING | FILE_BACKED | EXCLUSIVE };
	workDescriptors[EWorkType::EXP_LIST] = {"EXP_LIST"sv, "Expired Files Listing"sv, "justShow="sv, &creators::expiration, BLOCKING };
	workDescriptors[EWorkType::EXP_PURGE] = {"EXP_PURGE"sv, "Expired Files Purging"sv, "justRemove="sv, &creators::expiration, BLOCKING };
	workDescriptors[EWorkType::EXP_LIST_DAMAGED] = {"EXP_LIST_DAMAGED"sv, "Listing Damaged Files"sv, "justShowDamaged="sv, &creators::expiration, BLOCKING };
	workDescriptors[EWorkType::EXP_PURGE_DAMAGED] = {"EXP_PURGE_DAMAGED"sv, "Truncating Damaged Files"sv, "justRemoveDamaged="sv, &creators::expiration, BLOCKING };
	workDescriptors[EWorkType::EXP_TRUNC_DAMAGED] = {"EXP_TRUNC_DAMAGED"sv, "Truncating damaged files to zero size"sv, "justTruncDamaged="sv, &creators::expiration, BLOCKING };
	workDescriptors[EWorkType::USER_INFO] = {"USER_INFO"sv, "General Configuration Information"sv, se, &creators::tShowInfo, 0 };
	workDescriptors[EWorkType::TRACE_START] = {"TRACE_START"sv, "Status Report and Maintenance Tasks Overview"sv, "doTraceStart="sv, &creators::tMaintOverview, 0 };
	workDescriptors[EWorkType::TRACE_END] = {"TRACE_END"sv, "Status Report and Maintenance Tasks Overview"sv, "doTraceEnd="sv, &creators::tMaintOverview, 0 };
	workDescriptors[EWorkType::REPORT] = {"REPORT"sv, "Status Report and Maintenance Tasks Overview"sv, se, &creators::tMaintOverview, BLOCKING };
	workDescriptors[EWorkType::COUNT_STATS] = {"COUNT_STATS"sv, "Status Report With Statistics"sv, "doCount="sv, &creators::tMaintOverview, BLOCKING };
	workDescriptors[EWorkType::AUTH_REQ] = {"AUT_REQ"sv, "Authentication Required"sv, se, &creators::tAuthRequest, 0 };
	workDescriptors[EWorkType::AUTH_DENY] = {"AUTH_DENY"sv, "Authentication Denied"sv, se, &creators::authbounce, 0 };
	workDescriptors[EWorkType::IMPORT] = {"IMPORT"sv, "Data Import"sv, "doImport="sv, &creators::pkgimport, BLOCKING | EXCLUSIVE | FILE_BACKED };
	workDescriptors[EWorkType::MIRROR] = {"MIRROR"sv, "Archive Mirroring"sv, "doMirror="sv, &creators::pkgmirror, BLOCKING | EXCLUSIVE | FILE_BACKED};
	workDescriptors[EWorkType::DELETE] = {"DELETE"sv, "Manual File Deletion"sv, "doDeleteYes="sv, &creators::deleter, BLOCKING };
	workDescriptors[EWorkType::DELETE_CONFIRM] = {"DELETE_CONFIRM"sv, "Manual File Deletion (Confirmed)"sv, "doDelete="sv, &creators::deleter, BLOCKING };
	workDescriptors[EWorkType::TRUNCATE] = {"TRUNCATE"sv, "Manual File Truncation"sv, "doTruncateYes="sv, &creators::truncator, BLOCKING };
	workDescriptors[EWorkType::TRUNCATE_CONFIRM] = {"TRUNCATE_CONFIRM"sv, "Manual File Truncation (Confirmed)"sv, "doTruncate="sv, &creators::truncator, BLOCKING };
#ifdef DEBUG
	workDescriptors[EWorkType::DBG_SLEEPER] = {"DBG_SLEEPER"sv, "SpecialOperation"sv, "sleeper="sv, &creators::sleeper, BLOCKING };
	workDescriptors[EWorkType::DBG_BGSTREAM] = {"DBG_BGSTREAM"sv, "SpecialOperation"sv, "pingMe="sv, &creators::tBgTester, BLOCKING };
#endif
	workDescriptors[EWorkType::STYLESHEET] = {"STYLESHEET"sv, "SpecialOperation"sv, se, nullptr, 0};
	workDescriptors[EWorkType::FAVICON] = {"FAVICON"sv, "SpecialOperation"sv, se, nullptr, 0};

#ifdef DEBUG
	for(auto& p: workDescriptors)
	{
		if (p.flags & EXCLUSIVE)
		{
			ASSERT(p.flags & FILE_BACKED);
		}
	}
#endif
}

const string & mainthandler::GetMyHostPort()
{
	if(!m_sHostPort.empty())
		return m_sHostPort;

	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	char hbuf[NI_MAXHOST], pbuf[10];

	if (0 == getsockname(m_parms.fd,
						 (struct sockaddr*) &ss, &slen) &&
			0 == getnameinfo((struct sockaddr*) &ss, sizeof(ss),
							 hbuf, sizeof(hbuf),
							 pbuf, sizeof(pbuf),
							 NI_NUMERICHOST | NI_NUMERICSERV))
	{
		auto p = hbuf;
		bool bAddBrs(false);
		if (0 == strncmp(hbuf, "::ffff:", 7) && strpbrk(p, "0123456789."))
			p += 7; // no more colons there, looks like v4 IP in v6 space -> crop it
		else if (strchr(p, (int) ':'))
			bAddBrs = true; // full v6 address for sure, add brackets

		if (bAddBrs)
			m_sHostPort = se + '[' + p + ']';
		else
			m_sHostPort = p;
	}
	else
		m_sHostPort = "IP-of-this-cache-server"sv;

	m_sHostPort += ':';
	m_sHostPort += tPortFmter().fmt(cfg::port);
	return m_sHostPort;
}

tSS mainthandler::GetCacheKey()
{
	return tSS(50) << desc().typeName << "." << m_startTime.tv_sec << "." << m_startTime.tv_usec;
}

IMaintJobItem::IMaintJobItem(std::unique_ptr<mainthandler> &&han, IMaintJobItem *owner) :
	fileitem("_internal_task"),
	handler(std::move(han))
{
	handler->m_parms.owner = owner;
}

void IMaintJobItem::AddExtraHeaders(mstring appendix)
{
	if (! evabase::IsMainThread())
		return evabase::Post([this, appendix ]() { m_extraHeaders = move(appendix); });
	m_extraHeaders = move(appendix);
}

cmstring &IMaintJobItem::GetExtraResponseHeaders()
{
	ASSERT_IS_MAIN_THREAD;
	return m_extraHeaders;
}

void IMaintJobItem::Abandon()
{
	ASSERT_IS_MAIN_THREAD;

	if (handler && handler->m_bItemIsHot)
	{
		handler->m_bSigTaskAbort = true;
		if (AC_LIKELY(!handler->m_itemLock))
			handler->m_itemLock = as_lptr(this);
	}
	return fileitem::Abandon();
}

}
