
#include "meta.h"

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <dirent.h>

#include "debug.h"
#include "aclogger.h"
#include "acfg.h"
#include "lockable.h"
#include "filereader.h"
#include "fileio.h"

#include <vector>
#include <deque>
#include <iostream>
#include <fstream>
#include <atomic>

using namespace std;

namespace acng
{
namespace log
{

ofstream fErr, fStat, fDbg;
static acmutex mx;

#ifndef DEBUG
ACNG_API bool logIsEnabled = false;
#else
ACNG_API bool logIsEnabled = true;
#endif

#define MAX_DBG_LIMIT 500*1000000

std::atomic<off_t> totalIn(0), totalOut(0);

std::pair<off_t,off_t> GetCurrentCountersInOut()
		{
	return std::make_pair(totalIn.load(), totalOut.load());
		}

std::pair<off_t,off_t> oldCounters(0,0);

void ResetOldCounters()
{
	char lbuf[600];
	// safe some cycles an snprintf
	auto xl = (CACHE_BASE.size() + cfg::privStoreRelQstatsSfx.size());
	if (xl > 550)
		return; // heh?
	memcpy(lbuf, CACHE_BASE.data(), CACHE_BASE.size());
	memcpy(lbuf + CACHE_BASE.size(), cfg::privStoreRelQstatsSfx.data(),
			cfg::privStoreRelQstatsSfx.size());

	for (char foldNam :
	{ 'i', 'o' })
	{
		lbuf[xl] = 0;
		auto xoff = sprintf(lbuf + xl, "/%c/", foldNam);
		auto lptr = lbuf + xl + xoff;
		auto dirp = opendir(lbuf);
		if (!dirp)
			continue;
		while (true)
		{
			auto ent = readdir(dirp);
			if (!ent)
				break;
			auto llen = strlen(ent->d_name);
			if (llen > 25 || llen < 4)
				continue;
			memcpy(lptr, ent->d_name, llen + 1);
			unlink(lbuf);
		}
		closedir(dirp);
	}
	oldCounters = decltype(oldCounters)();
}

decltype(oldCounters) GetOldCountersInOut(bool calcIncomming, bool calcOutgoing)
{
#ifndef MINIBUILD
	// needs to do first reading of old stats?
	char lbuf[600];
	// safe some cycles an snprintf
	auto xl=(CACHE_BASE.size() + cfg::privStoreRelQstatsSfx.size());
	if(xl > 550)
		return oldCounters; // heh?
	memcpy(lbuf, CACHE_BASE.data(), CACHE_BASE.size());
	memcpy(lbuf+CACHE_BASE.size(), cfg::privStoreRelQstatsSfx.data(), cfg::privStoreRelQstatsSfx.size());
	if (!oldCounters.first && !oldCounters.second)
	{
		auto rfunc = [&lbuf, xl](off_t& pRet, char foldNam)
		{
			lbuf[xl]=0;
			auto xoff=sprintf(lbuf+xl, "/%c/", foldNam);
			auto lptr = lbuf+xl+xoff;
			auto dirp = opendir(lbuf);
			if(!dirp)
				return;
			char buf[30];
			while(true)
			{
				auto ent = readdir(dirp);
				if(!ent) break;
				auto llen=strlen(ent->d_name);
				if(llen > 25 || llen<4) continue;
				memcpy(lptr, ent->d_name, llen+1);
				auto tpos=readlink(lbuf, buf, _countof(buf)-1);
				if(tpos < 1) continue;
				buf[tpos]=0;
				pRet += acng::strsizeToOfft(buf);
			}
			closedir(dirp);
		};
		if(calcIncomming)
			rfunc(oldCounters.first, 'i');
		if(calcOutgoing)
			rfunc(oldCounters.second, 'o');
	}
#endif
	return oldCounters;
}

ACNG_API mstring g_szLogPrefix = "apt-cacher";

mstring open()
{
	// only called in the beginning or when reopening, already locked...
	// lockguard g(&mx);

	if(cfg::logdir.empty())
		return sEmptyString;
	
	logIsEnabled = true;

	string apath = PathCombine(cfg::logdir, g_szLogPrefix + ".log"),
			epath = PathCombine(cfg::logdir, g_szLogPrefix + ".err"),
			dpath = PathCombine(cfg::logdir, g_szLogPrefix + ".dbg");
	
	mkbasedir(apath);

	if(fErr.is_open())
		fErr.close();
	if(fStat.is_open())
		fStat.close();
	if(fDbg.is_open())
		fDbg.close();

	fErr.open(epath.c_str(), ios::out | ios::app);
	if (!fErr.is_open())
		return tErrnoFmter("Cannot open apt-cacher.err - ");
	fStat.open(apath.c_str(), ios::out | ios::app);
	if (!fStat.is_open())
		return tErrnoFmter("Cannot open apt-cacher.log - ");
	fDbg.open(dpath.c_str(), ios::out | ios::app);
	if (!fStat.is_open())
		return tErrnoFmter("Cannot open apt-cacher.dbg - ");
	return sEmptyString;
}

void transfer(uint64_t bytesIn,
		uint64_t bytesOut,
		cmstring& sClient,
		cmstring& sPath,
		bool bAsError)
{
	totalIn.fetch_add(bytesIn);
	totalOut.fetch_add(bytesOut);

	if(!logIsEnabled)
		return;

	lockguard g(&mx);

	if(!fStat.is_open())
		return;
	auto tNow=GetTime();
	if (bytesIn)
	{
		fStat << tNow << "|I|" << bytesIn;
		if (cfg::verboselog)
			fStat << '|' << sClient << '|' << sPath;
		fStat << '\n'; // not endl, it might flush
	}
	if (bytesOut)
	{
		fStat << tNow << (bAsError ? "|E|" : "|O|") << bytesOut;
		if (cfg::verboselog)
			fStat << '|' << sClient << '|' << sPath;
		fStat << '\n'; // not endl, it might flush
	}

	if(cfg::debug & LOG_FLUSH) fStat.flush();
}

void misc(const string & sLine, const char cLogType)
{
	if(!logIsEnabled)
		return;

	lockguard g(&mx);
	if(!fStat.is_open())
		return;

	fStat << time(0) << '|' << cLogType << '|' << sLine << '\n';
	
	if(cfg::debug & LOG_FLUSH)
		fStat.flush();
}

void err(const char *msg, size_t len)
{
	if (!logIsEnabled)
		return;

	lockguard g(&mx);
	if (!fErr) return;

	if (!cfg::minilog)
	{
		static char buf[32];
		const time_t tm = time(nullptr);
		ctime_r(&tm, buf);
		buf[24] = '|';
		fErr.write(buf, 25);
	}
	fErr.write(msg, len).write(szNEWLINE, 1);

	if (cfg::debug & LOG_FLUSH)
		fErr.flush();
}


void dbg(const char *msg, size_t len)
{
	if (!logIsEnabled)
		return;

	static char buf[32];

	lockguard g(&mx);
	if (fDbg.is_open() && (cfg::debug & LOG_DEBUG))
	{
		auto tm=time(nullptr);
		ctime_r(&tm, buf);
		buf[24]='|';
		fDbg.write(buf, 25).write(msg, len);
		if (cfg::debug & LOG_FLUSH)
			fDbg << endl; // this auto-flushes
		else
			fDbg << szNEWLINE;
	}

	if (cfg::debug & LOG_DEBUG_CONSOLE)
	{	
		if (cfg::debug & LOG_FLUSH)
			cerr << endl; // auto-flushes
		else
			cerr.write(msg, len) << szNEWLINE;
	}
}


void ACNG_API flush()
{
	if(!logIsEnabled)
		return;
	off_t curSize(-1);
	{
		lockguard g(mx);
		for (auto* h: {&fErr, &fStat, &fDbg})
		{
			if(h->is_open())
				h->flush();
		}
		if (fDbg.is_open())
			curSize = fDbg.tellp();
	}
	if (curSize > MAX_DBG_LIMIT)
		close(true, true);

}

void close(bool bReopen, bool truncateDebugLog)
{
	// let's try to store a snapshot of the current stats
	auto snapIn = offttos(totalIn.exchange(0));
	auto snapOut = offttos(totalOut.exchange(0));
	timeval tp;
	gettimeofday(&tp, 0);
	auto inLinkPath = CACHE_BASE + cfg::privStoreRelQstatsSfx + "/i/"
			+ acng::offttos(tp.tv_sec) + "." + acng::ltos(tp.tv_usec);
	auto outLinkPath = CACHE_BASE + cfg::privStoreRelQstatsSfx + "/o/"
			+ acng::offttos(tp.tv_sec) + "." + acng::ltos(tp.tv_usec);
	ignore_value(symlink(snapIn.c_str(), inLinkPath.c_str()));
	ignore_value(symlink(snapOut.c_str(), outLinkPath.c_str()));


	if (!logIsEnabled)
		return;

	lockguard g(mx);
	if(cfg::debug >= LOG_MORE) cerr << (bReopen ? "Reopening logs...\n" : "Closing logs...\n");
	for (auto* h: {&fErr, &fStat, &fDbg})
	{
		if (h->is_open())
			h->close();
	}

	if (truncateDebugLog)
		ignore_value(truncate((cfg::logdir + "/" + g_szLogPrefix + ".dbg").c_str(), 0));

	if(bReopen)
		log::open();
}


#define DAYSECONDS (3600*24)
#define WEEKSECONDS (DAYSECONDS * 7)

#ifndef MINIBUILD
inline deque<tRowData> GetStats()
{
	string sDataFile=cfg::cachedir+SZPATHSEP"_stats_dat";
	deque<tRowData> out;
	
	time_t now = time(nullptr);

	for (int i = 0; i < 7; i++)
	{
		tRowData d;
		d.to = now - i * DAYSECONDS;
		d.from = d.to - DAYSECONDS;
		out.emplace_back(d);
	}

	for (auto& log : ExpandFilePattern(cfg::logdir + SZPATHSEP "apt-cacher*.log", false))
	{
		if (cfg::debug >= LOG_MORE)
			cerr << "Reading log file: "sv << log << endl;
		filereader reader;
		if (!reader.OpenFile(log))
		{
			cerr << "Error opening log file "sv << log;
			continue;
		}
		string sLine;
		tStrVec tokens;

		while (reader.GetOneLine(sLine))
		{
			// cout << "got line: " << sLine <<endl;
			tokens.clear();
			if (Tokenize(sLine, "|", tokens) < 3)
				continue;

			// cout << "having: " << tokens[0] << ", " << tokens[1] << ", " << tokens[2]<<endl;

			time_t when = strtoul(tokens[0].c_str(), 0, 10);
			if (when > out.front().to || when < out.back().from)
				continue;

			for (auto it = out.rbegin(); it != out.rend(); it++)
			{
				if (when < it->from || when > it->to)
					continue;

				unsigned long dcount = strtoul(tokens[2].c_str(), 0, 10);
				switch (*tokens[1].c_str())
				{
				case ('I'):
					it->byteIn += dcount;
					it->reqIn++;
					break;
				case ('O'):
					it->byteOut += dcount;
					it->reqOut++;
					break;
				default:
					continue;
				}
			}
		}
	}
	return out;
}


string GetStatReport()
{
	string ret;
	vector<char> buf(1024);
	for (auto& entry : log::GetStats())
	{
		auto reqMax = std::max(entry.reqIn, entry.reqOut);
		auto dataMax = std::max(entry.byteIn, entry.byteOut);

		if (0 == dataMax || 0 == reqMax)
			continue;

		char tbuf[50];
		size_t zlen(0);
		ctime_r(&entry.from, tbuf);
		struct tm *tmp = localtime(&entry.from);
		if (!tmp)
			goto time_error;
		zlen = strftime(tbuf, sizeof(tbuf), TIMEFORMAT, tmp);
		if (!zlen)
			goto time_error;

		if (entry.from != entry.to)
		{
			tmp = localtime(&entry.to);
			if (!tmp)
				goto time_error;
			if (0 == strftime(tbuf + zlen, sizeof(tbuf) - zlen,
					" - " TIMEFORMAT, tmp))
				goto time_error;
		}
		snprintf(&buf[0], buf.size(), "<tr bgcolor=\"white\">"

			"<td class=\"colcont\">%s</td>"

			"<td class=\"coltitle\"><span>&nbsp;</span></td>"

			"<td class=\"colcont\">%lu (%2.2f%%)</td>"
			"<td class=\"colcont\">%lu (%2.2f%%)</td>"
			"<td class=\"colcont\">%lu</td>"

			"<td class=\"coltitle\"><span>&nbsp;</span></td>"

			"<td class=\"colcont\">%2.2f MiB (%2.2f%%)</td>"
			"<td class=\"colcont\">%2.2f MiB (%2.2f%%)</td>"
			"<td class=\"colcont\">%2.2f MiB</td>"
			"</tr>", tbuf, reqMax - entry.reqIn, double(reqMax - entry.reqIn)
				/ reqMax * 100, // hitcount
				entry.reqIn, double(entry.reqIn) / reqMax * 100, // misscount
				reqMax,

				double(dataMax - entry.byteIn) / 1048576, double(dataMax
						- entry.byteIn) / dataMax * 100, // hitdata
				double(entry.byteIn) / 1048576, double(entry.byteIn) / dataMax
						* 100, // missdata
				double(dataMax) / 1048576

		); //, int(entry.ratioSent*nRatWid), int((1.0-entry.ratioSent)*nRatWid));
		ret += &buf[0];
		continue;
		time_error: ret
				+= " Invalid time value detected, check the stats database. ";
	}
	return ret;
}
#endif
}

#ifdef DEBUG

thread_local unsigned gtls_indent_level = 0;

t_logger::t_logger(const char *szFuncName,  const void * ptr, const char *szIndentString)
: m_szName(szFuncName), m_szIndentString(szIndentString)
{
	if(!cfg::debug)
		return;

	// explicit truncation, don't care about the PID part of it
	auto id = uint32_t((uint64_t)pthread_self());
	m_threadNameBEGIN = std::string(" [T:") + std::to_string(id);
	m_objectIdEND = std::string(" P:") + std::to_string((uintptr_t)ptr) +"]";
	// writing to the level of parent since it's being "created there"
	GetFmter(" >> ") << m_szName << m_threadNameBEGIN << m_objectIdEND;
	Write();
	gtls_indent_level++;
}

t_logger::~t_logger()
{
	if(!cfg::debug)
		return;
	gtls_indent_level--;

	if(m_szName)
	{
		GetFmter(" << ") << m_szName << m_threadNameBEGIN << m_objectIdEND;
	}
	// otherwise there is unfinished string in buffer for the printing
	Write();
}

/**
 * Start formating the last string in this scope, which shall replace the exit string
 */
tSS & t_logger::GetFmter4End()
{
	//gtls_indent_level--;
	auto& ret = GetFmter();
	GetFmter(" << ") << m_szName << m_threadNameBEGIN << m_objectIdEND;
	m_szName = nullptr;
	return ret;
}
/**
 * Reset the format object into ready-to-format state, adjust indentation as needed
 */
tSS & t_logger::GetFmter(const char *szPrefix)
{
	m_strm.clear();
	for(unsigned i=0; i < gtls_indent_level; i++)
		m_strm << m_szIndentString;
	if(szPrefix)
		m_strm << szPrefix;
	return m_strm;
}

void t_logger::Write()
{
	log::dbg(m_strm);
	m_strm.clear();
}

void t_logger::WriteWithContext(const char *pSourceLocation)
{
	if(pSourceLocation)
	{
		const char *p=strrchr(pSourceLocation, CPATHSEP);
		pSourceLocation=p?(p+1):pSourceLocation;
		m_strm << m_threadNameBEGIN << " S:" << pSourceLocation << m_objectIdEND;
	}
	Write();
}

#endif

}
