#include "meta.h"

#include "mainthandler.h"
#include "expiration.h"
#include "pkgimport.h"
#include "showinfo.h"
#include "mirror.h"
#include "aclogger.h"
#include "filereader.h"
#include "acfg.h"
#include "acbuf.h"
#include "sockio.h"
#include "caddrinfo.h"
#include "portutils.h"
#include "debug.h"
#include "ptitem.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <signal.h>

#include "aevutil.h"

using namespace std;

#define MAINT_HTML_DECO "maint.html"
static string cssString("style.css");

namespace acng {

tSpecialRequestHandler::tSpecialRequestHandler(tRunParms&& parms) :
		m_parms(move(parms))
{
}

tSpecialRequestHandler::~tSpecialRequestHandler()
{
}

class tAuthRequest : public tSpecialRequestHandler
{
public:
	using tSpecialRequestHandler::tSpecialRequestHandler;

	void Run() override
	{
		string_view msg = "Not Authorized. Please contact Apt-Cacher NG administrator for further questions.\n\n"
				   "For Admin: Check the AdminAuth option in one of the *.conf files in Apt-Cacher NG "
				   "configuration directory, probably " CFGDIR;
		m_parms.output.ConfigHeader(401, "Not Authorized"sv, "text/plain"sv, "", msg.size());
		m_parms.output.AddExtraHeaders("WWW-Authenticate: Basic realm=\"For login data, see AdminAuth in Apt-Cacher NG config files\"\r\n");
		SendChunkRemoteOnly(msg);
	}
};

class authbounce : public tSpecialRequestHandler
{
public:
	using tSpecialRequestHandler::tSpecialRequestHandler;

	void Run() override
	{
		string_view msg = "Not Authorized. To start this action, an administrator password must be set and "
						  "you must be logged in.";
		m_parms.output.ConfigHeader(403, "Access Forbidden", "text/plain"sv, ""sv, msg.size());
		SendChunkRemoteOnly(msg);
	}
};

static tSpecialRequestHandler* MakeMaintWorker(tSpecialRequestHandler::tRunParms&& parms)
{
	if (cfg::DegradedMode() && parms.type != ESpecialWorkType::workSTYLESHEET)
		parms.type = ESpecialWorkType::workUSERINFO;

	switch (parms.type)
	{
	case ESpecialWorkType::workTypeDetect:
		return nullptr;
	case ESpecialWorkType::workExExpire:
	case ESpecialWorkType::workExList:
	case ESpecialWorkType::workExPurge:
	case ESpecialWorkType::workExListDamaged:
	case ESpecialWorkType::workExPurgeDamaged:
	case ESpecialWorkType::workExTruncDamaged:
		return new expiration(move(parms));
	case ESpecialWorkType::workUSERINFO:
		return new tShowInfo(move(parms));
	case ESpecialWorkType::workMAINTREPORT:
	case ESpecialWorkType::workCOUNTSTATS:
	case ESpecialWorkType::workTraceStart:
	case ESpecialWorkType::workTraceEnd:
		return new tMaintPage(move(parms));
	case ESpecialWorkType::workAUTHREQUEST:
		return new tAuthRequest(move(parms));
	case ESpecialWorkType::workAUTHREJECT:
		return new authbounce(move(parms));
	case ESpecialWorkType::workIMPORT:
		return new pkgimport(move(parms));
	case ESpecialWorkType::workMIRROR:
		return new pkgmirror(move(parms));
	case ESpecialWorkType::workDELETE:
	case ESpecialWorkType::workDELETECONFIRM:
		return new tDeleter(move(parms), "Delet");
	case ESpecialWorkType::workTRUNCATE:
	case ESpecialWorkType::workTRUNCATECONFIRM:
		return new tDeleter(move(parms), "Truncat");
	case ESpecialWorkType::workSTYLESHEET:
		return new tStyleCss(move(parms));
#if 0
	case workJStats:
		return new jsonstats(parms);
#endif
	}
	return nullptr;
}

class BufferedPtItem : public BufferedPtItemBase
{
	unique_ptr<tSpecialRequestHandler> handler;

public:

	tSpecialRequestHandler* GetHandler() { return handler.get(); }

	mutex m_memberMutex;

	bufferevent* m_in_out[2];

	BufferedPtItem(ESpecialWorkType jobType, mstring cmd, bufferevent *bev)
		: BufferedPtItemBase("_internal_task")
		// XXX: resolve the name to task type for the logs? Or replace the name with something useful later? Although, not needed, and also w/ format not fitting the purpose.
	{
		ASSERT_HAVE_MAIN_THREAD;

		m_status = FiStatus::FIST_DLPENDING;

		try
		{
			handler.reset(MakeMaintWorker({jobType, move(cmd), bufferevent_getfd(bev), *this}));
			if (handler)
			{
				m_status = FiStatus::FIST_DLASSIGNED;
				m_responseStatus = { 200, "OK" };
			}
			else
			{
				m_status = FiStatus::FIST_DLERROR;
				m_responseStatus = { 500, "Internal processing error" };
			}
		}
		catch(const std::exception& e)
		{
			m_status = FiStatus::FIST_DLERROR;
			m_responseStatus = { 500, e.what() };
		}
		catch(...)
		{
			m_status = FiStatus::FIST_DLERROR;
			m_responseStatus = { 500, "Unable to start background items" };
		}
	}
	FiStatus Setup() override
	{
		return m_status;
	}

	mstring m_extraHeaders;

	BufferedPtItemBase& ConfigHeader(int statusCode, string_view statusMessage, string_view mimetype, string_view originOrRedirect, off_t contLen)
	override
	{
		ASSERT(!statusMessage.empty());
		ASSERT(m_status < FIST_COMPLETE);
		auto q = [pin = as_lptr(this), statusCode, statusMessage, mimetype, originOrRedirect, contLen]()
		{
			pin->m_responseStatus = {statusCode, string(statusMessage)};
			if (!mimetype.empty())
				pin->m_contentType = mimetype;
			pin->m_responseOrigin = originOrRedirect;
			if (contLen >= 0)
				pin->m_nContentLength = contLen;
			if (pin->m_status < FIST_DLGOTHEAD)
				pin->m_status = FIST_DLGOTHEAD;
		};
		if (evabase::IsMainThread())
			q();
		else
			evabase::Post(q);

		return *this;
	}
	BufferedPtItemBase& AddExtraHeaders(mstring appendix) override
	{
		ASSERT_HAVE_MAIN_THREAD;

		m_extraHeaders = move(appendix);
		return *this;
	}
	//std::unique_ptr<ICacheDataSender> GetCacheSender(off_t startPos) override;

	// fileitem interface
public:
	std::unique_ptr<ICacheDataSender> GetCacheSender(off_t startPos) override;
	const string &GetExtraResponseHeaders() override
	{
		return m_extraHeaders;
	}
};

void tSpecialRequestHandler::SendChunkRemoteOnly(string_view sv)
{
	if(!sv.data() || sv.empty())
		return;
	evbuffer_add(PipeIn(), sv);
}

void tSpecialRequestHandler::SendChunkRemoteOnly(beview &data)
{
	evbuffer_add_buffer(PipeIn(), data.be);
}

const string & tSpecialRequestHandler::GetMyHostPort()
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

string_view GetTaskName(ESpecialWorkType type)
{
	switch(type)
	{
	case ESpecialWorkType::workTypeDetect: return "SpecialOperation"sv;
	case ESpecialWorkType::workExExpire: return "Expiration"sv;
	case ESpecialWorkType::workExList: return "Expired Files Listing"sv;
	case ESpecialWorkType::workExPurge: return "Expired Files Purging"sv;
	case ESpecialWorkType::workExListDamaged: return "Listing Damaged Files"sv;
	case ESpecialWorkType::workExPurgeDamaged: return "Truncating Damaged Files"sv;
	case ESpecialWorkType::workExTruncDamaged: return "Truncating damaged files to zero size"sv;
	//case ESpecialWorkType::workRAWDUMP: /*fall-through*/
	//case ESpecialWorkType::workBGTEST: return "42";
	case ESpecialWorkType::workUSERINFO: return "General Configuration Information"sv;
	case ESpecialWorkType::workTraceStart:
	case ESpecialWorkType::workTraceEnd:
	case ESpecialWorkType::workMAINTREPORT: return "Status Report and Maintenance Tasks Overview"sv;
	case ESpecialWorkType::workAUTHREQUEST: return "Authentication Required"sv;
	case ESpecialWorkType::workAUTHREJECT: return "Authentication Denied"sv;
	case ESpecialWorkType::workIMPORT: return "Data Import"sv;
	case ESpecialWorkType::workMIRROR: return "Archive Mirroring"sv;
	case ESpecialWorkType::workDELETE: return "Manual File Deletion"sv;
	case ESpecialWorkType::workDELETECONFIRM: return "Manual File Deletion (Confirmed)"sv;
	case ESpecialWorkType::workTRUNCATE: return "Manual File Truncation"sv;
	case ESpecialWorkType::workTRUNCATECONFIRM: return "Manual File Truncation (Confirmed)"sv;
	case ESpecialWorkType::workCOUNTSTATS: return "Status Report With Statistics"sv;
	case ESpecialWorkType::workSTYLESHEET: return "CSS"sv;
	// case ESpecialWorkType::workJStats: return "Stats";
	}
	return "UnknownTask"sv;
}

ESpecialWorkType DetectWorkType(cmstring& cmd, cmstring& sHost, const char* auth)
{
	LOGSTARTs("DispatchMaintWork");
	LOG("cmd: " << cmd);

	if (sHost == "style.css")
		return ESpecialWorkType::workSTYLESHEET;

#if 0 // defined(DEBUG)
	if(cmd.find("tickTack")!=stmiss)
	{
		tBgTester(conFD).Run(cmd);
		return;
	}
#endif

	auto epos=cmd.find('?');
	if(epos == stmiss)
		epos=cmd.length();
	auto spos=cmd.find_first_not_of('/');
	auto wlen=epos-spos;

	// this can also hide in the URL!
	if(wlen==cssString.length() && 0 == (cmd.compare(spos, wlen, cssString)))
		return ESpecialWorkType::workSTYLESHEET;

	// not starting like the maint page?
	if(cmd.compare(spos, wlen, cfg::reportpage))
		return ESpecialWorkType::workTypeDetect;

	// ok, filename identical, also the end, or having a parameter string?
	if(epos == cmd.length())
		return ESpecialWorkType::workMAINTREPORT;

	// not smaller, was already compared, can be only longer, means having parameters,
	// means needs authorization

	// all of the following need authorization if configured, enforce it
	switch(cfg::CheckAdminAuth(auth))
	{
     case 0:
#ifdef HAVE_CHECKSUM
        break; // auth is ok or no passwort is set
#else
        // most data modifying tasks cannot be run safely without checksumming support 
		return ESpecialWorkType::workAUTHREJECT;
#endif
	 case 1: return ESpecialWorkType::workAUTHREQUEST;
	 default: return ESpecialWorkType::workAUTHREJECT;
	}

	struct { LPCSTR trigger; ESpecialWorkType type; } matches [] =
	{
			{"doExpire=", ESpecialWorkType::workExExpire},
			{"justShow=", ESpecialWorkType::workExList},
			{"justRemove=", ESpecialWorkType::workExPurge},
			{"justShowDamaged=", ESpecialWorkType::workExListDamaged},
			{"justRemoveDamaged=", ESpecialWorkType::workExPurgeDamaged},
			{"justTruncDamaged=", ESpecialWorkType::workExTruncDamaged},
			{"doImport=", ESpecialWorkType::workIMPORT},
			{"doMirror=", ESpecialWorkType::workMIRROR},
			{"doDelete=", ESpecialWorkType::workDELETECONFIRM},
			{"doDeleteYes=", ESpecialWorkType::workDELETE},
			{"doTruncate=", ESpecialWorkType::workTRUNCATECONFIRM},
			{"doTruncateYes=", ESpecialWorkType::workTRUNCATE},
			{"doCount=", ESpecialWorkType::workCOUNTSTATS},
			{"doTraceStart=", ESpecialWorkType::workTraceStart},
			{"doTraceEnd=", ESpecialWorkType::workTraceEnd},
//			{"doJStats", workJStats}
	};

#warning check perfromance, might be inefficient, maybe use precompiled regex for do* and just* or at least separate into two groups
	for(auto& needle: matches)
		if(StrHasFrom(cmd, needle.trigger, epos))
			return needle.type;

	// something weird, go to the maint page
	return ESpecialWorkType::workMAINTREPORT;
}

tFileItemPtr Create(ESpecialWorkType jobType, bufferevent *bev, const tHttpUrl& url, cmstring& refinedPath, const header& reqHead)
{
	ESpecialWorkType xt;

	try
	{
		if (jobType == ESpecialWorkType::workTypeDetect)
			return tFileItemPtr(); // not for us?

		auto item = new BufferedPtItem(jobType, refinedPath, bev);
		auto ret = as_lptr<fileitem>(item);
		if (! item->GetHandler())
			return tFileItemPtr();

		auto runner = [item, ret] ()
		{
			try
			{
				item->GetHandler()->Run();
				evabase::Post([item, ret]()
				{
					item->Finish();
				});
			}
			catch (...)
			{

			}
		};
		if (han->IsNonBlocking())
		{
			runner();
		}
		else
		{
			if(!tpool->runBackgrounded(runner))
			{
				delete ret;
				return nullptr;
			}
		}
	}  catch (...) {
		return nullptr;

	}
}


}
