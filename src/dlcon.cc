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

#include <algorithm>
#include <list>
#include <map>
#include <regex>
#include <unordered_map>

#include <unistd.h>
#include <sys/time.h>

using namespace std;

// evil hack to simulate random disconnects
//#define DISCO_FAILURE

#define MAX_RETRY cfg::dlretriesmax

namespace acng
{

static cmstring sGenericError("502 Bad Gateway");
static const fileitem::tSpecialPurposeAttr g_dummy_attributes;
class CDlConn;
struct tDlJob;

static uint64_t g_ivDlJobId = 0;

using TDlJobQueue = std::list<tDlJob>;

struct TDlStream : public tLintRefcounted
{
	TDlJobQueue m_backlog, m_requested;

	enum class EAddResult
	{
		ERROR = -1,
		CONSUMED_OR_NO_FIT,
		TARGET_CHANGED
	};

	/**
	 * @brief addJobs transfers new jobs from the input to here, if possible.
	 * Shall take more jobs for the same target, if they fit. In case the target
	 * key changes, write the changed key back to changedKey parameter and report
	 * TARGET_CHANGED.
	 */
	EAddResult takeNewJobs(TDlJobQueue& input, mstring& changedKey, bool hostFitWasChecked);

	void shallRequestMore();

	aobservable::subscription m_blockingItemSubscription;
};

class CDlConn : public dlcontroller
{
	friend struct tDlJob;
	// keeping the jobs which cannot be added yet because of restrictions
	TDlJobQueue m_backlog;

	unordered_multimap<string, lint_ptr<TDlStream>> m_streams;

	/**
	 * @brief InstallJobs scatters jobs on the available download streams or creates new streams as needed.
	 * @param jobs
	 * @return
	 */
	bool InstallJobs(TDlJobQueue& jobs);

	// dlcontroller interface
public:
	void Dispose() override;
	bool AddJob(lint_ptr<fileitem> fi, tHttpUrl src, bool isPT, mstring extraHeaders) override;
	bool AddJob(lint_ptr<fileitem> fi, tRepoResolvResult repoSrc, bool isPT, mstring extraHeaders) override;
	CDlConn() =default;
};

struct tDlJob
{
	tFileItemPtr m_pStorageRef;
	fileitem* storage() { return m_pStorageRef.get(); }
	mstring sErrorMsg;
	TDlStream* m_parent = nullptr;

	string m_extraHeaders;

	tHttpUrl m_remoteUri;
	const tHttpUrl *m_pCurBackend = nullptr;

	bool m_bBackendMode = false;
	// input parameters
	bool m_bIsPassThroughRequest = false;
	bool m_bAllowStoreData = true;
	bool m_bFileItemAssigned = false;

	int m_nRedirRemaining = cfg::redirmax;

	const fileitem::tSpecialPurposeAttr& GetFiAttributes() { return storage() ? storage()->m_spattr : g_dummy_attributes; }

	// state attribute
	off_t m_nRest = 0;

	// flag to use ranges and also define start if >= 0
	off_t m_nUsedRangeStartPos = -1;
	off_t m_nUsedRangeLimit = -1;

	const uint_fast32_t m_orderId;

#define HINT_MORE 0
#define HINT_DONE 1
#define HINT_RECONNECT_NOW 2
#define EFLAG_JOB_BROKEN 4
#define EFLAG_MIRROR_BROKEN 8
#define EFLAG_STORE_COLLISION 16
#define HINT_SWITCH 32
#define EFLAG_LOST_CON 64
#define HINT_KILL_LAST_FILE 128
#define HINT_TGTCHANGE 256
#define HINT_RECONNECT_SOON 512

	const tRepoData *m_pRepoDesc = nullptr;

	enum EStreamState : uint8_t
	{
		STATE_GETHEADER,
		//STATE_REGETHEADER,
		STATE_PROCESS_DATA,
		STATE_GETCHUNKHEAD,
		STATE_PROCESS_CHUNKDATA,
		STATE_GET_CHUNKTRAILER,
		STATE_FINISHJOB
	}
	m_DlState = EStreamState::STATE_GETHEADER;

	enum class EResponseEval
	{
		GOOD, BUSY_OR_ERROR, RESTART_NEEDED
	};

	inline bool HasBrokenStorage()
	{
		return (!storage() || storage()->GetStatus() > fileitem::FIST_COMPLETE);
	}

	/*!
	 * Returns a reference to http url where host and port and protocol match the current host
	 * Other fields in that member have undefined contents. ;-)
	 */
	inline const tHttpUrl& GetPeerHost()
	{
		return m_pCurBackend ? *m_pCurBackend : m_remoteUri;
	}

	inline tRepoUsageHooks* GetConnStateTracker()
	{
		return m_pRepoDesc ? m_pRepoDesc->m_pHooks : nullptr;
	}

	inline void SetSource(tHttpUrl &&src)
	{
		LOGSTARTFUNC;
		ldbg("uri: " << src.ToURI(false));
		m_remoteUri = move(src);
	}

	inline void SetSource(tRepoResolvResult &&repoSrc)
	{
		LOGSTARTFUNC;
		ldbg("repo: " << uintptr_t(repoSrc.repodata) << ", restpath: " << repoSrc.sRestPath);
		m_remoteUri.sPath = move(repoSrc.sRestPath);
		m_pRepoDesc = move(repoSrc.repodata);
		m_bBackendMode = true;
	}

	inline tDlJob(uint_fast32_t id, const tFileItemPtr& pFi, bool isPT, mstring extraHeaders) :
		m_extraHeaders(move(extraHeaders)),
		m_bIsPassThroughRequest(isPT),
		m_orderId(id)
	{
		ASSERT_HAVE_MAIN_THREAD;
		if (pFi)
		{
			m_pStorageRef = pFi;
			pFi->DlRefCountAdd();
		}
	}

#warning fixme: unsubscribe properly if needed, change the parent then
	void reparent(TDlStream *p);

	// Default move ctor is ok despite of pointers, we only need it in the beginning, list-splice operations should not move the object around
	tDlJob(tDlJob &&other) = default;

	~tDlJob()
	{
		LOGSTART("tDlJob::~tDlJob");
		if (storage())
		{
			dbgline;
			storage()->DlRefCountDec({503, sErrorMsg.empty() ?
                    "Download Expired" : move(sErrorMsg)});
		}
	}

	void ResetStreamState()
	{
		m_nRest = 0;
		m_DlState = STATE_GETHEADER;
		m_nUsedRangeStartPos = -1;
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
            sErrorMsg = "Redirection loop";
			return false;
		}

		if (!pNewUrl || !*pNewUrl)
		{
            sErrorMsg = "Bad redirection";
			return false;
		}

		// start modifying the target URL, point of no return
		m_pCurBackend = nullptr;
		bool bWasBeMode = m_bBackendMode;
		m_bBackendMode = false;

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
                sErrorMsg = "Bad redirection target";
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

	bool SetupJobConfig(mstring &sReasonMsg,
			tStrMap &blacklist)
	{
		LOGSTART("CDlConn::SetupJobConfig");

		// using backends? Find one which is not blacklisted
		if (m_bBackendMode)
		{
			// keep the existing one if possible
			if (m_pCurBackend)
			{
				LOG(
						"Checking [" << m_pCurBackend->sHost << "]:" << m_pCurBackend->GetPort());
				const auto bliter = blacklist.find(m_pCurBackend->GetHostPortKey());
				if (bliter == blacklist.end())
					LOGRET(true);
			}

			// look in the constant list, either it's usable or it was blacklisted before
			for (const auto &bend : m_pRepoDesc->m_backends)
			{
				const auto bliter = blacklist.find(bend.GetHostPortKey());
				if (bliter == blacklist.end())
				{
					m_pCurBackend = &bend;
					LOGRET(true);
				}

				// uh, blacklisted, remember the last reason
				if (sReasonMsg.empty())
				{
					sReasonMsg = bliter->second;
					LOG(sReasonMsg);
				}
			}
			if (sReasonMsg.empty())
				sReasonMsg = "Mirror blocked due to repeated errors";
			LOGRET(false);
		}

		// ok, not backend mode. Check the mirror data (vs. blacklist)
		auto bliter = blacklist.find(GetPeerHost().GetHostPortKey());
		if (bliter == blacklist.end())
			LOGRET(true);

		sReasonMsg = bliter->second;
		LOGRET(false);
	}

	// needs connectedHost, blacklist, output buffer from the parent, proxy mode?
	inline void AppendRequest(tSS &head, const tHttpUrl *proxy)
	{
		LOGSTARTFUNC;

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

		m_nUsedRangeStartPos = storage()->m_nSizeChecked >= 0 ?
				storage()->m_nSizeChecked : storage()->m_nSizeCachedInitial;

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
			else if (m_nUsedRangeStartPos == storage()->m_nContentLength
					&& m_nUsedRangeStartPos > 1)
			{
				m_nUsedRangeStartPos--; // the probe trick
			}

			if (!storage()->m_responseModDate.isSet()) // date unusable but needed for volatile files?
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
			if (storage()->m_responseModDate.isSet())
				head << "If-Range: " << storage()->m_responseModDate.view() << CRLF;
			head << "Range: bytes=" << m_nUsedRangeStartPos << "-";
			if (m_nUsedRangeLimit > 0)
				head << m_nUsedRangeLimit;
			head << CRLF;
		}

		if (storage()->IsVolatile())
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

	inline uint_fast8_t NewDataHandler(evbuffer* pBuf)
	{
		LOGSTART("tDlJob::NewDataHandler");
		beconsum inBuf(pBuf);
		off_t nToStore = min((off_t) inBuf.size(), m_nRest);
		ASSERT(nToStore >= 0);

		if (m_bAllowStoreData && nToStore > 0)
		{
			ldbg("To store: " <<nToStore);
			auto n = storage()->DlAddData(pBuf, nToStore);
			if (n < 0)
			{
				sErrorMsg = "Cannot store";
				return HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN;
			}
			m_nRest -= n;
			inBuf.drop(n);
			if (n != nToStore)
			{
#warning implement me. Shall subscribe to notify on some poke method, and stop acting here until the poke method reports storabe availability
				//m_parent->Subscribe2BlockingItem(m_pStorageRef);
				return HINT_MORE;
			}
		}
		else
		{
			m_nRest -= nToStore;
			inBuf.drop(nToStore);
		}

		ldbg("Rest: " << m_nRest);
		if (m_nRest != 0)
			return HINT_MORE; // will come back

		m_DlState = (STATE_PROCESS_DATA == m_DlState) ?
						STATE_FINISHJOB : STATE_GETCHUNKHEAD;
		return HINT_SWITCH;
	}

	/*!
	 *
	 * Process new incoming data and write it down to disk or other receivers.
	 */
	unsigned ProcessIncomming(evbuffer* pBuf, bool bOnlyRedirectionActivity)
	{
		LOGSTARTFUNC;
		ASSERT_HAVE_MAIN_THREAD;
		if (AC_UNLIKELY(!storage() || !pBuf))
		{
            sErrorMsg = "Bad cache item";
			return HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN;
		}
		beconsum inBuf(pBuf);

		for (;;) // returned by explicit error (or get-more) return
		{
			ldbg("switch: " << (int)m_DlState);

			if (STATE_GETHEADER == m_DlState)
			{
				ldbg("STATE_GETHEADER");
				header h;
				if (inBuf.size() == 0)
					return HINT_MORE;

				dbgline;

				auto hDataLen = h.Load(pBuf);
				// XXX: find out why this was ever needed; actually we want the opposite,
				// download the contents now and store the reported file as XORIG for future
				// https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Location
				if (0 == hDataLen)
					return HINT_MORE;
				if (hDataLen < 0)
				{
					sErrorMsg = "Invalid header";
					LOG(sErrorMsg);
					// can be followed by any junk... drop that mirror, previous file could also contain bad data
					return EFLAG_MIRROR_BROKEN | HINT_RECONNECT_NOW
							| HINT_KILL_LAST_FILE;
				}

				ldbg("header: " << pBuf);

				if (h.type != header::ANSWER)
				{
					dbgline;
                    sErrorMsg = "Unexpected response type";
					// smells fatal...
					return EFLAG_MIRROR_BROKEN | EFLAG_LOST_CON | HINT_RECONNECT_NOW;
				}
				dbgline;

				unsigned ret = 0;

				auto pCon = h.h[header::CONNECTION];
				if (!pCon)
					pCon = h.h[header::PROXY_CONNECTION];

				if (pCon && 0 == strcasecmp(pCon, "close"))
				{
					ldbg("Peer wants to close connection after request");
					ret |= HINT_RECONNECT_SOON;
				}

				// processing hint 102, or something like 103 which we can ignore
				if (h.getStatusCode() < 200)
				{
					inBuf.drop(size_t(hDataLen));
					return ret | HINT_MORE;
				}

				if (cfg::redirmax) // internal redirection might be disabled
				{
					if (h.getStatus().isRedirect())
					{
						if (!RewriteSource(h.h[header::LOCATION]))
							return ret | EFLAG_JOB_BROKEN;

						// drop the redirect page contents if possible so the outer loop
						// can scan other headers
						off_t contLen = atoofft(h.h[header::CONTENT_LENGTH], 0);
						if (contLen <= (off_t) inBuf.size())
							inBuf.drop(contLen);
						return ret | HINT_TGTCHANGE; // no other flags, caller will evaluate the state
					}

					// for non-redirection responses process as usual

					// unless it's a probe run from the outer loop, in this case we
					// should go no further
					if (bOnlyRedirectionActivity)
						return ret | EFLAG_LOST_CON | HINT_RECONNECT_NOW;
				}

				// explicitly blacklist mirror if key file is missing
				if (h.getStatusCode() >= 400 && m_pRepoDesc && m_remoteUri.sHost.empty())
				{
					for (const auto &kfile : m_pRepoDesc->m_keyfiles)
					{
						if (endsWith(m_remoteUri.sPath, kfile))
						{
                            sErrorMsg = "Keyfile N/A, mirror blacklisted";
							return ret | HINT_RECONNECT_NOW | EFLAG_MIRROR_BROKEN;
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
                    sErrorMsg = "Missing Content-Length";
					return ret | HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN;
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
					if (storage()->IsVolatile())
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
						pBuf, hDataLen, contentLength);
				inBuf.drop(size_t(hDataLen));

				if (storage() && storage()->m_spattr.bHeadOnly)
					m_nRest = 0;

				if (storeResult == EResponseEval::RESTART_NEEDED)
					return ret | EFLAG_LOST_CON | HINT_RECONNECT_NOW; // recoverable

				if (storeResult == EResponseEval::BUSY_OR_ERROR)
				{
					ldbg("Item dl'ed by others or in error state --> drop it, reconnect");
					m_DlState = STATE_PROCESS_DATA;
                    sErrorMsg = "Busy Cache Item";
					return ret | HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN
							| EFLAG_STORE_COLLISION;
				}
			}
			else if (m_DlState == STATE_PROCESS_CHUNKDATA
					|| m_DlState == STATE_PROCESS_DATA)
			{
				// similar states, just handled differently afterwards
				ldbg("STATE_GETDATA");
				auto res = NewDataHandler(pBuf);
				if (HINT_SWITCH != res)
					return res;
			}
			else if (m_DlState == STATE_FINISHJOB)
			{
				ldbg("STATE_FINISHJOB");
				storage()->DlFinish(false);
				m_parent->m_blockingItemSubscription.reset();
				m_DlState = STATE_GETHEADER;
				return HINT_DONE;
			}
			else if (m_DlState == STATE_GETCHUNKHEAD)
			{
				ldbg("STATE_GETCHUNKHEAD");
				// came back from reading, drop remaining newlines?
				auto sres = evbuffer_search_eol(pBuf, nullptr, nullptr, evbuffer_eol_style::EVBUFFER_EOL_CRLF_STRICT);
				while (sres.pos == 0) // weed out newline(s) from previous chunk
				{
					inBuf.drop(2);
					sres = evbuffer_search_eol(pBuf, nullptr, nullptr, evbuffer_eol_style::EVBUFFER_EOL_CRLF_STRICT);
				}
				if (sres.pos < 0)
					return HINT_MORE;
				auto pStart = (LPCSTR) evbuffer_pullup(pBuf, sres.pos + 2);
				unsigned len(0);
				if (1 != sscanf(pStart, "%x", &len))
				{
                    sErrorMsg = "Invalid stream";
					return EFLAG_JOB_BROKEN; // hm...?
				}
				inBuf.drop(sres.pos + 2);
				if (len > 0)
				{
					m_nRest = len;
					m_DlState = STATE_PROCESS_CHUNKDATA;
				}
				else
					m_DlState = STATE_GET_CHUNKTRAILER;
			}
			else if (m_DlState == STATE_GET_CHUNKTRAILER)
			{
				while (m_DlState != STATE_FINISHJOB)
				{
					auto sres = evbuffer_search_eol(pBuf, nullptr, nullptr, evbuffer_eol_style::EVBUFFER_EOL_CRLF_STRICT);
					switch (sres.pos)
					{
					case -1: return HINT_MORE;
					case 0:
						inBuf.drop(2);
						m_DlState = STATE_FINISHJOB;
						break;
					default:
						inBuf.drop(sres.pos + 2);
						continue;
					}
				}
			}
		}
		ASSERT(!"unreachable");
        sErrorMsg = "Bad state";
		return EFLAG_JOB_BROKEN;
	}

	EResponseEval CheckAndSaveHeader(header&& h, evbuffer* pBuf, size_t headerLen, off_t contLen)
	{
		LOGSTARTFUNC;

		auto& remoteStatus = h.getStatus();
		auto& sPathRel = storage()->m_sPathRel;
		auto& fiStatus = storage()->m_status;

		auto mark_assigned = [&]()
		{
			m_bFileItemAssigned = true;
			storage()->m_nTimeDlStarted = GetTime();
		};

        auto withError = [&](string_view message,
                fileitem::EDestroyMode destruction = fileitem::EDestroyMode::KEEP) {
            m_bAllowStoreData = false;
			mark_assigned();
			USRERR(sPathRel << " response or storage error [" << message << "], last errno: " << tErrnoFmter());
			storage()->DlSetError({503, mstring(message)}, destruction);
            return EResponseEval::BUSY_OR_ERROR;
        };

		USRDBG( "Download started, storeHeader for " << sPathRel << ", current status: " << (int) fiStatus);

		storage()->m_nIncommingCount += headerLen;

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
			if (storage()->IsVolatile() &&
				storage()->m_nSizeCachedInitial > 0 &&
				contLen == storage()->m_nSizeCachedInitial &&
				storage()->m_nSizeCachedInitial - 1 == startPos &&
				storage()->m_responseModDate == h.h[header::LAST_MODIFIED])
			{
				m_bAllowStoreData = false;
				mark_assigned();
				storage()->m_nSizeChecked = storage()->m_nContentLength = storage()->m_nSizeCachedInitial;
				storage()->DlFinish(true);
				return EResponseEval::GOOD;
			}
			// in other cases should resume and the expected position, or else!
			if (startPos == -1 ||
					m_nUsedRangeStartPos != startPos ||
					startPos < storage()->m_nSizeCachedInitial)
			{
                return withError("Server reports unexpected range");
            }
			break;
		}
		case 416:
			// that's bad; it cannot have been completed before (the -1 trick)
			// however, proxy servers with v3r4 cl3v3r caching strategy can cause that
			// if if-mo-since is used and they don't like it, so attempt a retry in this case
			if(storage()->m_nSizeChecked < 0)
			{
				USRDBG( "Peer denied to resume previous download (transient error) " << sPathRel );
				storage()->m_nSizeCachedInitial = 0; // XXX: this is ok as hint to request cooking but maybe add dedicated flag
				storage()->m_bWriterMustReplaceFile = true;
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

		if (!storage()->DlStarted(pBuf, headerLen, tHttpDate(h.h[header::LAST_MODIFIED]),
                                   sLocation.empty() ? RemoteUri(false) : sLocation,
                                   remoteStatus,
                                   m_nUsedRangeStartPos, contLen))
		{
			return EResponseEval::BUSY_OR_ERROR;
		}

        if (!m_bAllowStoreData)
        {
			// XXX: better ensure that it's processed from outside loop and use custom return code?
			storage()->DlFinish(true);
		}
		return EResponseEval::GOOD;
	}

private:
	// not to be copied ever
	tDlJob(const tDlJob&);
	tDlJob& operator=(const tDlJob&);
};

bool CDlConn::InstallJobs(TDlJobQueue &jobs)
{
	LOGSTARTFUNC;
	if (jobs.empty())
		return false;

	auto key = jobs.front().GetPeerHost().GetHostPortProtoKey();

	while(!jobs.empty())
	{
		auto cand = m_streams.equal_range(key);
		for(auto it = cand.first; it != cand.second ; it++)
		{
			switch (it->second->takeNewJobs(jobs, key, it == cand.first))
			{
			case TDlStream::EAddResult::ERROR:
				return false;
			case TDlStream::EAddResult::CONSUMED_OR_NO_FIT:
				continue;
			case TDlStream::EAddResult::TARGET_CHANGED:
				goto continue_key_changed;
			}
		}

		{
			// this is not exactly as above :-(
			auto it = m_streams.insert(make_pair(key, as_lptr(new TDlStream)));
			switch (it->second->takeNewJobs(jobs, key, true))
			{
			case TDlStream::EAddResult::ERROR:
				return false;
			case TDlStream::EAddResult::CONSUMED_OR_NO_FIT:
				continue;
			case TDlStream::EAddResult::TARGET_CHANGED:
				dbgline;
				return false;
			}
		}

continue_key_changed:;
	}
	return jobs.empty();
}

void CDlConn::Dispose()
{
	ASSERT(!"implementme");
}

TDlStream::EAddResult TDlStream::takeNewJobs(TDlJobQueue &input, mstring &changedKey, bool hostFitWasChecked)
{
	auto posHint = m_backlog.end();
	auto blsize = m_backlog.size();

	while (!input.empty())
	{
		const auto& target = input.front().GetPeerHost();
		if (hostFitWasChecked)
		{
			auto keyNow = target.GetHostPortProtoKey();
			if (keyNow != changedKey)
			{
				changedKey.swap(keyNow);
				return EAddResult::TARGET_CHANGED;
			}
		}
		auto id = input.front().m_orderId;
		if (!m_requested.empty())
		{
			if (id < m_requested.back().m_orderId)
				return EAddResult::CONSUMED_OR_NO_FIT;
		}

		// ok, can add to backlog? search only if needed
		if (m_backlog.empty() || id > m_backlog.back().m_orderId)
		{
			m_backlog.splice(m_backlog.end(), input, input.begin());
		}
		else if (id < m_backlog.front().m_orderId)
		{
			m_backlog.splice(m_backlog.begin(), input, input.begin());
		}
		else
		{
			auto opAfter = [&](const tDlJob& it) {
				return it.m_orderId >= id;
			};

			if (posHint != m_backlog.end())
			{
				if (id > posHint->m_orderId)
					posHint = find_if(posHint, m_backlog.end(), opAfter);
				else
					posHint = find_if(m_backlog.begin(), posHint, opAfter);
#warning optimize this: judging by the index distance, searching backwards might make more sense in the last case
			}
			m_backlog.splice(posHint, input, input.begin());
			posHint--;
		}
#warning Add this, needed in the new design?
/*
					&& tgt.GetPort() == con->GetPort());
							  // or don't care port
							  || !con->GetPort())
						  );
						  */
	}
	if (blsize != m_backlog.size())
	{
		ASSERT(!"implementme");
		//shallRequestMore();
	}
	return EAddResult::CONSUMED_OR_NO_FIT;
	//return input.empty() ? EAddResult::CONSUMED_OR_NO_FIT : EAddResult::ERROR;
}

bool CDlConn::AddJob(lint_ptr<fileitem> fi, tHttpUrl src, bool isPT, mstring extraHeaders)
{
	TDlJobQueue q;
	q.emplace_back(g_ivDlJobId++, fi, isPT, move(extraHeaders));
	q.back().SetSource(move(src));
	return InstallJobs(q);
}

bool CDlConn::AddJob(lint_ptr<fileitem> fi, tRepoResolvResult repoSrc, bool isPT, mstring extraHeaders)
{
	TDlJobQueue q;
	q.emplace_back(g_ivDlJobId++, fi, isPT, move(extraHeaders));
	q.back().SetSource(move(repoSrc));
	return InstallJobs(q);
}

lint_ptr<dlcontroller> dlcontroller::CreateRegular()
{
	return static_lptr_cast<dlcontroller>(make_lptr<CDlConn>());
}

}
