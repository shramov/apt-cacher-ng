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
#include "aclocal.h"
#include "tpool.h"

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
		m_parms.output.ManualStart(401, "Not Authorized", "text/plain", "", msg.size());
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
		m_parms.output.ManualStart(403, "Access Forbidden", "text/plain", se, msg.size());
		SendChunkRemoteOnly(msg);
	}
};

static tSpecialRequestHandler* MakeMaintWorker(tSpecialRequestHandler::tRunParms&& parms)
{
	if (cfg::DegradedMode() && parms.type != EWorkType::STYLESHEET)
		parms.type = EWorkType::USER_INFO;

	switch (parms.type)
	{
	case EWorkType::UNKNOWN:
	case EWorkType::REGULAR:
		return nullptr;
	case EWorkType::LOCALITEM:
		return new aclocal(move(parms));
	case EWorkType::EXPIRE:
	case EWorkType::EXP_LIST:
	case EWorkType::EXP_PURGE:
	case EWorkType::EXP_LIST_DAMAGED:
	case EWorkType::EXP_PURGE_DAMAGED:
	case EWorkType::EXP_TRUNC_DAMAGED:
		return new expiration(move(parms));
	case EWorkType::USER_INFO:
		return new tShowInfo(move(parms));
	case EWorkType::REPORT:
	case EWorkType::COUNT_STATS:
	case EWorkType::TRACE_START:
	case EWorkType::TRACE_END:
		return new tMaintPage(move(parms));
	case EWorkType::AUT_REQ:
		return new tAuthRequest(move(parms));
	case EWorkType::AUTH_DENY:
		return new authbounce(move(parms));
	case EWorkType::IMPORT:
		return new pkgimport(move(parms));
	case EWorkType::MIRROR:
		return new pkgmirror(move(parms));
	case EWorkType::DELETE:
	case EWorkType::DELETE_CONFIRM:
		return new tDeleter(move(parms), "Delet");
	case EWorkType::TRUNCATE:
	case EWorkType::TRUNCATE_CONFIRM:
		return new tDeleter(move(parms), "Truncat");
	case EWorkType::STYLESHEET:
		return new tStyleCss(move(parms));
#if 0
	case workJStats:
		return new jsonstats(parms);
#endif
	}
	return nullptr;
}

void cb_notify_new_pipe_data(struct bufferevent *bev, void *ctx);
void cb_bgpipe_event(struct bufferevent *bev, short what, void *ctx);

class BufferedPtItem : public BufferedPtItemBase
{
	unique_ptr<tSpecialRequestHandler> handler;

public:

	tSpecialRequestHandler* GetHandler() { return handler.get(); }

	// where the cursor is, matches the begin of the current buffer
	off_t m_nCursor = 0;

	~BufferedPtItem()
	{
		ASSERT_HAVE_MAIN_THREAD;
		if (m_pipeInOut[0]) bufferevent_free(m_pipeInOut[0]);
		if (m_pipeInOut[1]) bufferevent_free(m_pipeInOut[1]);
	}

	BufferedPtItem(EWorkType jobType, mstring cmd, bufferevent *bev, SomeData* arg)
		: BufferedPtItemBase("_internal_task")
		// XXX: resolve the name to task type for the logs? Or replace the name with something useful later? Although, not needed, and also w/ format not fitting the purpose.
	{
		ASSERT_HAVE_MAIN_THREAD;

		m_status = FiStatus::FIST_DLPENDING;

		try
		{
			handler.reset(MakeMaintWorker({jobType, move(cmd), bufferevent_getfd(bev), *this, arg}));
			if (handler)
			{
				auto flags =  (handler->m_bNeedsBgThread * BEV_OPT_THREADSAFE)
							  | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS;
				if (AC_UNLIKELY(bufferevent_pair_new(evabase::base, flags, m_pipeInOut)))
				{
					throw std::bad_alloc();
				}
				// trigger of passed data notification
				bufferevent_setcb(m_pipeInOut[1], cb_notify_new_pipe_data, nullptr, cb_bgpipe_event, this);
				bufferevent_enable(m_pipeInOut[1], EV_READ);
				bufferevent_enable(m_pipeInOut[0], EV_WRITE);

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

	void AddExtraHeaders(mstring appendix) override
	{
		if (! evabase::IsMainThread())
			return evabase::Post([this, appendix ]() { m_extraHeaders = move(appendix); });
		m_extraHeaders = move(appendix);
	}
	void Eof()
	{
		bufferevent_flush(m_pipeInOut[0], EV_WRITE, BEV_FINISHED);
	}

	void BgPipeEvent(short what)
	{
		LOGSTARTFUNCs;
		ldbg(what);
		if (what & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT | BEV_EVENT_EOF))
		{
			Finish();
		}
	}

	void GotNewData()
	{
		ASSERT_HAVE_MAIN_THREAD;
		LOGSTARTFUNC;
		auto len = evbuffer_get_length(PipeRx());
		m_nSizeChecked = m_nCursor + len;
		ldbg(len << " -> " << m_nSizeChecked);
		if (m_status == FIST_DLGOTHEAD)
			m_status = FIST_DLRECEIVING;
		NotifyObservers();
	}

	std::unique_ptr<ICacheDataSender> GetCacheSender() override;

	const string &GetExtraResponseHeaders() override
	{
		ASSERT_HAVE_MAIN_THREAD;
		return m_extraHeaders;
	}
private:
	/**
	 * @brief Finish should not be called directly, only through the Pipe EOF callback!
	 */
	void Finish()
	{
		ASSERT_HAVE_MAIN_THREAD;
		LOGSTARTFUNC;

		if (m_status < FIST_DLGOTHEAD)
		{
			ldbg("shall not finish b4 start");
			evabase::Post([pin = as_lptr(this)](){ pin->Finish(); });
			return;
		}

		if (m_status < FiStatus::FIST_COMPLETE)
			m_status = FiStatus::FIST_COMPLETE;
		if (m_nSizeChecked < 0)
			m_nSizeChecked = 0;
		if (m_nContentLength < 0)
			m_nContentLength = m_nSizeChecked;
		if (m_responseStatus.code < 200)
			m_responseStatus.code = 200;
		if (m_responseStatus.msg.empty())
			m_responseStatus.msg = "ACNG internal error"sv;
		ldbg( "fist: " << m_status << ", goodsize: " << m_nSizeChecked);
		NotifyObservers();
	}
};

void tSpecialRequestHandler::SendChunkRemoteOnly(string_view sv)
{
	// push everything into the pipe, the output will make notifications as needed

	if(!sv.data() || sv.empty())
		return;
	send(PipeTx(), sv);
}

void tSpecialRequestHandler::SendChunkRemoteOnly(evbuffer* data)
{
	// push everything into the pipe, the output will make notifications as needed
	evbuffer_add_buffer(PipeTx(), data);
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

string_view GetTaskName(EWorkType type)
{
	switch(type)
	{
	case EWorkType::UNKNOWN: return "SpecialOperation"sv;
	case EWorkType::LOCALITEM: return "Local File Server"sv;
	case EWorkType::REGULAR: return se;
	case EWorkType::EXPIRE: return "Expiration"sv;
	case EWorkType::EXP_LIST: return "Expired Files Listing"sv;
	case EWorkType::EXP_PURGE: return "Expired Files Purging"sv;
	case EWorkType::EXP_LIST_DAMAGED: return "Listing Damaged Files"sv;
	case EWorkType::EXP_PURGE_DAMAGED: return "Truncating Damaged Files"sv;
	case EWorkType::EXP_TRUNC_DAMAGED: return "Truncating damaged files to zero size"sv;
	//case ESpecialWorkType::workRAWDUMP: /*fall-through*/
	//case ESpecialWorkType::workBGTEST: return "42";
	case EWorkType::USER_INFO: return "General Configuration Information"sv;
	case EWorkType::TRACE_START:
	case EWorkType::TRACE_END:
	case EWorkType::REPORT: return "Status Report and Maintenance Tasks Overview"sv;
	case EWorkType::AUT_REQ: return "Authentication Required"sv;
	case EWorkType::AUTH_DENY: return "Authentication Denied"sv;
	case EWorkType::IMPORT: return "Data Import"sv;
	case EWorkType::MIRROR: return "Archive Mirroring"sv;
	case EWorkType::DELETE: return "Manual File Deletion"sv;
	case EWorkType::DELETE_CONFIRM: return "Manual File Deletion (Confirmed)"sv;
	case EWorkType::TRUNCATE: return "Manual File Truncation"sv;
	case EWorkType::TRUNCATE_CONFIRM: return "Manual File Truncation (Confirmed)"sv;
	case EWorkType::COUNT_STATS: return "Status Report With Statistics"sv;
	case EWorkType::STYLESHEET: return "CSS"sv;
	// case ESpecialWorkType::workJStats: return "Stats";
	}
	return "UnknownTask"sv;
}

EWorkType DetectWorkType(const tHttpUrl& reqUrl, string_view rawCmd, const char* auth)
{
	LOGSTARTs("DispatchMaintWork");

	LOG("cmd: " << rawCmd);

#if 0 // defined(DEBUG)
	if(cmd.find("tickTack")!=stmiss)
	{
		tBgTester(conFD).Run(cmd);
		return;
	}
#endif

	if (reqUrl.sHost == "style.css")
		return EWorkType::STYLESHEET;

	if (reqUrl.sHost == cfg::reportpage && reqUrl.sPath == "/")
		return EWorkType::REPORT;

	trimBack(rawCmd);
	trimFront(rawCmd, "/");

	if (!startsWith(rawCmd, cfg::reportpage))
		return EWorkType::UNKNOWN;

	rawCmd.remove_prefix(cfg::reportpage.length());
	if (rawCmd.empty() || rawCmd[0] != '?')
		return EWorkType::REPORT;
	rawCmd.remove_prefix(1);

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
	 case 1: return EWorkType::AUT_REQ;
	 default: return EWorkType::AUTH_DENY;
	}

	struct { string_view trigger; EWorkType type; } matches [] =
	{
			{"doExpire="sv, EWorkType::EXPIRE},
			{"justShow="sv, EWorkType::EXP_LIST},
			{"justRemove="sv, EWorkType::EXP_PURGE},
			{"justShowDamaged="sv, EWorkType::EXP_LIST_DAMAGED},
			{"justRemoveDamaged="sv, EWorkType::EXP_PURGE_DAMAGED},
			{"justTruncDamaged="sv, EWorkType::EXP_TRUNC_DAMAGED},
			{"doImport="sv, EWorkType::IMPORT},
			{"doMirror="sv, EWorkType::MIRROR},
			{"doDelete="sv, EWorkType::DELETE_CONFIRM},
			{"doDeleteYes="sv, EWorkType::DELETE},
			{"doTruncate="sv, EWorkType::TRUNCATE_CONFIRM},
			{"doTruncateYes="sv, EWorkType::TRUNCATE},
			{"doCount="sv, EWorkType::COUNT_STATS},
			{"doTraceStart="sv, EWorkType::TRACE_START},
			{"doTraceEnd="sv, EWorkType::TRACE_END},
//			{"doJStats", workJStats}
	};

#warning check perfromance, might be inefficient, maybe use precompiled regex for do* and just* or at least separate into two groups
	for(auto& needle: matches)
	{
		if (rawCmd.find(needle.trigger) != stmiss)
			return needle.type;
	}

	// something weird, go to the maint page
	return EWorkType::REPORT;
}

tFileItemPtr Create(EWorkType jobType, bufferevent *bev, const tHttpUrl& url, SomeData* arg)
{
	try
	{
		if (jobType == EWorkType::UNKNOWN)
			return tFileItemPtr(); // not for us?

		auto item = new BufferedPtItem(jobType, url.sPath, bev, arg);
		auto ret = as_lptr<fileitem>(item);
		if (! item->GetHandler())
			return tFileItemPtr();

		if (!item->GetHandler()->m_bNeedsBgThread)
		{
			item->GetHandler()->Run();
			item->Eof();
			return ret;
		}

		// we only pass the bare pointer to it, release the reference on the main thread only and only after the BG thread action is finished!
		ret->__inc_ref();
		auto runner = [item] ()
		{
			try
			{
				item->GetHandler()->Run();
				item->Eof();
				// release the potentially last reference when done
				evabase::Post([item]() { tFileItemPtr p(item, false); });
			}
			catch (const std::exception& exe)
			{
				string msg=exe.what();
				evabase::Post([item, msg]()
				{
					item->DlSetError({500, msg}, fileitem::EDestroyMode::DELETE);
				});
			}
			catch (...)
			{
				evabase::Post([item]()
				{
					item->DlSetError({500, "Unknown processing error"}, fileitem::EDestroyMode::DELETE);
				});
			}
		};

		if(g_tpool->schedule(runner))
			return ret;

		// FAIL STATE!
		ret->__dec_ref();
		return tFileItemPtr();
	}
	catch (...)
	{
		return tFileItemPtr();
	}
}

class BufItemPipeReader : public fileitem::ICacheDataSender
{
	lint_ptr<BufferedPtItem> source;
public:
	BufItemPipeReader(BufferedPtItem* p) : source(p)
	{
	}
	ssize_t SendData(bufferevent *target, off_t& callerSendPos, size_t maxTake) override
	{
		if (callerSendPos < source->m_nCursor)
			return 0;
		auto eb = source->PipeRx();
		if (callerSendPos > source->m_nCursor)
		{
			auto todrop = min(off_t(evbuffer_get_length(eb)), callerSendPos - source->m_nCursor);
			if (evbuffer_drain(eb, todrop))
				return -1;
			source->m_nCursor += todrop;
			// still not enough, come back later
			if (callerSendPos != source->m_nCursor)
				return 0;
		}
		auto ret = evbuffer_remove_buffer(eb, besender(target), maxTake);
		INCPOS(callerSendPos, ret);
		INCPOS(source->m_nCursor, ret);
		return ret;
	}
};

std::unique_ptr<fileitem::ICacheDataSender> BufferedPtItem::GetCacheSender()
{
	return make_unique<BufItemPipeReader>(this);
}

void cb_notify_new_pipe_data(struct bufferevent *, void *ctx)
{
	((BufferedPtItem*)ctx)->GotNewData();
}
void cb_bgpipe_event(struct bufferevent *, short what, void *ctx)
{
	((BufferedPtItem*)ctx)->BgPipeEvent(what);
}


}
