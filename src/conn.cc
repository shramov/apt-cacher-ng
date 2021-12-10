
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
#include "rex.h"

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
class acres;

void ACNG_API StartServingBE(unique_bufferevent_fdclosing&& be, string clientName, acres&);

class connImpl : public IConnBase
{
	acres& m_res;
	unique_bufferevent_flushclosing m_be;
	header m_h;
	size_t m_hSize = 0;
	aobservable::subscription m_keepalive;

	enum ETeardownMode
	{
		ACTIVE,
		PREP_SHUTDOWN,
		PREP_TYPECHANGE
	} m_opMode = ACTIVE;

	deque<job> m_jobs;
	lint_user_ptr<dlcontroller> m_pDlClient;
	mstring m_sClientHost;

public:
	dlcontroller* GetDownloader() override
	{
		if(!m_pDlClient)
			m_pDlClient = dlcontroller::CreateRegular(m_res);
		return m_pDlClient.get();
	}

	lint_ptr<IFileItemRegistry> m_itemRegistry;

	void writeAnotherLogRecord(const mstring &pNewFile,
			const mstring &pNewClient);

	connImpl(header&& he, size_t hRawSize, mstring clientName, lint_ptr<IFileItemRegistry> ireg, acres& res) :
		m_res(res),
		m_h(move(he)),
		m_hSize(hRawSize),
		m_sClientHost(move(clientName)),
		m_itemRegistry(move(ireg))
	{
		LOGSTARTFUNCx(m_sClientHost);
		m_keepalive = res.GetKeepAliveBeat().AddListener([this] () mutable
		{
			//DBGQLOG("ka: "sv << (long) this);
			KeepAlive();
		});
	};
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
#if 0
		if (m_jobs.empty())
			return false;
		unsigned offset = 0;
		if (m_jobs.size() > 1)
		{
			offset = jobId - m_jobs.front().GetId();
			if (offset > m_jobs.size())
				return false;
		}
		ASSERT(m_jobs[offset].GetId() == jobId);
		if (offset > 0) // not an active job, push it anyway so it can synchronize state as needed
			m_jobs[offset].Poke(m_be.get());
		else
			continueJobs();

		return !m_jobs.empty() && m_jobs.front().GetId() <= jobId;

#endif
	}

	void setup()
	{
		LOGSTARTFUNC;
		set_connect_sock_flags(bufferevent_getfd(*m_be));
		bufferevent_setcb(*m_be, cbRead, cbCanWrite, cbStatus, this);
		if (m_hSize > 0)
			addOneJob();
	}

	void KeepAlive()
	{
		if (m_jobs.empty())
			return;
		m_jobs.front().KeepAlive(m_be.get());
	}

	static void go(bufferevent* pBE, header&& h, size_t hRawSize, string clientName, acres& res)
	{
		lint_ptr<connImpl> worker;
		try
		{
			worker.reset(new connImpl(move(h), hRawSize, move(clientName), SetupServerItemRegistry(), res));
			worker->m_be.m_p = pBE;
			pBE = nullptr;
			worker->setup();
			ignore_ptr(worker.release()); // cleaned by its own callback
		}
		catch (const bad_alloc&)
		{
			if (pBE)
				be_free_close(pBE);
		}
	}

	// ISharedConnectionResources interface
public:
	// This method collects the logged data counts for certain file.
	// Since the user might restart the transfer again and again, the counts are accumulated (for each file path)
//	void LogDataCounts(cmstring &sFile, mstring xff, off_t nNewIn, off_t nNewOut, bool bAsError) override;
	lint_ptr<IFileItemRegistry> GetItemRegistry() override { return m_itemRegistry; }

	static void cbStatus(bufferevent*, short what, void* ctx)
	{
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
		{
			auto destroyer = as_lptr((connImpl*)ctx, false);
			if (destroyer->m_be.get())
				be_free_close(destroyer->m_be.release());
		}
	}
	static void cbRead(bufferevent* pBE, void* ctx)
	{
		auto me = (connImpl*)ctx;
		me->onRead(pBE);
	}
	static void cbCanWrite(bufferevent*, void* ctx)
	{
		auto me = (connImpl*)ctx;
		me->continueJobs();
	}

	void addOneJob(string_view errorStatus = svEmpty)
	{
		if (m_h.type == header::GET)
		{
#warning excpt. safe?
			m_jobs.emplace_back(*this);
			if (!errorStatus.empty())
				m_jobs.back().PrepareFatalError(m_h, errorStatus);
			else
				m_jobs.back().Prepare(m_h, *m_be, m_sClientHost, m_res);
			evbuffer_drain(bereceiver(*m_be), m_hSize);
		}
		else
		{
			m_opMode = ETeardownMode::PREP_TYPECHANGE;
		}
		continueJobs();
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
			return;
		default:
			m_hSize = m_h.Load(obuf);
			if (m_hSize < 0)
				addOneJob("400 Bad Request"sv);
			else if (m_hSize == 0)
				return; // more data to ome
			else
				addOneJob();
		}
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
		{
			// can switch right away!
			auto destroyer(as_lptr(this));
			bufferevent_disable(*m_be, EV_READ|EV_WRITE);
			bufferevent_setcb(*m_be, nullptr, nullptr, nullptr, nullptr);
			return StartServingBE(unique_bufferevent_fdclosing(m_be.release()), m_sClientHost, m_res);
		}
		case PREP_SHUTDOWN:
			auto destroyer = as_lptr(this);
			return be_flush_free_close(m_be.release());
		}
	}

	// IConnBase interface
public:
	cmstring &getClientName() override
	{
		return m_sClientHost;
	}
};

/**
 * @brief Vending point for CONNECT processing
 * Flushes and releases bufferevent in the end.
 */
struct TDirectConnector
{
	tHttpUrl url;
	unique_bufferevent_flushclosing m_be;
	lint_ptr<atransport> outStream;
	TFinalAction m_connBuilder;
	string_view m_httpProto;
	acres& m_res;

	TDirectConnector(acres& res) : m_res(res) {}

	static void PassThrough(bufferevent* be, cmstring& uri, const header& reqHead, acres& res)
	{
		TDirectConnector *hndlr = nullptr;
		// to be terminated here, in one way or another
		unique_bufferevent_flushclosing xbe(be);

		try
		{
			hndlr = new TDirectConnector(res);
			xbe.swap(hndlr->m_be);
			hndlr->m_httpProto = reqHead.GetProtoView();
		}
		catch (const std::bad_alloc&)
		{
			unique_bufferevent_fdclosing closer(be);
		}
		if(res.GetMatchers().Match(uri, rex::PASSTHROUGH) && hndlr->url.SetHttpUrl(uri))
			return hndlr->Go();
		ebstream(be) << hndlr->m_httpProto << " 403 CONNECT denied (ask the admin to allow HTTPS tunnels)\r\n\r\n"sv;
		return delete hndlr;
	}
	void Go()
	{
		auto act = [this] (atransport::tResult&& rep)
		{
			if (!rep.err.empty())
			{
				ebstream(*m_be) << m_httpProto << " 502 CONNECT error: "sv << rep.err << "\r\n\r\n"sv;
				return delete this;
			}
			outStream = rep.strm;
			auto targetCon = outStream->GetBufferEvent();
			ebstream(*m_be) << m_httpProto << " 200 Connection established\r\n\r\n"sv;
			bufferevent_setcb(*m_be, cbClientReadable, cbClientWriteable, cbEvent, this);
			bufferevent_setcb(targetCon, cbClientWriteable, cbClientReadable, cbEvent, this);
			setup_be_bidirectional(*m_be);
			setup_be_bidirectional(targetCon);
			m_connBuilder.reset();
		};
		m_connBuilder = atransport::Create(url, move(act), m_res,
										   atransport::TConnectParms()
										   .SetDirectConnection(true)
										   .SetNoTlsOnTarget(true));
	}
	static void cbClientReadable(struct bufferevent*, void *ctx)
	{
		auto me = (TDirectConnector*)ctx;
		bufferevent_write_buffer(me->outStream->GetBufferEvent(), bereceiver(*me->m_be));
	};
	static void cbClientWriteable(struct bufferevent*, void *ctx)
	{
		auto me = ((TDirectConnector*)ctx);
		bufferevent_write_buffer(*me->m_be, bereceiver(me->outStream->GetBufferEvent()));
	}
	static void cbEvent(struct bufferevent *, short what, void *ctx)
	{
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
			return delete ((TDirectConnector*)ctx);
	}
};

struct Dispatcher
{
	// simplify the reliable releasing whenver this object is deleted
	unique_bufferevent_fdclosing m_be;
	string clientName;
	acres& m_res;

	Dispatcher(string name, acres& res) : clientName(name), m_res(res) {}
	bool Go() noexcept
	{
		// dispatcher only fetches the relevant type header
		bufferevent_setcb(*m_be, Dispatcher::cbRead, nullptr, Dispatcher::cbStatus, this);
#warning tune me
//		bufferevent_setwatermark(*m_be, EV_READ, 0, MAX_IN_BUF);
		return 0 == bufferevent_enable(*m_be, EV_READ);
	}

	static void cbStatus(bufferevent*, short what, void* ctx)
	{
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
			return delete (Dispatcher*) (ctx);
	}
	static void cbRead(bufferevent* pBE, void* ctx)
	{
		auto* me = (Dispatcher*)ctx;
		auto rbuf = bereceiver(pBE);
		header h;
		auto hlen = h.Load(rbuf);
		if (!hlen)
			return; // will come back with more data

		ebstream sout(pBE);
		const auto& tgt = h.getRequestUrl();
		auto flushClose = [&]()
		{
			AppendMetaHeaders(sout);
			return be_flush_free_close(me->m_be.release());
		};

		if (hlen < 0) // client sends BS?
		{
			sout << "HTTP/1.0 400 Bad Request\r\n"sv;
			flushClose();
		}
		else if(h.type == header::GET)
		{
			evbuffer_drain(rbuf, hlen);
			connImpl::go(me->m_be.release(), move(h), hlen, move(me->clientName), me->m_res);
		}
		else if (h.type == header::CONNECT)
		{
			evbuffer_drain(rbuf, hlen);
			TDirectConnector::PassThrough(me->m_be.release(), tgt, h, me->m_res);
		}
		// is this something we support?
		else if (h.type == header::POST)
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

			flushClose();
		}
		else
		{
			sout << "HTTP/1.0 403 Forbidden\r\n"sv;
			flushClose();
		}

		return delete me;
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
};

void StartServingBE(unique_bufferevent_fdclosing&& be, string clientName, acres& res)
{
	if (!be.valid())
		return; // force-close everything
	auto* dispatcha = new Dispatcher {move(clientName), res};
	// excpt.safe
	dispatcha->m_be.reset(be.release());
	if (!dispatcha->Go())
		delete dispatcha; // bad, need to close the socket!
}


void StartServing(unique_fd&& fd, string clientName, acres& res)
{
	evutil_make_socket_nonblocking(fd.get());
	evutil_make_socket_closeonexec(fd.get());
	// fd ownership moves to bufferevent closer
	unique_bufferevent_fdclosing be(bufferevent_socket_new(evabase::base, fd.release(), BEV_OPT_DEFER_CALLBACKS));
#warning tune watermarks! set timeout!
	return StartServingBE(move(be), move(clientName), res);
}
}
