#include "meta.h"

#include "mainthandler.h"
#include "aclogger.h"
#include "filereader.h"
#include "acfg.h"
#include "acbuf.h"
#include "sockio.h"
#include "caddrinfo.h"
#include "portutils.h"
#include "debug.h"
#include "ptitem.h"
#include "tpool.h"
#include "acsmartptr.h"
#include "bgtask.h"

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
		const auto& trigger = GetTaskInfo((EWorkType)i).trigger;
		if (!trigger.empty() && rawCmd.find(trigger) != stmiss)
			return (EWorkType) i;
	}

	// something weird, go to the maint page
	return EWorkType::REPORT;
}

lint_ptr<IMaintJobItem> g_exclActiveJob;

class FileBackedItem : public IMaintJobItem
{
	unique_fd m_outFile;
public:
	FileBackedItem(unique_ptr<mainthandler>&& han)
		: IMaintJobItem(move(han), this)
	{
		m_bPureStreamNoStorage = false;

		timeval tv;
		gettimeofday(&tv, 0);
		tSS nam;
		nam << CACHE_BASE << MJSTORE_SUBPATH;
		mkdirhier(nam.c_str());
		nam << "/" << handler->desc().typeName << "." << tv.tv_sec << "." << tv.tv_usec << ".html";
		m_outFile.reset(open(nam.c_str(), O_WRONLY | O_CREAT | O_BINARY, cfg::fileperms));
		nam.drop(CACHE_BASE_LEN);
		m_sPathRel = nam.view();

		if (!AC_UNLIKELY(m_outFile.valid()))
		{
			m_responseStatus = { 500, "Internal error"};
			m_status = FIST_DLERROR;
		}
	}

	void GotData(ssize_t l)
	{
		ASSERT_IS_MAIN_THREAD;

		if (l < 0)
		{
			DlSetError({500, "Internal Error"}, fileitem::EDestroyMode::KEEP);
			return;
		}

		if (m_nSizeChecked < 0)
			m_nSizeChecked = 0;
		m_nSizeChecked += l;
		NotifyObservers();
		if (m_status < FIST_DLBODY)
			m_status = FIST_DLBODY;
	}
	void Send(evbuffer *data) override
	{
		if (!m_outFile.valid())
			return;
		auto l = eb_dump_chunks(data, *m_outFile);
		evabase::Post([this, l](){ GotData(l); });
	}
	void Send(string_view sv) override
	{
		if (!m_outFile.valid())
			return;
		auto l = dumpall(*m_outFile, sv);
		evabase::Post([this, l](){ GotData(l); });
	}
	void Eof() override
	{
		ASSERT_IS_MAIN_THREAD;
		DlFinish(true);
	}

	std::unique_ptr<fileitem::ICacheDataSender> GetCacheSender() override
	{
		LOGSTARTFUNC;
		ASSERT_IS_MAIN_THREAD;

		USRDBG("Opening tempfile at " << m_sPathRel);
		return GetStoredFileSender(m_sPathRel, m_nSizeChecked, m_status == FIST_COMPLETE);
	}
};

class BufferedPtItem : public IMaintJobItem
{
	struct bufferevent *m_pipeInOut[2] = { NULL, NULL };

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

	struct evbuffer* PipeTx() { return bufferevent_get_output(m_pipeInOut[0]); }
	struct evbuffer* PipeRx() { return bufferevent_get_input(m_pipeInOut[1]); }

	~BufferedPtItem()
	{
		ASSERT_IS_MAIN_THREAD;
		if (m_pipeInOut[0]) bufferevent_free(m_pipeInOut[0]);
		if (m_pipeInOut[1]) bufferevent_free(m_pipeInOut[1]);
	}

	void Send(string_view sv) override
	{
		// push everything into the pipe, the output will make notifications as needed

		if(!sv.data() || sv.empty())
			return;
		send(PipeTx(), sv);
	}

	void Send(evbuffer* data) override
	{
		if (!data)
			return;
		// push everything into the pipe, the output will make notifications as needed
		evbuffer_add_buffer(PipeTx(), data);
	}

	BufferedPtItem(unique_ptr<mainthandler>&& han)
		:  IMaintJobItem(move(han), this)
		// XXX: resolve the name to task type for the logs? Or replace the name with something useful later? Although, not needed, and also w/ format not fitting the purpose.
	{
		ASSERT_IS_MAIN_THREAD;

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

	void AddExtraHeaders(mstring appendix)
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
		ASSERT_IS_MAIN_THREAD;
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
		ASSERT_IS_MAIN_THREAD;
		return m_extraHeaders;
	}
private:
	/**
	 * @brief Finish should not be called directly, only through the Pipe EOF callback!
	 */
	void Finish()
	{
		ASSERT_IS_MAIN_THREAD;
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

tFileItemPtr Create(EWorkType jobType, bufferevent *bev, const tHttpUrl& url, SomeData* arg, acres& reso)
{
	LOGSTARTFUNCsx(url.ToURI(true));

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

		const auto& desc = GetTaskInfo(jobType);

		lint_ptr<IMaintJobItem> item;

		try
		{
			unique_ptr<mainthandler> handler;
			handler.reset(MakeMaintWorker({jobType, move(url.sPath), bufferevent_getfd(bev), nullptr, arg, reso}));
			if (!handler)
				return tFileItemPtr(new errorItem("Internal processing error"));

			if ((desc.flags & EXCLUSIVE) && g_exclActiveJob
				&& ! g_exclActiveJob->GetHandler()->m_bSigTaskAbort)
			{
				// attach to the running special job, unless it's shutting down already
				return static_lptr_cast<fileitem>(g_exclActiveJob);
			}
			else
			{
				if (desc.flags & FILE_BACKED)
					item.reset(new FileBackedItem(move(handler)));
				else
					item.reset(new BufferedPtItem(move(handler)));

				if (desc.flags & EXCLUSIVE)
					g_exclActiveJob = item;
			}
		}
		catch(const std::exception& e)
		{
			return tFileItemPtr(new errorItem(e.what()));
		}
		catch (...)
		{
			return tFileItemPtr(new errorItem("Unable to start background items"));
		}

		auto ret = static_lptr_cast<fileitem>(item);;

		if (item->GetStatus() > fileitem::FIST_COMPLETE)
		{
#define CHECK_UNSET_EXCL_JOB if (item == g_exclActiveJob) g_exclActiveJob.reset();
			CHECK_UNSET_EXCL_JOB;
			return ret;
		}

		if (0 == (desc.flags & BLOCKING))
		{
			item->GetHandler()->Run();
			item->Eof();
			CHECK_UNSET_EXCL_JOB;
			return ret;
		}

		// OKAY, prepare to execute on another thread

		// we only pass the bare pointer to it, release the reference on the main thread only and only after the BG thread action is finished!
		item->__inc_ref();
		auto runner = [rawItem = item.get()] ()
		{

			try
			{
				rawItem->GetHandler()->Run();
			}
			catch (const std::exception& exe)
			{
				string msg=exe.what();
				evabase::Post([rawItem, msg]()
				{
					rawItem->DlSetError({500, msg}, fileitem::EDestroyMode::DELETE);
				});
			}
			catch (...)
			{
				evabase::Post([rawItem]()
				{
					rawItem->DlSetError({500, "Unknown processing error"}, fileitem::EDestroyMode::DELETE);
				});
			}
			// release the potentially last reference when done
			evabase::Post([rawItem]()
			{
				rawItem->Eof();
				auto item = as_lptr(static_cast<IMaintJobItem*>(rawItem), false);
				item->GetHandler()->m_bItemIsHot = false;
				CHECK_UNSET_EXCL_JOB;
			});
		};

		item->GetHandler()->m_bItemIsHot = true;

		if (!g_tpool->schedule(runner))
		{
			LOG("Unable to start background thread, tear down to avoid leaks");
			// FAIL STATE! CLEANUP HERE ASAP!
			item->GetHandler()->m_bItemIsHot = false;
			item->__dec_ref();
			return tFileItemPtr();
		}
		return ret;
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

evbuffer* GetTxBufferForBufferedItem(fileitem& p)
{
	return static_cast<BufferedPtItem&>(p).PipeTx();
}

}
