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

#include <condition_variable>

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
//mutex g_bgTaskMx;
//condition_variable g_bgTaskCondVar;
bool tExclusiveUserAction::g_sigTaskAbort=false;
// not zero if a task is active
time_t nBgTimestamp = 0;

tExclusiveUserAction::tExclusiveUserAction(tRunParms &&parms)
	: tSpecialRequestHandler(move(parms))
{
}

tExclusiveUserAction::~tExclusiveUserAction()
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
void tExclusiveUserAction::Run()
{
	if (m_parms.cmd.find("&sigabort")!=stmiss)
	{
		g_sigTaskAbort = true;
		//g_bgTaskCondVar.notify_all();
		tStrPos nQuest = m_parms.cmd.find("?");
		if(nQuest != stmiss)
			return m_parms.bitem().ManualStart(302, "Redirect", se, m_parms.cmd.substr(0,nQuest));
	}

	m_parms.bitem().ManualStart(200, "OK", "text/html");

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
			SendRemoteOnly(deco.rptr(), mark-deco.rptr());
			deco.drop((mark-deco.rptr())+1);
		}
		else
		{
			// send fancy header only to the remote caller
			SendRemoteOnly(deco.c_str(), deco.size());
			deco.clear();
		}
	}

	tSS logPath;

	//time_t other_id=0;

	{
		// this is locked just to make sure that only one can register as master
		//lguard g(g_bgTaskMx);

		if (0 == nBgTimestamp) // ok, not running yet -> become the log source then
		{
			auto id = time(0);

			logPath.clear();
			logPath << cfg::logdir<<CPATHSEP<< MAINT_PFX << id << ".log.html";
			m_reportStream.open(logPath.c_str(), ios::out);
			if (m_reportStream.is_open())
			{
				m_reportStream << LOG_DECO_START;
				m_reportStream.flush();
				nBgTimestamp = id;
			}
			else
			{
				nBgTimestamp = 0;
				Send("Failed to create the log file, aborting.");
				return;
			}
		}
//		else
//			other_id = nBgTimestamp;
	}
#if 0
	if(other_id)
	{
		SendChunkSZ("<font color=\"blue\">A maintenance task is already running!</font>\n");
		SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
				<< "\">Cancel</a>)";
		SendChunkSZ("<br>Attempting to attach to the log output... <br>\n");

		tSS sendbuf(4096);
		ulock g(g_bgTaskMx);

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
					Send("ok:<br>\n");
				else
				{
					Send("Failed to open log output, please retry later.<br>\n");
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
					g_bgTaskCondVar.wait_for(g, std::chrono::seconds(1));
					if(!nBgTimestamp)
						break;
					continue;
				}
				if(r == 0)
				{
					g_bgTaskCondVar.wait_for(g, std::chrono::seconds(5));
					if(!nBgTimestamp)
						break;
					continue;
				}
				SendRemoteOnly(sendbuf.rptr(), sendbuf.size());
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
#endif
	{
			/*****************************************************
			 * This is the worker part
			 *****************************************************/
			//ulock g(g_bgTaskMx);
			g_sigTaskAbort=false;
			/*
			TFinalAction cleaner([&]()
			{
				g.lock();
				nBgTimestamp = 0;
				g_bgTaskCondVar.notify_all();
			});
			g.unlock();
*/
			SendFmt << "Maintenance task <b>" << GetTaskName(m_parms.type)
					<< "</b>, apt-cacher-ng version: " ACVERSION;
			string link = "http://" + GetMyHostPort() + "/" + cfg::reportpage;
			SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
					<< "\">Cancel</a>)"
					<< "\n<!--\n"
					<< maark << int(ControLineType::BeforeError)
					<< "Maintenance Task: " << GetTaskName(m_parms.type) << "\n"
					<< maark << int(ControLineType::BeforeError)
					<< "See file " << logPath << " for more details.\n"
					<< maark << int(ControLineType::BeforeError)
					<< "Server control address: " << link
					<< "\n-->\n";
			string xlink = "<br>\nServer link: <a href=\"" + link + "\">" + link + "</a><br>\n";
			SendLocalOnly(xlink.data(), xlink.size());
			SendFmt << "<br>\n";
			SendRemoteOnly(WITHLEN("<form id=\"mainForm\" action=\"#top\">\n"));

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
						SendRemoteOnly(WITHLEN(
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
					SendRemoteOnly(WITHLEN(
					"<i>Note: some uncompressed index versions like Packages and Sources are no"
					" longer offered by most mirrors and can be safely removed if a compressed version exists.</i>\n"
							));
				}

				SendRemoteOnly(WITHLEN(
				"<br><b>Action(s):</b><br>"
					"<input type=\"submit\" name=\"doDelete\""
					" value=\"Delete selected files\">"
					"|<input type=\"submit\" name=\"doTruncate\""
											" value=\"Truncate selected files to zero size\">"
					"|<button type=\"button\" onclick=\"checkOrUncheck(true);\">Check all</button>"
					"<button type=\"button\" onclick=\"checkOrUncheck(false);\">Uncheck all</button><br>"));
				auto blob=BuildCompressedDelFileCatalog();
				SendRemoteOnly(blob.data(), blob.size());
			}

			SendFmtRemote << "<br>\n<a href=\"/"<< cfg::reportpage<<"\">Return to main page</a>"
					"</form>";
			auto& f(GetFooter());
						SendRemoteOnly(f.data(), f.size());

	}

	finish_action:

	if(!deco.empty())
		SendRemoteOnly(deco.c_str(), deco.size());
}

bool tExclusiveUserAction::CheckStopSignal()
{
#warning restore protection
	//lockguard g(&g_StateCv);
	return g_sigTaskAbort || evabase::GetGlobal().IsShuttingDown();
}

void tExclusiveUserAction::DumpLog(time_t id)
{
	filereader reader;

	if (id<=0)
		return;

	tSS path(cfg::logdir.length()+24);
	path<<cfg::logdir<<CPATHSEP<<MAINT_PFX << id << ".log.html";
	if (!reader.OpenFile(path))
        SendRemoteOnly("Log not available");
	else
        SendRemoteOnly(reader.getView());
}

void tExclusiveUserAction::SendLocalOnly(const char *data, size_t len)
{
	if(m_reportStream.is_open())
	{
		m_reportStream.write(data, len);
		m_reportStream.flush();
		//g_bgTaskCondVar.notify_all();
	}
}

time_t tExclusiveUserAction::GetTaskId()
{
	//lguard guard(g_bgTaskMx);
	return nBgTimestamp;
}

#ifdef HAVE_ZLIB
mstring tExclusiveUserAction::BuildCompressedDelFileCatalog()
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

}
