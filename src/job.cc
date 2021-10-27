
#include "job.h"

#include "debug.h"
#include "meta.h"
#include "acfg.h"
#include "remotedb.h"
#include "conn.h"
#include "acfg.h"
#include "fileitem.h"
#include "dlcon.h"
#include "sockio.h"
#include "fileio.h" // for ::stat and related macros
#include "maintenance.h"
#include "evabase.h"
#include "aevutil.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <queue>

#include <dirent.h>
#include <event2/buffer.h>
#include <errno.h>

using namespace std;

//#define FORCE_CHUNKED

#define PT_BUF_MAX 64000
#define SB GetBufFmter()

// hunting Bug#955793: [Heisenbug] evbuffer_write_atmost returns -1 w/o updating errno
//#define DBG_DISCONNECT //std::cerr << "DISCO? " << __LINE__ << std::endl;

namespace acng
{

uint_fast32_t g_genJobId = 0;
namespace cfg
{
namespace rex {
enum eMatchType : int8_t;
}
}
string_view sHttp11("HTTP/1.1");
#warning restoreme
/*
tTraceData traceData;
void cfg::dump_trace()
{
	log::err("Paths with uncertain content types:");
	for (const auto& s : traceData)
		log::err(s);
}
tTraceData& tTraceData::getInstance()
{
	return traceData;
}
*/
#if 0
// base class for a fileitem replacement with custom storage for local data
class tGeneratedFitemBase : public fileitem
{
public:
	unique_fd GetFileFd() override { return unique_fd(); }; // something, don't care for now

	tSS m_data;

	tGeneratedFitemBase(const string &sFitemId, tRemoteStatus status, cmstring& origUrl = sEmptyString)
		: fileitem(sFitemId), m_data(256)
	{
		m_status=FIST_DLRECEIVING;
		m_responseOrigin = origUrl;
		m_responseStatus = status;
		m_contentType = "text/html";
	}
	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow)
	override
	{
		if (AC_UNLIKELY(m_status > FIST_COMPLETE || out_fd < 0))
		{
			errno = EBADFD;
			return -1;
		}
		if (AC_UNLIKELY(nMax2SendNow > m_data.size()))
		{
			errno = EOVERFLOW;
			return -1;
		}
		auto ret = m_data.dumpall(out_fd, nMax2SendNow);
		if (AC_LIKELY(ret > 0))
			nSendPos += ret;
		return ret;
	}
	inline void seal()
	{
		// finish the building and seal the item
		m_nSizeChecked = m_data.size();
		m_nContentLength = m_nSizeChecked;
		m_status = FIST_COMPLETE;
	}
};
#endif

static const string miscError(" [HTTP error, code: ");

job::~job()
{
	LOGSTART("job::~job");

#warning restore assert
	// shall have sent ANYTHING in response
//	ASSERT(m_nAllDataCount != 0);

	int stcode = 200;
	off_t inCount = 0;
	if (m_pItem.get())
	{
		stcode = m_pItem.get()->m_responseStatus.code;
		inCount = m_pItem.get()->TakeTransferCount();
	}

	bool bErr = m_sFileLoc.empty() || stcode >= 400;

	string sClient;
	if (!cfg::logxff || m_xff.empty()) // not to be logged or not available
		sClient = m_parent.getClientName();
	else if (!m_xff.empty())
	{
		sClient=move(m_xff);
		trimBoth(sClient);
		auto pos = sClient.find_last_of(SPACECHARS);
		if (pos!=stmiss)
			sClient.erase(0, pos+1);
	}
	auto repName = m_sFileLoc;
	if (bErr)
		repName += miscError + ltos(stcode) + ']';
	log::transfer(inCount, m_nAllDataCount, sClient, move(repName), bErr);
}



inline bool job::ParseRange(const header& h)
{

	/*
	 * Range: bytes=453291-
	 * ...
	 * Content-Length: 7271829
	 * Content-Range: bytes 453291-7725119/7725120
	 */

	const char *pRange = h.h[header::RANGE];
	// working around a bug in old curl versions
	if (!pRange)
		pRange = h.h[header::CONTENT_RANGE];
	if (pRange)
	{
		int nRangeItems = sscanf(pRange, "bytes=" OFF_T_FMT
								 "-" OFF_T_FMT, &m_nReqRangeFrom, &m_nReqRangeTo);
		// working around bad (old curl style) requests
		if (nRangeItems <= 0)
		{
			nRangeItems = sscanf(pRange, "bytes "
			OFF_T_FMT "-" OFF_T_FMT, &m_nReqRangeFrom, &m_nReqRangeTo);
		}

		if (nRangeItems < 1) // weird...
			m_nReqRangeFrom = m_nReqRangeTo = -2;
		else
			return true;
	}
	return false;
}

void job::Prepare(const header &h, bufferevent* be, size_t headLen, cmstring& callerHostname)
{
	LOGSTARTFUNC;
#ifdef DEBUGLOCAL
	cfg::localdirs["stuff"]="/tmp/stuff";
	log::dbg(m_pReqHead->ToString());
#endif

	string sReqPath, sPathResidual;
	tHttpUrl theUrl; // parsed URL

	fileitem::FiStatus fistate(fileitem::FIST_FRESH);
	bool bPtMode(false);

	if (h.h[header::XFORWARDEDFOR])
	{
		m_xff = h.h[header::XFORWARDEDFOR];
	}

	// some macros, to avoid goto style
	auto report_invport = [this]()
	{
		SetEarlySimpleResponse("403 Configuration error (confusing proxy mode) or prohibited port (see AllowUserPorts)"sv);
	};
	auto report_overload = [this](int line)
	{
		USRDBG("overload error, line " << line);
		SetEarlySimpleResponse("503 Server overload, try later"sv);
	};
	auto report_badcache = [this]()
	{
		SetEarlySimpleResponse("503 Error with cache data, please consult apt-cacher.err"sv);
	};
	auto report_invpath = [this]()
	{
		SetEarlySimpleResponse("403 Invalid path specification"sv);
	};
	auto report_notallowed = [this]()
	{
		SetEarlySimpleResponse("403 Forbidden file type or location"sv);
	};

	if(h.type!=header::GET)
	{
		m_bIsHeadOnly = h.type == header::eHeadType::HEAD;
		if(!m_bIsHeadOnly)
			return report_invpath();
	}

	UrlUnescapeAppend(h.getRequestUrl(), sReqPath);

	m_bIsHttp11 = h.proto == header::HTTP_11;

	USRDBG( "Decoded request URI: " << sReqPath);

	if(h.h[header::CONNECTION])
	{
		if (0 == strncasecmp(h.h[header::CONNECTION], WITHLEN("close")))
			m_keepAlive = CLOSE;
		if (0 == strncasecmp(h.h[header::CONNECTION], WITHLEN("Keep-Alive")))
			m_keepAlive = KEEP;
	}

	constexpr string_view fname = "/_actmp";

	// "clever" file system browsing attempt?
	if(rex::Match(sReqPath, rex::NASTY_PATH)
	   || stmiss != sReqPath.find(fname.data(), 0, fname.size())
	   || startsWithSz(sReqPath, "/_"))
	{
		LOG("ERROR: internal path or FS break-out attempt");
		return report_notallowed();
	}

	try
	{
		if (startsWithSz(sReqPath, "/HTTPS///"))
			sReqPath.replace(0, 6, PROT_PFX_HTTPS);
		// special case: proxy-mode AND special prefix are there
		if(0==strncasecmp(sReqPath.c_str(), WITHLEN("http://https///")))
			sReqPath.replace(0, 13, PROT_PFX_HTTPS);

		if(!theUrl.SetHttpUrl(sReqPath, false))
		{
			LOG("work type: USERINFO");
			m_pItem.reset(Create(EWorkType::USER_INFO, be, theUrl, h));
			return m_pItem ? void() : report_overload(__LINE__);;
		}
		LOG("refined path: " << theUrl.sPath << "\n on host: " << theUrl.sHost);

		// extract the actual port from the URL
		unsigned nPort = theUrl.GetPort();

		if(cfg::pUserPorts)
		{
			if(!cfg::pUserPorts->test(nPort))
				return report_invport();
		}
		else if(nPort != 80)
			return report_invport();

		// kill multiple slashes
		for(tStrPos pos=0; stmiss != (pos = theUrl.sPath.find("//", pos, 2)); )
			theUrl.sPath.erase(pos, 1);

		bPtMode=rex::MatchUncacheable(theUrl.ToURI(false), rex::NOCACHE_REQ);

		LOG("input uri: "<<theUrl.ToURI(false)<<" , dontcache-flag? " << bPtMode
			<< ", admin-page: " << cfg::reportpage);

		try
		{
			auto t = DetectWorkType(theUrl, h.h[header::AUTHORIZATION]);
			if (t > EWorkType::REGULAR)
			{
				m_pItem.reset(Create(t, be, theUrl, h));
				if (!m_pItem)
					return report_overload(__LINE__);
			}
		}
		catch (...)
		{
			return report_overload(__LINE__);
		}

		using namespace rex;

		{
			auto it = cfg::localdirs.find(theUrl.sHost);
			if (it != cfg::localdirs.end())
			{
				ParseRange(h);
				PrepareLocalDownload(sReqPath, it->second, theUrl.sPath);
				return;
			}
		}

		auto data_type = rex::eMatchType::FILE_INVALID;

		if (h.h[header::CACHE_CONTROL])
		{
			for (auto p = h.h[header::CACHE_CONTROL]; !!(p = strstr(p, "no")) ; p+=2)
			{
				// as per https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Cache-Control :
				// force the cache data to be validated
				if (0 == strcasecmp(p+2, "-cache"))
				{
					data_type = rex::eMatchType::FILE_VOLATILE;
				}
				else if (0 == strcasecmp(p+2, "-store"))
				{
					// the behavior should be basically the same, including considering cached files first
					bPtMode = true;
				}
			}
		}

		// we can proxy the directory requests, but only if they are identifiable as directories
		// (path ends in /) and are not matched as local directory server above and
		// the special acngfs hack is not detected
		if(endsWithSzAr(theUrl.sPath, "/"))
		{
			//isDir = true;
			if(!h.h[header::XORIG])
			{
#if 0


				m_type = GetFiletype(theUrl.sPath);
				// it's still a directory, can assume it to be scanable with volatile contents
				if(m_type == FILE_INVALID)
					m_type = FILE_VOLATILE;
#else
				data_type = FILE_VOLATILE;
				bPtMode=true;
#endif
			}
			if (data_type == FILE_INVALID)
			{
				LOG("generic user information page for " << theUrl.sPath);
				m_pItem.reset(Create(EWorkType::USER_INFO, be, theUrl, h));
				return m_pItem ? void() : report_overload(__LINE__);;
			}
		}

		// in PT mode we don't care about how to handle it, it's what user wants to do
		if(data_type == FILE_INVALID && bPtMode)
			data_type = FILE_SOLID;

		if(data_type == FILE_INVALID)
			data_type = GetFiletype(theUrl.sPath);

		if ( data_type == FILE_INVALID )
		{
			if(!cfg::patrace)
			{
				// just for the log
				m_sFileLoc = theUrl.sPath;
				return report_notallowed();
			}

			// ok, collect some information helpful to the user
			data_type = FILE_VOLATILE;
#warning XXX: restore tracer
			//traceData.insert(theUrl.sPath);
		}
		
		// got something valid, has type now, trace it
		USRDBG("Processing new job, " << h.getRequestUrl());
		auto repoSrc = remotedb::GetInstance().GetRepNameAndPathResidual(theUrl);
		if(repoSrc.psRepoName && !repoSrc.psRepoName->empty())
			m_sFileLoc = *repoSrc.psRepoName + SZPATHSEP + repoSrc.sRestPath;
		else
			m_sFileLoc=theUrl.sHost+theUrl.sPath;

		fileitem::tSpecialPurposeAttr attr {
			! cfg::offlinemode && data_type == FILE_VOLATILE,
					m_bIsHeadOnly,
					false,
					m_nReqRangeTo,
					""
        };

		m_ifMoSince = tHttpDate(h.h[header::IF_MODIFIED_SINCE]);

		ParseRange(h);

		m_pItem = m_parent.GetItemRegistry()->Create(m_sFileLoc,
														 attr.bVolatile ?
															 ESharingHow::AUTO_MOVE_OUT_OF_THE_WAY :
															 ESharingHow::ALWAYS_TRY_SHARING, attr);
		if( ! m_pItem.get())
		{
			USRERR("Error creating file item for "sv << m_sFileLoc << " -- check file permissions!"sv);
			return report_badcache();
		}

		if(cfg::DegradedMode())
			return SetEarlySimpleResponse("403 Cache server in degraded mode"sv);

#warning offload the setup call to background thread, due to the potential IO delay. Actually, need another fileitem state, like "presetup" which means that the task is active and other loaders shall back off and wait
		fistate = m_pItem.get()->Setup();

		LOG("Got initial file status: " << (int) fistate);

		if (bPtMode && fistate != fileitem::FIST_COMPLETE)
		{
			m_pItem.reset(new tPassThroughFitem(m_sFileLoc));
			fistate = fileitem::FIST_DLPENDING;
		}

		// might need to update the filestamp because nothing else would trigger it
		if(cfg::trackfileuse && fistate >= fileitem::FIST_DLGOTHEAD && fistate < fileitem::FIST_DLERROR)
			m_pItem.get()->UpdateHeadTimestamp();

		if(fistate == fileitem::FIST_COMPLETE)
			return; // perfect, done here

		if(cfg::offlinemode) { // make sure there will be no problems later in SendData or prepare a user message
			// error or needs download but freshness check was disabled, so it's really not complete.
			return SetEarlySimpleResponse("503 Unable to download in offline mode"sv);
		}
		dbgline;
		if( fistate < fileitem::FIST_DLASSIGNED) // we should assign a downloader then
		{
			dbgline;
			if(!m_parent.SetupDownloader())
			{
				USRDBG( "Error creating download handler for "<<m_sFileLoc);
				return report_overload(__LINE__);
			}

			dbgline;

			auto bHaveBackends = (repoSrc.repodata && !repoSrc.repodata->m_backends.empty());

			if (cfg::forcemanaged && !bHaveBackends)
				return report_notallowed();

			if (!bPtMode)
			{
				// XXX: this only checks the first found backend server, what about others?
				auto testUri = bHaveBackends ?
							repoSrc.repodata->m_backends.front().ToURI(
								false) + repoSrc.sRestPath :
							theUrl.ToURI(false);
				if (rex::MatchUncacheable(testUri, rex::NOCACHE_TGT))
				{
					m_pItem.reset(new tPassThroughFitem(m_sFileLoc));
					fistate = fileitem::FIST_DLPENDING;
				}
			}
			// if backend config not valid, download straight from the specified source

			string extraHeaders;
			if (cfg::exporigin && !callerHostname.empty())
			{
				extraHeaders = "X-Forwarded-For: "sv;
				if (!m_xff.empty())
				{
					extraHeaders += m_xff;
					extraHeaders += ", "sv;
				}
				extraHeaders += callerHostname;
				extraHeaders += svRN;
			}

			if (bPtMode && beconsum(be).front().length() > 0)
			{
				extraHeaders += header::ExtractCustomHeaders(beconsum(be).front().data(), bPtMode);
			}

			if (bHaveBackends
					? m_parent.SetupDownloader()->AddJob(as_lptr(m_pItem.get()), move(repoSrc), bPtMode, move(extraHeaders))
					: m_parent.SetupDownloader()->AddJob(as_lptr(m_pItem.get()), move(theUrl), bPtMode, move(extraHeaders)))
			{
				ldbg("Download job enqueued for " << m_sFileLoc);
			}
			else
			{
				USRERR("PANIC! Error creating download job for " << m_sFileLoc);
				return report_overload(__LINE__);
			}
		}
	}
	catch (const std::bad_alloc&) // OOM, may this ever happen here?
	{
		USRDBG("Out of memory");
		return report_overload(__LINE__);
	}
	catch(const std::out_of_range&) // better safe...
	{
		return report_invpath();
	}
}

void job::PrepareFatalError(const header &h, const string_view errorStatus)
{
	SetEarlySimpleResponse(errorStatus, true);
}

job::eJobResult job::subscribeAndExit()
{
	if (m_subKey.valid())
		return R_WILLNOTIFY;

	try
	{
		ASSERT(m_pItem); // very unlikely to fail
		m_subKey = m_pItem->Subscribe([this]() {
			return m_parent.poke(GetId());
		});
		return R_WILLNOTIFY;
	}
	catch (...)
	{
		return R_DISCON;
	}
}

off_t job::CheckSendableState()
{
	auto st = m_pItem->GetStatus();
	if (st > fileitem::FIST_COMPLETE)
		return -1;
	if (! m_dataSender && m_pItem)
		m_dataSender = m_pItem->GetCacheSender(m_nReqRangeFrom);
	if (! m_dataSender)
		return 0;
	return m_dataSender->NewBytesAvailable();
}

ebstream job::GetBufFmter()
{
	if (!m_preHeadBuf.valid())
		m_preHeadBuf.reset(evbuffer_new());
	return ebstream(*m_preHeadBuf);
};

job::eJobResult job::Resume(bool canSend, bufferevent* be)
{
	LOGSTARTFUNC;

	// use this return helper for better tracking, actually the caller should never return
	auto return_discon = [&]()
	{
		LOG("EXPLICIT DISCONNECT");
		m_activity = STATE_DISCO_ASAP;
		return R_DISCON;
	};
	auto fin_stream_good = [&]()
	{
		LOG("CLEAN JOB FINISH");
		if(m_keepAlive == KEEP)
			return m_activity = STATE_DONE, R_DONE;
		if(m_keepAlive == CLOSE)
		{
			return return_discon();
		}
		// unspecified?
		if (m_bIsHttp11)
			return m_activity = STATE_DONE, R_DONE;
		return return_discon();
	};

	if (AC_UNLIKELY(!be))
	{
		return return_discon(); // shouldn't be here
	}

	auto fi = m_pItem.get();
	ASSERT(fi);

	do
	{
		LOG(int(m_activity));

		if (m_preHeadBuf.valid())
		{
			ldbg("prebuf sending: " << m_preHeadBuf.get());
			if (0 != evbuffer_add_buffer(besender(be), *m_preHeadBuf))
				return R_DISCON;
			m_preHeadBuf.reset();
		}

		switch (m_activity)
		{
		case (STATE_DONE):
		{
			return fin_stream_good();
		}
		case (STATE_NOT_STARTED):
		{
#define CHECK_SENDABLE_OR_BACKOFF auto sstate = CheckSendableState(); \
	if (sstate == 0) return subscribeAndExit(); \
	if (sstate < 0) { if (HandleSuddenError()) continue; return R_DISCON; };

			if (fi->GetStatus() < fileitem::FIST_DLGOTHEAD)
				return subscribeAndExit();

			dbgline;
			CookResponseHeader();
			ASSERT(m_activity != STATE_NOT_STARTED);
			// state was changed to sending or error
			continue;
		}
		case (STATE_SEND_BUF_NOT_FITEM):
		{
			// got here when our head and optional data was send via sendbuf
			return fin_stream_good();
		}
		case (STATE_DISCO_ASAP):
		{
			return R_DISCON;
		}
		case (STATE_SEND_DATA):
		{
			if (m_bIsHeadOnly) // there is no data to come!
				return fin_stream_good();

			CHECK_SENDABLE_OR_BACKOFF;

			ASSERT(fi->GetCheckedSize() >=0);

			auto limit = fi->GetCheckedSize() - m_nSendPos;
			if (m_nReqRangeTo >= 0)
				limit = min(m_nReqRangeTo + 1 - m_nSendPos, limit);
			if (limit <= 0)
				return R_DISCON;
			ldbg("~senddata: to " << fi->GetCheckedSize() << ", OLD m_nSendPos: " << m_nSendPos);
			auto n = m_dataSender->SendData(be, limit);
			if (n < 0)
				return return_discon();
			m_nSendPos += n;
			m_nAllDataCount += n;
			if (n < limit)
				return subscribeAndExit();
			ldbg("~senddata: " << n << " new m_nSendPos: " << m_nSendPos);
			if ((fi->GetStatus() == fileitem::FIST_COMPLETE && m_nSendPos == fi->GetCheckedSize())
				|| (m_nReqRangeTo >= 0 && m_nSendPos >= m_nReqRangeTo + 1))
			{
				return fin_stream_good();
			}
			continue;
		}
		case (STATE_SEND_CHUNK_HEADER):
		{
			CHECK_SENDABLE_OR_BACKOFF;
			ebstream hfmt(be);
			if (fi->GetStatus() == fileitem::FIST_COMPLETE && m_nSendPos == fi->GetCheckedSize())
			{
				hfmt << "0\r\n\r\n"sv;
				m_activity = STATE_DONE;
			}
			else
			{
				m_nChunkEnd = fi->GetCheckedSize();
				hfmt << tSS::hex << (m_nChunkEnd - m_nSendPos) << svRN;
				m_activity = STATE_SEND_CHUNK_DATA;
			}
			continue;
		}
		case (STATE_SEND_CHUNK_DATA):
		{
			CHECK_SENDABLE_OR_BACKOFF;
			// this is only entered after STATE_SEND_CHUNK_HEADER
			auto limit = m_nChunkEnd - m_nSendPos;
			auto n = m_dataSender->SendData(be, limit);
			if (n < 0)
				return return_discon();
			m_nSendPos += n;
			m_nAllDataCount += n;
			if (n < limit)
				return subscribeAndExit();
			if (m_nSendPos == m_nChunkEnd)
			{
				SB << svRN;
				m_activity = STATE_SEND_CHUNK_HEADER;
			}
			continue;
		}
		}
	} while(true);
	return return_discon();
}

inline void job::AddPtHeader(cmstring& remoteHead)
{
	const static std::string dummyTE("\nX-No-Trans-Encode:"), badTE("\nTransfer-Encoding:");
	auto szHeadBegin = remoteHead.c_str();
	// don't care about its contents, with exception of chunked transfer-encoding
	// since it's too messy to support, use the plain close-on-end strategy here
	auto szTEHeader = strcasestr(szHeadBegin, badTE.c_str());
	if(szTEHeader == nullptr)
		SB << remoteHead;
	else
	{
		m_keepAlive = CLOSE;
		SB << string_view(szHeadBegin, szTEHeader - szHeadBegin);
		// as long as the te string
		SB << dummyTE;
		auto szRest = szTEHeader + badTE.length();
		SB << string_view(szRest, szHeadBegin + remoteHead.length() - szRest);
	}
#warning bs, use regex to catch space variants
	if(strcasestr(szHeadBegin, "Connection: close\r\n"))
		m_keepAlive = CLOSE;
}

bool job::HandleSuddenError()
{
	LOGSTARTFUNC;
	// response ongoing, can only reject the client now?
	if (m_nAllDataCount)
	{
		m_activity = STATE_DISCO_ASAP;
		return false;
	}
	m_activity = STATE_SEND_BUF_NOT_FITEM;

	if (!m_pItem.get())
		return SetEarlySimpleResponse("500 Error creating cache object"sv), true;

	auto st = m_pItem.get()->m_responseStatus;

	// do we have anything harmles useful to tell the user here?
	if (st.code < 500) // that's too strange, this should not cause a fatal error in item, report something generic instead
		SetEarlySimpleResponse("500 Remote or cache error"sv);
	else
		SetEarlySimpleResponse(ltos(st.code) + " " + (st.msg.empty() ? "Unknown internal error" : st.msg));
	return true;
}

inline void job::CookResponseHeader()
{
	LOGSTARTFUNC;

	// the default continuation unless changed
	m_activity = STATE_SEND_DATA;

	auto quickResponse = [this] (string_view msg, bool nobody = false, decltype(m_activity) nextActivity = STATE_SEND_BUF_NOT_FITEM)
	{
		SetEarlySimpleResponse(msg, nobody);
		m_activity = nextActivity;
	};
	auto fi = m_pItem.get();
	if (!fi)
		return quickResponse("500 Invalid cache object"sv, false, STATE_DISCO_ASAP);

	auto& remoteHead = fi->GetRawResponseHeader();
	if(!remoteHead.empty())
		return AddPtHeader(remoteHead);

	SB << tSS::dec;

	// we might need to massage this before responding
	auto status = fi->m_responseStatus;
	auto ds = fi->GetCheckedSize();
	auto fist = fi->GetStatus();

	auto addStatusLineFromItem = [&] ()
	{
		PrependHttpVariant();
		SB << status.code << " " << status.msg << svRN;
	};
	auto isRedir = status.isRedirect();

	if (isRedir || status.mustNotHaveBody())
	{
		addStatusLineFromItem();
		if (isRedir)
			SB << "Location: "sv << fi->m_responseOrigin << svRN;
		m_activity = STATE_SEND_BUF_NOT_FITEM;
		return AppendMetaHeaders();
	}
	// everything else is either an error, or not-modified, or must have content
	if (status.code != 200)
	{
		return quickResponse(ltos(status.code) + " " + status.msg);
	}

	if (m_ifMoSince.isSet() && fi->m_responseModDate.isSet() && m_ifMoSince >= fi->m_responseModDate)
	{
		return quickResponse("304 Not Modified"sv, true);
	}
	auto src = fi->m_responseOrigin.empty() ? "" : (string("X-Original-Source: ") + fi->m_responseOrigin + "\r\n");

	// detect special condition where we need chunked transfer - no optimizations
	// possible or considered here (because too nasty to track later errors,
	// better just resend it, this is a rare case anyway)
	auto contLen = fi->m_nContentLength;
#ifdef FORCE_CHUNKED
#warning FORCE_CHUNKED active!
	auto goChunked = true;
#else
	auto goChunked =
			contLen < 0 && fist == fileitem::FIST_DLRECEIVING
					// except for when only a range is wanted and that range is already available
					&& !(m_nReqRangeTo > 0 && ds >= m_nReqRangeTo);
#endif
	if (goChunked)
	{
		// set for full transfer in chunked mode
		m_activity = STATE_SEND_CHUNK_HEADER;
		m_nReqRangeTo = -1;
		m_nReqRangeFrom = 0;
		addStatusLineFromItem();
		SB << "Content-Type: "sv << fi->m_contentType << svRN;
		if (fi->m_responseModDate.isSet())
			SB << "Last-Modified: "sv << fi->m_responseModDate.view() << svRN;
		SB << "Transfer-Encoding: chunked\r\n"sv << src;
		AppendMetaHeaders();
		return;
	}
	// also check whether it's a sane range (if set)
	if (contLen >= 0 && (m_nReqRangeFrom >= contLen || m_nReqRangeTo + 1 > contLen))
		return quickResponse("416 Requested Range Not Satisfiable"sv);
	// okay, date is good, no chunking needed, can serve a range request smoothly (beginning is available) or fall back to 200?
	if (m_nReqRangeFrom >= 0 && ds > m_nReqRangeFrom && contLen > 0)
	{
		m_nSendPos = m_nReqRangeFrom;
		auto cl = m_nReqRangeTo < 0 ? contLen - m_nReqRangeFrom : m_nReqRangeTo + 1 - m_nReqRangeFrom;
		auto last = m_nReqRangeTo > 0 ? m_nReqRangeTo : contLen - 1;
		PrependHttpVariant();
		SB << "206 Partial Content"sv << svRN
		   << "Content-Type: "sv << fi->m_contentType << svRN
		   << "Last-Modified: "sv << fi->m_responseModDate.view() << svRN
		   << "Content-Range: bytes "sv << m_nReqRangeFrom << "-"sv << last << "/"sv << contLen << svRN
		   << "Content-Length: "sv << cl << svRN
		   << src;
		AppendMetaHeaders();
		return;
	}
	// everything else is plain full-body response
	m_nReqRangeTo = -1;
	m_nReqRangeFrom = 0;
	PrependHttpVariant();
	SB << "200 OK"sv << svRN
	   << "Content-Type: "sv << fi->m_contentType << svRN
	   << "Last-Modified: "sv << fi->m_responseModDate.view() << svRN
	   << "Content-Length: "sv << contLen << svRN
	   << src;
	AppendMetaHeaders();
	return;
}

void job::PrependHttpVariant()
{
	SB << (m_bIsHttp11 ? "HTTP/1.1 "sv : "HTTP/1.0 "sv);
}

void job::SetEarlySimpleResponse(string_view message, bool nobody)
{
	LOGSTARTFUNC;
	SB.clear();

	m_activity = STATE_SEND_BUF_NOT_FITEM;

	if (nobody)
	{
		PrependHttpVariant();
		SB << message << svRN;
		AppendMetaHeaders();
		return;
	}

	tSS body;

	body << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>"sv << message
		 << "</title>\n</head>\n<body><h1>"sv << message
		 << "</h1></body>"sv << GetFooter() << "</html>"sv;

	PrependHttpVariant();
	SB << message
	   << "\r\nContent-Length: "sv << body.size()
	   << "\r\nContent-Type: text/html\r\n"sv;
	AppendMetaHeaders();
	SB << body;
}

void job::PrepareLocalDownload(const mstring &visPath, const mstring &fsBase, const mstring &fsSubpath)
{
#warning implementme
}

void job::AppendMetaHeaders()
{
	auto fi = m_pItem.get();
	if (fi && ! fi->GetExtraResponseHeaders().empty())
		SB << fi->GetExtraResponseHeaders();

	if(m_keepAlive == KEEP)
		SB << "Connection: Keep-Alive\r\n"sv;
	else if(m_keepAlive == CLOSE)
		SB << "Connection: close\r\n"sv;
#ifdef DEBUG
	static atomic_int genHeadId(0);
	SB << "X-Debug: "sv << int(genHeadId++) << svRN;
#endif
	/*
	if (contentType.empty() && m_pItem.get())
		contentType = m_pItem.get()->m_contentType;
	if (!contentType.empty())
		m_sendbuf << "Content-Type: " << contentType << "\r\n";
	*/
	SB << "Date: "sv << tHttpDate(GetTime()).view()
			  << "\r\nServer: Debian Apt-Cacher NG/" ACVERSION
				 "\r\n\r\n"sv;
}

}
