//#include <netinet/in.h>
//#include <netdb.h>

#include "dlcon.h"
#include "debug.h"

#include "aconnect.h"
#include "fileitem.h"
#include "acfg.h"
#include "meta.h"
#include "remotedb.h"
#include "acbuf.h"
#include "aevutil.h"
#include "atransport.h"
#include "fileitem.h"
#include "fileio.h"
#include "sockio.h"
#include "evabase.h"
#include "aclock.h"

#include <algorithm>
#include <list>
#include <map>
#include <regex>
#include <unordered_map>
#include <stdexcept>
#include <queue>

#include <unistd.h>
#include <sys/time.h>
#include <event2/bufferevent.h>

using namespace std;

// evil hack to simulate random disconnects
//#define DISCO_FAILURE

#define MAX_RETRY cfg::dlretriesmax

#warning fixme, control via config
#define REQUEST_LIMIT 5
#define MAX_STREAMS_PER_USER 8

namespace acng
{

static cmstring sGenericError("502 Bad Gateway");
static const fileitem::tSpecialPurposeAttr g_dummy_attributes;

class CDlConn;
struct tDlJob;
struct tDlStream;
using tDlStreamPool = list<tDlStream>;
#define MOVE_FRONT_THERE_TO_BACK_HERE(there, here) here.splice(here.end(), there, there.begin())
static const struct timeval timeout_asap{0,0};

enum class eJobResult
{
	HINT_MORE=0
	,HINT_DONE=1
	,HINT_RECONNECT=2
	,JOB_BROKEN=4
	,MIRROR_BROKEN=8
	,MIRROR_BROKEN_KILL_LAST_FILE=16
};

/**
 * @brief Shared helper to validate source quality and correct if possible
 * For checks whether the remote source was validated (and is still valid) or was reported as broken, and how broken (and if needed, remembers the remote quality in the internal stats).
 */

struct tRemoteValidator
{
	struct tState
	{
		int errCount = 0;
		mstring reason;
	};
	int CurrentRevision() { return changeStamp; }
	tState* RegisterError(const tHttpUrl& hoPoKey, cmstring& m_sError, bool fatal)
	{
		auto& pKnownIssues = m_problemCounters[hoPoKey.GetHostPortProtoKey()];
		setIfNotEmpty(pKnownIssues.reason, m_sError);
		pKnownIssues.errCount += 1 + MAX_RETRY * fatal;
		changeStamp--;
		return & pKnownIssues;
	}
  tState* GetEntry(const tHttpUrl& hoPo)
  {
          auto it = m_problemCounters.find(hoPo.GetHostPortProtoKey());
          return it != m_problemCounters.end()
                  ? & it->second
                  : nullptr;
  }

	private:
	map<string,tState> m_problemCounters;
  // megative numbers are validator revision, positive are errors, 0 is initial state
	int changeStamp = -1;

};

using tDlJobPtr = unique_ptr<tDlJob>;

// custom priority queue which is transparent for special uses and also returns the popped element directly
struct tDlJobPrioQ
{
public:
	using T = tDlJobPtr;
	struct tCompJobPtrById
	{
		bool operator() (const T &a, const T &b) const;
	} m_comparator;

	inline void push(T&& element)
	{
		m_data.push_back(move(element));
		push_heap(m_data.begin(), m_data.end(), m_comparator);
	}
	inline T pop()
	{
		std::pop_heap(m_data.begin(), m_data.end(), m_comparator);
		auto ret = move(m_data.back());
		m_data.pop_back();
		return ret;
	}
	inline const T& top() { return m_data.at(0); }
	inline bool empty() { return m_data.empty(); }
	inline bool size() { return m_data.size(); }
	void clear() { m_data.clear(); }
	inline void swap(tDlJobPrioQ& other) { other.m_data.swap(m_data); }
private:
	std::vector<T> m_data;

};

struct tDlStream : public tLintRefcounted
{
	deque<tDlJobPtr> m_requested;
	tDlJobPrioQ m_waiting;
	lint_ptr<atransport> m_transport;
	tFileItemPtr m_memberLastItem;

	TFinalAction m_connectionToken;
	aobservable::subscription m_blockingItemSubscription;
	/**
	 * @brief m_dirty adds additional layer of safety, extra flag to mark a tainted state of the transport
	 */
	bool m_dirty = false;
	bool m_sacrificied = false;
	bool m_bWasRecycled = false;

	time_t m_idleSince = END_OF_TIME;

	tDlStream(const tHttpUrl& targetInfo) : m_targetInfo(targetInfo) {}

	tStrMap& GetBlackList();

	void Start(CDlConn* parent, tDlStreamPool::iterator meRef)
	{
		m_parent = parent;
		m_meRef = meRef;
	}
	bool Cancel()
	{
		m_waiting.clear();
		return m_requested.empty();
	}

	void Connect();

	static void cbStatus(bufferevent* pBE, short what, void* ctx)  { ((tDlStream*)ctx)->OnStatus(pBE, what); }
	static void cbRead(bufferevent* pBE, void* ctx) { ((tDlStream*)ctx)->OnRead(pBE); }
	void OnRead(bufferevent* pBE);
	void OnStatus(bufferevent* pBE, short what);
	/**
	 * @brief TakeJob accepts a job but only if that doesn't overtake an already requested one
	 * @return
	 */
	bool TakeJob(tDlJobPtr&&);
	const tHttpUrl& GetPeerHost() { return m_targetInfo; }

	void Subscribe2BlockingItem(tFileItemPtr fi)
	{
		m_blockingItemSubscription = fi->Subscribe([this]()
		{
			if (m_transport->GetBufferEvent() && !m_requested.empty())
				OnRead(m_transport->GetBufferEvent());
		});
	}

	~tDlStream();
	/**
	 * @brief GetRemainingBodyBytes returns expected remaining length of the currently active item.
	 * @return
	 */
	off_t GetRemainingBodyBytes();

	/**
	 *  TODO:
	 *  mark dirty
	 *  move all backlog stuff to parent backlog
	 *  decrease the retry count of the item since we sabotage it explicitly
	 *  set a timer to wait a couple of seconds, if not finished then, send a shutdown() on the socket. XXX: needs to track lifetime. Also parent needs to know which stream is being sabotaged. I.e. parent shall handle the timing. In the dtor, check whether we were sabotaged and do not send error to the item.
	 * @brief Sacrifice
	 */
	void Sacrifice();

private:
	CDlConn* m_parent;
	tDlStreamPool::iterator m_meRef;
	tHttpUrl m_targetInfo;

	/**
	 * @brief Send more remote requests if possible, and if not, make sure that continuation of the request will happen.
	 */
	void tryRequestMore();
	void wireTransport(lint_ptr<atransport>&& result);

	void handleDisconnect(string_view why, tComError cause);

	//unsigned m_requestLimit = DEFAULT_REQUEST_LIMIT;
	// shutdown condition where further request data is dropped and connection is closed ASAP
	//bool m_bShutdownLosingData = false;
};

const timeval idleCheckInterval { 7, 345678 };

class CDlConn : public dlcontroller, public tClock
{
	acres& m_res;

	unsigned m_nJobIdgen = 0;
//	unsigned m_nIdleCount = 0;
	bool m_bInShutdown = false, m_bDispatchPending = false;
	// self-reference which is set when the shutdown phase starts and there are external users of some of the served items
	lint_ptr<dlcontroller> m_shutdownLock;
	tRemoteValidator m_validator;

	friend struct tDlJob;
	// queue for jobs which are blocked until a new stream can handle them
	tDlJobPrioQ m_backlog;
	unique_event m_dispatchNotifier;

	tDlStreamPool m_streams;

	tDlStreamPool::iterator m_lastUsedStream = m_streams.end();

public:

	CDlConn(acres& res_) : tClock(idleCheckInterval), m_res(res_)
	{
		LOGSTARTFUNC;
		ASSERT_IS_MAIN_THREAD;
		m_dispatchNotifier.reset(evtimer_new(evabase::base, cbDispatchBacklog, this));
	}
	~CDlConn()
	{
		LOGSTARTFUNC;
		ASSERT_IS_MAIN_THREAD;
	}
	tRemoteValidator& GetValidator() { return m_validator; }
	acres& GetAppRes() { return m_res; }

	decltype (m_streams)::iterator AddStream(const tHttpUrl& tgt)
	{
		m_streams.emplace_back(tgt);
		auto refIt = m_streams.end();
		--refIt;
		m_streams.back().Start(this, refIt);
		return refIt;
	}

	void DeleteStream(tDlStreamPool::iterator what)
	{
		LOGSTARTFUNC;
		ASSERT_IS_MAIN_THREAD;
		if (m_lastUsedStream == what)
			m_lastUsedStream = m_streams.end();
		m_streams.erase(what);
		TermOrProcBacklog();
	}

	/**
	 * @brief TerminateAsNeeded
	 *
	 * Releases the self-lock but only after all streams were finished.
	 * This is aligned with Abandon() activity.
	 *
	 * @return True if shutdown was initiated or performed.
	 */
	bool TermUnlockIfPossible()
	{
		if (!m_bInShutdown)
			return false;
		if (!m_streams.empty() || !m_backlog.empty())
			return false;
		// potentially destroyting *this
		m_shutdownLock.reset();
		return true;
	}
	void ProcessBacklog()
	{
		if (m_streams.size() >= MAX_STREAMS_PER_USER)
			return; // pointless, wait for a free slot

		if (!m_backlog.empty())
			TriggerDispatch();
	}
	/**
	 * @brief TermOrProcBacklog
	 * If terminating scheduled, release this agent.
	 * @return True if shutdown is ongoing
	 */
	bool TermOrProcBacklog()
	{
		if (TermUnlockIfPossible())
			return true;
		ProcessBacklog();
		return false;
	}

	void TermOrProcBacklog(tDlStreamPool::iterator idlingReporter)
	{
		if (m_bInShutdown || evabase::GetGlobal().IsShuttingDown())
		{
			if (m_lastUsedStream == idlingReporter)
				m_lastUsedStream = m_streams.end();
			m_streams.erase(idlingReporter);

		}
		TermOrProcBacklog();
	}

	/**
	 * @brief Initiate a graceful shutdown, preparing for destruction but collaborating with potential external users.
	 */
	void Abandon() override
	{
		ASSERT_IS_MAIN_THREAD;
		m_bInShutdown = true;
		m_backlog.clear();

		m_lastUsedStream = m_streams.end();

		// try to abort all streams; if some are busy, self-lock and wait for the stream to report it's termination
		for (auto it = m_streams.begin(); it != m_streams.end();)
		{
			if (it->Cancel())
				it = m_streams.erase(it);
			else
				++it;
		}
		if (m_streams.empty())
			return;
		// okay, will be torn down by the stream later
		m_shutdownLock.reset(this);
	};

	void TeardownASAP() override
	{
		m_bInShutdown = true;
		OnClockTimeout();
		m_backlog.clear();
		m_streams.clear();
	}

	void DispatchDfrd(tDlJobPtr&& what, tDlJobPrioQ& unhandled);
	static void cbDispatchBacklog(evutil_socket_t, short, void *);

	bool AddJob(lint_ptr<fileitem> fi, const tHttpUrl* src, tRepoResolvResult* repoSrc, bool isPT, mstring extraHeaders) override;
	void TriggerDispatch();
	bool ToBacklog(tDlJobPtr&& j);

	// tClock interface
public:
	void OnClockTimeout() override
	{
		auto thold = GetTime() - idleCheckInterval.tv_sec;

		for (auto it = m_streams.begin(); it != m_streams.end();)
		{
			if (it->m_idleSince < thold)
			{
				if (it == m_lastUsedStream)
					m_lastUsedStream = m_streams.end();

				it = m_streams.erase(it);
			}
			else
				++it;
		}
	}
};

lint_user_ptr<dlcontroller> dlcontroller::CreateRegular(acres& res)
{
	return lint_user_ptr<dlcontroller>(new CDlConn(res));
}

struct tDlJob
{
	tFileItemPtr m_pStorageRef;
	mstring m_sError;

	string m_extraHeaders;

#warning memory waste, use a union
	tHttpUrl m_remoteUri;	
	const tRepoData *m_pRepoDesc = nullptr;
	const tHttpUrl *m_pCurBackend = nullptr;

	// input parameters
	bool m_bIsPassThroughRequest = false;
	bool m_bAllowStoreData = true;
	bool m_bFileItemAssigned = false;
	bool m_bConnectionClose = false;
	bool m_bRevalidationNeeded = false;

	int m_nRedirRemaining = cfg::redirmax;

	const fileitem::tSpecialPurposeAttr& GetFiAttributes() { return m_pStorageRef ? m_pStorageRef->m_spattr : g_dummy_attributes; }
	unsigned GetId() { return m_orderId;}

	// state attribute
	off_t m_nRest = 0;
	off_t m_nWatermark = 0;

	// flag to use ranges and also define start if >= 0
	off_t m_nUsedRangeStartPos = -1;
	off_t m_nUsedRangeLimit = -1;

	const unsigned m_orderId;

	enum EStreamState : uint8_t
	{
		STATE_GETHEADER,
		STATE_PROCESS_DATA,
		STATE_GETCHUNKHEAD,
		STATE_PROCESS_CHUNKDATA,
		STATE_GET_CHUNKEND,
		STATE_GET_CHUNKEND_LAST,
		STATE_FINISHJOB
	}
	m_DlState = EStreamState::STATE_GETHEADER;

	/**
	 * @brief m_sourceState remembers the quality of source information or error if positive value
	 */
	int m_sourceState = 0;

	enum class EResponseEval
	{
		GOOD, BUSY_OR_ERROR, RESTART_NEEDED
	};

	inline bool HasBrokenStorage()
	{
		return (!m_pStorageRef || m_pStorageRef->GetStatus() > fileitem::FIST_COMPLETE);
	}

	/*!
	 * Returns a reference to http url where host and port and protocol match the current host
	 * Other fields in that member have undefined contents. ;-)
	 */
	const tHttpUrl& GetPeerHost()
	{
		return m_pCurBackend ? *m_pCurBackend : m_remoteUri;
	}

	const tHttpUrl* GetJobProxyInfo()
	{
		return m_pRepoDesc && m_pRepoDesc->m_pProxy ? m_pRepoDesc->m_pProxy : cfg::GetProxyInfo();
	}

	inline tRepoUsageHooks* GetConnStateTracker()
	{
		return m_pRepoDesc ? m_pRepoDesc->m_pHooks : nullptr;
	}

	inline tDlJob(uint_fast32_t id, const tFileItemPtr& pFi, const tHttpUrl* src, tRepoResolvResult* repoSrc, bool isPT, mstring extraHeaders) :
		m_pStorageRef(pFi),
		m_extraHeaders(move(extraHeaders)),
		m_bIsPassThroughRequest(isPT),
		m_orderId(id)
	{
		ASSERT_IS_MAIN_THREAD;

		ASSERT(m_pStorageRef);
		m_pStorageRef->DlRefCountAdd();

		if (src)
			m_remoteUri = move(*src);
		else if (repoSrc && repoSrc->valid())
		{
			m_remoteUri.sPath = move(repoSrc->sRestPath);
			m_pRepoDesc = move(repoSrc->repodata);
			m_pCurBackend = & m_pRepoDesc->m_backends.front();
		}
		else
			throw std::invalid_argument(to_string("Exactly one source needs to be valid"sv));

		if (pFi->GetStatus() < fileitem::FIST_DLPENDING)
			pFi->m_status = fileitem::FIST_DLPENDING;
	}

	// Default move ctor is ok despite of pointers, we only need it in the beginning, list-splice operations should not move the object around
	tDlJob(tDlJob &&other) = default;

	enum class eResponsibility : int8_t
	{
		IRRELEVANT = -1,
		UNDECIDED, // maybe we start and end it, maybe someone else
		US,
		NOT_US
	};
	/**
	 * @brief WhoIsResponsible
	 * Which agent takes care about the final processing of that item?
	 * @return
	 */
	eResponsibility WhoIsResponsible()
	{
		if (AC_UNLIKELY(!m_pStorageRef))
			return eResponsibility::UNDECIDED;
		auto fist = m_pStorageRef->GetStatus();
		if (fist < fileitem::FIST_DLASSIGNED)
			return eResponsibility::UNDECIDED;
		if (fist >= fileitem::FIST_COMPLETE)
			return eResponsibility::IRRELEVANT;
		return m_bFileItemAssigned ? eResponsibility::US : eResponsibility::NOT_US;
	}

	~tDlJob()
	{
		LOGSTART("tDlJob::~tDlJob");
		if (m_pStorageRef)
			m_pStorageRef->DlRefCountDec(503, m_sError.empty() ? "DL aborted"sv : m_sError);
	}

	inline string RemoteUri(bool bUrlEncoded)
	{
		if (m_pCurBackend)
		{
			return m_pCurBackend->ToURI(bUrlEncoded)
					+ (bUrlEncoded ?
							UrlEscape(m_remoteUri.sPath) : m_remoteUri.sPath);
		}
		return m_remoteUri.ToURI(bUrlEncoded);
	}

	inline bool RewriteSource(const char *pNewUrl)
	{
		LOGSTART("tDlJob::RewriteSource");
		if (--m_nRedirRemaining <= 0)
		{
			m_sError = "Redirection loop";
			return false;
		}

		if (!pNewUrl || !*pNewUrl)
		{
			m_sError = "Bad redirection";
			return false;
		}

		// start modifying the target URL, point of no return
		m_pCurBackend = nullptr;
		bool bWasBeMode = m_pRepoDesc;
		m_pRepoDesc = nullptr;

		auto sLocationDecoded = UrlUnescape(pNewUrl);

		tHttpUrl newUri;
		if (newUri.SetHttpUrl(sLocationDecoded, false))
		{
			dbgline;
			m_remoteUri = newUri;
			return true;
		}
		// ok, some protocol-relative crap? let it parse the hostname but keep the protocol
		if (startsWithSz(sLocationDecoded, "//"))
		{
			stripPrefixChars(sLocationDecoded, "/");
			return m_remoteUri.SetHttpUrl(
					m_remoteUri.GetProtoPrefix() + sLocationDecoded);
		}

		// recreate the full URI descriptor matching the last download
		if (bWasBeMode)
		{
			if (!m_pCurBackend)
			{
				m_sError = "Bad redirection target";
				return false;
			}
			auto sPathBackup = m_remoteUri.sPath;
			m_remoteUri = *m_pCurBackend;
			m_remoteUri.sPath += sPathBackup;
		}

		if (startsWithSz(sLocationDecoded, "/"))
		{
			m_remoteUri.sPath = sLocationDecoded;
			return true;
		}
		// ok, must be relative
		m_remoteUri.sPath += (sPathSepUnix + sLocationDecoded);
		return true;
	}

	bool SetupSource(tRemoteValidator& validator)
	{
		LOGSTARTFUNC;

		if (m_sourceState == validator.CurrentRevision())
			return true;

		auto* pKnownIssues = m_sourceState > 0 
			? validator.RegisterError(GetPeerHost(), m_sError, IS_STREAM_FATAL_ERROR(m_sourceState))
            : validator.GetEntry(GetPeerHost());

		m_sourceState = validator.CurrentRevision();

		// try alternative backends?
		if (m_pRepoDesc)
		{
			ldbg(m_pRepoDesc->m_backends.front().sHost);

			while (pKnownIssues && pKnownIssues->errCount >= MAX_RETRY)
			{
				if (++m_pCurBackend > &m_pRepoDesc->m_backends.back())
				{
					setIfNotEmpty(m_sError, pKnownIssues->reason);
					LOGRET(false);
				}
				pKnownIssues = validator.GetEntry(GetPeerHost());
			}
		}
		else if(pKnownIssues && pKnownIssues->errCount >= MAX_RETRY)
		{
			setIfNotEmpty(m_sError, pKnownIssues->reason);
			LOGRET(false);
		}
		LOGRET(true);
	}

	// needs connectedHost, blacklist, output buffer from the parent, proxy mode?
	inline void AppendRequest(evbuffer* outBuf, const tHttpUrl *proxy)
	{
		LOGSTARTFUNC;

		ebstream head(outBuf);

#define CRLF "\r\n"

		if (GetFiAttributes().bHeadOnly)
		{
			head << "HEAD ";
			m_bAllowStoreData = false;
		}
		else
		{
			head << "GET ";
			m_bAllowStoreData = true;
		}

		if (proxy)
			head << RemoteUri(true);
		else // only absolute path without scheme
		{
			if (m_pCurBackend) // base dir from backend definition
				head << UrlEscape(m_pCurBackend->sPath);

			head << UrlEscape(m_remoteUri.sPath);
		}

		ldbg(RemoteUri(true));

		head << " HTTP/1.1" CRLF
			 << cfg::agentheader
			 << "Host: " << GetPeerHost().sHost << CRLF;

		if (proxy) // proxy stuff, and add authorization if there is any
		{
			ldbg("using proxy");
			if (!proxy->sUserPass.empty())
			{
				// could cache it in a static string but then again, this makes it too
				// easy for the attacker to extract from memory dump
				head << "Proxy-Authorization: Basic "
						<< EncodeBase64Auth(proxy->sUserPass) << CRLF;
			}
			// Proxy-Connection is a non-sensical copy of Connection but some proxy
			// might listen only to this one so better add it
			head << (cfg::persistoutgoing ?
						 "Proxy-Connection: keep-alive" CRLF :
						 "Proxy-Connection: close" CRLF);
		}

		const auto &pSourceHost = GetPeerHost();
		if (!pSourceHost.sUserPass.empty())
		{
			head << "Authorization: Basic "
					<< EncodeBase64Auth(pSourceHost.sUserPass) << CRLF;
		}

		m_nUsedRangeStartPos = -1;

		m_nUsedRangeStartPos = m_pStorageRef->m_nSizeChecked >= 0 ?
				m_pStorageRef->m_nSizeChecked : m_pStorageRef->m_nSizeCachedInitial;

		if (AC_UNLIKELY(m_nUsedRangeStartPos < -1))
			m_nUsedRangeStartPos = -1;

		/*
		 * Validate those before using them, with extra caution for ranges with date check
		 * on volatile files. Also make sure that Date checks are only used
		 * in combination with range request, otherwise it doesn't make sense.
		 */
		if (GetFiAttributes().bVolatile)
		{
			if (cfg::vrangeops <= 0)
			{
				m_nUsedRangeStartPos = -1;
			}
			else if (m_nUsedRangeStartPos == m_pStorageRef->m_nContentLength
					&& m_nUsedRangeStartPos > 1)
			{
				m_nUsedRangeStartPos--; // the probe trick
			}

			if (!m_pStorageRef->m_responseModDate.isSet()) // date unusable but needed for volatile files?
				m_nUsedRangeStartPos = -1;
		}

		m_nUsedRangeLimit = GetFiAttributes().nRangeLimit;

		if (m_nUsedRangeLimit >= 0)
		{
			if(m_nUsedRangeStartPos < 0)
			{
				m_nUsedRangeLimit = 0;
			}
			else if(AC_UNLIKELY(m_nUsedRangeLimit < m_nUsedRangeStartPos))
			{
				// must be BS, fetch the whole remainder!
				m_nUsedRangeLimit = -1;
			}
		}

		if (m_nUsedRangeStartPos > 0)
		{
			if (m_pStorageRef->m_responseModDate.isSet())
				head << "If-Range: " << m_pStorageRef->m_responseModDate.view() << CRLF;
			head << "Range: bytes=" << m_nUsedRangeStartPos << "-";
			if (m_nUsedRangeLimit > 0)
				head << m_nUsedRangeLimit;
			head << CRLF;
		}

		if (m_pStorageRef->IsVolatile())
		{
			head << "Cache-Control: " /*no-store,no-cache,*/ "max-age=0" CRLF;
		}

		head << cfg::requestapx << m_extraHeaders
			 << "Accept: application/octet-stream" CRLF;
		if (!m_bIsPassThroughRequest)
		{
			head << "Accept-Encoding: identity" CRLF
					"Connection: " << (cfg::persistoutgoing ?
										   "keep-alive" CRLF : "close" CRLF);

		}
		head << CRLF;
#ifdef SPAM
	//head.syswrite(2);
#endif
	}

	bool ValidateTargetConnection(atransport& tr)
	{
		// explicite hit? Perfect
		if (GetPeerHost().EqualHostPortProto(tr.GetTargetHost()))
			return true;

		// consider using a shared proxy, does it suit our job?
		const auto* trProxy = tr.GetUsedProxy();
		const auto* jobProxy = GetJobProxyInfo();
		return trProxy && jobProxy && !atransport::IsProxyNowBroken() && trProxy == jobProxy;
	}

	/*!
	 *
	 * Process new incoming data and write it down to disk or other receivers.
	 */
	eJobResult ProcessIncomming(bufferevent* peBuf, tDlStream& parent)
	{
		LOGSTARTFUNC;
		ASSERT_IS_MAIN_THREAD;
		if (AC_UNLIKELY(!m_pStorageRef || !peBuf))
		{
			m_sError = "Bad cache item";
			return eJobResult::JOB_BROKEN;
		}

		// various views depending on the purpose
		beconsum inBuf(peBuf);
		auto pBuf(bereceiver(peBuf));

		for (;;) // returned by explicit error (or get-more) return
		{
			ldbg("switch: " << (int)m_DlState);
			switch(m_DlState)
			{
			case STATE_GETHEADER:
			{
				ldbg("STATE_GETHEADER");
				header h;
				if (inBuf.size() == 0)
					return eJobResult::HINT_MORE;

				dbgline;

				auto hDataLen = h.Load(pBuf);
				// XXX: find out why this was ever needed; actually we want the opposite,
				// download the contents now and store the reported file as XORIG for future
				// https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Location
				if (0 == hDataLen)
					return eJobResult::HINT_MORE;
				if (hDataLen < 0)
				{
					/*
					 * The previous message in the stream might be a chunked one, and might have trailer fields,
					 * and since the trailer fields don't have a final termination mark, they might appear fused
					 * with the start of a new response. Therefore: drop them, we don't use them.
					 */
					while (true)
					{
						auto sres = evbuffer_search_eol(pBuf, nullptr, nullptr, evbuffer_eol_style::EVBUFFER_EOL_CRLF_STRICT);
						if (sres.pos < 0) // no lines?
							return eJobResult::HINT_MORE;
						if (sres.pos == 0) // at start?
							break; // bad data
						auto view = inBuf.linear(sres.pos);
						if (view.starts_with("HTTP/"sv))
						{
							// reparse the header!
							hDataLen = h.Load(pBuf);
							if (0 == hDataLen)
								return eJobResult::HINT_MORE;
							// okay, situation resolved
							goto TRAILER_JUNK_SKIPPED;
						}
						else
							inBuf.drop(sres.pos + 2);
					}

					m_sError = "Invalid header";
					LOG(m_sError);
					// can be followed by any junk... drop that mirror, previous file could also contain bad data
					return eJobResult::MIRROR_BROKEN_KILL_LAST_FILE;
				}
				else
				{
					ldbg("header: " << inBuf.front(hDataLen));
				}

TRAILER_JUNK_SKIPPED:

				if (h.type != header::ANSWER)
				{
					dbgline;
					m_sError = "Unexpected response type";
					// smells fatal...
					return eJobResult::MIRROR_BROKEN;
				}
				dbgline;

				auto pCon = h.h[header::CONNECTION];
				if (!pCon)
					pCon = h.h[header::PROXY_CONNECTION];

				if (pCon && 0 == strcasecmp(pCon, "close"))
				{
					ldbg("Peer wants to close connection after request");
					m_bConnectionClose = true;
				}

				// processing hint 102, or something like 103 which we can ignore
				if (h.getStatusCode() < 200)
				{
					inBuf.drop(size_t(hDataLen));
					return eJobResult::HINT_MORE;
				}

				// internal redirection might be disabled
				if (cfg::redirmax && h.getStatus().isRedirect())
				{
					if (!RewriteSource(h.h[header::LOCATION]))
						return eJobResult::JOB_BROKEN;

					// drop the redirect page contents if possible so the outer loop
					// can scan other headers
					off_t contLen = atoofft(h.h[header::CONTENT_LENGTH], 0);
					if (contLen <= (off_t) inBuf.size())
						inBuf.drop(contLen);
#warning FIXME, check better error handling
					return eJobResult::HINT_RECONNECT; // no other flags, caller will evaluate the state
				}

				// explicitly blacklist mirror if key file is missing
				if (h.getStatusCode() >= 400 && m_pRepoDesc && m_remoteUri.sHost.empty())
				{
					for (const auto &kfile : m_pRepoDesc->m_keyfiles)
					{
						if (endsWith(m_remoteUri.sPath, kfile))
						{
							m_sError = "Keyfile N/A, mirror blacklisted";
							return eJobResult::MIRROR_BROKEN;
						}
					}
				}

				off_t contentLength = atoofft(h.h[header::CONTENT_LENGTH], -1);

				if (GetFiAttributes().bHeadOnly)
				{
					dbgline;
					m_DlState = STATE_FINISHJOB;
				}
				else if (h.h[header::TRANSFER_ENCODING]
						 && 0
						 == strcasecmp(
							 h.h[header::TRANSFER_ENCODING],
							 "chunked"))
				{
					dbgline;
					m_DlState = STATE_GETCHUNKHEAD;
					h.del(header::TRANSFER_ENCODING); // don't care anymore
				}
				else if (contentLength < 0)
				{
					dbgline;
					m_sError = "Missing Content-Length";
					return eJobResult::JOB_BROKEN;
				}
				else
				{
					dbgline;
					// may support such endless stuff in the future but that's too unreliable for now
					m_nRest = contentLength;
					m_DlState = STATE_PROCESS_DATA;
				}

				// detect bad auto-redirectors (auth-pages, etc.) by the mime-type of their target
				if (cfg::redirmax && !cfg::badredmime.empty()
						&& cfg::redirmax != m_nRedirRemaining
						&& h.h[header::CONTENT_TYPE]
						&& strstr(h.h[header::CONTENT_TYPE],
								  cfg::badredmime.c_str())
						&& h.getStatusCode() < 300) // contains the final data/response
				{
					if (m_pStorageRef->IsVolatile())
					{
						// volatile... this is still ok, just make sure time check works next time
						h.set(header::LAST_MODIFIED, FAKEDATEMARK);
					}
					else
					{
						// this was redirected and the destination is BAD!
						h.setStatus(501, "Redirected to invalid target");
					}
				}

				// ok, can pass the data to the file handler
				auto storeResult = CheckAndSaveHeader(move(h),
													  peBuf, hDataLen, contentLength);
				inBuf.drop(size_t(hDataLen));

				if (m_pStorageRef && m_pStorageRef->m_spattr.bHeadOnly)
					m_nRest = 0;

				if (storeResult == EResponseEval::RESTART_NEEDED)
					return eJobResult::HINT_RECONNECT; // recoverable

				if (storeResult == EResponseEval::BUSY_OR_ERROR)
				{
					ldbg("Item dl'ed by others or in error state --> drop it");
					m_sError = "Busy Cache Item";
					return eJobResult::JOB_BROKEN;
				}
				break;
			}
			case STATE_PROCESS_CHUNKDATA:
			case STATE_PROCESS_DATA:
			{
				// similar states, just handled differently afterwards
				ldbg("STATE_GETDATA");


				beconsum inBuf(pBuf);
				off_t nToStore = min((off_t) inBuf.size(), m_nRest);
				ASSERT(nToStore >= 0);

				if (m_bAllowStoreData && nToStore > 0)
				{
					ldbg("To store: " <<nToStore);
					auto n = m_pStorageRef->DlConsumeData(pBuf, nToStore);
					if (n < 0)
					{
						m_sError = "Cannot store";
						return eJobResult::JOB_BROKEN;
					}
					m_nRest -= n;

					// unset watermark to get the remainder in real time
					if (m_nRest <= m_nWatermark)
						bufferevent_setwatermark(peBuf, EV_READ, 0, 0);

					if (n != nToStore)
					{
						parent.Subscribe2BlockingItem(m_pStorageRef);
						return eJobResult::HINT_MORE;
					}
				}
				else
				{
					m_nRest -= nToStore;
					inBuf.drop(nToStore);
				}

				ldbg("Rest: " << m_nRest);
				if (m_nRest != 0)
					return eJobResult::HINT_MORE; // will come back

				m_DlState = (STATE_PROCESS_DATA == m_DlState) ?
							STATE_FINISHJOB : STATE_GET_CHUNKEND;

				break;
			}
			case STATE_FINISHJOB:
			{
				ldbg("STATE_FINISHJOB");
				m_pStorageRef->DlFinish(false);
				parent.m_blockingItemSubscription.reset();

				if (m_bConnectionClose)
					return eJobResult::HINT_RECONNECT;

				m_DlState = STATE_GETHEADER;
				return eJobResult::HINT_DONE;
			}
			case STATE_GETCHUNKHEAD:
			{
				ldbg("STATE_GETCHUNKHEAD");
				// came back from reading, drop remaining newlines?
				auto sres = evbuffer_search_eol(pBuf, nullptr, nullptr, evbuffer_eol_style::EVBUFFER_EOL_CRLF_STRICT);
				if (sres.pos == -1)
					return eJobResult::HINT_MORE;
				auto line = inBuf.linear(sres.pos + 2);
				off_t len = Hex2Offt(line);
				if (len < 0)
				{
					m_sError = "Invalid chunked stream";
					return eJobResult::JOB_BROKEN; // hm...?
				}
				inBuf.drop(sres.pos + 2);
				m_nRest = len;
				m_DlState = len > 0 ? STATE_PROCESS_CHUNKDATA : STATE_GET_CHUNKEND_LAST;
				break;
			}
			case STATE_GET_CHUNKEND:
			case STATE_GET_CHUNKEND_LAST:
			{
				// drop two bytes of the newline
				auto sres = evbuffer_search_eol(pBuf, nullptr, nullptr, evbuffer_eol_style::EVBUFFER_EOL_CRLF_STRICT);
				if (sres.pos < 0)
					return eJobResult::HINT_MORE;
				ASSERT(sres.pos == 0);
				inBuf.drop(2);
				m_DlState = m_DlState == STATE_GET_CHUNKEND_LAST ?
							STATE_FINISHJOB : STATE_GETCHUNKHEAD;
				continue;
			}
			}
		}
		ASSERT(!"unreachable");
		m_sError = "Bad state";
		return eJobResult::JOB_BROKEN;
	}

	EResponseEval CheckAndSaveHeader(header&& h, bufferevent* peBuf, size_t headerLen, off_t contLen)
	{
		LOGSTARTFUNC;

		auto& remoteStatus = h.getStatus();
		auto& sPathRel = m_pStorageRef->m_sPathRel;
		auto& fiStatus = m_pStorageRef->m_status;

		auto mark_assigned = [&]()
		{
			m_bFileItemAssigned = true;
			m_pStorageRef->m_nTimeDlStarted = GetTime();
		};

        auto withError = [&](string_view message,
				fileitem::EDestroyMode destruction = fileitem::EDestroyMode::KEEP)
		{
            m_bAllowStoreData = false;
			mark_assigned();
			USRERR(sPathRel << " response or storage error [" << message << "], last errno: " << tErrnoFmter());
#warning Test that timeout is reported as 504
			m_pStorageRef->DlSetError({ (m_sourceState & eTransErrors::TRANS_TIMEOUT) ? 504 : 503,
										mstring(message)}, destruction);
            return EResponseEval::BUSY_OR_ERROR;
        };

		USRDBG( "Download started, storeHeader for " << sPathRel << ", current status: " << (int) fiStatus);

		m_pStorageRef->m_nIncommingCount += headerLen;

		if(fiStatus >= fileitem::FIST_COMPLETE)
		{
			USRDBG( "Download was completed or aborted, not restarting before expiration");
			return EResponseEval::BUSY_OR_ERROR;
		}

		if (!m_bFileItemAssigned && fiStatus > fileitem::FIST_DLPENDING)
		{
			// it's active but not served by us
			return EResponseEval::BUSY_OR_ERROR;
		}

		string sLocation;

        switch(remoteStatus.code)
		{
		case 200:
		{
			// forget the offset!!
			m_nUsedRangeStartPos = 0;
			break;
		}
		case 206:
		{
			/*
			 * Range: bytes=453291-
			 * ...
			 * Content-Length: 7271829
			 * Content-Range: bytes 453291-7725119/7725120
			 *
			 * RFC:
			 * HTTP/1.1 206 Partial content
       Date: Wed, 15 Nov 1995 06:25:24 GMT
       Last-Modified: Wed, 15 Nov 1995 04:58:08 GMT
       Content-Range: bytes 21010-47021/47022
       Content-Length: 26012
       Content-Type: image/gif
			 */
			h.setStatus(200, "OK");
			const char *p=h.h[header::CONTENT_RANGE];

			if(!p)
				return withError("Missing Content-Range in Partial Response");

			const static std::regex re("bytes(\\s*|=)(\\d+)-(\\d+|\\*)/(\\d+|\\*)");

			std::cmatch reRes;
			if (!std::regex_search(p, reRes, re))
			{
				return withError("Bad range");
			}
			auto tcount = reRes.size();
			if (tcount != 5)
			{
				return withError("Bad range format");
			}
			// * would mean -1
			contLen = atoofft(reRes[4].first, -1);
			auto startPos = atoofft(reRes[2].first, -1);

			// identify the special probe request which reports what we already knew
			if (m_pStorageRef->IsVolatile() &&
				m_pStorageRef->m_nSizeCachedInitial > 0 &&
				contLen == m_pStorageRef->m_nSizeCachedInitial &&
				m_pStorageRef->m_nSizeCachedInitial - 1 == startPos &&
				m_pStorageRef->m_responseModDate == h.h[header::LAST_MODIFIED])
			{
				m_bAllowStoreData = false;
				mark_assigned();
				m_pStorageRef->m_nSizeChecked = m_pStorageRef->m_nContentLength = m_pStorageRef->m_nSizeCachedInitial;
				m_pStorageRef->DlFinish(true);
				return EResponseEval::GOOD;
			}
			// in other cases should resume and the expected position, or else!
			if (startPos == -1 ||
					m_nUsedRangeStartPos != startPos ||
					startPos < m_pStorageRef->m_nSizeCachedInitial)
			{
                return withError("Server reports unexpected range");
            }
			break;
		}
		case 416:
			// that's bad; it cannot have been completed before (the -1 trick)
			// however, proxy servers with v3r4 cl3v3r caching strategy can cause that
			// if if-mo-since is used and they don't like it, so attempt a retry in this case
			if(m_pStorageRef->m_nSizeChecked < 0)
			{
				USRDBG( "Peer denied to resume previous download (transient error) " << sPathRel );
				m_pStorageRef->m_nSizeCachedInitial = 0; // XXX: this is ok as hint to request cooking but maybe add dedicated flag
				m_pStorageRef->m_bWriterMustReplaceFile = true;
				return EResponseEval::RESTART_NEEDED;
			}
			else
			{
				// -> kill cached file ASAP
				m_bAllowStoreData=false;
                return withError("Disagreement on file size, cleaning up", fileitem::EDestroyMode::TRUNCATE);
			}
			break;
		default: //all other codes don't have a useful body
			if (m_bFileItemAssigned) // resuming case
			{
				// got an error from the replacement mirror? cannot handle it properly
				// because some job might already have started returning the data
				USRDBG( "Cannot resume, HTTP code: " << remoteStatus.code);
				return withError(remoteStatus.msg);
			}

            if (remoteStatus.isRedirect())
            {
				if (!h.h[header::LOCATION] || !h.h[header::LOCATION][0])
					return withError("Invalid redirection (missing location)");
				sLocation = h.h[header::LOCATION];
            }
			// don't tell clients anything about the body
			m_bAllowStoreData = false;
            contLen = -1;
		}

		if(cfg::debug & log::LOG_MORE)
			log::misc(string("Download of ")+sPathRel+" started");

		mark_assigned();

		if (!m_pStorageRef->DlStarted(bereceiver(peBuf), headerLen, tHttpDate(h.h[header::LAST_MODIFIED]),
                                   sLocation.empty() ? RemoteUri(false) : sLocation,
                                   remoteStatus,
                                   m_nUsedRangeStartPos, contLen))
		{
			return EResponseEval::BUSY_OR_ERROR;
		}

        if (!m_bAllowStoreData)
        {
			// XXX: better ensure that it's processed from outside loop and use custom return code?
			m_pStorageRef->DlFinish(true);
		}

#warning EVALUATE: in case of malicious attacker, maybe restrict this to 30k in the begining and adjust later?
		for (auto n: { 120000, 90000, 56000, 40000, 30000 })
		{
			if (m_nRest > n)
			{
				m_nWatermark = n;
				bufferevent_setwatermark(peBuf, EV_READ, m_nWatermark, 2*m_nWatermark);
				break;
			}
		}

		return EResponseEval::GOOD;
	}

private:
	// not to be copied ever
	tDlJob(const tDlJob&);
	tDlJob& operator=(const tDlJob&);
};

void tDlStream::Connect()
{
	LOGSTARTFUNC;
	ASSERT(m_waiting.size());

	auto onConnect = [this](atransport::tResult result)
	{
		if (result.err.empty())
		{
			wireTransport(move(result.strm));
			tryRequestMore();
		}
		else
		{
			handleDisconnect(result.err, result.flags);
		}
	};
	m_connectionToken = atransport::Create(m_waiting.top()->GetPeerHost(), move(onConnect), m_parent->GetAppRes(),
										   atransport::TConnectParms().SetProxy(m_waiting.top()->GetJobProxyInfo()));
}

void tDlStream::OnRead(bufferevent *pBE)
{
	if (m_requested.empty())
		return handleDisconnect("Unexpected Data", TRANS_STREAM_ERR_FATAL);
	while (!m_requested.empty())
	{
		auto res = m_requested.front()->ProcessIncomming(pBE, *this);

		// reset optimization in case of any trouble
		if (res != eJobResult::HINT_MORE)
			bufferevent_setwatermark(pBE, EV_READ, 0, 0);

		switch (res)
		{
		case eJobResult::HINT_MORE:
			return;
		case eJobResult::JOB_BROKEN:
			m_dirty = true;
			m_requested.pop_front();
			// premature job termination means a broken stream
#warning actually, should differentiate between stream and local cause (conflict, 304 optimization). For local breakdown, maybe keep the stream running if the remaining part is small enough
			return handleDisconnect(se, TRANS_STREAM_ERR_FATAL);
		case eJobResult::HINT_RECONNECT: // uplink fscked, not sure why, should have the reason inside already
			return handleDisconnect(se, TRANS_STREAM_ERR_TRANSIENT);
		case eJobResult::MIRROR_BROKEN_KILL_LAST_FILE:
			 ASSERT(!"something is wrong");
			if (m_memberLastItem)
				m_memberLastItem->DlSetError({500, "Damaged Stream"}, fileitem::EDestroyMode::TRUNCATE);
			__just_fall_through;
		case eJobResult::MIRROR_BROKEN:
			m_dirty = true;
			return handleDisconnect(se, TRANS_STREAM_ERR_FATAL);
		case eJobResult::HINT_DONE:
		{
			auto close = m_requested.front()->m_bConnectionClose;
			m_memberLastItem = m_requested.front()->m_pStorageRef;
			m_requested.pop_front();

			if (close)
				return handleDisconnect(se, 0);

			tryRequestMore();
			continue;
		}
		}
	}

	if (m_requested.empty() && m_waiting.empty())
	{
		m_idleSince = GetTime();
		m_parent->TermOrProcBacklog(m_meRef);
	}
}

void tDlStream::OnStatus(bufferevent *, short what)
{
	if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
		handleDisconnect("Remote disconnect"sv, TRANS_STREAM_ERR_TRANSIENT);
}

bool tDlStream::TakeJob(tDlJobPtr&& pJob)
{
	if (!pJob)
		return false;
	if (!m_requested.empty() && pJob->GetId() < m_requested.back()->GetId())
		return false;
	// if already connected - can we serve that job from this connection?
	if (m_transport && !pJob->ValidateTargetConnection(*m_transport))
		return false;
	if (!m_transport)
	{
		// analyze whether there is a good chance that this connection will be able to serve it or not
#warning add target-specific-proxy check when implemented
		if (cfg::GetProxyInfo()
			|| (!m_waiting.empty() && m_waiting.top()->GetPeerHost() == pJob->GetPeerHost())

			)
		{
			pJob->m_bRevalidationNeeded = true;
		}
	}

	// otherwise, yeah, we can probably handle this, connect as needed
	pJob->m_DlState = tDlJob::STATE_GETHEADER;
	m_waiting.push(move(pJob));

	if (m_transport)
		tryRequestMore();
	else if (!m_connectionToken)
		Connect();

	m_idleSince = END_OF_TIME;
	return true;
}

tDlStream::~tDlStream()
{
	LOGSTARTFUNCx(m_dirty, m_transport.get());

	ASSERT(m_waiting.empty() && m_requested.empty());
	ASSERT_IS_MAIN_THREAD;

	if (m_transport && !m_dirty && m_requested.empty())
		atransport::Return(m_transport);
}

off_t tDlStream::GetRemainingBodyBytes()
{
	auto p = m_requested.empty()
			? (m_waiting.empty() ? nullptr : m_waiting.top().get())
			: m_requested.front().get();
	if (!p)
		return 0;
	if (p->WhoIsResponsible() != tDlJob::eResponsibility::US)
		return 0;
	return std::abs(p->m_pStorageRef->m_nContentLength - p->m_pStorageRef->GetCheckedSize());
}

void tDlStream::Sacrifice()
{
	LOGSTARTFUNC;

	m_sacrificied = true;
#warning if this is set, the item which got interrupted in the end must get an additional retry

	while (!m_waiting.empty())
		m_parent->ToBacklog(m_waiting.pop());

	if (m_transport && m_transport->GetBufferEvent())
	{
		auto fd = bufferevent_getfd(m_transport->GetBufferEvent());
		ldbg(fd);
		shutdown(fd, SHUT_WR);
	}
}

void tDlStream::tryRequestMore()
{
	while (!m_waiting.empty() && m_requested.size() < REQUEST_LIMIT)
	{
		auto p = m_waiting.pop();

		if (p->m_bRevalidationNeeded)
		{
			p->m_bRevalidationNeeded = false;
			if (!p->ValidateTargetConnection(*m_transport))
			{
				m_parent->ToBacklog(move(p));
				return;
			}
		}

		p->AppendRequest(besender(m_transport->GetBufferEvent()), m_transport->GetUsedProxy());
		m_requested.emplace_back(move(p));
	}
}

void tDlStream::wireTransport(lint_ptr<atransport>&& result)
{
	ASSERT(result);
	m_transport = move(result);
	bufferevent_setcb(m_transport->GetBufferEvent(), cbRead, nullptr, cbStatus, this);
	bufferevent_enable(m_transport->GetBufferEvent(), EV_READ|EV_WRITE);
}

void tDlStream::handleDisconnect(string_view why, tComError cause)
{
	LOGSTARTFUNCx(why, cause);
	m_blockingItemSubscription.reset();

	if (!cause)
		cause = TRANS_STREAM_ERR_TRANSIENT;

	if (m_requested.empty() && m_waiting.empty())
		return m_parent->DeleteStream(m_meRef);

	// hardcopy, no matter where it came from
	auto rsn(to_string(why));

	auto putBack = [&](tDlJobPtr&& what)
	{
		dbgline;
		// expected disconnect?
		if (what->m_DlState == tDlJob::STATE_FINISHJOB)
			return;

		dbgline;
		what->m_sourceState = cause;
		// only report first item as the cause
		if (cause)
			cause = 0;

		setIfNotEmpty(what->m_sError, rsn);

		// dispatch would also do it but can make a quick check here ASAP, the result is cached anyway
		if (!what->SetupSource(m_parent->GetValidator()))
			return;

		dbgline;
		evabase::Post([raw = what.release(), pin = as_lptr(m_parent)]()
		{
			pin->ToBacklog(unique_ptr<tDlJob>(raw));
		});
	};

	// return all jobs to parent
	while (!m_requested.empty())
	{
		putBack(move(m_requested.front()));
		m_requested.pop_front();
	}

	while (!m_waiting.empty())
		putBack(m_waiting.pop());

	m_connectionToken.reset();
	m_transport.reset();

	if (m_dirty)
		m_parent->DeleteStream(m_meRef);
}

bool CDlConn::ToBacklog(tDlJobPtr&& j)
{
	try
	{
		m_backlog.push(move(j));
	}
	catch (const std::bad_alloc&)
	{
		j.reset();
		return false;
	}
	TriggerDispatch();
	return true;
}

bool CDlConn::AddJob(lint_ptr<fileitem> fi, const tHttpUrl *src, tRepoResolvResult *repoSrc, bool isPT, mstring extraHeaders)
{
	LOGSTARTFUNC;

	tDlJobPtr j;
	try
	{
		j.reset(new tDlJob(m_nJobIdgen++, fi, src, repoSrc, isPT, move(extraHeaders)));
	}
	catch (const std::exception& ex)
	{
		DBGQLOG(ex.what());
		return false;
	}
	catch (...)
	{
		dbgline;
		return false;
	}
	return ToBacklog(move(j));
}

void CDlConn::TriggerDispatch()
{
	if (m_bDispatchPending)
		return;

	m_bDispatchPending = true;
	evtimer_add(m_dispatchNotifier.get(), &timeout_asap);
}

tDlJobPrioQ unhandled;

void CDlConn::cbDispatchBacklog(int, short, void *ctx)
{
	LOGSTARTFUNCs;
	auto me((CDlConn*)ctx);
	me->m_bDispatchPending = false;
	try
	{
		unhandled.clear();
		while (!me->m_backlog.empty())
			me->DispatchDfrd(me->m_backlog.pop(), unhandled);
		unhandled.swap(me->m_backlog);
	}
	catch (...)
	{
		unhandled.clear();
		me->m_backlog.clear();
	}
}

void CDlConn::DispatchDfrd(tDlJobPtr&& what, tDlJobPrioQ& unhandled)
{
	LOGSTARTFUNC;

	if (AC_UNLIKELY(!what))
		return;

	// drop broken or assigned to others, and check configuration
	auto st = what->WhoIsResponsible();
	if(st == tDlJob::eResponsibility::NOT_US
			|| st == tDlJob::eResponsibility::IRRELEVANT)
	{
		dbgline;
		if (! what->SetupSource(m_validator))
		{
			dbgline;
			return what.reset();
		}
	}

	auto accept = [&](tDlStreamPool::iterator it)
	{
		if (!it->TakeJob(move(what)))
			return false;
		m_lastUsedStream = it;
		return true;
	};

	// try fast path?
	if (m_lastUsedStream != m_streams.end())
	{
		dbgline;
		if(accept(m_lastUsedStream))
		{
			dbgline;
			return;
		}
	}
	// XXX: subject to optimization! There no preferrence of taking idle or busy streams first yet.
	for (auto it = m_streams.begin(); it != m_streams.end(); ++it)
	{
		if (m_lastUsedStream == it)
			continue;
		if (accept(it))
			return;
	}
	// okay, not accepted by any existing stream, create a new one if allowed
	if (m_streams.size() <= MAX_STREAMS_PER_USER)
	{
		if (!accept(AddStream(what->GetPeerHost())))
			return what.reset();
	}

	ldbg("2many streams, let's interrupt something");

	unhandled.push(move(what));
	dbgline;

	// pick the one with the fewest known bytes in the pipeline ATM
	tDlStream* sacrifice = nullptr;
	off_t remBytesMin = MAX_VAL(off_t);
	for(auto& el: m_streams)
	{
		off_t rb = el.GetRemainingBodyBytes();
		if (rb <= remBytesMin)
		{
			sacrifice = &el;
			remBytesMin = rb;
		}
	}
	dbgline;
	ASSERT(sacrifice);
	sacrifice->Sacrifice();
}

bool tDlJobPrioQ::tCompJobPtrById::operator()(const T &a, const T &b) const
{
auto idA = a->m_orderId;
auto idB = b->m_orderId;
// we actually want a min_heap
return idB < idA;
}

}

// vim: set noet ts=4 sw=4
