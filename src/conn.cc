
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

static uint64_t g_ivJobId = 0;

unordered_map<uintptr_t, lint_ptr<connImpl>> global_index;

void StartServingBE(unique_bufferevent_fdclosing&& be, string clientName);

class connImpl : public IConnBase
{
	unique_bufferevent_flushclosing m_be;
	header m_h;
	size_t m_hSize = 0;
	enum ETeardownMode
	{
		ACTIVE,
		PREP_SHUTDOWN,
		PREP_TYPECHANGE
	} m_opMode;

	deque<job> m_jobs;
	lint_ptr<dlcontroller> m_pDlClient;
	mstring m_sClientHost;

public:
	dlcontroller* SetupDownloader() override
	{
		if(!m_pDlClient)
			m_pDlClient = dlcontroller::CreateRegular();
		return m_pDlClient.get();
	}

	lint_ptr<IFileItemRegistry> m_itemRegistry;

	void writeAnotherLogRecord(const mstring &pNewFile,
			const mstring &pNewClient);

	connImpl(header&& he, size_t hRawSize, mstring clientName, lint_ptr<IFileItemRegistry> ireg) :
		m_h(move(he)),
		m_hSize(hRawSize),
		m_sClientHost(move(clientName)),
		m_itemRegistry(move(ireg))

	{
		LOGSTARTFUNCx(m_sClientHost);
	};
	virtual ~connImpl()
	{
		LOGSTART("con::~con (Destroying connection...)");
		// our user's connection is released but the downloader task created here may still be serving others
		// tell it to stop when it gets the chance and delete it then
		if(m_pDlClient)
			m_pDlClient->Dispose();
		m_jobs.clear();
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
		bufferevent_setcb(*m_be, cbRead, cbCanWrite, cbStatus, this);
		if (m_hSize > 0)
			addOneJob();
	}

	static void go(bufferevent* pBE, header&& h, size_t hRawSize, string clientName)
	{
		lint_ptr<connImpl> worker;
		try
		{
			//connImpl(header&& he, size_t hRawSize, mstring clientName, std::shared_ptr<IFileItemRegistry> ireg)
			worker.reset(new connImpl(move(h), hRawSize, move(clientName), SetupServerItemRegistry()));
			worker->m_be.m_p = pBE;
			pBE = nullptr;
			worker->setup();
			ignore_ptr(worker.release()); // cleaned by its own callback
		}
		catch (const bad_alloc&)
		{
			if (pBE)
				be_free_fd_close(pBE);
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
				be_free_fd_close(destroyer->m_be.release());
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
				m_jobs.back().Prepare(m_h, *m_be, m_hSize, m_sClientHost);
			evbuffer_drain(bufferevent_get_output(*m_be), m_hSize);
		}
		else
		{
			m_opMode = ETeardownMode::PREP_TYPECHANGE;
		}
		continueJobs();
	}
	void onRead(bufferevent* pBE)
	{
		auto obuf = bufferevent_get_output(pBE);
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
			return StartServingBE(unique_bufferevent_fdclosing(m_be.release()), m_sClientHost);
		}
		case PREP_SHUTDOWN:
			auto destroyer = as_lptr(this);
			return be_flush_and_release_and_fd_close(m_be.release());
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
struct TDirectConnector : public tLintRefcounted
{
	tHttpUrl url;
	unique_bufferevent_flushclosing be;
	lint_ptr<atransport> outStream;

	static void PassThrough(bufferevent* be, cmstring& uri)
	{
		auto pin = make_lptr<TDirectConnector>();
		pin->be.m_p = be;

		if(!rex::Match(uri, rex::PASSTHROUGH) || !pin->url.SetHttpUrl(uri))
		{
			bufferevent_write(be, "HTTP/1.0 403 CONNECT denied (ask the admin to allow HTTPS tunnels)\r\n\r\n"sv);
			return;
		}
		atransport::TConnectParms parms;
		parms.directConnection = true;
		parms.noTlsOnTarget = true;

		atransport::Create(pin->url,
						   [pin] (atransport::tResult res) mutable
		{
			if (!res.err.empty())
				return ebstream(*pin->be) << "HTTP/1.0 502 CONNECT error: "sv << res.err << "\r\n\r\n"sv, void();

			ebstream(*pin->be) << "HTTP/1.0 200 Connection established\r\n\r\n"sv;
			bufferevent_setcb(pin->be.get(), cbRxClient, cbTxClient, cbEvent, pin.get());
			bufferevent_setcb(res.strm->GetBufferEvent(), cbTxClient, cbRxClient, cbEvent, pin.get());
			pin->outStream = move(res.strm);
			setup_be_bidirectional(pin->be.get());
			setup_be_bidirectional(res.strm->GetBufferEvent());
			ignore_ptr(pin.release()); // cbEvent will care
		},
		parms
		);

#warning analyze
		/*
		 *
		 * src/conn.cc:302:2: Potential leak of memory pointed to by 'pin.m_ptr' [clang-analyzer-cplusplus.NewDeleteLeaks]
		 * */
	}
	static void cbRxClient(struct bufferevent *bev, void *)
	{
		evbuffer_add_buffer(bufferevent_get_output(bev), bufferevent_get_input(bev));
	};
	static void cbTxClient(struct bufferevent *bev, void *)
	{
		evbuffer_add_buffer(bufferevent_get_input(bev), bufferevent_get_output(bev));
	}
	static void cbEvent(struct bufferevent *, short what, void *ctx)
	{
		auto pin = as_lptr((TDirectConnector*)ctx);
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
			return; // destroying the handler
		ignore_ptr(pin.release());
	}
};

struct Dispatcher
{
	// simplify the reliable releasing whenver this object is deleted
	unique_bufferevent_fdclosing m_be;
	string clientName;

	Dispatcher(string name) : clientName(name)
	{
	}
	~Dispatcher()
	{

	}
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
		auto rbuf = bufferevent_get_input(pBE);
		header h;
		auto hlen = h.Load(rbuf);
		if (!hlen)
			return; // will come back with more data

		ebstream sout(pBE);
		const auto& tgt = h.getRequestUrl();
		auto flushClose = [&]()
		{
			AppendMetaHeaders(sout);
			be_flush_and_close(me->m_be.release());
		};

		if (hlen < 0) // client sends BS?
		{
			sout << "HTTP/1.0 400 Bad Request\r\n"sv;
			flushClose();
		}
		else if(h.type == header::GET)
		{
			evbuffer_drain(rbuf, hlen);
			connImpl::go(me->m_be.release(), move(h), hlen, move(me->clientName));
		}
		else if (h.type == header::CONNECT)
		{
			evbuffer_drain(rbuf, hlen);
			TDirectConnector::PassThrough(me->m_be.release(), tgt);
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

void StartServingBE(unique_bufferevent_fdclosing&& be, string clientName)
{
	if (!be.valid())
		return; // force-close everything
	auto* dispatcha = new Dispatcher {move(clientName)};
	// excpt.safe
	dispatcha->m_be.reset(be.release());
	if (!dispatcha->Go())
		delete dispatcha; // bad, need to close the socket!
}


void StartServing(unique_fd&& fd, string clientName)
{
	evutil_make_socket_nonblocking(fd.get());
	evutil_make_socket_closeonexec(fd.get());
	// fd ownership moves to bufferevent closer
	unique_bufferevent_fdclosing be(bufferevent_socket_new(evabase::base, fd.release(), BEV_OPT_DEFER_CALLBACKS));
#warning tune watermarks! set timeout!
	return StartServingBE(move(be), move(clientName));
}
}
