
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
#include "aclocal.h"
#include "ptitem.h"
#include "rex.h"

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


// hunting Bug#955793: [Heisenbug] evbuffer_write_atmost returns -1 w/o updating errno
//#define DBG_DISCONNECT //std::cerr << "DISCO? " << __LINE__ << std::endl;

namespace acng
{

uint_fast32_t g_genJobId = 0;
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



inline bool job::ParseRangeAndIfMo(const header& h)
{

	/*
	 * Range: bytes=453291-
	 * ...
	 * Content-Length: 7271829
	 * Content-Range: bytes 453291-7725119/7725120
	 */

	m_ifMoSince = tHttpDate(h.h[header::IF_MODIFIED_SINCE]);

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

void job::Prepare(const header &h, bufferevent* be, cmstring& callerHostname, acres& res)
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
		SetEarlySimpleResponse("503 Error in cache data, please consult the apt-cacher.err logfile"sv);
	};
	auto report_invpath = [this]()
	{
		SetEarlySimpleResponse("403 Invalid path specification"sv);
	};
	auto report_notallowed = [this]()
	{
		SetEarlySimpleResponse("403 Forbidden file type or location"sv);
	};

	if (h.type != header::GET)
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
		else if (0 == strncasecmp(h.h[header::CONNECTION], WITHLEN("Keep-Alive")))
			m_keepAlive = KEEP;
	}
	else if (m_bIsHttp11)
		m_keepAlive = CLOSE;

	constexpr string_view fname = "/_actmp";

	auto& matcher = res.GetMatchers();

	// "clever" file system browsing attempt?
	if(matcher.Match(sReqPath, rex::NASTY_PATH)
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
			m_pItem.reset(Create(EWorkType::USER_INFO, be, theUrl, nullptr, res));
			return m_pItem ? void() : report_overload(__LINE__);;
		}
		LOG("refined path: " << theUrl.sPath << "\n on host: " << theUrl.sHost);

		if (theUrl.m_schema != tHttpUrl::EProtoType::HTTP && theUrl.m_schema != tHttpUrl::EProtoType::HTTPS)
			return report_invpath();

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

		bPtMode=matcher.MatchUncacheable(theUrl.ToURI(false), rex::NOCACHE_REQ);

		LOG("input uri: "<<theUrl.ToURI(false)<<" , dontcache-flag? " << bPtMode
			<< ", admin-page: " << cfg::reportpage);

		try
		{
			auto t = DetectWorkType(theUrl, sReqPath, h.h[header::AUTHORIZATION]);
			ldbg("type: " << (int) t);
			if (t)
			{
				m_pItem.reset(Create(t, be, theUrl, nullptr, res));
				return m_pItem ? void() : report_overload(__LINE__);
			}
		}
		catch (const exception& ex)
		{
			LOG(ex.what());
			return report_overload(__LINE__);
		}
		catch (...)
		{
			return report_overload(__LINE__);
		}

		{
			auto it = cfg::localdirs.find(theUrl.sHost);
			if (it != cfg::localdirs.end())
			{
				ParseRangeAndIfMo(h);
				aclocal::TParms serveParms;
				serveParms.visPath = sReqPath;
				serveParms.fsBase = it->second;
				serveParms.fsSubpath = theUrl.sPath;
				serveParms.offset = m_nReqRangeFrom < 0 ? 0 : m_nReqRangeFrom;
				m_pItem.reset(Create(EWorkType::LOCALITEM, be, theUrl, &serveParms, res));
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
				data_type = rex::FILE_VOLATILE;
				bPtMode=true;
#endif
			}
			if (data_type == rex::FILE_INVALID)
			{
				LOG("generic user information page for " << theUrl.sPath);
				m_pItem.reset(Create(EWorkType::USER_INFO, be, theUrl, nullptr, res));
				return m_pItem ? void() : report_overload(__LINE__);;
			}
		}

		// in PT mode we don't care about how to handle it, it's what user wants to do
		if(data_type == rex::FILE_INVALID && bPtMode)
			data_type = rex::FILE_SOLID;

		if(data_type == rex::FILE_INVALID)
			data_type = matcher.GetFiletype(theUrl.sPath);

		if ( data_type == rex::FILE_INVALID )
		{
			if(!cfg::patrace)
			{
				// just for the log
				m_sFileLoc = theUrl.sPath;
				return report_notallowed();
			}

			// ok, collect some information helpful to the user
			data_type = rex::FILE_VOLATILE;
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
			! cfg::offlinemode && data_type == rex::FILE_VOLATILE,
					m_bIsHeadOnly,
					false,
					m_nReqRangeTo,
					""
        };

		ParseRangeAndIfMo(h);

		m_pItem = res.GetItemRegistry()->Create(m_sFileLoc,
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
			if(!m_parent.GetDownloader())
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
				if (matcher.MatchUncacheable(testUri, rex::NOCACHE_TGT))
					m_pItem.reset(new tPassThroughFitem(m_sFileLoc));
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
				// can get the first chunk in this eventbuffer because the header parser has previously linearized a sufficient range!
				extraHeaders += header::ExtractCustomHeaders(beconsum(be).front(), bPtMode);
			}

			if (m_parent.GetDownloader()->AddJob(as_lptr(m_pItem.get()),
												   bHaveBackends ? nullptr : &theUrl,
												   bHaveBackends ? &repoSrc : nullptr,
												   bPtMode, move(extraHeaders)))
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

void job::PrepareFatalError(string_view errorStatus)
{
	SetEarlySimpleResponse(errorStatus, true);
}

inline job::eJobResult job::subscribeAndExit(int IFDEBUG(line))
{
	if (m_subKey)
		return R_WILLNOTIFY;

	LOGSTARTFUNCx(line);

	try
	{
#warning better function for range start -> compare right there at the source and don't call us until condition is satisfied. must also check dl-receiving state and other conditions, though
		ASSERT(m_pItem); // very unlikely to fail
		m_subKey = m_pItem->Subscribe([this]() {
			return m_parent.poke(GetId());
		});
		return R_WILLNOTIFY;
	}
	catch (...)
	{
		dbgline;
		return R_DISCON;
	}
}

inline ebstream job::GetBufFmter()
{
	if (!m_preHeadBuf.valid())
		m_preHeadBuf.reset(evbuffer_new());
	return ebstream(*m_preHeadBuf);
};

job::eJobResult job::Resume(bool canSend, bufferevent* be)
{
	LOGSTARTFUNC;

	// use this return helper for better tracking, actually the caller should never return
	auto return_discon = [&](int IFDEBUG(line))
	{
		LOG("EXPLICIT DISCONNECT " << line);
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
			return return_discon(__LINE__);
		}
		// unspecified?
		if (m_bIsHttp11)
			return m_activity = STATE_DONE, R_DONE;
		return return_discon(__LINE__);
	};
#define HANDLE_OR_DISCON { if (HandleSuddenError()) continue ; return return_discon(__LINE__); }
#define SET_SENDER if (fist < fileitem::FiStatus::FIST_DLBODY) return subscribeAndExit(__LINE__); \
	if (!m_dataSender) m_dataSender = fi->GetCacheSender(); \
	if (!m_dataSender) HANDLE_OR_DISCON;

	if (AC_UNLIKELY(!be))
	{
		return return_discon(__LINE__); // shouldn't be here
	}

	auto fi = m_pItem.get();
	if (m_activity != STATE_SEND_BUF_NOT_FITEM
			&& m_activity != STATE_DONE
			&& !fi)
	{
		if (!HandleSuddenError())
			return R_DISCON;
	}

	fileitem::FiStatus fist;

	do
	{
		LOG(int(m_activity));

		// shouldn't be here.
//XXX: is this actually needed? No notifications are currently passed unless the sending position is reached.
		if (!canSend)
			return return_discon(__LINE__);

		if (m_preHeadBuf.valid())
		{
			ldbg("prebuf sending: " << *m_preHeadBuf);
			auto len = evbuffer_get_length(*m_preHeadBuf);
			if (0 != evbuffer_add_buffer(besender(be), *m_preHeadBuf))
				return R_DISCON;

			// never used again, even chunk-head-builder writes to stream
			m_preHeadBuf.reset();
			m_nAllDataCount += len;

			// that's a wildcard for special operation with just the prebuf and no file item in placex^
			if (m_activity == STATE_SEND_BUF_NOT_FITEM)
				return fin_stream_good();
		}

		if (fi)
		{
			fist = fi->GetStatus();
			if (fist > fileitem::FIST_COMPLETE)
				HANDLE_OR_DISCON;
		}

		switch (m_activity)
		{
		case (STATE_DONE):
		{
			return fin_stream_good();
		}
		case (STATE_NOT_STARTED):
		{
			if (fist < fileitem::FIST_DLGOTHEAD)
				return subscribeAndExit(__LINE__);
			if (fist < fileitem::FIST_COMPLETE)
			{
				auto have = fi->GetCheckedSize();
				if (have < (m_nReqRangeFrom != -1 ? m_nReqRangeFrom : 0))
					return subscribeAndExit(__LINE__);
			}

			CookResponseHeader();
			ASSERT(m_activity != STATE_NOT_STARTED);
			// state was changed to some sending state or error
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

			SET_SENDER;

			if (fi->GetCheckedSize() <= m_nSendPos)
				return subscribeAndExit(__LINE__);

			auto limit = fi->GetCheckedSize() - m_nSendPos;
			if (m_nReqRangeTo >= 0)
				limit = min(m_nReqRangeTo + 1 - m_nSendPos, limit);
			if (limit <= 0)
				return return_discon(__LINE__);
			//ldbg("~senddata: to " << fi->GetCheckedSize() << ", OLD m_nSendPos: " << m_nSendPos);
			auto n = m_dataSender->SendData(be, m_nSendPos, limit);
			if (n < 0)
				return return_discon(__LINE__);
			m_nAllDataCount += n;
			if (n < limit)
				// woot, a temporary glitch? let's try a few times
				return subscribeAndExit(__LINE__);
			//ldbg("~senddata: " << n << " new m_nSendPos: " << m_nSendPos);
			if ((fi->GetStatus() == fileitem::FIST_COMPLETE && m_nSendPos == fi->GetCheckedSize())
				|| (m_nReqRangeTo >= 0 && m_nSendPos >= m_nReqRangeTo + 1))
			{
				dbgline;
				return fin_stream_good();
			}
			continue;
		}
		case (STATE_SEND_CHUNK_HEADER):
		{
			m_nChunkEnd = fi->GetCheckedSize();
			if (m_nReqRangeTo >= 0 && m_nChunkEnd > m_nReqRangeTo)
				m_nChunkEnd = m_nReqRangeTo + 1;

			auto len = m_nChunkEnd - m_nSendPos;
			if (len == 0 && fist < fileitem::FIST_COMPLETE)
				return subscribeAndExit(__LINE__);

			ebstream outStream(be);
			LOG("clen: " << len);
			outStream << ebstream::imode::hex << len << svRN;

			if (len == 0)
			{
				m_activity = STATE_DONE;
				outStream << svRN;
			}
			else
			{
				m_activity = STATE_SEND_CHUNK_DATA;
			}
			continue;
		}
		case (STATE_SEND_CHUNK_DATA):
		{
			SET_SENDER;
			// entered after STATE_SEND_CHUNK_HEADER, end was checked
			auto limit = m_nChunkEnd - m_nSendPos;
			auto n = m_dataSender->SendData(be, m_nSendPos, limit);
			if (n < 0)
				return return_discon(__LINE__);
			m_nAllDataCount += n;
			if (n < limit)
				return subscribeAndExit(__LINE__);
			if (m_nSendPos == m_nChunkEnd)
			{
				bufferevent_write(be, svRN);
				m_activity = STATE_SEND_CHUNK_HEADER;
			}
			continue;
		}
		}
	} while(true);

	return return_discon(__LINE__);
}

bool job::KeepAlive(bufferevent *bev)
{
	if (m_activity > STATE_NOT_STARTED)
		return true;

	return 0 == bufferevent_write(bev,
								  (m_bIsHttp11
								   ? "HTTP/1.1 102 Processing\r\n\r\n"sv
								   : "HTTP/1.0 102 Processing\r\n\r\n"sv));
}

inline void job::AddPtHeader(cmstring& remoteHead)
{
	const static std::string dummyTE("\nX-No-Trans-Encode:"), badTE("\nTransfer-Encoding:");
	auto szHeadBegin = remoteHead.c_str();
	// don't care about its contents, with exception of chunked transfer-encoding
	// since it's too messy to support, use the plain close-on-end strategy here
	auto szTEHeader = strcasestr(szHeadBegin, badTE.c_str());
	auto SB = GetBufFmter();
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

inline bool job::HandleSuddenError()
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

	m_nAllDataCount++; // must not stay zero as a guard, and adding just 1 byte to count is negligible

	auto quickResponse = [this] (string_view msg, bool nobody = false, decltype(m_activity) nextActivity = STATE_SEND_BUF_NOT_FITEM)
	{
		SetEarlySimpleResponse(msg, nobody);
		m_activity = nextActivity;
	};
	auto fi = m_pItem.get();
	if (!fi)
		return quickResponse("500 Invalid cache object"sv, false, STATE_DISCO_ASAP);

	const auto& remoteHead = fi->GetRawResponseHeader();
	if(!remoteHead.empty())
		return AddPtHeader(remoteHead);

	auto SB = GetBufFmter();

	SB << ebstream::imode::dec;

	// we might need to massage this before responding
	auto status = fi->m_responseStatus;
	auto ds = fi->GetCheckedSize();

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
	// everything else is either an error, or not-modified, or must have content. For error pages, cut their content to avoid any side effects for user
	if (status.code != 200 && ! fi->IsLocallyGenerated())
	{
		return quickResponse(ltos(status.code) + " " + status.msg);
	}

	// detect special condition where we need chunked transfer - no optimizations
	// possible or considered here (because too nasty to track later errors,
	// better just resend it, this is a rare case anyway)
	auto contLen = fi->m_nContentLength;
	bool goChunked = false;
	const auto& reSt = fi->m_responseStatus;
	bool withRange = reSt.code == 200 && m_nReqRangeFrom >= 0;

	const auto& targetDate = fi->GetLastModified(true);
	if (m_ifMoSince.isSet() && targetDate.isSet() && targetDate <= m_ifMoSince)
	{
		return quickResponse("304 Not Modified"sv, true);
	}

	if (contLen >= 0)
	{
		if (reSt.code == 200)
		{
			// check whether it's a sane range
			if (m_nReqRangeFrom >= contLen || m_nReqRangeTo + 1 > contLen)
				return quickResponse("416 Requested Range Not Satisfiable"sv);
		}
	}
	else // unknown body length, what are the options?
	{
		goChunked = true;

		if (reSt.code == 200)
		{
			// have enough body range here?
			if (m_nReqRangeTo >= 0 && m_nReqRangeTo < ds)
				goChunked = false;
		}
	}
#if 0
#warning sending as chunked, no matter what
	goChunked = true;
#endif

	m_activity = goChunked ? STATE_SEND_CHUNK_HEADER : STATE_SEND_DATA;

	if (withRange)
		m_nSendPos = m_nReqRangeFrom;

	PrependHttpVariant();
	if (withRange)
		SB << "206 Partial Content"sv << svRN;
	else
		SB << reSt.code << " " << reSt.msg << svRN;
	if (!fi->m_contentType.empty())
		SB << "Content-Type: "sv << fi->m_contentType << svRN;
	if (fi->GetLastModified().isSet() && ! fi->GetLastModified().view().empty())
		SB << "Last-Modified: "sv << fi->GetLastModified().view() << svRN;
	if (!fi->m_responseOrigin.empty())
		SB << "X-Original-Source: "sv << fi->m_responseOrigin << svRN;
	if (goChunked)
		SB << "Transfer-Encoding: chunked"sv << svRN;
	if (withRange)
	{
		SB << "Content-Range: bytes "sv << m_nReqRangeFrom << "-"sv ;
		if (goChunked)
			SB << "-*/*"sv << svRN;
		else
		{
			auto last = m_nReqRangeTo > 0 ? m_nReqRangeTo : contLen - 1;
			auto cl = m_nReqRangeTo < 0 ? contLen - m_nReqRangeFrom : m_nReqRangeTo + 1 - m_nReqRangeFrom;
			SB << last << "/"sv << contLen << svRN
			   << "Content-Length: "sv << cl << svRN;
		}
	}
	else
	{
		if(!goChunked)
			SB << "Content-Length: "sv << contLen << svRN;
	}
	AppendMetaHeaders();
}

inline void job::PrependHttpVariant()
{
	auto SB = GetBufFmter();
	SB << (m_bIsHttp11 ? "HTTP/1.1 "sv : "HTTP/1.0 "sv);
}

void job::SetEarlySimpleResponse(string_view message, bool nobody)
{
	LOGSTARTFUNC;

	auto SB = GetBufFmter();
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

inline void job::AppendMetaHeaders()
{
	auto fi = m_pItem.get();
	auto SB = GetBufFmter();

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
