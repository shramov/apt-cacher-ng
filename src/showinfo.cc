#include "debug.h"
#include "showinfo.h"
#include "meta.h"
#include "acfg.h"
#include "filereader.h"
#include "fileio.h"
#include "job.h"
#include "rex.h"
#include "meta.h"
#include "bgtask.h"

#include <iostream>
#include <fstream>

#include <cstdlib>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

using namespace std;

#define SCALEFAC 250

namespace acng
{

tRemoteStatus stOK {200, "OK"};

static cmstring sReportButton("<tr><td class=\"colcont\">"
					"<input type=\"submit\" name=\"doCount\" value=\"Count Data\">"
					"</td><td class=\"colcont\" colspan=8 valign=top><font size=-2>"
					"<i>Not calculated, click \"Count data\"</i></font></td></tr>");

// some NOOPs
tMarkupFileSend::tMarkupFileSend(mainthandler::tRunParms&& parms,
		const char *s,
		const char *m, const tRemoteStatus& st)
:
	mainthandler(move(parms)),
	m_sOuterDecoFile(s), m_sMimeType(m), m_httpStatus(st)
{
}

void acng::tMarkupFileSend::SendProcessedData(string_view sv)
{
	auto pr = sv.data();
	auto pend = pr + sv.size();

	auto lastchar=pend-1;
	while(pr<pend)
	{
		auto restlen=pend-pr;
		auto propStart=(LPCSTR) memchr(pr, (uint) '$', restlen);
		if (propStart) {
			if (propStart < lastchar && propStart[1] == '{') {
				Send(pr, propStart-pr);
				pr=propStart;
				// found begin of a new property key
				auto propEnd = (LPCSTR) memchr(propStart+2, (uint) '}', pend-propStart+2);
				if(!propEnd)// unclosed, seriously? Just dump the rest at the user
					goto no_more_props;
				if(propStart+6<propEnd && ':' == *(propStart+2))
					SendIfElse(propStart+3, propEnd);
				else
				{
					string key(propStart+2, propEnd-propStart-2);
					SendProp(key);
				}
				pr=propEnd+1;
			} else {
				// not a property string, send as-is
				propStart++;
				Send(pr, propStart-pr);
				pr=propStart;
			}
		} else // no more props
		{
			no_more_props:
			Send(pr, restlen);
			break;
		}
	}
}

void tMarkupFileSend::Run()
{
	LOGSTARTFUNCx(m_parms.cmd);
	tMarkupInput input(m_sOuterDecoFile, m_bFatalError);
	if(input.bError)
	{
		log::err(string("Error reading local page template: " ) + m_sOuterDecoFile);
		auto msg = "<html><h1>500 Template not found</h1>Please contact the system administrator.</html>"sv;
		item().ManualStart(500, "Template Not Found", "text/html", se, msg.size());
		Send(msg);
		return;
	}
	item().ManualStart(m_httpStatus.code, m_httpStatus.msg, m_sMimeType);
	SendProcessedData(input.view());
}

void tDeleter::SendProp(cmstring &key)
{
	if(key=="count")
		return SendFmt << files.size(), void();
	else if(key=="countNZs")
		return Send((files.size()>1) ? "s" : "");
	else if(key == "stuff")
		return Send(sHidParms);
	else if(key=="vmode")
		return Send(sVisualMode.data(), sVisualMode.size());

	return tMarkupFileSend::SendProp(key);
}

// and deserialize it from GET parameter into m_delCboxFilter

tDeleter::tDeleter(tRunParms&& parms, const mstring& vmode)
	: tMarkupFileSend(move(parms), "delconfirm.html", "text/html", stOK),
  sVisualMode(vmode)
{
#define BADCHARS "<>\"'|\t"
	tStrPos qpos=m_parms.cmd.find("?");

	if(m_parms.cmd.find_first_of(BADCHARS) not_eq stmiss // what the f..., XSS attempt?
			|| qpos==stmiss)
	{
		m_bFatalError=true;
		return;
	}

	auto del = (m_parms.type == EWorkType::DELETE);
	mstring src;
	mstring params(m_parms.cmd, qpos+1);

	// repack GET parameters into hidden form values again
	for(tSplitWalk split(params, "&"); split.Next();)
	{
		mstring tok(split);
		if(startsWithSz(tok, "kf="))
		{
			char *end(0);
			auto val = strtoul(tok.c_str()+3, &end, 36);
			if(*end == 0 || *end=='&')
				files.emplace(val);
		}
		else if(startsWithSz(tok, "kbid="))
		{
			mstring sSourceJobId = tok;
			if (sSourceJobId.find("..") != stmiss)
			{
				sSourceJobId.clear();
				return;
			}
			sSourceJobId = sSourceJobId.substr(5);
			sHidParms << "<input type=\"hidden\" name=\"kbid\" value=\""sv << sSourceJobId <<  "\">\n"sv;
			src = CACHE_BASE + MJSTORE_SUBPATH + "/" + sSourceJobId + ".kb";
		}
	}

	tStrDeq filePaths;
	//mstring redoLink;

	std::ifstream in;
	in.open(src);
	string cs;
	if(!in.is_open())
		return;

	mstring line;
	while (!in.eof() && !in.fail())
	{
		std::getline(in, line);
		char* ep;
		auto beg = line.c_str();
		auto id = strtoul(beg, &ep, 10);
		if (ep == nullptr || *ep != ':' || !files.contains(id))
			continue;
		++ep;
		filePaths.emplace_back(ep, beg + line.length() - ep);
	}

	// do stricter path checks and prepare the query page data
	for(const auto& path : filePaths)
	{
		if(path.find_first_of(BADCHARS)!=stmiss  // what the f..., XSS attempt?
		 || m_parms.res.GetMatchers().Match(path, rex::NASTY_PATH))
		{
			m_bFatalError=true;
			return;
		}
	}

	// XXX: this is wasting some CPU cycles but is good enough for this case
	for (const auto& path : filePaths)
	{
		mstring bname(path);
		for(const auto& sfx: sfxXzBz2GzLzma)
			if(endsWith(path, sfx))
				bname = path.substr(0, path.size()-sfx.size());
		auto tryAdd=[this,&bname,&path](cmstring& sfx)
				{
					auto cand = bname+sfx;
					if(cand == path || ::access(SZABSPATH(cand), F_OK))
						return;
					extraFiles.push_back(cand);
				};
		for(const auto& sfx: sfxMiscRelated)
			tryAdd(sfx);
		if(endsWith(path, relKey))
			tryAdd(path.substr(0, path.size()-relKey.size())+inRelKey);
		if(endsWith(path, inRelKey))
			tryAdd(path.substr(0, path.size()-inRelKey.size())+relKey);
	}


	if (m_parms.type == EWorkType::DELETE_CONFIRM
			|| m_parms.type == EWorkType::TRUNCATE_CONFIRM)
	{
		for (const auto& path : filePaths)
			sHidParms << html_sanitize(path) << "<br>\n";
		for (const auto& pathId : files)
			sHidParms << "<input type=\"hidden\" name=\"kf\" value=\"" <<
			to_base36(pathId) << "\">\n";
		if(m_parms.type == EWorkType::DELETE_CONFIRM && !extraFiles.empty())
		{
			sHidParms << sBRLF << "<b>Extra files found</b>" << sBRLF
					<< "<p>It's recommended to delete the related files (see below) as well, otherwise "
					<< "the removed files might be resurrected by recovery mechanisms later.<p>"
					<< "<input type=\"checkbox\" name=\"cleanRelated\" value=\"1\" checked=\"checked\">"
					<< "Yes, please remove all related files<p>Example list:<p>";
			for (const auto& path : extraFiles)
				sHidParms << path << sBRLF;
		}
	}
	else
	{
		for (const auto& path : filePaths)
		{
			auto doFile=[this, &del](cmstring& path)
					{
				for (auto suf : { "", ".head" })
				{
					sHidParms << (del ? "Deleting " : "Truncating ") << path << suf << "<br>\n";
					auto p = cfg::cacheDirSlash + path + suf;
					if (YesNoErr::ERROR == DeleteAndAccount(SABSPATHEX(path, suf), del))
					{
						tErrnoFmter ferrno("<span class=\"ERROR\">[ error: ");
						sHidParms << ferrno << " ]</span>" << sBRLF;
					}
					if(!del)
						break;
				}
			};
			doFile(path);
			if(StrHas(m_parms.cmd, "cleanRelated="))
				for (const auto& path : extraFiles)
					doFile(path);

		}
		//sHidParms << "<br><a href=\""<< redoLink << "\">Repeat the last action</a><br>" << sBRLF;
	}
}

void tMaintOverview::Action()
{
	if(StrHas(m_parms.cmd, "doTraceStart"))
		cfg::patrace=true;
	else if(StrHas(m_parms.cmd, "doTraceStop"))
		cfg::patrace=false;
	else if(StrHas(m_parms.cmd, "doTraceClear"))
	{
#warning restoreme or just drop that feature, add a log parser and displayer instead
		/*auto& tr(tTraceData::getInstance());
		lockguard g(tr);
		tr.clear();
		*/
	}

	m_bSigTaskAbort = (m_parms.cmd.find("&sigAbort") != stmiss);

	ProcessResource("report.html");
}

// compares c string with a determined part of another string
#define RAWEQ(longstring, len, pfx) (len==(sizeof(pfx)-1) && 0==memcmp(longstring, pfx, sizeof(pfx)-1))
#define PFXCMP(longstring, len, pfx) ((sizeof(pfx)-1) <= len && 0==memcmp(longstring, pfx, sizeof(pfx)-1))

inline int tMarkupFileSend::CheckCondition(string_view key)
{
	//std::cerr << "check if: " << string(id, len) << std::endl;
	if(key.starts_with("cfg:"sv))
	{
		string skey(key.data()+4, key.length()-4);
		auto p=cfg::GetIntPtr(skey.c_str());
		if(p)
			return ! *p;
		if(skey == "degraded"sv)
			return cfg::DegradedMode();
    	return -1;
	}
	if(key == "delConfirmed"sv)
	{
		return m_parms.type != EWorkType::DELETE
				&& m_parms.type != EWorkType::TRUNCATE;
	}
	return -2;
}

void tMarkupFileSend::SendIfElse(LPCSTR pszBeginSep, LPCSTR pszEnd)
{
	//std::cerr << "got if: " << string(pszBeginSep, pszEnd-pszBeginSep) << std::endl;
	auto sep = pszBeginSep;
	auto key = sep+1;
	auto valYes = (LPCSTR) memchr(key, (uint) *sep, pszEnd-key);
	if(!valYes) // heh?
		return;
	auto sel = CheckCondition(string_view(key, valYes-key));
	//std::cerr << "sel: " << sel << std::endl;
	if(sel<0) // heh?
		return;
	valYes++; // now really there
	auto valNo = (LPCSTR) memchr(valYes, (uint) *sep, pszEnd-valYes);
	//std::cerr << "valNO: " << valNo<< std::endl;
	if(!valNo) // heh?
			return;
	if(0 == sel)
		Send(valYes, valNo-valYes);
	else
		Send(valNo+1, pszEnd-valNo-1);
}


tMaintOverview::tMaintOverview(tRunParms &&parms) : tMaintJobBase(std::move(parms))
{
}

void tMaintOverview::SendProp(cmstring &key)
{
#ifdef DISABLED_SUGAR
	if(key=="statsRow")
	{
		if(!StrHas(m_parms.cmd, "doCount"))
			return Send(sReportButton);
		return Send(log::GetStatReport());
	}
#endif
	static cmstring defStringChecked("checked");
	if(key == "aOeDefaultChecked")
		return Send(cfg::exfailabort ? defStringChecked : se);

#ifdef DISABLED_SUGAR
	if(key == "curPatTraceCol")
	{
		tFmtSendObj endPrinter(this);

		int bcount=0;
		auto& tr(tTraceData::getInstance());
		lguard g(tr);
		for(cmstring& x: tr)
		{
			if(x.find_first_of(BADCHARS) not_eq stmiss)
			{
				bcount++;
				continue;
			}
			m_fmtHelper<<x;
			if(&x != &(*tr.rbegin()))
				m_fmtHelper << "<br>"sv;
		}
		if(bcount)
			m_fmtHelper << "<br>some strings not considered due to security restrictions<br>"sv;
	}
#endif
	return tMaintJobBase::SendProp(key);
}
/*
void tMarkupFileSend::SendRaw(const char* pBuf, size_t len)
{
	// go the easy way if nothing to replace there
	m_fmtHelper.clean() << "HTTP/1.1 " << (m_sHttpCode ? m_sHttpCode : "200 OK")
			<< "\r\nConnection: close\r\nContent-Type: "
			<< (m_sMimeType ? m_sMimeType : "text/html")
			<< "\r\nContent-Length: " << len
			<< "\r\n\r\n";
	SendRawData(m_fmtHelper.rptr(), m_fmtHelper.size(), MSG_MORE | MSG_NOSIGNAL);
	SendRawData(pBuf, len, MSG_NOSIGNAL);
}
*/

void tMarkupFileSend::SendProp(cmstring &key)
{
	if (startsWithSz(key, "cfg:"))
	{
		auto ckey=key.c_str() + 4;
		auto ps(cfg::GetStringPtr(ckey));
		if(ps)
			return Send(*ps);
		auto pi(cfg::GetIntPtr(ckey));
		if(pi)
			SendFmt << *pi;
		return;
	}

	if (key == "serverhostport")
		return Send(GetMyHostPort());

	if (key == "footer")
		return Send(GetFooter());

	if (key == "hostname")
	{
		char buf[500];
		if(gethostname(buf, sizeof(buf)-2))
			return; // failed?
		buf[sizeof(buf)-1] = 0x0;
		return Send(buf);
	}
	if(key=="random")
		SendFmt << rand();
	else if(key=="dataInHuman")
	{
		auto stats = log::GetCurrentCountersInOut();
		return Send(offttosH(stats.first));
	}
	else if(key=="dataOutHuman")
	{
		auto stats = log::GetCurrentCountersInOut();
		return Send(offttosH(stats.second));
	}
	else if(key=="dataIn")
	{
		auto stats = log::GetCurrentCountersInOut();
		auto statsMax = std::max(stats.first, stats.second);
		auto pixels = statsMax ? (stats.first * SCALEFAC / statsMax) : 0;
		SendFmt << pixels;
	}
	else if(key=="dataOut")
	{
		auto stats = log::GetCurrentCountersInOut();
		auto statsMax = std::max(stats.second, stats.first);
		auto pixels = statsMax ? (SCALEFAC * stats.second / statsMax) : 0;
		SendFmt << pixels;
	}
	else if (key == "dataHistInHuman")
	{
		auto stats = pairSum(log::GetCurrentCountersInOut(), log::GetOldCountersInOut());
		return Send(offttosH(stats.first));
	}
	else if (key == "dataHistOutHuman")
	{
		auto stats = pairSum(log::GetCurrentCountersInOut(), log::GetOldCountersInOut());
		return Send(offttosH(stats.second));
	}
	else if (key == "dataHistIn")
	{
		auto stats = pairSum(log::GetCurrentCountersInOut(), log::GetOldCountersInOut());
		auto statsMax = std::max(stats.second, stats.first);
		auto pixels = statsMax ? (stats.first * SCALEFAC / statsMax) : 0;
		SendFmt << pixels;
	}
	else if (key == "dataHistOut")
	{
		auto stats = pairSum(log::GetCurrentCountersInOut(), log::GetOldCountersInOut());
		auto statsMax = std::max(stats.second, stats.first);
		auto pixels = statsMax ? (SCALEFAC * stats.second/statsMax) : 0;
		SendFmt << pixels;
	}
	else if (key == "taskTitle")
	{
		SendFmt << GetTaskInfo(m_parms.type).title;
	}
	else if (key == "taskName")
	{
		SendFmt << GetTaskInfo(m_parms.type).typeName;
	}
	else if (key == "cachekey")
	{
		Send(GetCacheKey().view());
	}
}

tMarkupFileSend::tMarkupInput::tMarkupInput(cmstring &fname, bool alreadyError)
{
	bError = alreadyError || ! ( fr.OpenFile(cfg::confdir + SZPATHSEP+fname, true) ||
								 (!cfg::suppdir.empty() && fr.OpenFile(cfg::suppdir + SZPATHSEP + fname, true)));
}

tMaintJobBase::tMaintJobBase(tRunParms &&parms) : tMarkupFileSend(std::move(parms), "maint.html", "text/html", stOK)
{
}

void tMaintJobBase::SendProp(cmstring &key)
{
	if (key == "action")
		return Action();
	if (key == "startTime")
	{
#warning apply local time somehow?
		SendFmt << tHttpDate(m_startTime.tv_sec).view();
		return;
	}

	return tMarkupFileSend::SendProp(key);
}

int tMaintJobBase::CheckCondition(string_view key)
{
	if (key == "showCancel"sv)
		return ! (GetTaskInfo(m_parms.type).flags & EXCLUSIVE);
	return tMarkupFileSend::CheckCondition(key);
}

void tMaintJobBase::ProcessResource(cmstring sFilename)
{
	tMarkupInput input(sFilename);
	if (input.bError)
		SendFmt << "<b>INTERNAL ERROR</b>";
	else
		SendProcessedData(input.view());
}

bool tMaintJobBase::CheckStopSignal()
{
	return m_bSigTaskAbort || evabase::GetGlobal().IsShuttingDown();
}

}
