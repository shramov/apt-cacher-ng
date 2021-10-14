#include "meta.h"

#include "maintenance.h"
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

using namespace std;

#define MAINT_HTML_DECO "maint.html"
static string cssString("style.css");

namespace acng {

tSpecialRequest::tSpecialRequest(tRunParms&& parms) :
		m_parms(move(parms))
{
}

tSpecialRequest::~tSpecialRequest()
{
}

class tAuthRequest : public tSpecialRequest
{
public:

	// XXX: c++11 using tSpecialRequest::tSpecialRequest;
	inline tAuthRequest(tSpecialRequest::tRunParms&& parms)
	: tSpecialRequest(move(parms)) {};

	void Run() override
	{
		const char authmsg[] = "HTTP/1.1 401 Not Authorized\r\nWWW-Authenticate: "
		"Basic realm=\"For login data, see AdminAuth in Apt-Cacher NG config files\"\r\n"
		"Connection: Close\r\n"
		"Content-Type: text/plain\r\nContent-Length:81\r\n\r\n"
		"Not Authorized. Please contact Apt-Cacher NG administrator for further questions.<br>"
		"<br>"
		"For Admin: Check the AdminAuth option in one of the *.conf files in Apt-Cacher NG "
		"configuration directory, probably " CFGDIR;
		SendRawData(authmsg, sizeof(authmsg)-1, 0);
	}
};

class authbounce : public tSpecialRequest
{
public:

	// XXX: c++11 using tSpecialRequest::tSpecialRequest;
	inline authbounce(tSpecialRequest::tRunParms&& parms)
	: tSpecialRequest(move(parms)) {};


	void Run() override
	{
		const char authmsg[] = "HTTP/1.1 200 Not Authorized\r\n"
		"Connection: Close\r\n"
		"Content-Type: text/plain\r\nContent-Length: 102\r\n\r\n"
		"Not Authorized. To start this action, an administrator password must be set and "
		"you must be logged in.";
		SendRawData(authmsg, sizeof(authmsg)-1, 0);
	}
};

class BackgroundThreadedItem : public fileitem
{
public:

	tSpecialRequest* MakeMaintWorker(tSpecialRequest::tRunParms&& parms)
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
			return new expiration(parms);
		case ESpecialWorkType::workUSERINFO:
			return new tShowInfo(parms);
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
	unique_ptr<tSpecialRequest> handler;

	condition_variable m_notifier;
	mutex m_mxNotifier;

	bufferevent* m_in_out[2];

	// also protected by mutex
	string m_header;

	BackgroundThreadedItem(ESpecialWorkType jobType, mstring cmd, bufferevent *bev)
		: fileitem("_internal_task")
		// XXX: resolve the name to task type for the logs? Or replace the name with something useful later? Although, not needed, and also w/ format not fitting the purpose.
	{
		m_status = FiStatus::FIST_DLPENDING;

		//zz_internalSubscribe([this]() { m_notifier.notify_all(); });

		try
		{
			handler.reset(MakeMaintWorker({jobType, move(cmd), bev, this}));
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

	const string &GetRawResponseHeader() override
	{
		unique_lock<mutex> lg(m_mxNotifier);
		// unlikely to loop here, but better be sure about data consistency
		while (m_status < FiStatus::FIST_DLGOTHEAD)
			m_notifier.wait(lg);

		return m_header;
	}


	std::unique_ptr<ICacheDataSender> GetCacheSender(off_t startPos) override;
};

void tSpecialRequest::SendChunkRemoteOnly(const char *data, size_t len)
{
	if(!data || !len)
		return;

#warning SEND TO STREAM
}

void tSpecialRequest::SendChunk(const char *data, size_t len)
{
	SendChunkRemoteOnly(data, len);
	SendChunkLocalOnly(data, len);
}

void tSpecialRequest::SetMimeResponseHeader(int statusCode, string_view statusMessage, string_view mimetype)
{
	lguard g(m_parms.owner->m_mxNotifier);
	m_parms.owner->m_contentType = mimetype;
	m_parms.owner->m_responseStatus.code = statusCode;
	m_parms.owner->m_responseStatus.msg = statusMessage;
	m_parms.owner->m_notifier.notify_all();
}

const string & tSpecialRequest::GetMyHostPort()
{
	if(!m_sHostPort.empty())
		return m_sHostPort;

	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	char hbuf[NI_MAXHOST], pbuf[10];

	if (0 == getsockname(m_parms.fd, (struct sockaddr*) &ss, &slen)
			&& 0 == getnameinfo((struct sockaddr*) &ss, sizeof(ss), hbuf, sizeof(hbuf), pbuf,
							sizeof(pbuf), NI_NUMERICHOST | NI_NUMERICSERV))
	{
		auto p = hbuf;
		bool bAddBrs(false);
		if (0 == strncmp(hbuf, "::ffff:", 7) && strpbrk(p, "0123456789."))
			p += 7; // no more colons there, looks like v4 IP in v6 space -> crop it
		else if (strchr(p, (int) ':'))
			bAddBrs = true; // full v6 address for sure, add brackets

		if (bAddBrs)
			m_sHostPort = sEmptyString + '[' + p + ']';
		else
			m_sHostPort = p;
	}
	else
		m_sHostPort = "IP-of-this-cache-server"sv;

	m_sHostPort += ':';
	m_sHostPort += tPortFmter().fmt(cfg::port);
	return m_sHostPort;
}

LPCSTR tSpecialRequest::GetTaskName(ESpecialWorkType type)
{
	switch(type)
	{
	case ESpecialWorkType::workTypeDetect: return "SpecialOperation";
	case ESpecialWorkType::workExExpire: return "Expiration";
	case ESpecialWorkType::workExList: return "Expired Files Listing";
	case ESpecialWorkType::workExPurge: return "Expired Files Purging";
	case ESpecialWorkType::workExListDamaged: return "Listing Damaged Files";
	case ESpecialWorkType::workExPurgeDamaged: return "Truncating Damaged Files";
	case ESpecialWorkType::workExTruncDamaged: return "Truncating damaged files to zero size";
	//case ESpecialWorkType::workRAWDUMP: /*fall-through*/
	//case ESpecialWorkType::workBGTEST: return "42";
	case ESpecialWorkType::workUSERINFO: return "General Configuration Information";
	case ESpecialWorkType::workTraceStart:
	case ESpecialWorkType::workTraceEnd:
	case ESpecialWorkType::workMAINTREPORT: return "Status Report and Maintenance Tasks Overview";
	case ESpecialWorkType::workAUTHREQUEST: return "Authentication Required";
	case ESpecialWorkType::workAUTHREJECT: return "Authentication Denied";
	case ESpecialWorkType::workIMPORT: return "Data Import";
	case ESpecialWorkType::workMIRROR: return "Archive Mirroring";
	case ESpecialWorkType::workDELETE: return "Manual File Deletion";
	case ESpecialWorkType::workDELETECONFIRM: return "Manual File Deletion (Confirmed)";
	case ESpecialWorkType::workTRUNCATE: return "Manual File Truncation";
	case ESpecialWorkType::workTRUNCATECONFIRM: return "Manual File Truncation (Confirmed)";
	case ESpecialWorkType::workCOUNTSTATS: return "Status Report With Statistics";
	case ESpecialWorkType::workSTYLESHEET: return "CSS";
	// case ESpecialWorkType::workJStats: return "Stats";
	}
	return "UnknownTask";
}

ESpecialWorkType GuessWorkType(cmstring& cmd, const char* auth)
{
	LOGSTARTs("DispatchMaintWork");
	LOG("cmd: " << cmd);

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

fileitem* tSpecialRequest::Create(ESpecialWorkType wType, const tHttpUrl& url, cmstring& refinedPath, const header& reqHead)
{
	ESpecialWorkType xt;

	if (wType == ESpecialWorkType::workTypeDetect)
	{
		xt = url.sHost == "style.css" ?
					ESpecialWorkType::workSTYLESHEET :
					GuessWorkType(refinedPath, reqHead.h[header::AUTHORIZATION]);
	}

	if (wType == ESpecialWorkType::workTypeDetect)
		return nullptr; // not for us?

	auto handler = MakeMaintWorker();
}



}
