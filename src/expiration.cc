#include "debug.h"
#include "expiration.h"
#include "meta.h"
#include "filereader.h"
#include "fileio.h"
#include "acregistry.h"
#include "acfg.h"
#include "acutilpath.h"

#include <fstream>
#include <map>
#include <string>
#include <iostream>
#include <algorithm>

#include <unistd.h>
#include <dirent.h>

using namespace std;

#define ENABLED

#ifndef ENABLED
#warning Unlinking parts defused
#endif

#define THOLDTIME (m_gMaintTimeNow-acng::cfg::extreshhold*86400)
#define TIMEEXPIRED(t) (t < THOLDTIME)

#define TIME_AGONY (m_gMaintTimeNow-acng::cfg::extreshhold*86400)

#define FNAME_PENDING "_expending_dat"
#define FNAME_DAMAGED "_expending_damaged"

#define INTERNAL_DATA_EXP_DAYS 10

namespace acng
{

// static data, preserved for HandlePkgEntry
string exPathBuf;
inline string& assemblePath(string_view folder, string_view fname)
{
	exPathBuf.reserve(folder.size() + fname.size());
	exPathBuf = folder;
	exPathBuf += fname;
	return exPathBuf;
}

#if 0

struct tExpReader
{
	struct tExpEntry
	{
		string_view sDirPathRel;
		string_view sFileName;
		string_view sDate;
		time_t exDate; // set if validated
	};
	string_view line;
	tExpEntry last;
	filereader f;
	bool Open(string_view srcRel) { return f.OpenFile(SABSPATH(srcRel), true); }
	tExpEntry* Next()
	{
		if (!f.GetOneLine(line))
			return nullptr;
		tSplitWalkStrict wk(line);
		return (wk.Next()
				&& 0 <= (last.exDate = atoofft(wk.view(), -1))
				&& wk.Next()
				&& (last.sDirPathRel = wk.view()).length() > 0
				&& wk.Next()
				&& (last.sFileName = wk.view()).length() > 0)
				? &last : nullptr;

		uint16_t slen;
		if (raw.size() < sizeof(last.exDate) + sizeof(slen))
			return nullptr;
		memcpy(&last.exDate, raw.data(), sizeof(last.exDate));
		memcpy(&slen, raw.data() + sizeof(last.exDate), sizeof(slen));
		raw.remove_prefix(sizeof (last.exDate) + sizeof (slen));
		if (AC_UNLIKELY(!slen))
			return nullptr;
		last.sDirPathRel = string_view(raw.data(), slen);
		raw.remove_prefix(slen);
		if (AC_UNLIKELY(raw.length() < sizeof(slen) + 1))
			return nullptr;
		memcpy(&slen, raw.data(), sizeof(slen));
		if (AC_UNLIKELY(!slen))
			return nullptr;
		last.sFileName = string_view(raw.data() + sizeof(slen), slen);
		raw.remove_prefix(sizeof(slen) + slen);
		return &last;

	}
};
#endif

void expiration::DoStateChecks(tDiskFileInfo& infoLocal, const tRemoteFileInfo& infoRemote, bool bPathMatched)
{
	auto sHeadAbs = SABSPATHEX(infoRemote.path, ".head"sv);
	string_view sPathAbs(sHeadAbs.data(), sHeadAbs.length() - 5);
	string_view sPathRel(sPathAbs.substr(CACHE_BASE_LEN));

	tReporter rep(this, sPathRel, eDlMsgSeverity::VERY_VERBOSE);

	auto lenFromStat = infoLocal.fpr.GetSize();
	off_t lenFromHeader=-1;

	// end line ending starting from a class and add checkbox as needed
	auto finish_damaged = [&](bool isError, string_view reason)
	{
		if (m_damageList.is_open())
			m_damageList << sPathRel << "\n";

		infoLocal.action = infoLocal.FORCE_REMOVE;
#warning FIXME, actually a potential error, eDlMsgSeverity::POTENTIAL_ERROR
		if (isError)
			rep.NonFatalError(reason);
		else
			rep << ENDL << RESTWARN << reason;
	};

	auto handle_incomplete = [&]()
	{
		// keep in UNDECIDED state, unless some resolution is required

		if (m_bIncompleteIsDamaged)
		{
			if (m_bTruncateDamaged)
			{
				if (lenFromStat >0)
				{
					evabase::GetGlobal().SyncRunOnMainThread([&]()
					{
						rep.Warning() << "(incomplete download, truncating as requested)"sv;
						auto hodler = m_parms.res.GetItemRegistry()->Create(to_string(sPathRel),
																			ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY,
																			fileitem::tSpecialPurposeAttr());
						if (hodler.get())
							hodler.get()->MarkFaulty(false);
						return 0;
					});
				}
			}
			else
				finish_damaged(false, "incomplete download, invalidating (as requested)"sv);
		}
		else
		{
			rep << "(incomplete download, keep it for now)"sv;
			infoLocal.action = infoLocal.FORCE_KEEP;
		}
	};

	// prints the file, and only in verbose mode
	auto finish_report_keep = [&]()
	{
		// ok, package matched, contents ok if checked, drop it from the removal list
		rep << "OKAY"sv;
		infoLocal.action = infoLocal.FORCE_KEEP;
	};

	auto report_oversize = [&]()
	{
		// user shall find a resolution here; for volatile files, not certain, print an action checkbox though
		rep.NonFatalError(
					m_parms.res.GetMatchers().GetFiletype(term(sPathRel)) == rex::FILE_VOLATILE
				   ? "bad file state while containing volatile index data"sv
				   : "size mismatch while strict check requested"sv);
		infoLocal.action = infoLocal.FORCE_KEEP;
	};
	// Basic header checks. Skip if the file was forcibly updated/reconstructed before.
	if (m_bSkipHeaderChecks || infoLocal.bNoHeaderCheck)
	{
		//				LOG("Skipped header check for " << sPathRel);
	}
	else if (infoRemote.fpr.GetSize() >= 0)
	{
		//				LOG("Doing basic header checks");
		header h;

		if (0 < h.LoadFromFile(sHeadAbs))
		{
			lenFromHeader = atoofft(h.h[header::CONTENT_LENGTH], -2);
			if(lenFromHeader < 0)
			{
				// better drop it, properly downloaded ones DO have the length
				return finish_damaged(true, "header file does not contain content length"sv);
			}
			if (lenFromHeader < lenFromStat)
			{
				if (YesNoErr::ERROR == DeleteAndAccount(sHeadAbs))
					rep << ENDL << RESTWARN << "Error removing file"sv;
				return finish_damaged(true, "metadata reports incorrect file size (smaller than existing file)"sv);
			}
		}
		else
		{
			auto& at = GetFlags(sPathRel);
			if (!at.forgiveDlErrors) // just warn
			{
				rep << ENDL << RESTWARN << "header file missing or damaged"sv;
			}
		}
	}

	// maybe can check a bit more against directory info for most cases
	// also shortcut without scanning
	if (bPathMatched && infoRemote.fpr.GetSize() >= 0)
	{
		if (lenFromStat > infoRemote.fpr.GetSize())
			return report_oversize();
		if (lenFromStat < infoRemote.fpr.GetSize())
			return handle_incomplete();
	}

	// are we done or do we need extra checks? Can only go there when sure about the identity
	if(!m_bByChecksum || !bPathMatched)
		return finish_report_keep();

	//// OKAY, needing content inspection

	// XXX: BS? Condition should have been checked before
#if 0
	//knowing the expected and real size, try a shortcut without scanning
	if (infoRemote.fpr.size >= 0 && infoLocal.action != infoLocal.FAKE)
	{
		if (AC_UNLIKELY(lenFromStat < 0)) // fake entry?
			lenFromStat = GetFileSize(m_spr = sPathAbs, -123);

		if (lenFromStat >=0 && lenFromStat < infoRemote.fpr.size)
			return handle_incomplete();
	}
#endif

	if (infoRemote.path.ends_with("/InRelease"sv) || infoRemote.path.ends_with("/Release"sv) )
	{
		if(infoLocal.fpr.GetCsType() != infoRemote.fpr.GetCsType() &&
				!infoLocal.fpr.ScanFile(term(sPathAbs), infoRemote.fpr.GetCsType()))
		{
			// IO error? better keep it for now, not sure how to deal with it
			log::err(Concat("Error reading "sv, sPathAbs));
			return finish_damaged(true, "IO error occurred while"
										" checksumming, leaving as-is for now.");
		}

		if (!infoLocal.fpr.csEquals(infoRemote.fpr))
			return finish_damaged(true, "checksum mismatch"sv);
	}

	// CS should be validated now if the sizes and paths were matched

	// good state, or cannot check so consider be good
	if (infoRemote.fpr.GetSize() < 0 || lenFromStat == infoRemote.fpr.GetSize())
		return finish_report_keep();

#warning XXX: what is the flow for checksum check on compressed content? Test, adjust or document properly!
	return infoLocal.fpr.GetSize() > infoRemote.fpr.GetSize() ? report_oversize() : handle_incomplete();
};

// represents the session's current incomming data count at the moment when expr. was run last time
// it's needed to correct the considerations based on the active session's download stats
off_t lastCurrentDlCount(0);

void expiration::HandlePkgEntry(const tRemoteFileInfo &entry)
{
#ifdef DEBUGSPAM
	LOG("expiration::HandlePkgEntry:" << "\npath:" << entry.path << "\nsize: " << entry.fpr.GetSize() << "\ncsum: " << entry.fpr.GetCsAsString());
#endif

	auto what = SplitDirPath(entry.path);

	auto range = m_delCand.equal_range(what.second);
	if (range.first == m_delCand.end())
		return; // none found

	for(auto it = range.first; it != range.second; ++it)
	{
		auto& el = it->second;
		// needs content examination etc.?
		auto bConsiderFolder = el.bNeedsStrictPathCheck | m_bByPath | m_bByChecksum;
		if (bConsiderFolder && el.folder != SimplifyPath(what.first))
			continue; // not for us

		if (el.action == el.UNDECIDED)
			DoStateChecks(el, entry, bConsiderFolder);
	}
}

void expiration::ReportExtraEntry(cmstring& sPathAbs, const tFingerprint& fpr)
{
	tRemoteFileInfo xtra;
	xtra.fpr = fpr;
	xtra.path = sPathAbs.substr(CACHE_BASE_LEN);
	HandlePkgEntry(xtra);
}

expiration::expiration(tRunParms &&parms) :
	cacheman(move(parms)),
	m_expDate(GetTime() - INTERNAL_DATA_EXP_DAYS * 86400)
{
}

inline void expiration::RemoveAndStoreStatus(bool bPurgeNow, tReporter& rep)
{
	LOGSTARTFUNC;

	auto sDbFileAbs = SABSPATH(FNAME_PENDING);
	unordered_map<tDiskFileInfo*, time_t> lostInfoCache;

	{
		filereader reader;
		if (reader.OpenFile(sDbFileAbs))
		{
			string_view sLine;
			while(reader.GetOneLine(sLine))
			{
				tSplitWalkStrict split(sLine, "\t");
				if (!split.Next()) continue;
				auto tv = split.view();
				if (!split.Next()) continue;
				time_t timestamp = atoofft(tv, -1);
				if (timestamp < 0) continue;
				auto dir = split.view();
				if (!split.Next()) continue;
				auto nam = split.view();
				auto* el = PickCand(dir, nam);
				if (!el) continue;
				lostInfoCache[el] = timestamp;
			}
		}
	}

	ofstream f;
	// unless purging ASAP, store the essential data in the cache file
    if(!bPurgeNow)
    {
		f.open(sDbFileAbs.c_str());
		if (!f.is_open())
        {
			Send(WITHLEN("Unable to open " FNAME_PENDING
            		" for writing, attempting to recreate... "));
            ::unlink(sDbFileAbs.c_str());
			f.open(sDbFileAbs.c_str());
			if (f.is_open())
				Send(WITHLEN("OK\n<br>\n"));
            else
            {
				Send(WITHLEN(
                		"<span class=\"ERROR\">"
                		"FAILED. ABORTING. Check filesystem and file permissions."
                		"</span>"));
                return;
            }
        }
    }

	int nCount(0);
	off_t tagSpace(0);
	auto exThold = THOLDTIME;
	string sDirnameTerminated; // reused string

	for (auto& el : m_delCand)
	{
		auto& desc = el.second;

		if (desc.FORCE_KEEP == desc.action)
			continue;

		string sPathRel = assemblePath(desc.folder, el.first);
		LPCSTR sFnameTerminated = sPathRel.data() + desc.folder.length();

		DBGQLOG("Checking " << sPathRel);
		if (m_parms.res.GetMatchers().Match(sFnameTerminated, rex::FILE_WHITELIST)
				 || m_parms.res.GetMatchers().Match(sPathRel, rex::FILE_WHITELIST))
		{
			// exception is stuff that should have some cover but doesn't
			if(!ContHas(m_managedDirs, sFnameTerminated))
			{
				LOG("Protected file, not to be removed");
				continue;
			}
		}

		auto sPathAbs = SABSPATH(sPathRel);
		Cstat st(sPathAbs);
		auto prev = lostInfoCache.find(&desc);
		auto disapTime = min(m_gMaintTimeNow, prev != lostInfoCache.end() ? prev->second : END_OF_TIME);

		if (bPurgeNow || disapTime < exThold || desc.action == desc.FORCE_REMOVE)
		{

#ifdef ENABLED
			SendFmt << "Removing " << sPathRel;
			if(YesNoErr::ERROR == DeleteAndAccount(sPathAbs, true, &st))
				Send(tErrnoFmter("<span class=\"ERROR\"> [ERROR] ")+"</span>");
			SendFmt << sBRLF << "Removing " << sPathRel << ".head";
			if(YesNoErr::ERROR == DeleteAndAccount(sPathAbs + ".head", true, &st))
				Send(tErrnoFmter("<span class=\"ERROR\"> [ERROR] ")+"</span>");
			Send(sBRLF);
			::rmdir(SZABSPATH(el.second.folder));
#endif
		}
		else if (f.is_open())
		{
			rep.Misc( MsgFmt << "Tagging " << sPathRel
			#ifdef DEBUG
							<< " (t-" << (m_gMaintTimeNow - disapTime) / 3600 << "h)"
			#endif
						);

				nCount++;
				tagSpace += desc.fpr.GetSize();
				f << disapTime << "\t"sv << el.second.folder << "\t"sv << el.first << svLF;
		}
		else
			rep << ENDL << ( MsgFmt << "Keeping " << sPathRel);

	}
    if(nCount)
    	TellCount(nCount, tagSpace);
}

void expiration::Action()
{
	switch(m_parms.type)
	{
	case EWorkType::EXP_PURGE:
		return ListExpiredFiles(true);
		return;
	case EWorkType::EXP_LIST:
		return ListExpiredFiles(false);
	case EWorkType::EXP_PURGE_DAMAGED:
	case EWorkType::EXP_LIST_DAMAGED:
	case EWorkType::EXP_TRUNC_DAMAGED:
		LoadHints();
		return HandleDamagedFiles();
	default:
		LoadHints();
		break;
	}

	bool tradeOffCheck = cfg::exstarttradeoff
						 && !StrHas(m_parms.cmd, "force=1")
						 && !StrHas(m_parms.cmd, "ignoreTradeOff=iTO")
						 && !m_bByChecksum
						 && !Cstat(SZABSPATH("_actmp/.ignoreTradeOff"));

	off_t newLastIncommingOffset = 0;
	bool okay = true;

	if(tradeOffCheck)
	{
		newLastIncommingOffset = log::GetCurrentCountersInOut().first;

		auto haveIncomming = newLastIncommingOffset - lastCurrentDlCount
				+ log::GetOldCountersInOut(true).first;
		if(haveIncomming < cfg::exstarttradeoff)
		{
			SendFmt << "Expiration suppressed due to costs-vs.-benefit considerations "
					"(see exStartTradeOff setting, currently: " << offttosH(haveIncomming) <<
					" vs. " << offttosH(cfg::exstarttradeoff)
					<< ") (<a href=\"javascript:doForce()\">FORCE IT</a>)"
					<< sBRLF;
			return;
		}
	}

	m_bIncompleteIsDamaged = StrHas(m_parms.cmd, "incomAsDamaged");
	m_bScanVolatileContents = StrHas(m_parms.cmd, "scanVolatile");

	{
		tReporter repSec(this, "Locating potentially expired files in the cache..."sv,
						 eDlMsgSeverity::INFO, tReporter::SECTION);

		ScanCache();
		if(CheckStopSignal())
			return;

		repSec << (MsgFmt << "Found " << m_nProgIdx << " files.");

#if 0 //def DEBUG
		for(auto& i: m_trashFile2dir2Info)
		{
			SendFmt << "<br>File: " << i.first <<sBRLF;
			for(auto& j: i.second)
				SendFmt << "Dir: " << j.first << " [ "<<j.second.fpr.size << " / " << j.second.nLostAt << " ]<br>\n";
		}
#endif

		/*	if(m_bByChecksum)
		m_fprCache.rehash(1.25*m_trashFile2dir2Info.size());
*/
		//cout << "found package files: " << m_trashCandidates.size()<<endl;
		//for(tS2DAT::iterator it=m_trashCandSet.begin(); it!=m_trashCandSet.end(); it++)
		//	SendChunk(tSS()<<it->second.sDirname << "~~~" << it->first << " : " << it->second.fpr.size<<"<br>");

		okay = UpdateVolatileFiles();

		if(!okay || CheckStopSignal())
			return;
	}

	{
		tReporter repSec(this, "Validating cache contents..."sv,
						 eDlMsgSeverity::INFO, tReporter::SECTION);

		m_damageList.open(SZABSPATH(FNAME_DAMAGED), ios::out | ios::trunc);

		okay = ProcessSeenIndexFiles([this](const tRemoteFileInfo &e)
		{
			HandlePkgEntry(e);
		});

		if(!okay || CheckStopSignal())
			return;
	}

	{
		tReporter repSec(this, "Reviewing candidates for removal...",
						 eDlMsgSeverity::INFO, tReporter::SECTION);

		RemoveAndStoreStatus(StrHas(m_parms.cmd, "purgeNow"), repSec);
		PurgeMaintLogsAndObsoleteFiles();

		DelTree(CACHE_BASE+"_actmp");

		TrimFiles();
	}

#warning BS. Now retrieved through a markup variable.
	//PrintStats("Allocated disk space");
	//ReportSectionLabel("Done!");

	if(tradeOffCheck)
	{
		lastCurrentDlCount = newLastIncommingOffset;
		log::ResetOldCounters();
	}
}

void expiration::ListExpiredFiles(bool bPurgeNow)
{
	off_t nSpace(0);
	unsigned cnt(0), errors(0);
	auto datPathAbs = SABSPATH(FNAME_PENDING);

	auto purge = [&](cmstring& path)
	{
		if (!bPurgeNow)
			return;
		if(0 != ::unlink(path.c_str()) && errno != ENOTDIR && errno != ENOENT)
		{
			errors++;
			Send (tErrnoFmter("NOTE: error removing this - "));
		}
	};

	filereader reader;
	string_view sLine;
	if (reader.OpenFile(datPathAbs))
	{
		while(reader.GetOneLine(sLine))
		{
			tSplitWalkStrict split(sLine, "\t");
			if (!(split.Next() && split.Next())) continue;
			auto dir = split.view();
			if (!split.Next()) continue;
			auto rel = Concat(dir, split.view());

			auto abspath = SABSPATH(rel);
			off_t sz = GetFileSize(abspath, -2);
			if (sz < 0)
				continue;

			cnt++;
			Send(rel + sBRLF);
			nSpace += sz;
			purge(abspath);

			abspath += ".head"sv;
			sz = GetFileSize(abspath, -2);
			if (sz >= 0)
			{
				nSpace += sz;
				Send(rel + ".head\n");
				purge(abspath);
			}
		}

		reader.Close();
	}
	TellCount(cnt, nSpace);

	if (bPurgeNow)
	{
		if (errors == 0)
			::unlink(datPathAbs.c_str());
	}
	else
	{
		if (cnt)
		{
			auto delURL = mstring("/") + cfg::reportpage + "?justRemove=1";
			SendFmt << "<a href=\"" << delURL << "\">Delete all listed files</a> "
												 "(no further confirmation)<br>\n";
		}
	}
}

void expiration::TrimFiles()
{
	if(m_oversizedFiles.empty())
		return;
	auto now = GetTime();
	tReporter rep(this, "Trimming cache files", eDlMsgSeverity::VERBOSE, tReporter::SECTION);
	rep << (MsgFmt <<  m_oversizedFiles.size() << " sparse file(s)");
	for(const auto& fil: m_oversizedFiles)
	{
		// still there and not changed?
		Cstat stinfo(fil);
		if (!stinfo)
			continue;
		if (now - 86400 < stinfo.msec())
			continue;

		tReporter rep(this, fil);

		evabase::GetGlobal().SyncRunOnMainThread([&]()
		{
			auto hodler = m_parms.res.GetItemRegistry()->Create(fil,
																ESharingHow::ALWAYS_TRY_SHARING,
																fileitem::tSpecialPurposeAttr());
			if ( ! hodler.get())
				return false;
			auto pFi = hodler.get();
			if (pFi->GetStatus() >= fileitem::FIST_DLGOTHEAD)
				return false;
			if (0 != truncate(fil.c_str(), stinfo.size())) // CHECKED!
			{
				rep.Warning() << (MsgFmt << "Trim error at " << fil << " (" << tErrnoFmter() << ")");
			}
			return true;
		});
	}
}

void expiration::ScanCache()
{
	tReporter rep(this, "Scanning cache..."sv,
					 eDlMsgSeverity::INFO, tReporter::SECTION);

	struct tCollector : public IFileHandler
	{
		unsigned m_fileCur;
		expiration& q;
		cacheman::tProgressTeller pt;
		tReporter& rep;
		tCollector(expiration& _q, tReporter& r) : q(_q), pt(r), rep(r) {}
		bool ProcessDirBefore(cmstring&, const struct stat &) { m_fileCur = 0; return true; }
		bool ProcessOthers(cmstring&, const struct stat &) { m_fileCur++; return true; }
		bool ProcessDirAfter(const std::string &sPath, const struct stat &st)
		{
			q.ProcessDir(sPath, !m_fileCur, st, rep);
			return true;
		}
		bool ProcessRegular(const string & sPathAbs, const struct stat &stinfo)
		{
			m_fileCur++;
			pt.Tell();
			return q.ProcessCacheItem(sPathAbs, stinfo, rep);
		}
	} collector(*this, rep);
	IFileHandler::DirectoryWalk(cfg::cachedir, &collector);
}

void expiration::ProcessDir(const std::string &sPath, bool isEmpty, const struct stat &st, tReporter& rep)
{
	if (isEmpty && st.st_mtim.tv_sec < m_expDate && !IsInternalItem(sPath, true))
	{
		rep << "Deleting old empty folder " << html_sanitize(sPath);
		rmdir(sPath.c_str());
	}
	m_lastDirCache = se;
}

void expiration::HandleDamagedFiles()
{
	filereader f;
	if(!f.OpenFile(SABSPATH(FNAME_DAMAGED)))
	{
		this->Send(WITHLEN("List of damaged files not found"));
		return;
	}
	string_view s;
	while(f.GetOneLine(s))
	{
		if(s.empty())
			continue;

#warning reimplement
		/*
		if(this->m_parms.type == EWorkType::EXP_PURGE_DAMAGED)
		{
			SendFmt << "Removing " << s << sBRLF;
			auto holder = GetDlRes().GetItemRegistry()->Create(s, ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY, fileitem::tSpecialPurposeAttr());
			if (holder.get())
			{
				holder.get()->MarkFaulty(true);
			}
			else
			{
				// still little risk but not of crashing
				unlink(SZABSPATH(s));
				unlink(SZABSPATH(s+".head"));
			}
		}
		else if(this->m_parms.type == EWorkType::EXP_TRUNC_DAMAGED)
		{
			SendFmt << "Truncating " << s << sBRLF;
			auto holder = GetDlRes().GetItemRegistry()->Create(s, ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY, fileitem::tSpecialPurposeAttr());
			if (holder.get())
				holder.get()->MarkFaulty();
		}
		else
			SendFmt << s << sBRLF;

		*/

	}
	return;
}

void expiration::PurgeMaintLogsAndObsoleteFiles()
{
	bool suppr = false;
	for (auto pat: {"/*.html"sv, "/*.kb"sv})
	{
		tStrDeq logs = ExpandFilePattern(Concat(CACHE_BASE, MJSTORE_SUBPATH, pat));
		if (logs.size() > 2 && !suppr)
			Send("Found required cleanup tasks: purging maintenance logs...<br>\n"sv);
		suppr = true;
		auto threshhold = GetTime() - cfg::extreshhold * 24*60*60;
		for (const auto& s: logs)
		{
#ifdef ENABLED
			Cstat sb(s);
			if (sb && sb.msec() < threshhold)
			{
				m_nSpaceReleased += s.size();
				::unlink(s.c_str());
			}
#endif
		}
	}
	if(!m_obsoleteStuff.empty())
	{
		Send("Removing obsolete items...<br>\n"sv);
		for(const auto &s: m_obsoleteStuff)
		{
			Cstat sb(s);
			if (!sb)
				continue;
			m_nSpaceReleased += s.size();
			SendFmt << s << sBRLF;
			::unlink(SZABSPATH(s));
		}
	}
}


int expiration::CheckCondition(string_view key)
{
	if (key == "purgeActionVisible"sv)
		return 0 + m_adminActionList.empty();
	return tExclusiveUserAction::CheckCondition(key);
}

void expiration::SendProp(cmstring &key)
{
	if (key == "itemactionselection")
	{
		for (const auto& el: m_adminActionList)
		Send(MsgFmt << "PRINTME: " << el.first << " ---- " << el.second);
	}
	else
		cacheman::SendProp(key);
}


bool expiration::ProcessCacheItem(cmstring& sPathAbs, const struct stat &stinfo, tReporter& rep)
{
	if (AC_UNLIKELY(CheckStopSignal()))
		return false;

	if (AC_UNLIKELY(sPathAbs.size() <= CACHE_BASE_LEN)) // heh?
		return false;

	auto diffMoreThan = [&stinfo](blkcnt_t diff)
	{
		return stinfo.st_blocks > diff && stinfo.st_size/stinfo.st_blksize < (stinfo.st_blocks - diff);
	};

	// detect invisible holes at the end of files (side effect of properly incorrect hidden allocation)
	// allow some tolerance of about 10kb, should cover all page alignment effects
	if (diffMoreThan(20))
	{
		auto now=GetTime();
		if (now - 86400 > stinfo.st_mtim.tv_sec)
		{
			m_oversizedFiles.emplace_back(sPathAbs);
			// don't spam unless the user wants it and the size is really large
			if(diffMoreThan(40))
			{
				rep.Misc(MsgFmt << "Trailing allocated space on " << sPathAbs << " (" << stinfo.st_blocks <<
						" blocks, expected: ~" << (stinfo.st_size/stinfo.st_blksize + 1) <<"), will be trimmed later");
			}
		}
	}

	string_view sPathRel(sPathAbs.data() + CACHE_BASE_LEN, sPathAbs.length() - CACHE_BASE_LEN);
	DBGQLOG(sPathRel);

	// detect strings which are only useful for shell or js attacks
	if (sPathRel.find_first_of("\r\n'\"<>{}")!=stmiss)
		return true;

	if (sPathRel[0] == '_' && !m_bScanInternals)
		return true; // not for us

	auto justHeader = endsWithSzAr(sPathRel, ".head");

	// special handling for the installer files, we need an index for them which might be not there
	tStrPos pos2, pos = sPathRel.rfind("/installer-");
	if (pos!=stmiss && stmiss !=(pos2=sPathRel.find("/images/", pos)))
	{
		/* pretend that it's there but not usable so the refreshing code will try to get at
		 * least one copy for that location if it's needed there
		 */
		auto idir = sPathRel.substr(0, pos2 + 8);
		auto& flags = SetFlags(idir + "SHA256SUMS");

		// and care only about the modern version of that index
		auto& oldflags = SetFlags(idir + "MD5SUMS");
		oldflags.vfile_ondisk = false;

		if (!flags.vfile_ondisk)
		{
			flags.vfile_ondisk = true;
			flags.uptodate = false;

			// the original source context will probably provide a viable source for
			// this URL - it might go 404 if the whole folder is missing but then the
			// referenced content would also be outdated/gone and not worth keeping
			// in the cache anyway

			flags.forgiveDlErrors = true;
		}
	}

	auto isVfile = AddIFileCandidate(sPathRel);
	if (isVfile)
	{
		auto &attr = SetFlags(sPathRel);
		attr.usedDiskSpace += stinfo.st_size;
		//attr.forgiveDlErrors = endsWith(sPathRel, sslIndex);
	}

	// ok, split to dir/file and add to the list, dup data here as needed (and not later)
	string_view folder, file;
	if (m_lastDirCache.empty())
	{
		auto folderNfile = SplitDirPath(sPathRel);
		folder = m_lastDirCache = m_stringStore.Add(folderNfile.first);
		file = m_stringStore.Add(folderNfile.second);
	}
	else
	{
		folder = m_lastDirCache;
		file = m_stringStore.Add(GetBaseName(sPathRel));
	}
	auto res = PickOrAdd(folder, file, false);
	if (res.second)
	{
		res.first.bHasBody |= !justHeader;
		// require exact checks for metadata
		res.first.bNeedsStrictPathCheck |= isVfile;
		// remember the size for content data, ignore for the header file
		if(!justHeader)
			res.first.fpr.SetSize(stinfo.st_size);
	}
	return true;
}

void expiration::LoadHints()
{
	filereader reader;
	if (!reader.OpenFile(cfg::confdir + SZPATHSEP + "ignore_list"))
	{
		if (cfg::suppdir.empty() || !reader.OpenFile(cfg::suppdir + SZPATHSEP + "ignore_list"))
				return;
	}
	string_view sTmp;
	while (reader.GetOneLine(sTmp))
	{
		trimFront(sTmp);
		if (sTmp.starts_with('#'))
			continue;
		trimBack(sTmp);
		if(sTmp.starts_with(CACHE_BASE))
			sTmp.remove_prefix(CACHE_BASE_LEN);
		if(sTmp.empty())
			continue;
		SetFlags(mstring(sTmp)).forgiveDlErrors = true;
		PickOrAdd(sTmp).first.action = tDiskFileInfo::FAKE;
	}
	reader.Close();
}

#warning ancient code. Redo, much simplier file dumping format needed, and apply that AFTER processing
#if 0
void expiration::LoadPreviousData(bool bForceInsert)
{
	filereader reader;
	reader.OpenFile(SABSPATH(FNAME_PENDING));
	string_view sLine;
	while(reader.GetOneLine(sLine))
	{
		tSplitWalkStrict split(sLine);
		if (!split.Next()) continue;
		time_t timestamp = atoofft(split.view(), -1);
		if (!split.Next()) continue;
		auto dir = split.view();
		if (!split.Next()) continue;
		auto& desc = m_trashFile2dir2Info[mstring(split.view())][mstring(dir)];
		// maybe add with timestamp from the last century (implies removal later)
		if(bForceInsert)
			desc.nLostAt=1;
		// considered file was already considered garbage, use the old date
		else if(desc.nLostAt>0)
			desc.nLostAt = timestamp;
	}

#if 0
	/*
	cout << "Unreferenced: ";
	for(trashmap_t::iterator it=m_trashCandidates.begin(); it!=m_trashCandidates.end(); it++)
		fprintf(stdout, "%lu\t%s\t%s\n",  it->second.first, it->second.second.c_str(), it->first.c_str());
	*/

	if(m_trashCandidates.size()>0)
	{
		// pretty printing for impatient users
		char buf[200];

		// map to to wait
		int nMax=cfg::extreshhold-((now-oldest)/86400);
		int nMin=cfg::extreshhold-((now-newest)/86400);

		snprintf(buf, _countof(buf), "Previously detected: %lu rotten package file(s), "
				"to be deleted in about %d-%d day(s)<br>\n",
				(unsigned long) m_trashCandidates.size(),
				nMin, nMax==nMin?nMax+1:nMax); // cheat a bit for the sake of code simplicity
		SendChunk(buf);
	}
#endif

}
#endif

void expiration::MarkObsolete(cmstring& sPathRel)
{
	m_obsoleteStuff.emplace_back(sPathRel);
}

bool expiration::_checkSolidHashOnDisk(cmstring& hexname, const tRemoteFileInfo& entry,
		cmstring& srcPrefix)
{
	if(m_delCand.find(hexname) == m_delCand.end())
		return false;
	return cacheman::_checkSolidHashOnDisk(hexname, entry, srcPrefix);
}

bool expiration::_QuickCheckSolidFileOnDisk(cmstring& sPathRel)
{
	auto dirNam = SplitDirPath(sPathRel);
	return PickCand(dirNam.first, dirNam.second) != nullptr;
}

std::pair<tDiskFileInfo &, bool> expiration::PickOrAdd(string_view folderName, string_view fileName, bool dupStrings)
{
	auto ret = PickCand(m_delCand.equal_range(fileName), folderName);
	if (ret)
		return {*ret, false};
	if (dupStrings)
	{
		folderName = m_stringStore.Add(folderName);
		fileName = m_stringStore.Add(fileName);
	}
	ret = & m_delCand.emplace(fileName, tDiskFileInfo())->second;
	ret->folder = folderName;
	return {*ret, true};
}

std::pair<tDiskFileInfo &, bool> expiration::PickOrAdd(string_view svPathRel)
{
	auto xy = SplitDirPath(svPathRel);
	return PickOrAdd(xy.first, xy.second, true);
}

tDiskFileInfo* expiration::PickCand(std::pair<decltype (m_delCand)::iterator, decltype (m_delCand)::iterator> range, string_view dir)
{
	auto it = std::find_if(range.first, range.second, [&dir](const auto& el)
	{
		return dir == el.second.folder;
		#warning test alt. impl. which checks a certain suffix first - is it worth it?
#if 0
		constexpr unsigned sfxLen = 8;
		const auto dlen = dir.length();
		if (dlen != el.second.folder.length())
			return false;
		if (dlen < sfxLen)
			return dir == el.second.folder;
		auto a(dir.data()), b(el.second.folder.data());
		auto cpos(dlen-sfxLen);
		return 0 == memcmp(a + cpos, b + cpos, sfxLen) && 0 == memcmp(a, b, cpos);
#endif
	});
	return it == range.second ? nullptr : & it->second;
}

tDiskFileInfo* expiration::PickCand(string_view folderName, string_view fileName)
{
	return PickCand(m_delCand.equal_range(fileName), folderName);
}

}
