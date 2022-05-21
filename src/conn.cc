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
#include "astrop.h"
#include "aclock.h"
#include "conserver.h"
#include "rex.h"

#include <iostream>
#include <thread>

#include <signal.h>
#include <string.h>
#include <errno.h>

using namespace std;

namespace acng
{

class connImpl;
#ifdef DEBUG
int g_connId = 0;
#endif

class connImpl : public IConnBase
{
	bool m_bPrepTypeChange = false;
	bool m_bIsLocalAdmin = false;
	bool m_bTerminated = false;

	bool m_bConnectMode = false;
	bool m_bJobMode = true;

	acres& m_res;
	unique_bufferevent_flushclosing m_be;
	mstring m_sClientHost;

	struct AD
	{
		header m_h;
		ssize_t m_hSize = 0;
		deque<job> m_jobs;
		lint_user_ptr<dlcontroller> m_pDlClient;
	};
	struct BD
	{
		tHttpUrl url;
		lint_ptr<atransport> outStream;
		TFinalAction m_connBuilder;
		string m_httpProto;
		off_t m_nBytesIn = 0;
//		off_t m_nBytesOut = 0;
	};
	union
	{
		AD ad;
		BD bd;
	};
#ifdef DEBUG
	int m_connId = g_connId++;
#endif

public:
	dlcontroller* GetDownloader() override
	{
		if(!ad.m_pDlClient)
			ad.m_pDlClient = dlcontroller::CreateRegular(m_res);
		return ad.m_pDlClient.get();
	}

	void writeAnotherLogRecord(const mstring &pNewFile,
			const mstring &pNewClient);

	connImpl(mstring&& clientName, acres& res, bool isAdmin) :
		m_bIsLocalAdmin(isAdmin),
		m_res(res),
		m_sClientHost(move(clientName))
	{
		LOGSTARTFUNCx(m_sClientHost);
		new (&ad) AD();
	}

	virtual ~connImpl()
	{
		LOGSTARTFUNC;

		if (m_bConnectMode)
		{
			log::transfer(bd.m_nBytesIn, bd.m_nBytesIn /* bd.m_nBytesOut*/, m_sClientHost,
						  bd.url.sHost + ':' + ltos(bd.url.GetPort()), false);
			bd.~BD();
		}

		if (m_bJobMode)
		{
			// order might matter
			ad.m_jobs.clear();
			ad.m_pDlClient.reset();
			ad.~AD();
		}
	}
private:
	void requestShutdown()
	{
		if (m_bTerminated)
			return;
		m_bTerminated = true;
		auto* owner = m_res.GetLastConserver();
		if (owner)
			owner->ReleaseConnection(this);
	};

public:
	void Abandon() override
	{
		if (!m_bConnectMode)
		{
			// in that order!
			ad.m_jobs.clear();
			// our user's connection is released but the downloader task created here may still be serving others
			// tell it to stop when it gets the chance and delete it then
			ad.m_pDlClient.reset();
		}
	}

	/**
	 * @brief poke
	 * @param jobId
	 * @return
	 */
	void GotMoreData(uint_fast32_t jobId) override
	{
		LOGSTARTFUNCx(jobId);
		if (AC_UNLIKELY(m_bTerminated))
			return;

#warning XXX: check performance. Alternative concept: set bufferevent watermark, always check fill status after data was added and set a flag to block writing, then wait for onCanWrite to unset it.
		if (m_be.valid() && off_t(evbuffer_get_length(besender(*m_be))) > cfg::sendwindow)
		{
			return;
		}
		NONDEBUGVOID(jobId);
		continueJobs();
	}

	void spawn(unique_bufferevent_flushclosing&& pBE)
	{
		LOGSTARTFUNC;
		m_be.reset(move(pBE));
		set_serving_sock_flags(bufferevent_getfd(*m_be));
		bufferevent_setcb(*m_be, cbRead, cbCanWrite, cbStatus, this);
		bufferevent_enable(*m_be, EV_WRITE|EV_READ);
		setReadTimeout(true);
		TuneSendWindow(*m_be);
	}

	static void cbStatus(bufferevent*, short what, void* ctx)
	{
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
		{
			auto me = as_lptr((connImpl*)ctx);
			if (!evabase::GetGlobal().IsShuttingDown() && me->m_be.get())
				be_free_close(me->m_be.release());
			return me->requestShutdown();
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
	void addRequest(string_view errorStatus = svEmpty)
	{
		LOGSTARTFUNC;
		if (ad.m_h.type == header::GET)
		{
#warning excpt. safe?
			ad.m_jobs.emplace_back(*this);
			if (!errorStatus.empty())
				ad.m_jobs.back().PrepareFatalError(errorStatus);
			else if (m_bIsLocalAdmin)
				ad.m_jobs.back().PrepareAdmin(ad.m_h, *m_be, m_res);
			else
				ad.m_jobs.back().Prepare(ad.m_h, *m_be, m_sClientHost, m_res);

			if (ad.m_jobs.size() == 1)
				setReadTimeout(false);

			evbuffer_drain(bereceiver(*m_be), ad.m_hSize);
		}
		else
		{
			// queue will be processed, then this flag will be considered
			m_bPrepTypeChange = true;
		}
	}

	void onRead(bufferevent* pBE)
	{
		auto obuf = bereceiver(pBE);
		while (!m_bPrepTypeChange)
		{
			ad.m_hSize = ad.m_h.Load(obuf);
			if (ad.m_hSize == 0)
				break; // more data to come in upcoming callback
			addRequest(ad.m_hSize < 0 ? "400 Bad Request"sv : se );
		}
		continueJobs();
	}
	/**
	 * @brief continueJobs runs remaining jobs
	 * @return True to continue, false if "this" is destroyed
	 */
	void continueJobs()
	{
		while (!ad.m_jobs.empty())
		{
			auto jr = ad.m_jobs.front().Resume(true, *m_be);
			switch (jr)
			{
			case job::eJobResult::R_DISCON:
			{
				DBGQLOG("Discon for " << ad.m_jobs.front().GetId());
				ad.m_jobs.clear();
				be_flush_free_close(m_be.release());
				return requestShutdown();
			}
			case job::eJobResult::R_DONE:
				ad.m_jobs.pop_front();
				if (ad.m_jobs.empty())
					setReadTimeout(true);
				continue;
			case job::eJobResult::R_WILLNOTIFY:
				return;
			}
		}
		// ok, jobs finished, what next?
		if (m_bPrepTypeChange)
			PerformTypeChange();
	}

	/**
	 * @brief PerformTypeChange handles special requests,
	 * which might either convert session to another type of handling
	 * OR respond with a hint and terminate (client shall be redirected as needed)
	 */
	void PerformTypeChange()
	{
		LOGSTARTFUNCs;
		// can only be here when a special request header was received before
		ASSERT(ad.m_hSize > 0);

		m_bPrepTypeChange = false;
		setReadTimeout(false);

		ebstream sout(*m_be);
		const auto& tgt = ad.m_h.getRequestUrl();
		string proto(ad.m_h.GetProtoView());

		if (ad.m_hSize <= 0) // client sends BS?
		{
			sout << proto << " 400 Bad Request\r\n"sv;
		}
		else if (ad.m_h.type == header::CONNECT)
		{
			evbuffer_drain(bereceiver(*m_be), ad.m_hSize);
			if (MorphToPassThrough(ad.m_h))
				return;
			sout << proto << " 403 CONNECT denied (ask the admin to allow HTTPS tunnels)\r\n"sv;
		}
		else if (ad.m_h.type == header::POST)
		{
			// is this something we support?
			constexpr auto BDOURL = "http://bugs.debian.org:80/"sv;
			if(startsWith(tgt, BDOURL))
			{
				string_view path(tgt);
				path.remove_prefix(BDOURL.length());
				// XXX: consider using 426 instead, https://datatracker.ietf.org/doc/html/rfc2817#section-4.2
				// OTOH we want to make it reconnect anyway
				sout << proto << " 302 Redirect\r\nLocation: https://bugs.debian.org:443/"sv << path << svRN;
			}
			else
				sout << proto << " 403 Forbidden\r\n"sv;
		}
		else
		{
			sout << proto << " 403 Forbidden\r\n"sv;
		}
		sout << "Connection: close\r\n"sv;
#ifdef DEBUG
		static atomic_int genHeadId(0);
		sout << "X-Debug: "sv << int(genHeadId++) << svRN;
#endif
		sout << "Date: "sv << tHttpDate(GetTime()).view()
			 << "\r\nServer: Debian Apt-Cacher NG/" ACVERSION "\r\n\r\n"sv;
		// flusher will take over
		return requestShutdown();
	}

	void setReadTimeout(bool set)
	{
		bufferevent_set_timeouts(*m_be, set ? cfg::GetNetworkTimeout() : nullptr, cfg::GetNetworkTimeout());
	}

	cmstring &getClientName() override
	{
		return m_sClientHost;
	}

#ifdef DEBUG
	// Analyzed interface
public:
	void DumpInfo(Dumper &dumper) override
	{
		if (m_bJobMode)
		{
			DUMPFMT << "CONN: " << m_connId << "/"  << m_sClientHost << "/" << bufferevent_getfd(*m_be);
			DUMPLIST(ad.m_jobs, "JOBS:");
			DUMPIFSET(ad.m_pDlClient, "DLER:"sv);
		}
		if (m_bConnectMode)
		{
			DUMPFMT << "CONN(PT): " << m_connId << "/"
					<< bd.m_nBytesIn << "/"
					// << bd.m_nBytesOut << "/"
					<< bufferevent_getfd(*m_be);
			DUMPFMT << bd.url.ToURI(false);
		}
	}
#endif

	static void cbPtClientRead(struct bufferevent*, void *ctx)
	{
		auto me = (connImpl*)ctx;
		auto* rcvr = bereceiver(*me->m_be);
		/*
		if (log::logIsEnabled)
			me->bd.m_nBytesOut += evbuffer_get_length(rcvr);
			*/
		bufferevent_write_buffer(me->bd.outStream->GetBufferEvent(), rcvr);
	};
	static void cbPtClientWrite(struct bufferevent*, void *ctx)
	{
		auto me = (connImpl*)ctx;
		auto* rcvr = bereceiver(me->bd.outStream->GetBufferEvent());
		if (log::logIsEnabled)
			me->bd.m_nBytesIn += evbuffer_get_length(rcvr);
		bufferevent_write_buffer(*me->m_be, rcvr);
	}
	static void cbPtStatus(struct bufferevent *, short what, void *ctx)
	{
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
			return ((connImpl*)ctx)->requestShutdown();
	}

	bool MorphToPassThrough(const header& reqHead)
	{
		LOGSTARTFUNCs;
		try
		{
			tHttpUrl url;

			if(m_res.GetMatchers().Match(reqHead.getRequestUrl(), rex::PASSTHROUGH)
					&& url.SetHttpUrl(reqHead.getRequestUrl()))
			{
				url.sPath.clear();
				string proto(reqHead.GetProtoView());
				ad.~AD();
				m_bJobMode = false;

				new (&bd) BD();
				m_bConnectMode = true;
				bd.m_httpProto = move(proto);
				bd.url = move(url);

				auto act = [this] (atransport::tResult&& rep)
				{
					try
					{
						if (!rep.err.empty())
						{
							ebstream(*m_be) << bd.m_httpProto << " 502 CONNECT error: "sv << rep.err << "\r\n\r\n"sv;
							return requestShutdown();
						}

						// be sure about THAT
						bufferevent_disable(*m_be, EV_READ|EV_WRITE);
						bufferevent_setcb(*m_be, nullptr,nullptr,nullptr,nullptr);

						bd.outStream = rep.strm;
						auto targetCon = bd.outStream->GetBufferEvent();
						ebstream(*m_be) << bd.m_httpProto << " 200 Connection established\r\n\r\n"sv;
						bufferevent_setcb(*m_be, cbPtClientRead, cbPtClientWrite, cbPtStatus, this);
						bufferevent_setcb(targetCon, cbPtClientWrite, cbPtClientRead, cbPtStatus, this);
						setup_be_bidirectional(*m_be);
						setup_be_bidirectional(targetCon);
						bd.m_connBuilder.reset();
					}
					catch (...)
					{
						return requestShutdown();
					}
				};
				bd.m_connBuilder = atransport::Create(bd.url, move(act), m_res,
												   atransport::TConnectParms()
												   .SetDirectConnection(true)
												   .SetNoTlsOnTarget(true));
				return true;
			}
		}
		catch (const std::bad_alloc&)
		{
		}
		return false;
	}
};

lint_user_ptr<IConnBase> StartServing(unique_fd&& fd, string clientName, acres& res, bool isAdmin)
{
	evutil_make_socket_nonblocking(fd.get());
	evutil_make_socket_closeonexec(fd.get());
	// fd ownership moves to bufferevent closer
	unique_bufferevent_flushclosing be(bufferevent_socket_new(evabase::base, fd.release(), BEV_OPT_DEFER_CALLBACKS));

	try
	{
		auto session = as_lptr(new connImpl(move(clientName), res, isAdmin));
		session->spawn(move(be));
		return lint_user_ptr(static_lptr_cast<IConnBase>(session));
	}
	catch (...)
	{
		return lint_user_ptr<IConnBase>();
	}
}


}
