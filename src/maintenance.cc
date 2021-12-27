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

namespace acng
{

mainthandler::mainthandler(tRunParms&& parms) :
		m_parms(move(parms))
{
}

mainthandler::~mainthandler()
{
}

class tAuthRequest : public mainthandler
{
public:
	using mainthandler::mainthandler;

	void Run() override
	{
		string_view msg = "Not Authorized. Please contact Apt-Cacher NG administrator for further questions.\n\n"
				   "For Admin: Check the AdminAuth option in one of the *.conf files in Apt-Cacher NG "
				   "configuration directory, probably " CFGDIR;
		m_parms.bitem().ManualStart(401, "Not Authorized", "text/plain", "", msg.size());
		m_parms.bitem().AddExtraHeaders("WWW-Authenticate: Basic realm=\"For login data, see AdminAuth in Apt-Cacher NG config files\"\r\n");
		SendRemoteOnly(msg);
	}
};

class authbounce : public mainthandler
{
public:
	using mainthandler::mainthandler;

	void Run() override
	{
		string_view msg = "Not Authorized. To start this action, an administrator password must be set and "
						  "you must be logged in.";
		m_parms.bitem().ManualStart(403, "Access Forbidden", "text/plain", se, msg.size());
		SendRemoteOnly(msg);
	}
};

class errorItem : public fileitem
{
public:
	errorItem(mstring msg) : fileitem("<fatalerror>")
	{
		ManualStart(500, move(msg), "text/plain", se, 0);
		m_status = FIST_DLERROR;
	}
public:
	std::unique_ptr<ICacheDataSender> GetCacheSender() override { return std::unique_ptr<ICacheDataSender>(); }
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
		m_parms.bitem().ManualStart(500, "DBG", "text/plain");
	}
};
#endif

array<tSpecialWorkDescription, (size_t) EWorkType::WORK_TYPE_MAX> workDescriptors;

const tSpecialWorkDescription& GetTaskInfo(EWorkType type)
{
	return AC_LIKELY(type < workDescriptors.max_size())
			? workDescriptors[type] : workDescriptors[0];
}

static mainthandler* MakeMaintWorker(mainthandler::tRunParms&& parms)
{
	if (parms.type >= workDescriptors.size())
		parms.type = EWorkType::USER_INFO; // XXX: report as error in the log?
	if (workDescriptors[parms.type].creator)
		return workDescriptors[parms.type].creator(move(parms));
	return workDescriptors[EWorkType::USER_INFO].creator(move(parms));
}

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

void InitSpecialWorkDescriptors()
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
	workDescriptors[EWorkType::IMPORT] = {"IMPORT"sv, "Data Import"sv, "doImport="sv, &creators::pkgimport, BLOCKING | EXCLUSIVE };
	workDescriptors[EWorkType::MIRROR] = {"MIRROR"sv, "Archive Mirroring"sv, "doMirror="sv, &creators::pkgmirror, BLOCKING | EXCLUSIVE};
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
}


EWorkType DetectWorkType(const tHttpUrl& reqUrl, string_view rawCmd, const char* auth)
{
	LOGSTARTs("DispatchMaintWork");

	LOG("cmd: " << rawCmd);

	if (reqUrl.sHost == "style.css")
		return EWorkType::STYLESHEET;

	if (reqUrl.sHost == "favicon.ico")
		return EWorkType::FAVICON;

	if (reqUrl.sHost == cfg::reportpage && reqUrl.sPath == "/")
		return EWorkType::REPORT;

	// others are passed through the report page extra functions

	if (cfg::reportpage.empty())
		return EWorkType::REGULAR;

	trimBack(rawCmd);
	trimFront(rawCmd, "/");

	if (!startsWith(rawCmd, cfg::reportpage))
		return EWorkType::REGULAR;

	rawCmd.remove_prefix(cfg::reportpage.length());
	if (rawCmd.empty() || rawCmd[0] != '?')
		return EWorkType::REPORT;
	rawCmd.remove_prefix(1);

	// not shorter, was already compared, can be only longer, means having parameters,
	// -> means needs authorization

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
	 case 1: return EWorkType::AUTH_REQ;
	 default: return EWorkType::AUTH_DENY;
	}

	for (unsigned i = EWorkType::REGULAR + 1; i < EWorkType::WORK_TYPE_MAX; ++i)
	{
		const auto& trigger = workDescriptors[i].trigger;
		if (!trigger.empty() && rawCmd.find(trigger) != stmiss)
			return (EWorkType) i;
	}

	// something weird, go to the maint page
	return EWorkType::REPORT;
}

class BufferedPtItem : public MaintStreamItemBase
{

	static void cb_notify_new_pipe_data(struct bufferevent *, void *ctx)
	{
		((BufferedPtItem*)ctx)->GotNewData();
	}
	static void cb_bgpipe_event(struct bufferevent *, short what, void *ctx)
	{
		((BufferedPtItem*)ctx)->BgPipeEvent(what);
	}

public:

	// where the cursor is, matches the begin of the current buffer
	off_t m_nCursor = 0;

	~BufferedPtItem()
	{
		ASSERT_HAVE_MAIN_THREAD;
		if (m_pipeInOut[0]) bufferevent_free(m_pipeInOut[0]);
		if (m_pipeInOut[1]) bufferevent_free(m_pipeInOut[1]);
	}

	BufferedPtItem(unique_ptr<mainthandler>&& han)
		: MaintStreamItemBase("_internal_task")
		// XXX: resolve the name to task type for the logs? Or replace the name with something useful later? Although, not needed, and also w/ format not fitting the purpose.
	{
		ASSERT_HAVE_MAIN_THREAD;
		ASSERT(han);

		han.swap(handler);
		handler->m_parms.owner = this;

		m_bPureStreamNoStorage = true;

		m_status = FiStatus::FIST_DLERROR;
		try
		{

			auto flags = BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS
						 | BEV_OPT_THREADSAFE * !!(GetTaskInfo(handler->m_parms.type).flags & BLOCKING);

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
		catch(const std::exception& e)
		{
			m_responseStatus = { 500, e.what() };
		}
		catch(...)
		{
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
	void Eof() override
	{
		bufferevent_flush(m_pipeInOut[0], EV_WRITE, BEV_FINISHED);
	}

	inline void BgPipeEvent(short what)
	{
		LOGSTARTFUNCs;
		ldbg(what);
		if (what & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT | BEV_EVENT_EOF))
		{
			Finish();
		}
	}

	inline void GotNewData()
	{
		ASSERT_HAVE_MAIN_THREAD;
		LOGSTARTFUNC;
		auto len = evbuffer_get_length(PipeRx());
		m_nSizeChecked = m_nCursor + len;
		ldbg(len << " -> " << m_nSizeChecked);
		if (m_status == FIST_DLGOTHEAD)
			m_status = FIST_DLBODY;
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

void mainthandler::SendRemoteOnly(string_view sv)
{
	// push everything into the pipe, the output will make notifications as needed

	if(!sv.data() || sv.empty())
		return;
	send(PipeTx(), sv);
}

void mainthandler::SendRemoteOnly(evbuffer* data)
{
	if (!data)
		return;
	// push everything into the pipe, the output will make notifications as needed
	evbuffer_add_buffer(PipeTx(), data);
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

tFileItemPtr Create(EWorkType jobType, bufferevent *bev, const tHttpUrl& url, SomeData* arg, acres& reso)
{
	try
	{
		if (jobType == EWorkType::REGULAR)
			return tFileItemPtr(); // not for us?

		if (jobType >= EWorkType::WORK_TYPE_MAX)
			return tFileItemPtr(new errorItem("Bad Type"));

		if (jobType == EWorkType::STYLESHEET)
			return tFileItemPtr(new TResFileItem("style.css", "text/css"));

		if (jobType == EWorkType::FAVICON)
			return tFileItemPtr(new TResFileItem("favicon.ico", "image/x-icon"));

		const auto& desc = workDescriptors[jobType];

		lint_ptr<IMaintJobItem> item;

		try
		{
			unique_ptr<mainthandler> handler;
			handler.reset(MakeMaintWorker({jobType, move(url.sPath), bufferevent_getfd(bev), nullptr, arg, reso}));
			if (!handler)
				return tFileItemPtr(new errorItem("Internal processing error"));
			item.reset(new BufferedPtItem(move(handler)));
		}
		catch(const std::exception& e)
		{
			return tFileItemPtr(new errorItem(e.what()));
		}
		catch (...)
		{
			return tFileItemPtr(new errorItem("Unable to start background items"));
		}

		auto ret = item.release();

		if (0 == (desc.flags & BLOCKING))
		{
			item->GetHandler()->Run();
			item->Eof();
			return as_lptr<fileitem>(ret);
		}

		// OKAY, prepare to execute on another thread

		// we only pass the bare pointer to it, release the reference on the main thread only and only after the BG thread action is finished!
		ret->__inc_ref();
		auto runner = [ret] ()
		{
			try
			{
				ret->GetHandler()->Run();
				// release the potentially last reference when done
				evabase::Post([ret]() { tFileItemPtr destroyer(ret, false); });
			}
			catch (const std::exception& exe)
			{
				string msg=exe.what();
				evabase::Post([ret, msg]()
				{
					ret->DlSetError({500, msg}, fileitem::EDestroyMode::DELETE);
				});
			}
			catch (...)
			{
				evabase::Post([ret]()
				{
					ret->DlSetError({500, "Unknown processing error"}, fileitem::EDestroyMode::DELETE);
				});
			}
			evabase::Post([ret]()
			{
				ret->Eof();
				tFileItemPtr destroyer(ret, false);
			});
		};

		if(g_tpool->schedule(runner))
			return as_lptr<fileitem>(ret);

		// FAIL STATE! CLEANUP HERE ASAP!
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
		auto ret = eb_move_range(eb, besender(target), maxTake);
		INCPOS(callerSendPos, ret);
		INCPOS(source->m_nCursor, ret);
		return ret;
	}
};

std::unique_ptr<fileitem::ICacheDataSender> BufferedPtItem::GetCacheSender()
{
	return make_unique<BufItemPipeReader>(this);
}

}
