
#include "debug.h"
#include "meta.h"
#include "conn.h"
#include "acfg.h"
#include "job.h"
#include "header.h"
#include "dlcon.h"
#include "job.h"
#include "acbuf.h"
#include "tcpconnect.h"
#include "cleaner.h"
#include "lockable.h"
#include "sockio.h"
#include "evabase.h"

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
namespace conserver {
void FinishConnection(int);
}

class conn::Impl
{
	friend class conn;
	conn* _q = nullptr;

	int m_confd;
	bool m_badState = false;

	deque<job> m_jobs2send;

#ifdef KILLABLE
      // to awake select with dummy data
      int wakepipe[2];
#endif

	std::thread m_dlerthr;

	// for jobs
	friend class job;
	bool SetupDownloader();
	std::shared_ptr<dlcon> m_pDlClient;
	mstring m_sClientHost;

	// some accounting
	mstring logFile, logClient;
	off_t fileTransferIn = 0, fileTransferOut = 0;
	bool m_bLogAsError = false;

	std::shared_ptr<IFileItemRegistry> m_itemRegistry;


	void writeAnotherLogRecord(const mstring &pNewFile,
			const mstring &pNewClient);
	// This method collects the logged data counts for certain file.
	// Since the user might restart the transfer again and again, the counts are accumulated (for each file path)
    void LogDataCounts(cmstring &file, std::string xff, off_t countIn,
			off_t countOut, bool bAsError);

#ifdef DEBUG
      unsigned m_nProcessedJobs;
#endif

	Impl(unique_fd fd, mstring c, std::shared_ptr<IFileItemRegistry> ireg) :
		m_sClientHost(c), m_itemRegistry(ireg)
	{
		LOGSTARTx("con::con", fd.get(), c);
#ifdef DEBUG
		m_nProcessedJobs=0;
#endif
		// ok, it's our responsibility now
		m_confd = fd.release();
	};
	~Impl() {
		LOGSTART("con::~con (Destroying connection...)");

		// our user's connection is released but the downloader task created here may still be serving others
		// tell it to stop when it gets the chance and delete it then

		m_jobs2send.clear();

		writeAnotherLogRecord(sEmptyString, sEmptyString);

		if(m_pDlClient)
			m_pDlClient->SignalStop();
		if(m_dlerthr.joinable())
			m_dlerthr.join();
		log::flush();
		// this is not closed here but there, after graceful termination handling
		conserver::FinishConnection(m_confd);
	}

  	void WorkLoop();
};

// call forwarding
conn::conn(unique_fd&& fd, mstring sClient, std::shared_ptr<IFileItemRegistry> ireg) :
	_p(new Impl(move(fd), move(sClient), move(ireg))) { _p->_q = this;}

std::shared_ptr<IFileItemRegistry> conn::GetItemRegistry() { return _p->m_itemRegistry; };
conn::~conn() { delete _p; }
void conn::WorkLoop() {	_p->WorkLoop(); }
void conn::LogDataCounts(cmstring &file, mstring xff, off_t countIn, off_t countOut,
        bool bAsError) {return _p->LogDataCounts(file, move(xff), countIn, countOut, bAsError); }
dlcon* conn::SetupDownloader()
{ return _p->SetupDownloader() ? _p->m_pDlClient.get() : nullptr; }

namespace RawPassThrough
{

#define BDOURL "http://bugs.debian.org:80/"
#define POSTMARK "POST " BDOURL

inline static bool CheckListbugs(const header &ph)
{
	return ph.type == header::POST && startsWithSz(ph.getRequestUrl(), BDOURL);
}
inline static void RedirectBto2https(int fdClient, cmstring& uri)
{
	tSS clientBufOut;
	clientBufOut << "HTTP/1.1 302 Redirect\r\nLocation: https://bugs.debian.org:443/";
	constexpr auto offset = _countof(POSTMARK) - 6;
	clientBufOut.append(uri.c_str() + offset, uri.size() - offset);
	clientBufOut << "\r\nConnection: close\r\n\r\n";
	clientBufOut.send(fdClient);
	// XXX: there is a minor risk of confusing the client if the POST body is bigger than the
	// incoming buffer (probably 64k). But OTOH we shutdown the connection properly, so a not
	// fully stupid client should cope with that. Maybe this should be investigate better.
	return;
}
void PassThrough(acbuf &clientBufIn, int fdClient, cmstring& uri)
{
	tDlStreamHandle m_spOutCon;

	string sErr;
	tSS clientBufOut;
	clientBufOut.setsize(32 * 1024); // out to be enough for any BTS response

	// arbitrary target/port, client cares about SSL handshake and other stuff
	tHttpUrl url;
	if (!url.SetHttpUrl(uri))
		return;
	auto proxy = cfg::GetProxyInfo();

	signal(SIGPIPE, SIG_IGN);

	if (!proxy)
	{
		direct_connect:
		m_spOutCon = g_tcp_con_factory.CreateConnected(url.sHost, url.GetPort(), sErr, 0, 0,
				false, cfg::nettimeout, true);
	}
	else
	{
		// switch to HTTPS tunnel in order to get a direct connection through the proxy
		m_spOutCon = g_tcp_con_factory.CreateConnected(proxy->sHost, proxy->GetPort(),
				sErr, 0, 0, false, cfg::optproxytimeout > 0 ?
						cfg::optproxytimeout : cfg::nettimeout,
						true);

		if (m_spOutCon)
		{
			if (!m_spOutCon->StartTunnel(tHttpUrl(url.sHost, url.GetPort(),
					true), sErr, & proxy->sUserPass, false))
			{
				m_spOutCon.reset();
			}
		}
		else if(cfg::optproxytimeout > 0) // ok... try without
		{
			cfg::MarkProxyFailure();
			goto direct_connect;
		}
	}

	if (m_spOutCon)
		clientBufOut << "HTTP/1.0 200 Connection established\r\n\r\n";
	else
	{
		clientBufOut << "HTTP/1.0 502 CONNECT error: " << sErr << "\r\n\r\n";
		clientBufOut.send(fdClient);
		return;
	}

	if (!m_spOutCon)
		return;

	// for convenience
	int ofd = m_spOutCon->GetFD();
	acbuf &serverBufOut = clientBufIn, &serverBufIn = clientBufOut;

	int maxfd = 1 + std::max(fdClient, ofd);

	while (true)
	{
		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		// can send to client?
		if (clientBufOut.size() > 0)
			FD_SET(fdClient, &wfds);

		// can receive from client?
		if (clientBufIn.freecapa() > 0)
			FD_SET(fdClient, &rfds);

		if (serverBufOut.size() > 0)
			FD_SET(ofd, &wfds);

		if (serverBufIn.freecapa() > 0)
			FD_SET(ofd, &rfds);

		int nReady = select(maxfd, &rfds, &wfds, nullptr, nullptr);
		if (nReady < 0)
			return;

		if (FD_ISSET(ofd, &wfds))
		{
			if (serverBufOut.dumpall(ofd) < 0)
				return;
		}

		if (FD_ISSET(fdClient, &wfds))
		{
			if (clientBufOut.dumpall(fdClient) < 0)
				return;
		}

		if (FD_ISSET(ofd, &rfds))
		{
			if (serverBufIn.sysread(ofd) <= 0)
				return;
		}

		if (FD_ISSET(fdClient, &rfds))
		{
			if (clientBufIn.sysread(fdClient) <= 0)
				return;
		}
	}
	return;
}
}

void conn::Impl::WorkLoop() {

	LOGSTART("con::WorkLoop");

	signal(SIGPIPE, SIG_IGN);

#ifdef DEBUG
	tDtorEx defuseGuards([this, &__logobj]()
	{
		for(auto& j: m_jobs2send)
		{
			LOG("FIXME: disconnecting while job not processed");
			j.Dispose();
		}
	});
#endif

	acbuf inBuf;
	inBuf.setsize(32*1024);

	// define a much shorter timeout than network timeout in order to be able to disconnect bad clients quickly
	auto client_timeout(GetTime() + cfg::nettimeout);

	int maxfd=m_confd;
	while(!evabase::in_shutdown && !m_badState) {
		fd_set rfds, wfds;
		FD_ZERO(&wfds);
		FD_ZERO(&rfds);

		FD_SET(m_confd, &rfds);
		if(inBuf.freecapa()==0)
			return; // shouldn't even get here

		bool hasMoreJobs = m_jobs2send.size()>1;

		if ( !m_jobs2send.empty()) FD_SET(m_confd, &wfds);

		ldbg("select con");
		int ready = select(maxfd+1, &rfds, &wfds, nullptr, CTimeVal().For(SHORT_TIMEOUT));

		if(evabase::in_shutdown)
			break;

		if(ready == 0)
		{
			if(GetTime() > client_timeout)
			{
				USRDBG("Timeout occurred, apt client disappeared silently?");
				return; // yeah, time to leave
			}
			continue;
		}
		else if (ready<0)
		{
			if (EINTR == errno)
				continue;

			ldbg("select error in con, errno: " << errno);
			return; // FIXME: good error message?
		}
		else
		{
			// ok, something is still flowing, increase deadline
			client_timeout = GetTime() + cfg::nettimeout;
		}

		ldbg("select con back");

		if(FD_ISSET(m_confd, &rfds))
		{
			int n=inBuf.sysread(m_confd);
			ldbg("got data: " << n <<", inbuf size: "<< inBuf.size());
			if(n<=0) // error, incoming junk overflow or closed connection
			{
				if(n==-EAGAIN)
					continue;
				else
				{
					ldbg("client closed connection");
					return;
				}
			}
        }
        header h;
		// split new data into requests
		while (inBuf.size() > 0)
		{
			try
			{
                h.clear();
                int nHeadBytes = h.Load(inBuf.view());
				ldbg("header parsed how? " << nHeadBytes);
				if(nHeadBytes == 0)
				{ // Either not enough data received, or buffer full; make space and retry
					inBuf.move();
					break;
				}
				if(nHeadBytes < 0)
				{
					ldbg("Bad request: " << inBuf.rptr() );
					return;
				}

				// also must be identified before
				if (h.type == header::POST)
				{
					if (cfg::forwardsoap && !m_sClientHost.empty())
					{
						if (RawPassThrough::CheckListbugs(h))
						{
							RawPassThrough::RedirectBto2https(m_confd, h.getRequestUrl());
						}
						else
						{
							ldbg("not bugs.d.o: " << inBuf.rptr());
						}
						// disconnect anyhow
						return;
					}
					ldbg("not allowed POST request: " << inBuf.rptr());
					return;
				}

				if(h.type == header::CONNECT)
				{
					const auto& tgt = h.getRequestUrl();
					inBuf.drop(nHeadBytes);
					if(rex::Match(tgt, rex::PASSTHROUGH))
						RawPassThrough::PassThrough(inBuf, m_confd, tgt);
					else
					{
						tSS response;
						response << "HTTP/1.0 403 CONNECT denied (ask the admin to allow HTTPS tunnels)\r\n\r\n";
						response.dumpall(m_confd);
					}

					return;
				}

				if (m_sClientHost.empty()) // may come from wrapper... MUST identify itself
				{

					inBuf.drop(nHeadBytes);

					if(h.h[header::XORIG] && * h.h[header::XORIG])
					{
						m_sClientHost=h.h[header::XORIG];
						continue; // OK
					}
					else
						return;
				}

				ldbg("Parsed REQUEST: " << h.type << " " << h.getRequestUrl());
				ldbg("Rest: " << (inBuf.size()-nHeadBytes));
                m_jobs2send.emplace_back(*_q);
				m_jobs2send.back().Prepare(h, inBuf.view(), m_sClientHost);
				if (m_badState)
					return;
				inBuf.drop(nHeadBytes);
#ifdef DEBUG
				m_nProcessedJobs++;
#endif
			}
			catch(const bad_alloc&)
			{
				return;
			}
		}

		if(inBuf.freecapa()==0)
			return; // cannot happen unless being attacked

		if(FD_ISSET(m_confd, &wfds) && !m_jobs2send.empty())
		{
			try
			{
				switch(m_jobs2send.front().SendData(m_confd, hasMoreJobs))
				{
				case(job::R_DISCON):
				{
					ldbg("Disconnect advise received, stopping connection");
					return;
				}
				case(job::R_DONE):
				{
#ifdef DEBUG
					m_jobs2send.front().Dispose();
#endif
					m_jobs2send.pop_front();

					ldbg("Remaining jobs to send: " << m_jobs2send.size());
					break;
				}
				case(job::R_AGAIN):
				default:
					break;
				}
			}
			catch(...)
			{
				return;
			}
		}
	}
}

bool conn::Impl::SetupDownloader()
{
	if(m_badState)
		return false;

	if (m_pDlClient)
		return true;

	try
	{
		m_pDlClient = dlcon::CreateRegular(g_tcp_con_factory);
		if(!m_pDlClient)
			return false;
		auto pin = m_pDlClient;
		m_dlerthr = thread([pin]()
		{
			pin->WorkLoop();
		});
		m_badState = false;
		return true;
	}
	catch(...)
	{
		m_badState = true;
		m_pDlClient.reset();
		return false;
	}
}

void conn::Impl::LogDataCounts(cmstring & sFile, mstring xff, off_t nNewIn,
		off_t nNewOut, bool bAsError)
{
	string sClient;
    if (!cfg::logxff || xff.empty()) // not to be logged or not available
		sClient=m_sClientHost;
    else if (!xff.empty())
	{
        sClient=move(xff);
		trimBoth(sClient);
		auto pos = sClient.find_last_of(SPACECHARS);
		if (pos!=stmiss)
			sClient.erase(0, pos+1);
	}
	if(sFile != logFile || sClient != logClient)
		writeAnotherLogRecord(sFile, sClient);
	fileTransferIn += nNewIn;
	fileTransferOut += nNewOut;
	if(bAsError) m_bLogAsError = true;
}

// sends the stats to logging and replaces file/client identities with the new context
void conn::Impl::writeAnotherLogRecord(const mstring &pNewFile, const mstring &pNewClient)
{
		log::transfer(fileTransferIn, fileTransferOut, logClient, logFile, m_bLogAsError);
		fileTransferIn = fileTransferOut = 0;
		m_bLogAsError = false;
		logFile = pNewFile;
		logClient = pNewClient;
}

}
