/*
 * bgtask.cpp
 *
 *  Created on: 18.09.2009
 *      Author: ed
 */

#include <cstring>

#include "bgtask.h"

#include "acfg.h"
#include "meta.h"
#include "filereader.h"
#include "evabase.h"

#include <limits.h>
#include <errno.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

using namespace std;

#define LOG_DECO_START "<html><head><style type=\"text/css\">" \
".WARNING { color: orange; }\n.ERROR { color: red; }\n" \
"</style></head><body>"
#define LOG_DECO_END "</body></html>"

namespace acng
{

// for start/stop and abort hint
base_with_condition tSpecOpDetachable::g_StateCv;
bool tSpecOpDetachable::g_sigTaskAbort=false;
// not zero if a task is active
time_t nBgTimestamp = 0;

tSpecOpDetachable::~tSpecOpDetachable()
{
	if(m_reportStream.is_open())
	{
		m_reportStream << LOG_DECO_END;
		m_reportStream.close();
	}
	checkforceclose(m_logFd);
}

/*
 *  TODO: this is kept in expiration class for historical reasons. Should be moved to some shared upper
 * class, like "detachedtask" or something like that
 */
void tSpecOpDetachable::Run()
{

	if (m_parms.cmd.find("&sigabort")!=stmiss)
	{
		g_sigTaskAbort=true;
		g_StateCv.notifyAll();
		tStrPos nQuest=m_parms.cmd.find("?");
		if(nQuest!=stmiss)
		{
			tSS buf(255);
			buf << "HTTP/1.1 302 Redirect\r\nLocation: "
				<< m_parms.cmd.substr(0,nQuest)<< "\r\nConnection: close\r\n\r\n";
			SendRawData(buf.data(), buf.size(), 0);
		}
		return;
	}

	SetMimeResponseHeader("200 OK", "text/html");

	tSS deco;
	const char *mark(nullptr);
	if(m_szDecoFile &&
			( deco.initFromFile((cfg::confdir+SZPATHSEP+m_szDecoFile).c_str())
					|| (!cfg::suppdir.empty() &&
							deco.initFromFile((cfg::suppdir+SZPATHSEP+m_szDecoFile).c_str()))))
	{
		mark=::strchr(deco.rptr(), '~');
		if(mark)
		{
			// send fancy header only to the remote caller
			SendChunkRemoteOnly(deco.rptr(), mark-deco.rptr());
			deco.drop((mark-deco.rptr())+1);
		}
		else
		{
			// send fancy header only to the remote caller
			SendChunkRemoteOnly(deco.c_str(), deco.size());
			deco.clear();
		}
	}

	tSS logPath;

	time_t other_id=0;

	{ // this is locked just to make sure that only one can register as master
		lockguard guard(g_StateCv);
		if(0 == nBgTimestamp) // ok, not running yet -> become the log source then
		{
			auto id = time(0);

			logPath.clear();
			logPath<<cfg::logdir<<CPATHSEP<< MAINT_PFX << id << ".log.html";
			m_reportStream.open(logPath.c_str(), ios::out);
			if(m_reportStream.is_open())
			{
				m_reportStream << LOG_DECO_START;
				m_reportStream.flush();
				nBgTimestamp = id;
			}
			else
			{
				nBgTimestamp = 0;
				SendChunk("Failed to create the log file, aborting.");
				return;
			}
		}
		else other_id = nBgTimestamp;
	}

	if(other_id)
	{
		SendChunkSZ("<font color=\"blue\">A maintenance task is already running!</font>\n");
		SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
				<< "\">Cancel</a>)";
		SendChunkSZ("<br>Attempting to attach to the log output... <br>\n");

		tSS sendbuf(4096);
		lockuniq g(g_StateCv);

		for(;;)
		{
			if(other_id != nBgTimestamp)
			{
				// task is gone or replaced by another?
				SendChunkSZ("<br><b>End of log output. Please reload to run again.</b>\n");
				goto finish_action;
			}
			if(m_logFd < 0)
			{
				logPath.clear();
				logPath<<cfg::logdir<<CPATHSEP<<MAINT_PFX << other_id << ".log.html";
				m_logFd=open(logPath.c_str(), O_RDONLY|O_NONBLOCK);

				if(m_logFd>=0)
					SendChunk("ok:<br>\n");
				else
				{
					SendChunk("Failed to open log output, please retry later.<br>\n");
					goto finish_action;
				}

			}
			while(true)
			{
				int r = sendbuf.sysread(m_logFd);
				if(r < 0) // error
					goto finish_action;
				if(r == -EAGAIN)
				{
					g_StateCv.wait_for(g, 1, 1);
					if(!nBgTimestamp)
						break;
					continue;
				}
				if(r == 0)
				{
					g_StateCv.wait_for(g, 5, 1);
					if(!nBgTimestamp)
						break;
					continue;
				}
				SendChunkRemoteOnly(sendbuf.rptr(), sendbuf.size());
				sendbuf.clear();
				if(r>0)
				{
					// read more once?
					continue;
				}
				if(!nBgTimestamp)
					break;
			}
		}
		// unreachable
		goto finish_action;
	}
	else
	{
			/*****************************************************
			 * This is the worker part
			 *****************************************************/
			lockuniq g(&g_StateCv);
			g_sigTaskAbort=false;
			tDtorEx cleaner([&](){g.reLockSafe(); nBgTimestamp = 0; g_StateCv.notifyAll();});
			g.unLock();

			SendFmt << "Maintenance task <b>" << GetTaskName()
					<< "</b>, apt-cacher-ng version: " ACVERSION;
			string link = "http://" + GetMyHostPort() + "/" + cfg::reportpage;
			SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
					<< "\">Cancel</a>)"
					<< "\n<!--\n"
					<< maark << int(ControLineType::BeforeError)
					<< "Maintenance Task: " << GetTaskName() << "\n"
					<< maark << int(ControLineType::BeforeError)
					<< "See file " << logPath << " for more details.\n"
					<< maark << int(ControLineType::BeforeError)
					<< "Server control address: " << link
					<< "\n-->\n";
			string xlink = "<br>\nServer link: <a href=\"" + link + "\">" + link + "</a><br>\n";
			SendChunkLocalOnly(xlink.data(), xlink.size());
			SendFmt << "<br>\n";
			SendChunkRemoteOnly(WITHLEN("<form id=\"mainForm\" action=\"#top\">\n"));

			Action();


			if (!m_pathMemory.empty())
			{
				bool unchint=false;
				bool ehprinted=false;

				for(const auto& err: m_pathMemory)
				{
					if(err.second.msg.empty())
						continue;

					if(!ehprinted)
					{
						ehprinted=true;
						SendChunkRemoteOnly(WITHLEN(
								"<br><b>Error summary:</b><br>"));
					}

					unchint = unchint
							||endsWithSzAr(err.first, "Packages")
							||endsWithSzAr(err.first, "Sources");

					SendFmtRemote << err.first << ": <label>"
							<< err.second.msg
							<<  "<input type=\"checkbox\" name=\"kf\" value=\""
							<< to_base36(err.second.id)
							<< "\"</label>" << hendl;
				}

				if(unchint)
				{
					SendChunkRemoteOnly(WITHLEN(
					"<i>Note: some uncompressed index versions like Packages and Sources are no"
					" longer offered by most mirrors and can be safely removed if a compressed version exists.</i>\n"
							));
				}

				SendChunkRemoteOnly(WITHLEN(
				"<br><b>Action(s):</b><br>"
					"<input type=\"submit\" name=\"doDelete\""
					" value=\"Delete selected files\">"
					"|<input type=\"submit\" name=\"doTruncate\""
											" value=\"Truncate selected files to zero size\">"
					"|<button type=\"button\" onclick=\"checkOrUncheck(true);\">Check all</button>"
					"<button type=\"button\" onclick=\"checkOrUncheck(false);\">Uncheck all</button><br>"));
				auto blob=BuildCompressedDelFileCatalog();
				SendChunkRemoteOnly(blob.data(), blob.size());
			}

			SendFmtRemote << "<br>\n<a href=\"/"<< cfg::reportpage<<"\">Return to main page</a>"
					"</form>";
			auto& f(GetFooter());
						SendChunkRemoteOnly(f.data(), f.size());

	}

	finish_action:

	if(!deco.empty())
		SendChunkRemoteOnly(deco.c_str(), deco.size());
}

bool tSpecOpDetachable::CheckStopSignal()
{
#warning restore protection
	//lockguard g(&g_StateCv);
	return g_sigTaskAbort || evabase::in_shutdown;
}

void tSpecOpDetachable::DumpLog(time_t id)
{
	filereader reader;

	if (id<=0)
		return;

	tSS path(cfg::logdir.length()+24);
	path<<cfg::logdir<<CPATHSEP<<MAINT_PFX << id << ".log.html";
	if (!reader.OpenFile(path))
        SendChunkRemoteOnly("Log not available");
	else
        SendChunkRemoteOnly(reader.getView());
}

void tSpecOpDetachable::SendChunkLocalOnly(const char *data, size_t len)
{
	if(m_reportStream.is_open())
	{
		m_reportStream.write(data, len);
		m_reportStream.flush();
		g_StateCv.notifyAll();
	}
}

time_t tSpecOpDetachable::GetTaskId()
{
	lockguard guard(&g_StateCv);
	return nBgTimestamp;
}

#ifdef HAVE_ZLIB
mstring tSpecOpDetachable::BuildCompressedDelFileCatalog()
{
	mstring ret;
	tSS buf;

	// add the recent command, then the file records

	auto addLine = [&buf](unsigned id, cmstring& s)
		{
		unsigned len=s.size();
		buf.add((const char*) &id, sizeof(id))
				.add((const char*) &len, sizeof(len))
				.add(s.data(), s.length());
		};
	// don't care about the ID, compression will solve it
	addLine(0, m_parms.cmd);
	for(const auto& kv: m_pathMemory)
		addLine(kv.second.id, kv.first);

	unsigned uncompSize=buf.size();
	tSS gzBuf;
	uLongf gzSize = compressBound(buf.size())+32; // extra space for length header
	gzBuf.setsize(gzSize);
	// length header
	gzBuf.add((const char*)&uncompSize, sizeof(uncompSize));
	if(Z_OK == compress((Bytef*) gzBuf.wptr(), &gzSize,
			(const Bytef*)buf.rptr(), buf.size()))
	{
		ret = "<input type=\"hidden\" name=\"blob\"\nvalue=\"";
		ret += EncodeBase64(gzBuf.rptr(), (unsigned short) gzSize+sizeof(uncompSize));
		ret += "\">";
		return ret;
	}
	return "";
}

#endif

#ifdef DEBUG
void tBgTester::Action()
{
	for (int i = 0; i < 10 && !CheckStopSignal(); i++, sleep(1))
	{
		char buf[1024];
		time_t t;
		struct tm *tmp;
		t = time(nullptr);
		tmp = localtime(&t);
		strftime(buf, sizeof(buf), "%c", tmp);
		SendFmt << buf << "<br>\n";
	}
}

#endif // DEBUG


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
			SetEarlySimpleResponse("403 Permission denied");
			break;
		case EBADF:
		case EFAULT:
		case ENOMEM:
		case EOVERFLOW:
		default:
			SetEarlySimpleResponse("500 Internal server error");
			break;
		case ELOOP:
			SetEarlySimpleResponse("500 Infinite link recursion");
			break;
		case ENAMETOOLONG:
			SetEarlySimpleResponse("500 File name too long");
			break;
		case ENOENT:
		case ENOTDIR:
			SetEarlySimpleResponse("404, File or directory not found");
			break;
		}
		return;
	}

	if(S_ISDIR(stbuf.st_mode))
	{
		// unconfuse the browser
		if (!endsWithSzAr(visPath, SZPATHSEPUNIX))
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
			m_pItem = m_parent.GetItemRegistry()->Create(make_shared<dirredirect>(visPath), false);
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
		m_pItem = m_parent.GetItemRegistry()->Create(p, false);
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
		SetEarlySimpleResponse("403 Unsupported data type");
		return;
	}
	/*
	 * This variant of file item handler sends a local file. The
	 * header data is generated as needed, the relative cache path variable
	 * is reused for the real path.
	 */
	class tLocalGetFitem : public TFileitemWithStorage
	{
	public:
		tLocalGetFitem(string sLocalPath, struct stat &stdata) : TFileitemWithStorage(sLocalPath)
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
	m_pItem = m_parent.GetItemRegistry()->Create(make_shared<tLocalGetFitem>(absPath, stbuf), false);
}


}
