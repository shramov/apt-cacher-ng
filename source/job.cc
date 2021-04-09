
//#define LOCAL_DEBUG
#include "debug.h"

#include "job.h"
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <queue>
using namespace std;

#include "conn.h"
#include "acfg.h"
#include "fileitem.h"
#include "dlcon.h"
#include "sockio.h"
#include "fileio.h" // for ::stat and related macros
#include <dirent.h>
#include <algorithm>
#include "maintenance.h"
#include "evabase.h"
#include <event2/buffer.h>

#include <errno.h>

#if 0 // defined(DEBUG)
#define CHUNKDEFAULT true
#warning testing chunk mode
#else
#define CHUNKDEFAULT false
#endif

#define PT_BUF_MAX 16000

// hunting Bug#955793: [Heisenbug] evbuffer_write_atmost returns -1 w/o updating errno
#define DBG_DISCONNECT //std::cerr << "DISCO? " << __LINE__ << std::endl;
namespace acng
{

mstring sHttp11("HTTP/1.1");

tTraceData traceData;
void cfg::dump_trace()
{
	log::err("Paths with uncertain content types:");
	lockguard g(traceData);
	for (const auto& s : traceData)
		log::err(s);
}
tTraceData& tTraceData::getInstance()
{
	return traceData;
}

/*
 * Unlike the regular store-and-forward file item handler, this ones does not store anything to
 * harddisk. Instead, it uses the download buffer and lets job object send the data straight from
 * it to the client.
 *
 * The header is put in its original raw format aside, and is reprocessed again by the job while filtering the offending header values ONLY.
 */
class tPassThroughFitem : public fileitem
{
protected:
	evbuffer* m_q;

public:
	tPassThroughFitem(std::string s) : m_q(evbuffer_new())
	{
		LOGSTARTFUNC;
		if(!m_q)
			throw std::bad_alloc();
		m_sPathRel = s;
		m_nSizeChecked = m_nSizeCachedInitial = -1;
	};
	~tPassThroughFitem()
	{
		evbuffer_free(m_q);
	}
    virtual FiStatus Setup() override
	{
        //m_nSizeChecked = m_nSizeCachedInitial = 0;
		return m_status = FIST_INITED;
	}

	string m_sHeader;
	const std::string& GetRawResponseHeader() override { return m_sHeader; }

    void DlFinish(bool) override
	{
		lockuniq g(this);
		LOGSTARTFUNC
		notifyAll();
		m_status = FIST_COMPLETE;
	}

	unique_fd GetFileFd() override { return unique_fd(); }; // something, don't care for now

	bool DlAddData(string_view chunk) override
	{
		lockuniq g(this);

		LOGSTARTFUNCx(chunk.size(), m_status);

		// something might care, most likely... also about BOUNCE action
		notifyAll();

		if (m_status < FIST_DLRECEIVING)
		{
			m_status = FIST_DLRECEIVING;
			m_nSizeChecked = 0;
		}

		m_nIncommingCount += chunk.size();
		m_nSizeChecked += chunk.size();

		dbgline;
		if (m_status > fileitem::FIST_COMPLETE || m_status < FIST_DLGOTHEAD)
			return false;

		dbgline;
		m_status = FIST_DLRECEIVING;
		while (true)
		{
			// abandoned by the user?
			if (m_status >= FIST_DLERROR)
				LOGRET(false);
			if (evabase::in_shutdown)
				LOGRET(false);
			auto in_buffer = evbuffer_get_length(m_q);
			off_t nAddLimit = PT_BUF_MAX - in_buffer;
			auto nToAppend = std::min(nAddLimit, off_t(chunk.size()));
			if (0 == nToAppend)
			{
				wait_for(g, 5, 400);
				continue;
			}
			LOG("appending " << nToAppend << " to queue")
			bool failed = evbuffer_add(m_q, chunk.data(), nToAppend);
			if (failed)
				LOGRET(false);
			chunk.remove_prefix(nToAppend);
			if (chunk.empty() == 0)
				break;
		}

		LOGRET(true);
	}
	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow) override
	{
		LOGSTARTFUNC;
		lockuniq g(this);
		notifyAll();
		if (m_status > FIST_COMPLETE || evabase::in_shutdown)
			return -1;
		errno = -42;
		auto ret = evbuffer_write_atmost(m_q, out_fd, nMax2SendNow);
#ifdef WIN32
#error check WSAEWOULDBLOCK
#endif
		if (ret > 0)
			nSendPos += ret;
		if (ret < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == -42)
				return 0;
			DBG_DISCONNECT
			//std::cerr << errno << " for ret: " << ret << endl;
		}
		return ret;
	}

    // fileitem interface
protected:
    bool DlStarted(string_view rawHeader, const tHttpDate &, cmstring &origin, tRemoteStatus status, off_t, off_t bytesAnnounced) override
    {

            LOGSTARTFUNC
            setLockGuard

            m_sHeader = rawHeader;
            m_status = FIST_DLGOTHEAD;
            m_responseOrigin = origin;
            m_responseStatus = status;
#warning check usages, must cope with -1
            m_nContentLength = bytesAnnounced;
            return true;
    }
};

// base class for a fileitem replacement with custom storage for local data
class tGeneratedFitemBase : public fileitem
{
public:
	unique_fd GetFileFd() override { return unique_fd(); }; // something, don't care for now

	tSS m_data;

    tGeneratedFitemBase(const string &sFitemId, tRemoteStatus status, cmstring& origUrl = sEmptyString) : m_data(256)
	{
        m_status=FIST_DLRECEIVING;
		m_sPathRel=sFitemId;
        m_responseOrigin = origUrl;
        m_responseStatus = status;
        m_contentType = "text/html";
	}
	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow)
	override
	{
		if (m_status > FIST_COMPLETE || out_fd < 0)
			return -1;
		auto r = m_data.syswrite(out_fd, nMax2SendNow);
		if(r>0) nSendPos+=r;
		return r;
	}
	inline void seal()
	{
        // finish the building and seal the item
		m_nSizeChecked = m_data.size();
        m_nContentLength = m_nSizeChecked;
        m_status = FIST_COMPLETE;
    }
};

static const string miscError(" [HTTP error, code: ");

job::~job()
{
	LOGSTART("job::~job");
	int stcode = 200;
	if (m_pItem.get())
    {
        lockguard g(* m_pItem.get());
        stcode = m_pItem.get()->m_responseStatus.code;
	}

	bool bErr = m_sFileLoc.empty() || stcode >= 400;

	m_pParentCon->LogDataCounts(
			m_sFileLoc + (bErr ? (miscError + ltos(stcode) + ']') : sEmptyString),
            move(m_xff),
			(m_pItem.get() ? m_pItem.get()->TakeTransferCount() : 0),
			m_nAllDataCount, bErr);
}


inline void job::PrepareLocalDownload(const string &visPath,
		const string &fsBase, const string &fsSubpath)
{
	mstring absPath = fsBase+SZPATHSEP+fsSubpath;
	Cstat stbuf(absPath);
	if (!stbuf)
	{
		switch(errno)
		{
		case EACCES:
            SetEarlyErrorResponse("403 Permission denied");
			break;
		case EBADF:
		case EFAULT:
		case ENOMEM:
		case EOVERFLOW:
		default:
            SetEarlyErrorResponse("500 Internal server error");
			break;
		case ELOOP:
            SetEarlyErrorResponse("500 Infinite link recursion");
			break;
		case ENAMETOOLONG:
            SetEarlyErrorResponse("500 File name too long");
			break;
		case ENOENT:
		case ENOTDIR:
            SetEarlyErrorResponse("404, File or directory not found");
			break;
		}
		return;
	}

	if(S_ISDIR(stbuf.st_mode))
	{
		// unconfuse the browser
		if(!endsWithSzAr(visPath, SZPATHSEPUNIX))
		{
			class dirredirect : public tGeneratedFitemBase
			{
			public:	dirredirect(const string &visPath)
            : tGeneratedFitemBase(visPath, {301, "Moved Permanently"}, visPath + "/")
                {
					m_data << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>301 Moved Permanently</title></head><body><h1>Moved Permanently</h1>"
					"<p>The document has moved <a href=\""+visPath+"/\">here</a>.</p></body></html>";
					seal();
				}
			};
			m_pItem = TFileItemHolder::Create(make_shared<dirredirect>(visPath), false);
			return;
		}

		class listing: public tGeneratedFitemBase
		{
		public:
			listing(const string &visPath) :
                tGeneratedFitemBase(visPath, {200, "OK"})
			{
				seal(); // for now...
			}
		};
		auto p = make_shared<listing>(visPath);
		m_pItem = TFileItemHolder::Create(p, false);
		tSS & page = p->m_data;

		page << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>Index of "
				<< visPath << "</title></head>"
		"<body><h1>Index of " << visPath << "</h1>"
		"<table><tr><th>&nbsp;</th><th>Name</th><th>Last modified</th><th>Size</th></tr>"
		"<tr><th colspan=\"4\"><hr></th></tr>";

		DIR *dir = opendir(absPath.c_str());
		if (!dir) // weird, whatever... ignore...
			page<<"ERROR READING DIRECTORY";
		else
		{
			// quick hack with sorting by custom keys, good enough here
			priority_queue<tStrPair, std::vector<tStrPair>, std::greater<tStrPair>> sortHeap;
			for(struct dirent *pdp(0);0!=(pdp=readdir(dir));)
			{
				if (0!=::stat(mstring(absPath+SZPATHSEP+pdp->d_name).c_str(), &stbuf))
					continue;

				bool bDir=S_ISDIR(stbuf.st_mode);

				char datestr[32]={0};
				struct tm tmtimebuf;
				strftime(datestr, sizeof(datestr)-1,
						"%d-%b-%Y %H:%M", localtime_r(&stbuf.st_mtime, &tmtimebuf));

				string line;
				if(bDir)
					line += "[DIR]";
				else if(startsWithSz(cfg::GetMimeType(pdp->d_name), "image/"))
					line += "[IMG]";
				else
					line += "[&nbsp;&nbsp;&nbsp;]";
				line += string("</td><td><a href=\"") + pdp->d_name
						+ (bDir? "/\">" : "\">" )
						+ pdp->d_name
						+"</a></td><td>"
						+ datestr
						+ "</td><td align=\"right\">"
						+ (bDir ? string("-") : offttosH(stbuf.st_size));
				sortHeap.push(make_pair(string(bDir?"a":"b")+pdp->d_name, line));
				//dbgprint((mstring)line);
			}
			closedir(dir);
			while(!sortHeap.empty())
			{
				page.add(WITHLEN("<tr><td valign=\"top\">"));
				page << sortHeap.top().second;
				page.add(WITHLEN("</td></tr>\r\n"));
				sortHeap.pop();
			}

		}
		page << "<tr><td colspan=\"4\">" <<GetFooter();
		page << "</td></tr></table></body></html>";
		p->seal();
		return;
	}
	if(!S_ISREG(stbuf.st_mode))
	{
        SetEarlyErrorResponse("403 Unsupported data type");
		return;
	}
	/*
	 * This variant of file item handler sends a local file. The
	 * header data is generated as needed, the relative cache path variable
	 * is reused for the real path.
	 */
	class tLocalGetFitem : public fileitem_with_storage
	{
	public:
		tLocalGetFitem(string sLocalPath, struct stat &stdata) : fileitem_with_storage(sLocalPath)
		{
			m_status=FIST_COMPLETE;
			m_nSizeChecked=m_nSizeCachedInitial=stdata.st_size;
            m_spattr.bVolatile=false;
            m_responseStatus = { 200, "OK"};
            m_nContentLength = m_nSizeChecked = stdata.st_size;
            m_responseModDate = tHttpDate(stdata.st_mtim.tv_sec);
			cmstring &sMimeType=cfg::GetMimeType(sLocalPath);
			if(!sMimeType.empty())
                m_contentType = sMimeType;
		};
		unique_fd GetFileFd() override
		{
			int fd=open(m_sPathRel.c_str(), O_RDONLY);
		#ifdef HAVE_FADVISE
			// optional, experimental
			if(fd>=0)
				posix_fadvise(fd, 0, m_nSizeChecked, POSIX_FADV_SEQUENTIAL);
		#endif
			return unique_fd(fd);
		}
	};
	m_pItem = TFileItemHolder::Create(make_shared<tLocalGetFitem>(absPath, stbuf), false);
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
#warning use regex from dlcon
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

void job::Prepare(const header &h, string_view headBuf) {

    LOGSTART("job::PrepareDownload");
    
#ifdef DEBUGLOCAL
    cfg::localdirs["stuff"]="/tmp/stuff";
    log::dbg(m_pReqHead->ToString());
#endif

    string sReqPath, sPathResidual;
    tHttpUrl theUrl; // parsed URL

    dlrequest rq;

    fileitem::FiStatus fistate(fileitem::FIST_FRESH);
    bool bPtMode(false);

    if (h.h[header::XFORWARDEDFOR])
	{
		m_xff = h.h[header::XFORWARDEDFOR];
	}

	tSplitWalk tokenizer(h.frontLine, SPACECHARS);
	// some macros, to avoid goto style
    auto report_invport = [this]()
    {
        SetEarlyErrorResponse("403 Configuration error (confusing proxy mode) or prohibited port (see AllowUserPorts)");
    };
    auto report_overload = [this]()
    {
    	SetEarlyErrorResponse("503 Server overload, try later");
    };
    auto report_invpath = [this]()
    {
    	SetEarlyErrorResponse("403 Invalid path specification");
    };
    auto report_notallowed = [this]()
    {
    	SetEarlyErrorResponse("403 Forbidden file type or location");
    };

    if(h.type!=header::GET && h.type!=header::HEAD)
        return report_invpath();
    if(!tokenizer.Next() || !tokenizer.Next()) // at path...
        return report_invpath();
    UrlUnescapeAppend(tokenizer, sReqPath);
    if(!tokenizer.Next()) // at proto
        return report_invpath();
    m_bIsHttp11 = (sHttp11 != tokenizer.str());
    
    USRDBG( "Decoded request URI: " << sReqPath);

    if(h.h[header::CONNECTION])
	{
        if (0 == strncasecmp(h.h[header::CONNECTION], WITHLEN("close")))
			m_keepAlive = CLOSE;
        if (0 == strncasecmp(h.h[header::CONNECTION], WITHLEN("Keep-Alive")))
			m_keepAlive = KEEP;
	}

    // "clever" file system browsing attempt?
	if(rex::Match(sReqPath, rex::NASTY_PATH)
			|| stmiss != sReqPath.find(MAKE_PTR_0_LEN("/_actmp"))
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
			m_eMaintWorkType=tSpecialRequest::workUSERINFO;
			LOG("work type: USERINFO");
			return;
		}
		LOG("refined path: " << theUrl.sPath << "\n on host: " << theUrl.sHost);

		// extract the actual port from the URL
		char *pEnd(0);
		unsigned nPort = 80;
		LPCSTR sPort=theUrl.GetPort().c_str();
		if(!*sPort)
		{
			nPort = (uint) strtoul(sPort, &pEnd, 10);
			if('\0' != *pEnd || pEnd == sPort || nPort > TCP_PORT_MAX || !nPort)
                return report_invport();
		}

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

		if(!cfg::reportpage.empty() || theUrl.sHost == "style.css")
		{
			m_eMaintWorkType = tSpecialRequest::DispatchMaintWork(sReqPath,
                    h.h[header::AUTHORIZATION]);
			if(m_eMaintWorkType != tSpecialRequest::workNotSpecial)
			{
				m_sFileLoc = sReqPath;
				return;
			}
		}

		using namespace rex;

		{
			auto it = cfg::localdirs.find(theUrl.sHost);
			if (it != cfg::localdirs.end())
			{
				PrepareLocalDownload(sReqPath, it->second, theUrl.sPath);
                ParseRange(h);
				return;
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
				m_type = FILE_VOLATILE;
				bPtMode=true;
#endif
			}
			if (m_type == FILE_INVALID)
			{
				LOG("generic user information page for " << theUrl.sPath);
				m_eMaintWorkType = tSpecialRequest::workUSERINFO;
				return;
			}
		}

		// in PT mode we don't care about how to handle it, it's what user wants to do
		if(m_type == FILE_INVALID && bPtMode)
			m_type = FILE_SOLID;

		if(m_type == FILE_INVALID)
			m_type = GetFiletype(theUrl.sPath);

		if ( m_type == FILE_INVALID )
		{
			if(!cfg::patrace)
			{
				// just for the log
				m_sFileLoc = theUrl.sPath;
                return report_notallowed();
			}

			// ok, collect some information helpful to the user
			m_type = FILE_VOLATILE;
			lockguard g(traceData);
			traceData.insert(theUrl.sPath);
		}
		
		// got something valid, has type now, trace it
        USRDBG("Processing new job, " << h.frontLine);
        rq.repoSrc = cfg::GetRepNameAndPathResidual(theUrl);
        if(rq.repoSrc.psRepoName && !rq.repoSrc.psRepoName->empty())
            m_sFileLoc=*rq.repoSrc.psRepoName+SZPATHSEP+rq.repoSrc.sRestPath;
		else
			m_sFileLoc=theUrl.sHost+theUrl.sPath;

        fileitem::tSpecialPurposeAttr attr {
            ! cfg::offlinemode && m_type == FILE_VOLATILE,
                    h.type == header::eHeadType::HEAD,
                    m_nReqRangeTo,
                    ""
        };

        m_ifMoSince = tHttpDate(h.h[header::IF_MODIFIED_SINCE]);

        ParseRange(h);

        m_pItem = TFileItemHolder::Create(m_sFileLoc,
                                          attr.bVolatile ?
                                              ESharingHow::AUTO_MOVE_OUT_OF_THE_WAY :
                                              ESharingHow::ALWAYS_TRY_SHARING, attr);
	}
	catch(const std::out_of_range&) // better safe...
	{
        return report_invpath();
    }
    
    if( ! m_pItem.get())
    {
    	USRDBG("Error creating file item for " << m_sFileLoc);
        return report_overload();
    }

    if(cfg::DegradedMode())
        return SetEarlyErrorResponse("403 Cache server in degraded mode");
    
    fistate = m_pItem.get()->Setup();
	LOG("Got initial file status: " << (int) fistate);

	if (bPtMode && fistate != fileitem::FIST_COMPLETE)
		fistate = _SwitchToPtItem();


	// might need to update the filestamp because nothing else would trigger it
	if(cfg::trackfileuse && fistate >= fileitem::FIST_DLGOTHEAD && fistate < fileitem::FIST_DLERROR)
		m_pItem.get()->UpdateHeadTimestamp();

	if(fistate==fileitem::FIST_COMPLETE)
		return; // perfect, done here

    if(cfg::offlinemode) { // make sure there will be no problems later in SendData or prepare a user message
    	// error or needs download but freshness check was disabled, so it's really not complete.
        return SetEarlyErrorResponse("503 Unable to download in offline mode");
    }
    dbgline;
    if( fistate < fileitem::FIST_DLGOTHEAD) // needs a downloader
    {
    	dbgline;
    	if(!m_pParentCon->SetupDownloader())
    	{
    		USRDBG( "Error creating download handler for "<<m_sFileLoc);
            return report_overload();
    	}
    	
        dbgline;
        try
		{
            auto bHaveBackends = (rq.repoSrc.repodata
                    && !rq.repoSrc.repodata->m_backends.empty());

            if (cfg::forcemanaged && !bHaveBackends)
                return report_notallowed();

			if (!bPtMode)
			{
				// XXX: this only checks the first found backend server, what about others?
                auto testUri = bHaveBackends ?
                            rq.repoSrc.repodata->m_backends.front().ToURI(
                                false) + rq.repoSrc.sRestPath :
                            theUrl.ToURI(false);
				if (rex::MatchUncacheable(testUri, rex::NOCACHE_TGT))
					fistate = _SwitchToPtItem();
			}
            rq.setXff(h.h[header::XFORWARDEDFOR]);
			rq.isPassThroughRequest = bPtMode;

            // if backend config not valid, download straight from the specified source
            if (!bHaveBackends)
				rq.setSrc(theUrl);
			if (bPtMode)
                rq.setRqHeadString(headBuf.data());

			if (m_pParentCon->SetupDownloader()->AddJob(m_pItem.get(), rq))
			{
				ldbg("Download job enqueued for " << m_sFileLoc);
			}
			else
			{
				log::err(tSS() << "PANIC! Error creating download job for " << m_sFileLoc);
                return report_overload();
			}
		} catch (const std::bad_alloc&) // OOM, may this ever happen here?
		{
			USRDBG("Out of memory");
            return report_overload();
		};
	}
}

job::eJobResult job::SendData(int confd, bool haveMoreJobs)
{
    LOGSTARTFUNCx(m_activity, confd, haveMoreJobs);

    if (confd < 0)
    {
        DBG_DISCONNECT
        return R_DISCON; // shouldn't be here
    }
	if (m_eMaintWorkType != tSpecialRequest::eMaintWorkType::workNotSpecial)
	{
		tSpecialRequest::RunMaintWork(m_eMaintWorkType, m_sFileLoc, confd);
		DBG_DISCONNECT
		return R_DISCON; // just stop and close connection
	}

	if (!m_sendbuf.empty())
	{
		ldbg("prebuf sending: "<< m_sendbuf.c_str());
		auto r = send(confd, m_sendbuf.rptr(), m_sendbuf.size(),
				MSG_MORE * (m_activity == STATE_NOT_STARTED || haveMoreJobs));

		if (r < 0)
		{
			if (errno == EAGAIN || errno == EINTR || errno == ENOBUFS)
				return R_AGAIN;
			else
				return R_DISCON;
		}
		m_nAllDataCount += r;
		m_sendbuf.drop(r);
		if (!m_sendbuf.empty())
		{
			return R_AGAIN;
		}
	}

    off_t nBodySizeSoFar(0);
	fileitem::FiStatus fistate(fileitem::FIST_DLERROR);

    auto fi = m_pItem.get();

    auto return_stream_ok = [&]()
    {
        if(m_keepAlive == KEEP)
            return R_DONE;
        if(m_keepAlive == CLOSE)
        {
            DBG_DISCONNECT;
            return R_DISCON;
        }
        // unspecified?
        return m_bIsHttp11 ? R_DONE : R_DISCON;
    };

    /**
     * this is left only by a) item state mutating to error (returns false) or b) enough data becomes available (returns true)
     */
    auto AwaitSendableState = [&]()
    {
        if (!fi)
            return false;

        // make sure to collect enough data to continue
        lockuniq g(*fi);
        while(true)
        {
            fistate = fi->GetStatusUnlocked(nBodySizeSoFar);
            if (fistate >= fileitem::FIST_COMPLETE)
                break;
            dbgline;
            if (nBodySizeSoFar > m_nSendPos)
                break;
            fi->wait(g);
#warning add a 5s timeout and send a 102 or so for waiting?
        }
        return fistate <= fileitem::FIST_COMPLETE;
    };

    switch (m_activity)
    {
    case (STATE_DONE):
    {
        return return_stream_ok();
    }
    case (STATE_NOT_STARTED):
    {
        if (!AwaitSendableState())
            return HandleSuddenError();

        dbgline;
        CookResponseHeader();

        // decide where to go, unless changed by response analysis already
        if (m_activity == STATE_NOT_STARTED)
            m_activity = STATE_SEND_DATA;
        // prebuf was filled -> come back to send header  and continue with activity
        return R_AGAIN;
    }
    case (STATE_SEND_DATA):
    {
        if (!AwaitSendableState())
            return HandleSuddenError();

        m_filefd.reset(fi->GetFileFd());
        if (!m_filefd.valid())
            return HandleSuddenError();

        int n = fi->SendData(confd, m_filefd.get(), m_nSendPos, nBodySizeSoFar - m_nSendPos);
        ldbg("~senddata: " << n << " new m_nSendPos: " << m_nSendPos);
        if (n < 0)
            return HandleSuddenError();
        m_nAllDataCount += n;
        if (fistate == fileitem::FIST_COMPLETE && m_nSendPos == nBodySizeSoFar)
            return return_stream_ok();
        return R_AGAIN;
    }
    case (STATE_SEND_CHUNK_HEADER):
    {
        if (!AwaitSendableState())
            return HandleSuddenError();

        if (fistate == fileitem::FIST_COMPLETE && m_nSendPos == nBodySizeSoFar)
        {
            m_sendbuf << "0\r\n\r\n";
            m_activity = STATE_DONE;
            return R_AGAIN;
        }
        m_filefd.reset(fi->GetFileFd());
        if (!m_filefd.valid())
            return HandleSuddenError();

        m_nChunkEnd = nBodySizeSoFar;
        m_sendbuf << tSS::hex << (m_nChunkEnd - m_nSendPos) << "\r\n";
        m_activity = STATE_SEND_CHUNK_DATA;
        return R_AGAIN;
    }
    case (STATE_SEND_CHUNK_DATA):
    {
        // this is only entered after STATE_SEND_CHUNK_HEADER
        int n = fi->SendData(confd, m_filefd.get(), m_nSendPos, m_nChunkEnd - m_nSendPos);
        ldbg("~sendchunk: " << n << " new m_nSendPos: " << m_nSendPos);
        if (n < 0)
            return HandleSuddenError();
        m_nAllDataCount += n;
        if (m_nSendPos == m_nChunkEnd)
            m_activity = STATE_SEND_CHUNK_HEADER;
        return R_AGAIN;
    }
    }
	return R_DISCON;
}

inline void job::AddPtHeader(cmstring& remoteHead)
{
    const static std::string dummyTE("\nX-No-Trans-Encode:"), badTE("\nTransfer-Encoding:");
    auto szHeadBegin = remoteHead.c_str();
#warning review that
    // don't care about its contents, with exception of chunked transfer-encoding
    // since it's too messy to support, use the plain close-on-end strategy here
    auto szTEHeader = strcasestr(szHeadBegin, badTE.c_str());
    if(szTEHeader == nullptr)
        m_sendbuf << remoteHead;
    else
    {
        m_keepAlive = CLOSE;
        m_sendbuf.add(szHeadBegin, szTEHeader - szHeadBegin);
        // as long as the te string
        m_sendbuf<<dummyTE;
        auto szRest = szTEHeader + badTE.length();
        m_sendbuf.add(szRest, szHeadBegin + remoteHead.length() - szRest);
    }
    if(strcasestr(szHeadBegin, "Connection: close\r\n"))
        m_keepAlive = CLOSE;
}

job::eJobResult job::HandleSuddenError()
{
    if (m_nAllDataCount)
        return R_DISCON;
    m_activity = STATE_DONE;
    if (!m_pItem.get())
        return SetEarlyErrorResponse("500 Error creating cache object"), R_AGAIN;
    lockguard g(*m_pItem.get());
    auto st = m_pItem.get()->m_responseStatus;
    if (st.code < 400) // that's too strange, this should not cause a fatal error in item, report something generic instead
        return SetEarlyErrorResponse("500 Fatal cache object error"), R_AGAIN;

    return SetEarlyErrorResponse(ltos(st.code) + " " + st.msg), R_AGAIN;
}

inline void job::CookResponseHeader()
{
    LOGSTARTFUNC;

	auto& remoteHead = m_pItem.get()->GetRawResponseHeader();
	if(!remoteHead.empty())
        return AddPtHeader(remoteHead);

    m_sendbuf.clean() << tSS::dec;

    auto fi = m_pItem.get();
    if (!fi)
        return SetEarlyErrorResponse("500 Invalid cache object");

    lockguard g(*fi);
    // we might need to massage this before responding
    auto status = fi->m_responseStatus;
    off_t ds;
    auto fist = fi->GetStatusUnlocked(ds);

    auto firstLineFromStatus = [&] ()
    {
        return PrependHttpVariant() << status.code << " " << status.msg << svRN;
    };
    auto firstLineCoTypeCoDate = [&] ()
    {
        firstLineFromStatus()
                << "Content-Type: " << fi->m_contentType << svRN;
        if (fi->m_responseModDate.isSet())
            m_sendbuf << "Last-Modified: " << fi->m_responseModDate.any() << svRN;
        return m_sendbuf;
    };
    auto isRedir = status.isRedirect();
    if (isRedir || status.mustNotHaveBody())
    {
        firstLineFromStatus();
        if (isRedir)
            m_sendbuf << "Location: " << fi->m_responseOrigin << svRN;
        return AppendMetaHeaders();
    }
    // everything else is either an error, or not-modified, or must have content
    if (status.code != 200)
    {
        return SetEarlyErrorResponse(ltos(status.code) + " " + status.msg);
    }

    if (m_ifMoSince.isSet() && m_ifMoSince >= fi->m_responseModDate)
    {
        return SetEarlyErrorResponse("304 Not Modified");
    }
    auto src = fi->m_responseOrigin.empty() ? "" : (string("X-Original-Source: ") + fi->m_responseOrigin + "\r\n");

    // detect special condition where we need chunked transfer - no optimizations
    // possible or considered here (because too nasty to track later errors,
    // better just resend it, this is a rare case anyway)
    auto contLen = fi->m_nContentLength;
    if (contLen < 0 && fist == fileitem::FIST_DLRECEIVING
            // except for when only a range is wanted and that range is already available
            && !(m_nReqRangeTo > 0 && ds >= m_nReqRangeTo))
    {
        // set for full transfer in chunked mode
        m_activity = STATE_SEND_CHUNK_HEADER;
        m_nReqRangeTo = -1;
        m_nReqRangeFrom = 0;
        firstLineCoTypeCoDate() << "Transfer-Encoding: chunked\r\n" << src;
        AppendMetaHeaders();
        return;
    }
    // also check whether it's a sane range (if set)
    if (contLen >= 0 && (m_nReqRangeFrom >= contLen || m_nReqRangeTo + 1 >= contLen))
        return SetEarlyErrorResponse("416 Requested Range Not Satisfiable");
    // okay, date is good, no chunking needed, can serve a range request smoothly (beginning is available) or fall back to 200?
    if (m_nReqRangeFrom >= 0 && ds > m_nReqRangeFrom)
    {
        PrependHttpVariant() << "206 Partial Content" << svRN
                             << "Content-Type: " << fi->m_contentType << svRN
                             << "Last-Modified: " << fi->m_responseModDate.any() << svRN
                             << "Content-Range: " << m_nReqRangeFrom << "-" << m_nReqRangeTo << "/" << contLen << svRN
                             << src;
        AppendMetaHeaders();
        return;
    }
    // everything else is plain full-body response
    m_nReqRangeTo = -1;
    m_nReqRangeFrom = 0;
    PrependHttpVariant() << "200 OK" << svRN
                         << "Content-Type: " << fi->m_contentType << svRN
                         << "Last-Modified: " << fi->m_responseModDate.any() << svRN
                         << "Content-Length:" << contLen << svRN
                         << src;
    AppendMetaHeaders();
    return;

#if 0



	// make sure that header has consistent state and there is data to send which is expected by the client
    LOG("State: " << status.code);
	// nothing to send for special codes (like not-unmodified) or w/ GUARANTEED empty body
    bool forbiddenBody = status.mustNotHaveBody();

    bool bHasSendableData = !forbiddenBody;
#error murks, kein atoofften überall
    if (!respHead.h[header::CONTENT_LENGTH]) // not set, maybe chunked data
        {
            bHasSendableData = true;
        }
        if (atoofft(respHead.h[header::CONTENT_LENGTH])) // set, to non-0
            bHasSendableData = true;

	tSS &sb=m_sendbuf;
	sb.clear();
	sb << respHead.frontLine <<"\r\n";

	bool bLenWasPrinted(false);

	if(bHasSendableData)
	{
		LOG("has sendable content");
		if( ! respHead.h[header::CONTENT_LENGTH]
#ifdef DEBUG // might have been defined before for testing purposes
		                 || m_bChunkMode
#endif
				)
		{
			// unknown length but must have data, will have to improvise: prepare chunked transfer
			if ( m_proto != HTTP_11 ) // you cannot process this? go away
				return "505 HTTP version not supported for this file";
			m_bChunkMode=true;
			sb << "Transfer-Encoding: chunked\r\n";
		}
		else if(200==httpstatus) // state: good data response with known length, can try some optimizations
		{
			LOG("has known content length, optimizing response...");

			// Handle If-Modified-Since and Range headers;
			// we deal with them equally but need to know which to use
			const char *pIfmo = m_reqHead.h[header::RANGE] ?
					m_reqHead.h[header::IFRANGE] : m_reqHead.h[header::IF_MODIFIED_SINCE];
			const char *pLastMo = respHead.h[header::LAST_MODIFIED];

			// consider contents "fresh" for non-volatile data, or when "our" special client is there, or the client simply doesn't care
			bool bDataIsFresh = (m_type != rex::FILE_VOLATILE
				|| m_reqHead.h[header::ACNGFSMARK] || !pIfmo);

			auto tm1=tm(), tm2=tm();
			bool bIfModSeenAndChecked=false;
			if(pIfmo && header::ParseDate(pIfmo, &tm1) && header::ParseDate(pLastMo, &tm2))
			{
				time_t a(mktime(&tm1)), b(mktime(&tm2));
				LOG("if-mo-since: " << a << " vs. last-mo: " << b);
				bIfModSeenAndChecked = (a==b);
			}

			// is it fresh? or is this relevant? or is range mode forced?
			if(  bDataIsFresh || bIfModSeenAndChecked)
			{
				off_t nContLen=atoofft(respHead.h[header::CONTENT_LENGTH]);

				// Client requested with Range* spec?
				if(m_nReqRangeFrom >=0)
				{
					if(m_nReqRangeTo<0 || m_nReqRangeTo>=nContLen) // open-end? set the end to file length. Also when request range would be too large
						m_nReqRangeTo=nContLen-1;

					// or simply don't care within that rage
					bool bPermitPartialStart = (
							fistate >= fileitem::FIST_DLGOTHEAD
							&& fistate <= fileitem::FIST_COMPLETE
                            && nBodySizeSoFar >= ( m_nReqRangeFrom - cfg::maxredlsize));

					/*
					 * make sure that our client doesn't just hang here while the download thread is
					 * fetching from 0 to start position for many minutes. If the resumed position
					 * is beyond of what we already have, fall back to 200 (complete download).
					 */
					if(fistate==fileitem::FIST_COMPLETE
							// or can start sending within this range (positive range-from)
							|| bPermitPartialStart	// don't care since found special hint from acngfs (kludge...)
							|| m_reqHead.h[header::ACNGFSMARK] )
					{
						// detect errors, out-of-range case
						if(m_nReqRangeFrom>=nContLen || m_nReqRangeTo<m_nReqRangeFrom)
							return "416 Requested Range Not Satisfiable";

						m_nSendPos = m_nReqRangeFrom;
						m_nCurrentRangeLast = m_nReqRangeTo;
						// replace with partial-response header
						sb.clear();
						sb << "HTTP/1.1 206 Partial Response\r\nContent-Length: "
						 << (m_nCurrentRangeLast-m_nSendPos+1) <<
								"\r\nContent-Range: bytes " << m_nSendPos
								<< "-" << m_nCurrentRangeLast << "/" << nContLen << "\r\n";
						bLenWasPrinted = true;
					}
				}
				else if(bIfModSeenAndChecked)
				{
					// file is fresh, and user sent if-mod-since -> fine
					return "304 Not Modified";
				}
			}
		}

		// has cont-len available but this header was not set yet in the code above
		if( !bLenWasPrinted && !m_bChunkMode)
		{
			sb << "Content-Length: " << respHead.h[header::CONTENT_LENGTH] << "\r\n";
		}

		// OK, has data for user and has set content-length and/or range or chunked transfer mode, now add various meta headers...
		if(respHead.h[header::LAST_MODIFIED])
		{
			sb<<"Last-Modified: "<<respHead.h[header::LAST_MODIFIED]<<"\r\n";
		}

		sb<<"Content-Type: ";
		if(respHead.h[header::CONTENT_TYPE])
			sb<<respHead.h[header::CONTENT_TYPE]<<"\r\n";
		else
			sb<<"application/octet-stream\r\n";
	}
	else
	{
		sb<<"Content-Length: 0\r\n";
		bLenWasPrinted = true;
		m_backstate=STATE_ALLDONE;
	}
#warning remap from origsource field only for 30x, check by method in status struct
	if(respHead.h[header::LOCATION])
		sb<<"Location: "<<respHead.h[header::LOCATION]<<"\r\n";

	if(!m_sOrigUrl.empty())
		sb<<"X-Original-Source: "<< m_sOrigUrl <<"\r\n";


	LOG("response prepared:" << sb);

	if(m_reqHead.type==header::HEAD)
		m_backstate=STATE_ALLDONE; // simulated head is prepared but don't send stuff

	return nullptr;
#endif
}

fileitem::FiStatus job::_SwitchToPtItem()
{
	// Changing to local pass-through file item
	LOGSTART("job::_SwitchToPtItem");
	// exception-safe sequence
	m_pItem = TFileItemHolder::Create(make_shared<tPassThroughFitem>(m_sFileLoc), false);
    return m_pItem.get()->Setup();
}

tSS& job::PrependHttpVariant()
{
    return m_sendbuf << tSS::dec << (m_bIsHttp11 ? "HTTP/1.1 " : "HTTP/1.0 ");
}

void job::SetEarlyErrorResponse(string_view message)
{
    LOGSTARTFUNC
    m_sendbuf.clear();
    tSS body;

    body << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>" << message
    		<< "</title>\n</head>\n<body><h1>" << message
			<< "</h1></body>" << GetFooter() << "</html>";

	m_activity = STATE_DONE;
    PrependHttpVariant() << message
              << "\r\nContent-Length: " << body.size()
              << "\r\nContent-Type: text/html\r\n";
    AppendMetaHeaders();
    m_sendbuf << body;

}
void job::AppendMetaHeaders()
{
	if(m_keepAlive == KEEP)
		m_sendbuf << "Connection: Keep-Alive\r\n";
	else if(m_keepAlive == CLOSE)
        m_sendbuf << "Connection: close\r\n";

    /*
    if (contentType.empty() && m_pItem.get())
        contentType = m_pItem.get()->m_contentType;
    if (!contentType.empty())
        m_sendbuf << "Content-Type: " << contentType << "\r\n";
    */
    m_sendbuf << "Date: " << tHttpDate(GetTime()).any()
	<< "\r\nServer: Debian Apt-Cacher NG/" ACVERSION "\r\n"
	"\r\n";
}
}
