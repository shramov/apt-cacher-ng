#include "debug.h"
#include "meta.h"
#include "conn.h"
#include "acfg.h"
#include "job.h"
#include "header.h"
#include "dlcon.h"
#include "job.h"
#include "acbuf.h"
#include "atransport.h"
#include "sockio.h"
#include "evabase.h"
#include "acsmartptr.h"
#include "aconnect.h"
#include "tcpconnect.h"
#include "astrop.h"
#include "aclock.h"
#include "acforwarder.h"

#include <iostream>
#include <thread>

#include <sys/select.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

using namespace std;

#define SHORT_TIMEOUT 4

namespace acng
{

class connImpl;

class connImpl : public IConnBase
{
	acres& m_res;
	unique_bufferevent_flushclosing m_be;
	header m_h;
	ssize_t m_hSize = 0;

	enum ETeardownMode
	{
		ACTIVE,
		PREP_SHUTDOWN,
		PREP_TYPECHANGE
	} m_opMode = ACTIVE;

	deque<job> m_jobs;
	lint_user_ptr<dlcontroller> m_pDlClient;
	mstring m_sClientHost;
	aobservable::subscription m_baseSub;

public:
	dlcontroller* GetDownloader() override
	{
		if(!m_pDlClient)
			m_pDlClient = dlcontroller::CreateRegular(m_res);
		return m_pDlClient.get();
	}

	void writeAnotherLogRecord(const mstring &pNewFile,
			const mstring &pNewClient);

	connImpl(mstring&& clientName, acres& res) :
		m_res(res),
		m_sClientHost(move(clientName))
	{
		LOGSTARTFUNCx(m_sClientHost);
		m_baseSub = evabase::GetGlobal().subscribe([this](){ cbStatus(nullptr, BEV_EVENT_EOF, this);} );
	}

	virtual ~connImpl()
	{
		LOGSTARTFUNC;
		// in that order!
		m_jobs.clear();
		// our user's connection is released but the downloader task created here may still be serving others
		// tell it to stop when it gets the chance and delete it then
		m_pDlClient.reset();

	}

	/**
	 * @brief poke
	 * @param jobId
	 * @return
	 */
	void poke(uint_fast32_t jobId) override
	{
		LOGSTARTFUNCx(jobId);
		NONDEBUGVOID(jobId);
		continueJobs();
	}

	void setup_regular()
	{
		LOGSTARTFUNC;
		set_serving_sock_flags(bufferevent_getfd(*m_be));
		bufferevent_setcb(*m_be, cbRead, cbCanWrite, cbStatus, this);
		bufferevent_enable(*m_be, EV_WRITE|EV_READ);
	}

	void KeepAlive()
	{
		if (m_jobs.empty())
			return;
		m_jobs.front().KeepAlive(m_be.get());
	}

	void spawn(unique_bufferevent_flushclosing&& pBE)
	{
		try
		{
			m_be.reset(move(pBE));
			setup_regular();
		}
		catch (const bad_alloc&)
		{
			delete this;
		}
	}

	static void cbStatus(bufferevent*, short what, void* ctx)
	{
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
		{
			auto destroyer = as_lptr((connImpl*)ctx, false);
			if (!evabase::GetGlobal().IsShuttingDown() && destroyer->m_be.get())
				be_free_close(destroyer->m_be.release());
		}
	}
	static void cbRead(bufferevent* pBE, void* ctx)	{ ((connImpl*)ctx)->onRead(pBE); }

	static void cbCanWrite(bufferevent*, void* ctx)
	{
		auto me = (connImpl*)ctx;
		me->continueJobs();
	}

	/**
	 * @brief addRegularJob
	 * @param errorStatus Optional - forced error status line to report as job result
	 * @return True if a regular request was extracted, false if the request was something else
	 */
	bool addRegularRequest(string_view errorStatus = svEmpty)
	{
		bool ret = false;
		if (m_h.type == header::GET)
		{
#warning excpt. safe?
			m_jobs.emplace_back(*this);
			if (!errorStatus.empty())
				m_jobs.back().PrepareFatalError(m_h, errorStatus);
			else
				m_jobs.back().Prepare(m_h, *m_be, m_sClientHost, m_res);
			evbuffer_drain(bereceiver(*m_be), m_hSize);
			ret = true;
		}
		else
		{
			// queue will be processed, then this flag will be considered
			m_opMode = ETeardownMode::PREP_TYPECHANGE;
		}
		continueJobs();
		return ret;
	}

	void onRead(bufferevent* pBE)
	{
		auto obuf = bereceiver(pBE);
		switch (m_opMode)
		{
		case PREP_SHUTDOWN:
			evbuffer_drain(obuf, evbuffer_get_length(obuf));
			return;
		case ETeardownMode::PREP_TYPECHANGE:
			return; // don't care now, but after mode switching
		default:
			bool normalJob = false;
			do
			{
				m_hSize = m_h.Load(obuf);
				if (m_hSize < 0)
					normalJob = addRegularRequest("400 Bad Request"sv);
				else if (m_hSize == 0)
					return; // more data to come in upcoming callback
				else
					normalJob = addRegularRequest();
			} while(normalJob);
		}
		continueJobs();
	}
	void continueJobs()
	{
		while (!m_jobs.empty())
		{
			auto jr = m_jobs.front().Resume(true, *m_be);
			switch (jr)
			{
			case job::eJobResult::R_DISCON:
				DBGQLOG("Discon for " << m_jobs.front().GetId());
				m_jobs.clear();
				m_opMode = PREP_SHUTDOWN; // terminated below
				break;
			case job::eJobResult::R_DONE:
				m_jobs.pop_front();
				continue;
			case job::eJobResult::R_WILLNOTIFY:
				return;
			}
		}
		ASSERT(m_jobs.empty());
		switch (m_opMode)
		{
		case ACTIVE: // wait for new requests or timeout
			return;
		case PREP_TYPECHANGE:
			return PerformTypeChange();
		case PREP_SHUTDOWN:
			auto destroyer = as_lptr(this);
			return be_flush_free_close(m_be.release());
		}
	}

	void PerformTypeChange()
	{
		LOGSTARTFUNCs;
		// can only be here when a special request header was received before
		ASSERT(m_hSize > 0);
		ebstream sout(*m_be);
		const auto& tgt = m_h.getRequestUrl();

		if (m_hSize <= 0) // client sends BS?
		{
			sout << "HTTP/1.0 400 Bad Request\r\n"sv;
			AppendMetaHeaders(sout);
			return delete this;
		}

		if (m_h.type == header::CONNECT)
		{
			evbuffer_drain(bereceiver(*m_be), m_hSize);
			// this will replace the callbacks and continue, we are done here
			PassThrough(m_be.release(), tgt, m_h, m_res);
			return delete this;
		}
		// is this something we support?
		if (m_h.type == header::POST)
		{
			constexpr auto BDOURL = "http://bugs.debian.org:80/"sv;
			if(startsWith(tgt, BDOURL))
			{
#warning TESTME
				string_view path(tgt);
				path.remove_prefix(BDOURL.length());
				// XXX: consider using 426 instead, https://datatracker.ietf.org/doc/html/rfc2817#section-4.2
				// OTOH we want to make it reconnect anyway
				sout << "HTTP/1.1 302 Redirect\r\nLocation: https://bugs.debian.org:443/"sv << path << svRN;
			}
			else
				sout << "HTTP/1.0 403 Forbidden\r\n"sv;

			AppendMetaHeaders(sout);
			return delete this;
		}
		sout << "HTTP/1.0 403 Forbidden\r\n"sv;
		AppendMetaHeaders(sout);
		return delete this;
	}

	static void AppendMetaHeaders(ebstream& sout)
	{
		sout << "Connection: close\r\n"sv;
#ifdef DEBUG
		static atomic_int genHeadId(0);
		sout << "X-Debug: "sv << int(genHeadId++) << svRN;
#endif
		sout << "Date: "sv << tHttpDate(GetTime()).view()
			 << "\r\nServer: Debian Apt-Cacher NG/" ACVERSION "\r\n"
															  "\r\n"sv;
	}

	cmstring &getClientName() override
	{
		return m_sClientHost;
	}
};

void StartServing(unique_fd&& fd, string clientName, acres& res)
{
	evutil_make_socket_nonblocking(fd.get());
	evutil_make_socket_closeonexec(fd.get());
	// fd ownership moves to bufferevent closer
	unique_bufferevent_flushclosing be(bufferevent_socket_new(evabase::base, fd.release(), BEV_OPT_DEFER_CALLBACKS));
#warning watermarks? timeout!
	(new connImpl(move(clientName), res))->spawn(move(be));

}
}
