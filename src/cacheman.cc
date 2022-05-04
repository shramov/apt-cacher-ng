
//#define DEBUGSPAM

#include "debug.h"
#include "cacheman.h"
#include "expiration.h"
#include "acfg.h"
#include "meta.h"
#include "filereader.h"
#include "fileitem.h"
#include "dlcon.h"
#include "dirwalk.h"
#include "header.h"
#include "job.h"
#include "remotedb.h"
#include "fileio.h"
#include "acworm.h"
#include "acpatcher.h"
#include "astrop.h"
#include "actemplates.h"
#include "acutilpath.h"

#include <map>
#include <unordered_map>
#include <string>
#include <iostream>
#include <algorithm>
#include <future>
#include <charconv>

#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <regex.h>

#define MAX_TOP_COUNT 10

#define HARD_TIMEOUT std::chrono::minutes(10)

using namespace std;

#define ESPAN "<span class=\"ERROR\">"sv
#define WSPAN "<span class=\"WARNING\">"sv

#define ECLASS ESPAN "ERROR: "sv
#define WCLASS WSPAN "WARNING: "sv
#define GCLASS "<span class=\"GOOD\">OK: "sv
#define CLASSEND "</span>\n"sv

namespace acng
{

static cmstring dis("/binary-");
static cmstring oldStylei18nIdx("/i18n/Index");
static cmstring diffIdxSfx(".diff/Index");

time_t m_gMaintTimeNow=0;

atomic_ptrdiff_t g_dler;

#define PATCH_TEMP_DIR "_actmp/"
#define PATCH_COMBINED_NAME "combined.diff"
static cmstring sPatchCombinedRel(PATCH_TEMP_DIR PATCH_COMBINED_NAME);
#define PATCH_BASE_NAME "patch.base"
static cmstring sPatchInputRel(PATCH_TEMP_DIR PATCH_BASE_NAME);
#define PATCH_RESULT_NAME "patch.result"
static cmstring sPatchResultRel(PATCH_TEMP_DIR PATCH_RESULT_NAME);

#define ERRMSGABORT  dbgline; if(m_nErrorCount && m_bErrAbort) { Send(sErr); return false; }
#define ERRABORT dbgline; if(m_nErrorCount && m_bErrAbort) { return false; }

inline tStrPos FindCompSfxPos(const string &s)
{
	for(auto &p : sfxXzBz2GzLzma)
	{
		if(endsWith(s, p))
			return(s.size()-p.size());
	}
	return stmiss;
}

static short FindCompIdx(cmstring &s)
{
	for(unsigned i=0;i<_countof(sfxXzBz2GzLzma); ++i)
	{
		if(endsWith(s, sfxXzBz2GzLzma[i]))
			return i;
	}
	return -1;
}

// not like above, by size (not likelyhood) and uncompressed comes last
unsigned MapByBestCompression(cmstring& s)
{
	for (unsigned i = 0; i < _countof(sfxXzBz2GzLzmaNone); ++i)
		if (endsWith(s, sfxXzBz2GzLzmaNone[i]))
			return i;
	return _countof(sfxXzBz2GzLzmaNone);
}

struct tLessByBestCompression
{
  bool operator()(cmstring& a, cmstring &b) const
  {
	  return MapByBestCompression(a) < MapByBestCompression(b);
  }
} lessByBestCompression;

struct tContentKey
{
	mstring distinctName;
	tFingerprint fpr;
	mstring toString() const
	{
		return valid() ? distinctName + "|" + (mstring) fpr : se;
	}
	bool operator<(const tContentKey& other) const
	{
		if(fpr == other.fpr)
			return distinctName < other.distinctName;
		return fpr < other.fpr;
	}
	bool valid() const { return fpr.GetCsType() != CSTYPES::CSTYPE_INVALID; }
};
struct tFileGroup
{
	// the list shall be finally sorted by compression type (most favorable first)
	// and among the same type by modification date so the newest appears on top which
	// should be the most appropriate for patching
	tStrDeq paths;

	tContentKey diffIdxId;
#ifdef EXPLICIT_INDEX_USE_CHECKING
	bool isReferenced = false;
#endif
};

class tFileGroups : public std::map<tContentKey, tFileGroup> {};
cmstring& cacheman::GetFirstPresentPath(const tFileGroups& groups, const tContentKey& ckey)
{
	auto it = groups.find(ckey);
	if(it == groups.end())
		return se;

	for(auto& s: it->second.paths)
		if(GetFlags(s).vfile_ondisk)
			return s;

	return se;
}

cacheman::cacheman(tRunParms&& parms) :
	tExclusiveUserAction(move(parms)),
	m_bErrAbort(false), m_bForceDownload(false),
	m_bScanInternals(false), m_bByPath(false), m_bByChecksum(false), m_bSkipHeaderChecks(false),
	m_bTruncateDamaged(false),
	m_nErrorCount(0),
	m_nProgIdx(0), m_nProgTell(1)
{
	m_gMaintTimeNow=GetTime();
	m_bErrAbort=(m_parms.cmd.find("abortOnErrors=aOe")!=stmiss);
	m_bByChecksum=(m_parms.cmd.find("byChecksum")!=stmiss);
	m_bByPath=(StrHas(m_parms.cmd, "byPath") || m_bByChecksum);
	m_bForceDownload=(m_parms.cmd.find("forceRedownload")!=stmiss);
	m_bSkipHeaderChecks=(m_parms.cmd.find("skipHeadChecks")!=stmiss);
	m_bTruncateDamaged=(m_parms.cmd.find("truncNow")!=stmiss);
	m_bSkipIxUpdate=(m_parms.cmd.find("skipIxUp=si")!=stmiss);

	if (m_parms.cmd.find("beVerbose")!=stmiss)
		m_printSeverityMin = eDlMsgSeverity::VERBOSE;
}

bool cacheman::AddIFileCandidate(string_view sPathRel)
{

 	if(sPathRel.empty())
 		return false;

	enumMetaType t;
	if (
			(
				// SUSE stuff, not volatile but also contains file index data
				endsWithSzAr(sPathRel, ".xml.gz")
				||
				rex::FILE_VOLATILE == m_parms.res.GetMatchers().GetFiletype(term(sPathRel))
				)

			&& enumMetaType::EIDX_UNKNOWN != (t = GuessMetaTypeFromPath(sPathRel)))
	{
		auto & atts = SetFlags(sPathRel);
		atts.vfile_ondisk=true;
		atts.eIdxType=t;
 		return true;
     }
 	return false;
}

// defensive getter/setter methods, don't create non-existing entries
const cacheman::tIfileAttribs & cacheman::GetFlags(string_view sPathRel) const
{
	auto it = m_metaFilesRel.find(sPathRel);
	if (m_metaFilesRel.end() == it)
		return attr_dummy;
	return it->second;
}

cacheman::tIfileAttribs &cacheman::SetFlags(string_view sPathRel)
{
	ASSERT(!sPathRel.empty());
	auto it = m_metaFilesRel.find(sPathRel);
	if (it != m_metaFilesRel.end())
		return it->second;
	return m_metaFilesRel[m_stringStore.Add(sPathRel)];
}

cacheman::tMetaMap::iterator cacheman::SetFlags(string_view sPathRel, bool &reportCreated)
{
	ASSERT(!sPathRel.empty());
	auto it = m_metaFilesRel.find(sPathRel);
	reportCreated = it == m_metaFilesRel.end();
	return reportCreated
			? m_metaFilesRel.emplace(m_stringStore.Add(sPathRel), tIfileAttribs()).first
			: it;
}

thread_local string sendDecoBuf;

void cacheman::SendDecoratedComment(string_view msg, eDlMsgSeverity colorHint)
{
	if (colorHint < m_printSeverityMin && m_print.state == printcfg::BEGIN)
		return;

	if (m_print.state > printcfg::BEGIN)
	{
		Append(sendDecoBuf, "[", msg, "]");
		ReportCont(sendDecoBuf, colorHint);
		sendDecoBuf.clear();
	}
	else
	{
		// then format and print here
		auto typ = colorHint < eDlMsgSeverity::WARNING
				? "<span>"sv : (colorHint >= eDlMsgSeverity::NONFATAL_ERROR ? ESPAN : WSPAN);
		SendFmt << typ << msg << "</span><br>\n"sv;
	}
}

void cacheman::CloseLine()
{
	if (m_print.state == printcfg::BEGIN)
		return;

	m_print.state = printcfg::BEGIN;

	switch(m_print.format)
	{
	case acng::cacheman::printcfg::DEV:
	case acng::cacheman::printcfg::WEB:
		switch(m_print.fmtdepth)
		{
// try using predefined instead of extra format code
		case 0:
			Send("<br>\n"sv);
			break;
		case 1:
			Send("</span><br>\n"sv);
			break;
		case 2:
			Send("</span></span><br>\n"sv);
			break;
		default:
			for (;m_print.fmtdepth; --m_print.fmtdepth)
				sendDecoBuf += "</span>"sv;
			sendDecoBuf += "<br>\n"sv;
			Send(sendDecoBuf);
			sendDecoBuf.clear();
			break;
		}
		m_print.fmtdepth = 0;
		break;
	case acng::cacheman::printcfg::CRONJOB:
		Send("\n"sv);
		break;
	}
}

void cacheman::ReportBegin(string_view what, eDlMsgSeverity sev, bool bForceCollecting, bool bIgnoreErrors)
{
	if (m_print.state != m_print.BEGIN)
	{
		ReportEnd(""sv);
	}

	if (bIgnoreErrors)
		m_print.sevMax = m_printSeverityMin;

	if (sev > m_print.sevMax)
		sev = m_print.sevMax;

	m_print.sevCur = sev;

	m_print.curFileRel = what;
	if (sev < m_printSeverityMin || bForceCollecting)
	{
		m_print.state = m_print.COLLECTING;
		m_print.buf = what;
		m_print.buf += ": "sv;
#warning implement format as needed
	}
	else
	{
		m_print.state = m_print.PRINTING;
		SendFmt << what << ": "sv;
	}
//	SendFmt << "TODO: ReportBegin, type=" << (int) sev << ", force="<<bForceCollecting <<", what=" << what << "End-Reportbegin\n";
//#warning implement me
}

void cacheman::ReportCont(string_view msg, eDlMsgSeverity sev)
{
//	SendFmt << "TODO: ReportCont, type=" << (int) sev << ", msg=" << msg << "~ReportCont~";
//#warning implement me

	if (sev > m_print.sevMax)
		sev = m_print.sevMax;

	if (AC_UNLIKELY(m_print.state == m_print.BEGIN))
	{
		ReportBegin("<UNKNOWN>"sv, sev, false, false);
	}

	if (sev > m_print.sevMax)
		sev = m_print.sevMax;

	if (sev > m_print.sevCur)
	{
		m_print.sevCur = sev;
	}

	// time to switch?
	if (sev >= m_printSeverityMin && m_print.state == m_print.COLLECTING)
	{
#warning add ESPAN or WSPAN
		m_print.buf += msg;
		m_print.state = m_print.PRINTING;
		Send(m_print.buf);
		m_print.buf.clear();
	}
	else if (m_print.state == m_print.PRINTING)
	{
		Send(msg);
	}
	else if (m_print.state == m_print.COLLECTING)
	{
		#warning add ESPAN or WSPAN
		m_print.buf += msg;
	}
}

void cacheman::ReportEnd(string_view msg, eDlMsgSeverity sev, unsigned hints)
{
	//SendFmt << "TODO: ReportEnd, type=" << (int) sev << "=: " << msg << "End-ReportEnd<br>\n";
	ReportCont(msg, sev);
#warning fixme
//	AddDelCbox(sPathRel, reason);
	CloseLine();
}

int cacheman::CheckCondition(string_view key)
{
	if (key == "purgeActionVisible"sv)
		return 0 + m_adminActionList.empty();
	return tExclusiveUserAction::CheckCondition(key);
}

void cacheman::ReportData(eDlMsgSeverity sev, string_view path, string_view reason)
{
	SendFmt << "TODO: ReportData, type=" << (int) sev << ", reason="<< reason << ", path=" << path << "End-Reportdata<br>\n";
#warning implement me
}

#if 0
void cacheman::Rep(eDlPrintHint type, eDlMsgSeverity sev, string_view msg)
{
#define LBEGIN ""sv
#define LEND "<br>\n"sv

	if (m_printState == eDlPrintHint::INHIBITED && type != eDlPrintHint::BEGIN)
		return;

	if (m_printState > eDlPrintHint::BEGIN && type == eDlPrintHint::BEGIN)
	{
		Send(LEND);
		m_printState = eDlPrintHint::BEGIN;
	}
	switch (type)
	{
	case eDlPrintHint::BEGIN:
		m_curDlFileRel = msg;
		m_printState = eDlPrintHint::STATUS;
	break;
	}
}

#endif

// detects when an architecture has been removed entirely from the Debian archive
bool cacheman::IsDeprecatedArchFile(cmstring &sFilePathRel)
{
	tStrPos pos = sFilePathRel.rfind("/dists/");
	if(pos == stmiss)
		return false; // cannot tell
	pos=sFilePathRel.find_first_not_of('/', pos+7);
	if(pos == stmiss)
		return false;
	pos=sFilePathRel.find('/', pos);
	if(pos == stmiss)
		return false;
	// should match the path up to Release/InRelease file

	if(endsWithSzAr(sFilePathRel, "Release") && pos >= sFilePathRel.length()-9)
		return false; // that would be the file itself, or InRelease

	string s;
	filereader reader;
	if ( (s = sFilePathRel.substr(0, pos) + relKey,
			GetFlags(s).uptodate && reader.OpenFile(SABSPATH(s)))
			||
			(s=sFilePathRel.substr(0, pos) + inRelKey, GetFlags(s).uptodate && reader.OpenFile(SABSPATH(s))
		)
	)
	{
		pos = sFilePathRel.find("/binary-", pos);
		if(stmiss == pos)
			return false; // heh?
		pos += 8;
		tStrPos posend = sFilePathRel.find('/', pos);
		if(stmiss == posend)
			return false; // heh?

		string_view sLine;
		while(reader.GetOneLine(sLine))
		{
			tSplitWalk w(sLine, SPACECHARS);
			if(!w.Next() || w.view() != "Architectures:"sv)
				continue;
			while(w.Next())
			{
				if(sFilePathRel.compare(pos, posend-pos, w.view()) == 0)
					return false; // architecture is there, not deprecated
			}
			return true; // okay, now that arch should have been there :-(
		}
	}

	return false;
}

/*
mstring FindCommonPath(cmstring& a, cmstring& b)
{
	LPCSTR pa(a.c_str), pb(b.c_str());
	LPCSTR po(pa), lspos(pa);
	while(*pa && *pb) { if(*pa == '/') lspos = pa;  ++pa; ++pb; }
	return a.substr(0, lspos-po);
}
*/

struct tRewriteHint
{
	string_view fromEnd, toEnd, extraCheck;
};
static constexpr tRewriteHint sourceRewriteMap[] =
{
		{ "/Packages.bz2", "/Packages.xz", "" },
		{ "/Sources.bz2", "/Sources.xz", "" },
		{ "/Release", "/InRelease", "" },
		{ "/InRelease", "/Release", "" },
		{ ".bz2", ".xz", "i18n/Translation-" },
		{ "/Packages.gz", "/Packages.xz", "" },
		{ "/Sources.gz", "/Sources.xz", "" }
};

typedef std::function<void(cacheman::eDlResult)> tDlRep;

class TDownloadController : public enable_shared_from_this<TDownloadController>
{
public:

	struct TDownloadState;
	lint_user_ptr<dlcontroller> dler;
	deque<lint_user_ptr<TDownloadState>> states;
	cacheman& m_owner;
	promise<cacheman::eDlResult> m_repResult;
	aobservable::subscription m_shutdownCanceler;
	const tRewriteHint *pUsedRewrite = nullptr;

	struct TDownloadState : public tLintRefcounted, public tExtRefExpirer
	{
		TDownloadController& m_owner;
		cacheman& Q;

		fileitem::FiStatus initState = fileitem::FIST_FRESH;
		tRepoResolvResult repinfo;
		TFileItemHolder fiaccess;
		mstring sGuessedFrom;
		tHttpUrl usedUrl; // copy of the last download attempt
		tHttpUrl altUrl; // fallback to this on first failure
		acworm scratchpad;

		//uint64_t prog_before = 0;

		tRepoResolvResult repoSrc;
		header hor;
		fileitem::tSpecialPurposeAttr fiAttr;

		aobservable::subscription observer;

		void DLFIN(cacheman::eDlResult res, string_view result_msg)
		{
			if (fiaccess)
			{
				auto dlCount = fiaccess->TakeTransferCount();
				static cmstring sInternal("[INTERNAL:");
				// need to account both, this traffic as officially tracked traffic, and also keep the count
				// separately for expiration about trade-off calculation
				log::transfer(dlCount, 0, Concat(sInternal, GetTaskInfo(m_owner.m_owner.m_parms.type).typeName, "]"),  opts.sFilePathRel, false);
			}

			if (res == cacheman::eDlResult::OK && opts.bIsVolatileFile)
				m_owner.m_owner.SetFlags(opts.sFilePathRel).uptodate = true;

			m_owner.Finish(res, result_msg, this);
		}
		cacheman::tDlOpts opts;

		TDownloadState(TDownloadController& owner, cacheman::tDlOpts&& xopts)
			: m_owner(owner), Q(owner.m_owner), opts(move(xopts))
		{
		}

		void ItemEvent()
		{
			LOGSTARTFUNC;

			if (!__user_ref_cnt())
			{
				observer.reset();
				return;
			}

			if (!fiaccess.get())
			{
				DBGQLOG("Error, event from unknown");
				return;
			}

			dbgline;
			{
				// simple limiter, not more dots than one per second
				static auto spamLimit = GetTime();
				auto now = GetTime();
				if (now > spamLimit + 1)
				{

					m_owner.m_owner.ReportCont(".");
					spamLimit = now;
				}
			}

			auto fist = fiaccess->GetStatus();
			ldbg(fist);
			ASSERT(fist >= fileitem::FIST_DLPENDING);

			if (fist < fileitem::FIST_COMPLETE)
				return; // should came back again

			if (fist == fileitem::FIST_COMPLETE && fiaccess->m_responseStatus.code == 200)
				return DLFIN(cacheman::eDlResult::OK, MsgFmt << " <i>("sv << fiaccess->TakeTransferCount() / 1024 << "KiB)</i>\n"sv);

			ResolveError();
		}

		void ResolveError()
		{
			LOGSTARTFUNC;

			/////////////////////////////////////////////////////
			/// \brief DL failed, find a workaround strategy
			/////////////////////////////////////////////////////


			ASSERT(!(fiaccess->m_responseStatus.code == 500 && fiaccess->m_responseStatus.msg.empty())); // catch a strange condition
			LOG("having alternative url and fitem was created here anyway");

			auto replUrl = [&](tHttpUrl &alt) { return move(cacheman::tDlOpts(move(opts)).ForceUrl(move(alt))); };

			if(!altUrl.sHost.empty() && fiaccess.get())
			{
				dbgline;

				m_owner.m_owner.ReportCont(MsgFmt << "<i>Remote peer is not usable but the alternative source might be guessed from the path as "
						<< altUrl.ToURI(true) << " . If this is the better option, please remove the file "
						<< opts.sFilePathRel << ".head manually from the cache folder or remove the whole index file with the Delete button below.</i>\n");

				return m_owner.Restart(this, replUrl(altUrl));
			}

			if ( (cfg::follow404 && fiaccess->m_responseStatus.code == 404) || m_owner.m_owner.IsDeprecatedArchFile(opts.sFilePathRel))
			{
				m_owner.m_owner.MarkObsolete(opts.sFilePathRel);
				return DLFIN(cacheman::eDlResult::GONE, "(no longer available)"sv);
			}

			if (m_owner.m_owner.GetFlags(opts.sFilePathRel).forgiveDlErrors
					 ||
					 (fiaccess->m_responseStatus.code == 404 && endsWith(opts.sFilePathRel, oldStylei18nIdx))
					 )
			{
				return DLFIN(cacheman::eDlResult::OK, "(ignored)"sv);
			}


			if(repoSrc.repodata
					&& repoSrc.repodata->m_backends.empty()
					&& !hor.h[header::XORIG]
					&& opts.forcedURL.sHost.empty())
			{
				// oh, that crap: in a repo, but no backends configured, and no original source
				// to look at because head file is probably damaged :-(
				// try to re-resolve relative to InRelease and retry download
				m_owner.m_owner.ReportCont("<span class=\"WARNING\">"sv
					 "Warning, running out of download locations (probably corrupted "sv
					 "cache). Trying an educated guess...</span><br>\n"sv);

				cmstring::size_type pos = opts.sFilePathRel.length();
				while (true)
				{
					pos = opts.sFilePathRel.rfind(CPATHSEPUNX, pos);
					if (pos == stmiss)
						break;
					for (const auto& sfx : {&inRelKey, &relKey})
					{
						if(endsWith(opts.sFilePathRel, *sfx))
							continue;

						auto testpath = opts.sFilePathRel.substr(0, pos) + *sfx;
						if (!m_owner.m_owner.GetFlags(testpath).vfile_ondisk)
							continue;

						header hare;
						if (!hare.LoadFromFile(SABSPATHEX(testpath, ".head")))
							continue;

						if(!hare.h[header::XORIG])
							continue;

						string url(hare.h[header::XORIG]);
						url.replace(url.size() - sfx->size(), sfx->size(),
									opts.sFilePathRel.substr(pos));
						tHttpUrl tu;
						if (tu.SetHttpUrl(url, false))
						{
							m_owner.m_owner.ReportCont("Restarting download..."sv);
							return m_owner.Restart(this, replUrl(tu));
						}
					}
					if (!pos)
						break;
					pos--;
				}
			}

			if(fiaccess && opts.bGuessReplacement && fiaccess->m_responseStatus.code == 404)
			{
				// another special case, slightly ugly :-(
				// this is explicit hardcoded repair code
				// it switches to a version with better compression silently

				for (const auto& fix : sourceRewriteMap)
				{
					if (!endsWith(opts.sFilePathRel, fix.fromEnd) || !StrHas(opts.sFilePathRel, fix.extraCheck))
						continue;
					m_owner.m_owner.ReportCont("Attempting to download the alternative version..."sv);
					// if we have it already, use the old URL as base, otherwise recreate from whatever URL was recorded as download source
					if (usedUrl.sHost.empty())
					{
						auto p = fiaccess->m_responseOrigin;
						if (!usedUrl.SetHttpUrl(p))
							continue;
					}
					if (usedUrl.sHost.empty())
						continue; // crap, we need it!
					if (!endsWith(usedUrl.sPath, fix.fromEnd) || !StrHas(usedUrl.sPath, fix.extraCheck))
						continue;

					decltype (opts) newOpts;
					newOpts.GuessReplacement(false);
					newOpts.forcedURL = usedUrl;
					newOpts.forcedURL.sPath.replace(newOpts.forcedURL.sPath.size()-fix.fromEnd.size(),
													fix.fromEnd.size(), fix.toEnd);
					return m_owner.Restart(this, move(newOpts));
#warning readd the checkbox handling, also below
#if 0
					auto xpath = Concat(sFilePathRel.substr(0, sFilePathRel.size() - fix.fromEnd.size()), fix.toEnd);
					auto contFunc = [ourRep = sp->repResult, this, sFilePathRel](cacheman::eDlResult res)
					{
						if (res == eDlResult::OK)
						{
							MarkObsolete(sFilePathRel);
							return ourRep(eDlResult::OK);
						}
						// XXX: this sucks a little bit since we don't want to show the checkbox
						// when the fallback download succeeded... but on failures, the previous one
						// already added a newline before
						AddDelCbox(sFilePathRel, m_dlCtx->states.back()->sErr, true);
						return ourRep(eDlResult::FAIL_REMOTE);
					};
#endif
				}
			}
			string sErr(message_detox(fiaccess->m_responseStatus.msg, fiaccess->m_responseStatus.code));

#if 0
			if(!GetFlags(sFilePathRel).hideDlErrors)
			{
				SendFmt << "<span class=\"ERROR\">"sv << sErr << "</span>\n"sv;
				if(0 == (sp->m_parms.hints & DL_HINT_NOTAG))
					AddDelCbox(sFilePathRel, sErr);
			}
			DLFIN(cacheman::eDlResult::FAIL_REMOTE, sErr);
#endif
			return DLFIN(cacheman::eDlResult::FAIL_REMOTE, "(not downloadable)"sv);
		}

		void Run()
		{
			LOGSTARTFUNC;
			ASSERT_IS_MAIN_THREAD;

			auto sFilePathAbs = SABSPATH(opts.sFilePathRel);
			fiAttr.bVolatile = opts.bIsVolatileFile || Q.m_bForceDownload || opts.bForceReDownload;

			// header could contained malformed data and be nuked in the process,
			// try to get the original source whatever happens
			hor.LoadFromFile(sFilePathAbs + ".head");

			dbgline;
			if(! Q.m_parms.res.GetItemRegistry())
				return DLFIN(cacheman::cacheman::eDlResult::FAIL_LOCAL, "Internal Cache Error"sv);

			fiaccess = Q.m_parms.res.GetItemRegistry()->Create(opts.sFilePathRel,
																   (Q.m_bForceDownload | opts.bForceReDownload)
																   ? ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY
																   : (fiAttr.bVolatile
																	  ? ESharingHow::AUTO_MOVE_OUT_OF_THE_WAY
																	  : ESharingHow::ALWAYS_TRY_SHARING),
																   fiAttr);
			if (!fiaccess)
				return DLFIN(cacheman::eDlResult::FAIL_LOCAL, "Internal Cache Error: could not create file item handler"sv);

			observer = fiaccess->Subscribe([pin = as_lptr(this)] { pin->ItemEvent(); });

			dbgline;

			initState = fiaccess->Setup();

			if (initState > fileitem::FIST_COMPLETE ||
				(fileitem::FIST_COMPLETE == initState && fiaccess.get()->m_responseStatus.code != 200))
			{
				return DLFIN(cacheman::eDlResult::FAIL_REMOTE,
							 message_detox(fiaccess.get()->m_responseStatus.msg,
										   fiaccess.get()->m_responseStatus.code));
			}

			tHttpUrl *pResolvedDirectUrl = nullptr, *fallbackUrl = nullptr;
			tHttpUrl tempHeadPath, tempHeadOrig;

			if (fileitem::FIST_COMPLETE == initState)
				return DLFIN(cacheman::eDlResult::OK, "... (complete)"sv);

			if (!opts.forcedURL.sHost.empty())
				pResolvedDirectUrl = & opts.forcedURL;
			else
			{
				// must have the URL somewhere

				bool bCachePathAsUriPlausible = tempHeadPath.SetHttpUrl(opts.sFilePathRel, false);
				ldbg("Find backend for " << opts.sFilePathRel << " parsed as host: "  << tempHeadPath.sHost
						<< " and path: " << tempHeadPath.sPath << ", ok? " << bCachePathAsUriPlausible);

				if (!cfg::stupidfs
						&& bCachePathAsUriPlausible
						&& !! (repoSrc.repodata = remotedb::GetInstance().GetRepoData(tempHeadPath.sHost))
						&& ! repoSrc.repodata->m_backends.empty())
				{
					ldbg("will use backend mode, subdirectory is path suffix relative to backend uri");
					repoSrc.sRestPath = scratchpad.Add(tempHeadPath.sPath.substr(1));
					repoSrc.psRepoName = scratchpad.Add(tempHeadPath.sHost);
				}
				else
				{
					// ok, cache location does not hint to a download source,
					// try to resolve to an URL based on the old header information;
					// if not possible, guessing by looking at related files and making up
					// the URL as needed

					dbgline;
					if (bCachePathAsUriPlausible) // default option, unless the next check matches
					{
						StrSubst(tempHeadPath.sPath, "//", "/");
						pResolvedDirectUrl = &tempHeadPath;
					}
					// and prefer the source from xorig which is likely to deliver better result
					if (hor.h[header::XORIG] && tempHeadOrig.SetHttpUrl(hor.h[header::XORIG], false))
					{
						dbgline;
						fallbackUrl = pResolvedDirectUrl;
						StrSubst(tempHeadPath.sPath, "//", "/");
						pResolvedDirectUrl = &tempHeadOrig;
					}
					else if (!sGuessedFrom.empty()
							&& hor.LoadFromFile(SABSPATH(sGuessedFrom + ".head"))
							&& hor.h[header::XORIG]) // might use a related file as reference
					{
						mstring refURL(hor.h[header::XORIG]);

						tStrPos spos(0); // if not 0 -> last slash sign position if both
						for (tStrPos i=0; i < sGuessedFrom.size() && i < opts.sFilePathRel.size(); ++i)
						{
							if (opts.sFilePathRel[i] != sGuessedFrom.at(i))
								break;
							if (opts.sFilePathRel[i] == '/')
								spos = i;
						}
						// cannot underflow since checked by less-than
						auto chopLen = sGuessedFrom.size() - spos;
						auto urlSlashPos = refURL.size()-chopLen;
						if (chopLen < refURL.size() && refURL[urlSlashPos] == '/')
						{
							dbgline;
							refURL.erase(urlSlashPos);
							refURL += opts.sFilePathRel.substr(spos);
							//refURL.replace(urlSlashPos, chopLen, sPathSep.substr(spos));
							if (tempHeadOrig.SetHttpUrl(refURL, false))
							{
								StrSubst(tempHeadPath.sPath, "//", "/");
								pResolvedDirectUrl = &tempHeadOrig;
							}
						}
					}

					if(!pResolvedDirectUrl)
						return DLFIN(cacheman::eDlResult::FAIL_REMOTE, "Failed to calculate the original URL"sv);
				}
			}

			// might still need a repo data description
			if (pResolvedDirectUrl)
			{
				dbgline;
				repinfo = remotedb::GetInstance().GetRepNameAndPathResidual(*pResolvedDirectUrl);
				if (repinfo.valid())
				{
					dbgline;
					// also need to use local memory, to recover in the next cycle
					repinfo.sRestPath = scratchpad.Add(repinfo.sRestPath);
					pResolvedDirectUrl = nullptr;
					repoSrc = repinfo;
				}
			}
			dbgline;

			if ( ! m_owner.dler->AddJob(as_lptr(fiaccess.get()),
										 pResolvedDirectUrl,
										 pResolvedDirectUrl ? nullptr : & repoSrc))
			{
				return DLFIN(cacheman::eDlResult::FAIL_LOCAL,
							 MsgFmt << "Cannot send download request, aborting "sv
							 << (pResolvedDirectUrl
							 ? pResolvedDirectUrl->ToURI(true)
							 : repoSrc.sRestPath));
			}

			LOG("Download of " << opts.sFilePathRel << " invoked");

			// keep a copy for fixup attempts if needed
			if (fallbackUrl)
				altUrl = move(*fallbackUrl);
			if (pResolvedDirectUrl)
				usedUrl = move(*pResolvedDirectUrl);
		}

	public:
		void Abandon() override
		{
			observer.reset();
			fiaccess.reset();
		};
	};

	void Start(std::promise<cacheman::eDlResult>& pro, cacheman::tDlOpts&& opts)
	{
		m_repResult.swap(pro);
		states.emplace_back(lint_user_ptr(new TDownloadState(*this, move(opts))));
		states.back()->Run();
	}

	void Restart(TDownloadState* caller, cacheman::tDlOpts&& opts)
	{
		states.emplace_back(as_lptr(new TDownloadState(*this, move(opts))));
		states.back()->Run();
		Abort(caller);
	}

	void Abort(TDownloadState* what)
	{
		ASSERT_IS_MAIN_THREAD;
		erase_if(states, [&](const auto & el){ return el.get() == what; });
	}
	void Finish(cacheman::eDlResult res, string_view result_msg, TDownloadState* caller2destroy)
	{
		LOGSTARTFUNCx((int)res, result_msg, (uintptr_t)caller2destroy);
		ASSERT_IS_MAIN_THREAD;

#warning pass the checkbox control flags, if error
		m_owner.ReportEnd(result_msg);

		auto pin(shared_from_this());
		try
		{
			m_repResult.set_value(res);
		}
		catch (...)
		{
			LOG("IDler exception");
		}
		Abort(caller2destroy);
	}

	TDownloadController(acres& res, cacheman& owner)
		: dler(dlcontroller::CreateRegular(res)), m_owner(owner)
	{
		ASSERT_IS_MAIN_THREAD;
		m_shutdownCanceler = evabase::GetGlobal().subscribe([&]()
		{
			Finish(cacheman::eDlResult::FAIL_LOCAL, "Abort on shutdown"sv, nullptr);
		});
	}
};

cacheman::~cacheman()
{
	if (m_dler)
	{
		evabase::GetGlobal().SyncRunOnMainThread([&]()
		{
			m_dler.reset();
			return 0;
		});
	}
}

cacheman::eDlResult cacheman::Download(string_view sFilePathRel, tDlOpts opts)
{
	LOGSTARTFUNC;
	ReportBegin(sFilePathRel, opts.msgVerbosityLevel, false, opts.bIgnoreErrors);

	opts.sFilePathRel = sFilePathRel;

	if (GetFlags(sFilePathRel).uptodate)
	{
		ReportEnd(opts.bIsVolatileFile ? "... (fresh)"sv : "... (complete)"sv);
		return cacheman::eDlResult::OK;
	}

	if(opts.bIsVolatileFile && m_bSkipIxUpdate)
	{
		ReportEnd("... (skipped, as requested)"sv);
		return cacheman::eDlResult::OK;
	}

	std::promise<cacheman::eDlResult> pro;
	auto fut = pro.get_future();
	evabase::GetGlobal().SyncRunOnMainThread([&]()
	{
		if (!m_dler)
			m_dler.reset(new TDownloadController(m_parms.res, *this));

		m_dler->Start(pro, move(opts));
		return 0;
	});
	try
	{
		return fut.get();
	}
	catch (...)
	{
		return eDlResult::FAIL_LOCAL;
	}
}

void ACNG_API DelTree(const string &what)
{
	class killa : public IFileHandler
	{
		virtual bool ProcessRegular(const mstring &sPath, const struct stat &)
		{
			::unlink(sPath.c_str()); // XXX log some warning?
			return true;
		}
		bool ProcessOthers(const mstring &sPath, const struct stat &x)
		{
			return ProcessRegular(sPath, x);
		}
		bool ProcessDirAfter(const mstring &sPath, const struct stat &)
		{
			::rmdir(sPath.c_str()); // XXX log some warning?
			return true;
		}
		bool ProcessDirBefore(const mstring &, const struct stat &) { return true;}
	} hh;
	IFileHandler::DirectoryWalk(what, &hh, false, false);
}

/*
 * XXX: most users of this function don't have reliable modification date, therefore it's not used.
 * TODO: optionally fetch HEAD from remote and use the date from there if the size is matching.
 */
bool cacheman::Inject(string_view fromRel, string_view toRel,
		bool bSetIfileFlags, off_t contLen, tHttpDate lastModified, string_view forceOrig)
{
	LOGSTARTFUNCx(fromRel, toRel, bSetIfileFlags, contLen, lastModified.value(0), forceOrig);

	// XXX should it really filter it here?
	if(GetFlags(toRel).uptodate)
		return true;

	int fd = open(SZABSPATH(fromRel), O_RDONLY);
	if (fd == -1)
		return false;
	unique_eb src(evbuffer_new());
	if (!src.valid() || 0 != evbuffer_add_file(*src, fd, 0, -1))
		return false;
	auto haveLen = off_t(evbuffer_get_length(*src));
	LOG(haveLen << " vs. " << contLen);
	if (contLen >= 0 && haveLen != contLen)
		return false;
	auto& res = m_parms.res;

	fileitem::tSpecialPurposeAttr attr;
	attr.bVolatile = m_parms.res.GetMatchers().GetFiletype(to_string(toRel)) == rex::FILE_VOLATILE;
	auto act = [&]()
	{

		auto hodler = res.GetItemRegistry()->Create(to_string(toRel), ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY, attr);
		if (!hodler.get())
			return false;
		auto fi = hodler.get();
		hodler.get()->Setup();
		auto fist = fi->GetStatus();
		if (fist > fileitem::FIST_COMPLETE)
		{
			return false;
		}
		if (fist == fileitem::FIST_COMPLETE)
		{
			return true;
		}
		if (fist >= fileitem::FIST_DLASSIGNED)
		{
			return false; // item is fresh (forced), there should be no downloader here yet
		}
		auto xorig = forceOrig.empty() ? fi->m_responseOrigin : forceOrig;
		// otherwise process here
		if (!fi->DlStarted(nullptr, 0, lastModified, xorig, { 200, "OK" }, 0, contLen))
		{
			return false;
		}
		for (int i = 0; i < 10; ++i)
		{
			auto rest = evbuffer_get_length(*src);
			if (rest == 0)
			{
				fi->DlFinish(true);
				return fi->GetStatus() == fileitem::FIST_COMPLETE;
			}
			auto r = fi->DlConsumeData(*src, rest);
			if (r < 0)
				return false;
		}
		return false;
	};
	auto result = evabase::GetGlobal().SyncRunOnMainThread([&](){ return (uintptr_t) act(); });
	if (result)
	{
		if (bSetIfileFlags)
		{
			tIfileAttribs &atts = SetFlags(toRel);
			atts.uptodate = atts.vfile_ondisk = true;
		}
		return true;
	}
	return false;
}

void cacheman::ExtractReleaseDataAndAutofixPatchIndex(tFileGroups& idxGroups, string_view sPathRel)
{
#ifdef DEBUG
	SendFmt << "Start parsing " << sPathRel << "<br>";
#endif
	// raw data extraction

	typedef map<string, tContentKey> tFile2Cid;
	// pull all contents into a sorted dictionary for later filtering
	// the key represents the identity of the file
	tFile2Cid file2cid;

	auto recvInfo = [&file2cid](const tRemoteFileInfo &entry)
	{
#if 0 // bad, keeps re-requesting update of such stuff forever. Better let the quick content check analyze and skip them.
		tStrPos compos=FindCompSfxPos(entry.sFileName);
		// skip some obvious junk and its gzip version
		if(0==entry.fpr.size || (entry.fpr.size<33 && stmiss!=compos))
			return;
#endif
		auto& cid = file2cid[entry.path];
		cid.fpr = entry.fpr;

		auto pos = entry.path.rfind(dis);
		// if looking like Debian archive, keep the part after binary-...
		if (stmiss != pos)
			cid.distinctName = entry.path.substr(pos);
		else
			cid.distinctName = GetBaseName(entry.path);
#warning ^^^^ this follows the old logic, but is the scheme valid for private repos?
	};

	ParseAndProcessMetaFile(recvInfo, m_metaFilesRel.find(sPathRel), EIDX_RELEASE);

	if (file2cid.empty())
		return;

#ifndef EXPLICIT_INDEX_USE_CHECKING
	// first, look around for for .diff/Index files on disk, update them, check patch base file
	// and make sure one is there or something is not right
	for (const auto& cid : file2cid)
	{
		// not diff index or not in cache?
		if (!endsWith(cid.first, diffIdxSfx))
			continue;
		auto& flags = GetFlags(cid.first);
		if (!flags.vfile_ondisk)
			continue;
		//if(!flags.uptodate && !Download(cid.first, true, eMsgShow))
		//	continue;

		// ok, having a good .diff/Index file, what now?

		auto sBase = cid.first.substr(0, cid.first.size() - diffIdxSfx.size());

		// two rounds, try to find any in descending order, then try to download one
		for (int checkmode=0; checkmode < 3; checkmode++)
		{
			for (auto& suf: sfxXzBz2GzLzma)
			{
				auto cand(sBase+suf);
				if (checkmode == 0)
				{
					if (_QuickCheckSolidFileOnDisk(cand))
						goto found_base;
				}
				else if (checkmode == 1) // now really check on disk
				{
					if (0 == ::access(SZABSPATH(cand), F_OK))
						goto found_base;
				}
				else
				{
					SendFmt << "No base file to use patching on " << cid.first << ", trying to fetch " << cand << hendl;
					if (eDlResult::OK == Download(cand, tDlOpts().Verbosity(eDlMsgSeverity::NEVER).GuessedFrom(cid.first)))
					{
						SetFlags(cand).vfile_ondisk=true;
						goto found_base;
					}
				}
			}
		}
found_base:;
	}
#endif
	//dbgState();

	// now refine all extracted information and store it in eqClasses for later processing
	for (const auto& if2cid : file2cid)
	{
		string sNativeName=if2cid.first.substr(0, FindCompSfxPos(if2cid.first));
		tContentKey groupId;

		// identify the key for this group. Ideally, this is the descriptor
		// of the native representation, if found in the release file, in doubt
		// take the one from the best compressed form or the current one
		auto it2=file2cid.find(sNativeName);
		if (it2 != file2cid.end())
			groupId=it2->second;
		else
		{
			for(auto& ps : sfxXzBz2GzLzma)
				ifThereStoreThereAndBreak(file2cid, sNativeName+ps, groupId);
		}

		if (!groupId.valid())
			groupId = if2cid.second;

		tFileGroup &tgt = idxGroups[groupId];
		tgt.paths.emplace_back(if2cid.first);

		// also the index file id
		if (!tgt.diffIdxId.valid()) // XXX if there is no index at all, avoid repeated lookups somehow?
			ifThereStoreThere(file2cid, sNativeName+diffIdxSfx, tgt.diffIdxId);

		// and while we are at it, check the checksum of small files in order
		// to reduce server request count
		if (InRange(0, if2cid.second.fpr.GetSize(), 42000))
		{
			auto& flags = SetFlags(if2cid.first);
			if(flags.vfile_ondisk && if2cid.second.fpr.CheckFile(SABSPATH(if2cid.first)))
				flags.uptodate = true;
		}
	}
}

/*
* First, strip the list down to those which are at least partially present in the cache.
* And keep track of some folders for expiration.
*/

void cacheman::FilterGroupData(tFileGroups& idxGroups)
{
	for(auto it=idxGroups.begin(); it!=idxGroups.end();)
	{
		unsigned found = 0;
		for(auto& path: it->second.paths)
		{
			// WARNING: this check works only as long as stuff is volatile AND index type
			if(!GetFlags(path).vfile_ondisk)
				continue;
			found++;
			auto pos = path.rfind('/');
			if(pos!=stmiss)
				m_managedDirs.insert(path.substr(0, pos+1));
		}

		if(found)
		{
#ifdef EXPLICIT_INDEX_USE_CHECKING
			//bool holdon = StrHas(it->second.paths.front(), "contrib/binary-amd64/Packa");
			// remember that index file might be used by other groups
			if(it->second.diffIdxId.valid())
			{
				auto indexIter = idxGroups.find(it->second.diffIdxId);
				if(indexIter != idxGroups.end())
					indexIter->second.isReferenced = true;
			}
#endif
			++it;
		}
		else
			idxGroups.erase(it++);
	}
#ifdef DEBUG
	SendFmt << "Folders with checked stuff:" << hendl;
	for(auto s : m_managedDirs)
		SendFmt << s << hendl;
#endif
#ifdef EXPLICIT_INDEX_USE_CHECKING
	// some preparation of patch index processing
	for(auto& group: idxGroups)
	{
		if(!endsWith(group.first.distinctName, diffIdxSfx))
			continue;
		for(auto& indexPath: group.second.paths)
			Download(indexPath, true, eMsgShow, tFileItemPtr(), 0, 0);

		if(group.second.isReferenced)
			continue;

		// existing but unreferenced pdiff index files are bad, that means that some client
		// is tracking this stuff via diff update but ACNG has no clue of the remaining contents
		// -> let's make sure the best compressed version is present on disk
		// In that special case the extra index files will not become active ASAP but that's
		// probably good enough for expiration activity (use it the day after).

		for(auto& indexPath: group.second.paths)
		{
			auto sBase = indexPath.substr(0, indexPath.size()-diffIdxSfx.size());
			SendFmt << "Warning: no base file to use patching on " << indexPath
					<< ", trying to fetch some" << hendl;
			for(auto& suf : sfxXzBz2GzLzma)
			{
				auto cand(sBase+suf);
				if(Download(cand, true, eMsgShow, tFileItemPtr(), 0, 0, &indexPath))
				{
					SetFlags(cand).vfile_ondisk=true;
					break;
				}
			}
		}
	}
#endif
}

int cacheman::PatchOne(cmstring& pindexPathRel, const tStrDeq& siblings)
{
#define PATCH_FAIL __LINE__
	unsigned injected = 0;

	auto need_update = std::find_if(siblings.begin(), siblings.end(), [&](cmstring& pb)
	{
			const auto& fl = GetFlags(pb);
			return fl.vfile_ondisk && !fl.uptodate;
	});

	if (need_update == siblings.end())
		return -1;

	filereader reader;
	if (!reader.OpenFile(SABSPATH(pindexPathRel), true, 1))
		return PATCH_FAIL;
	map<string, deque<string> > contents;
	ParseGenericRfc822File(reader, "", contents);
	auto& sStateCurrent = contents["SHA256-Current"];
	if (sStateCurrent.empty() || sStateCurrent.front().empty())
		return PATCH_FAIL;
	auto& csHist = contents["SHA256-History"];
	if (csHist.empty() || csHist.size() < 2)
		return PATCH_FAIL;

	tFingerprint probeStateWanted, // the target data
			probe, // temp scan object
			probeOrig; // appropriate patch base stuff

	if(!probeStateWanted.Set(tSplitWalk(sStateCurrent.front()), CSTYPE_SHA256))
		return PATCH_FAIL;

	unordered_map<string,tFingerprint> patchSums;
	for(const auto& line: contents["SHA256-Patches"])
	{
		tSplitWalk split(line);
		tFingerprint probe;
		if(!probe.Set(split, CSTYPE_SHA256) || !split.Next())
			continue;
		patchSums.emplace(split.view(), probe);
	}
	cmstring sPatchResultAbs(SABSPATH(sPatchResultRel));
	cmstring sPatchInputAbs(SABSPATH(sPatchInputRel));
	cmstring sPatchCombinedAbs(SABSPATH(sPatchCombinedRel));

	// returns true if a new patched file was created
	auto tryPatch = [&]() -> int
	{
		// XXX: use smarter line matching or regex
		auto probeCS = probeOrig.GetCsAsString();
		auto probeSize = offttos(probeOrig.GetSize());
		FILE_RAII pf;
		string pname;
		for(const auto& histLine: csHist)
		{
			// quick filter
			if(!pf.m_p && !startsWith(histLine, probeCS))
				continue;

			// analyze the state line
			tSplitWalk split(histLine, SPACECHARS);
			if(!split.Next() || !split.Next())
				continue;
			// at size token
			if(!pf.m_p && probeSize != split.view())
				return PATCH_FAIL; // faulty data?
			if(!split.Next())
				continue;
			pname = split.str();
			trimBoth(pname);
			if (!startsWithSz(pname, "T-2"))
				return PATCH_FAIL;
			if (pname.empty())
				return PATCH_FAIL;
			break;
		}

		if (pname.empty())
			return PATCH_FAIL;

		// ok, first patch of the sequence found
		if(!pf.m_p)
		{
			acng::mkbasedir(sPatchCombinedAbs);
			// append mode!
			pf.m_p=fopen(sPatchCombinedAbs.c_str(), "w");
			if(!pf.m_p)
			{
				Send("Failed to create intermediate patch file, stop patching...");
				return PATCH_FAIL;
			}
		}
		// ok, so we started collecting patches...

		string patchPathRel(pindexPathRel.substr(0, pindexPathRel.size()-5) +
							pname + ".gz");
		if(eDlResult::OK != Download(patchPathRel,
									 tDlOpts()
									 .SetVolatile(false)
									 .Verbosity(eDlMsgSeverity::NEVER)
									 .GuessedFrom(pindexPathRel)))
		{
			return PATCH_FAIL;
		}
#warning fixme, still needing to check? even if there is no parser?
		//SetFlags(patchPathRel).parseignore = true; // static stuff, hands off!

		// append context to combined diff while unpacking
		// XXX: probe result can be checked against contents["SHA256-History"]
		if(!probe.ScanFile(SABSPATH(patchPathRel), CSTYPE_SHA256, true, pf.m_p))
		{
			ReportMisc(MsgFmt << "Failure on checking of intermediate patch data in " << patchPathRel << ", stop patching...");
			return PATCH_FAIL;
		}
		if(probe != patchSums[pname])
		{
			SendFmt<< "Bad patch data in " << patchPathRel <<" , stop patching...";
			return PATCH_FAIL;
		}

		if(pf.valid())
		{
			::fprintf(pf.m_p, "w patch.result\n");
			::fflush(pf.m_p); // still a slight risk of file closing error but it's good enough for now
			if(::ferror(pf.m_p))
			{
				Send("Patch application error");
				return PATCH_FAIL;
			}
			checkForceFclose(pf.m_p);

			ReportMisc("Patching...<br>"sv, SEV_DBG);

#if 0
			tSS cmd;
			cmd << "cd '" << CACHE_BASE << PATCH_TEMP_DIR "' && ";
			auto act = cfg::suppdir + SZPATHSEP "acngtool";
			if(!cfg::suppdir.empty() && 0==access(act.c_str(), X_OK))
			{
				cmd << "'" << act
					<< "' patch " PATCH_BASE_NAME " " PATCH_COMBINED_NAME " " PATCH_RESULT_NAME;
			}
			else
			{
				cmd << " red --silent " PATCH_BASE_NAME " < " PATCH_COMBINED_NAME;
			}

			auto szCmd = cmd.c_str();
#ifdef UNDER_TEST
			cerr << cmd.view() << endl;
#endif
			if (::system(szCmd))
			{
				MTLOGASSERT(false, "Command failed: " << cmd);
				return PATCH_FAIL;
			}
#else
			try
			{
				acpatcher().Apply(sPatchInputAbs, sPatchCombinedAbs, sPatchResultAbs);
			}
			catch (const std::exception& ex)
			{
				MTLOGASSERT(false, "Patch operation failed - " << ex.what());
				return PATCH_FAIL;
			}

#endif
			if (!probe.ScanFile(sPatchResultAbs, CSTYPE_SHA256, false))
			{
				MTLOGASSERT(false, "Scan failed: " << sPatchResultAbs);
				return PATCH_FAIL;
			}

			if(probe != probeStateWanted)
			{
				MTLOGASSERT(false,"Final verification failed");
				return PATCH_FAIL;
			}
			ReportMisc("<b>Patch result OKAY.</b>"sv, SEV_DBG);
			return 0;
		};
		return PATCH_FAIL;
	};
	// start with uncompressed type, xz, bz2, gz, lzma
	for(auto itype : { -1, 0, 1, 2, 3})
	{
		for(const auto& pb : siblings)
		{
			if(itype != FindCompIdx(pb))
				continue;

			FILE_RAII df;
			DelTree(SABSPATH("_actmp"));
			acng::mkbasedir(sPatchInputAbs);
			df.m_p = fopen(sPatchInputAbs.c_str(), "w");
			if(!df.m_p)
			{
				SendFmt << "Cannot write temporary patch data to "
						<< sPatchInputRel << "<br>";
				return PATCH_FAIL;
			}
			if(!probeOrig.ScanFile(SABSPATH(pb), CSTYPE_SHA256, true, df.get()))
			{
				continue;
			}
			df.reset();
			if(probeStateWanted == probeOrig)
			{
				SetFlags(pb).uptodate = true;
				SyncSiblings(pb, siblings);
				return 0; // the file is uptodate already...
			}

			if(tryPatch())
				continue;

			// install to one of uncompressed locations, let SyncSiblings handle the rest
			for(auto& path: siblings)
			{
				// if possible, try to reconstruct reliable download source information
				header h;
				if(h.LoadFromFile(SABSPATH(pindexPathRel) + ".head")
				   && h.h[header::XORIG])
				{
					auto len = strlen(h.h[header::XORIG]);
					if(len < diffIdxSfx.length())
						return PATCH_FAIL; // heh?
					h.h[header::XORIG][len-diffIdxSfx.length()] = 0;
				}

				ReportMisc(MsgFmt << "Installing as " << path << ", state: "
						   << (string) probeStateWanted, SEV_DBG);

				/*
				 * We don't know the change date from the index, let's consider it very old.
				 * Should be an "okay" trade-off because the purpose of that file is mostly for our
				 * metadata processing, probably no user will download them directly
				 * (only by-hash variants which are different story).
				 */
				if(FindCompIdx(path) < 0
				   && Inject(sPatchResultRel, path, true,
							 probeStateWanted.GetSize(),
							 tHttpDate(1),
							 h.get(header::XORIG, se)))
				{
					SyncSiblings(path, siblings);
					injected++;
				}
			}

			// patched, installed, DONE!
			return injected ? 0 : PATCH_FAIL;
		}
	}
	return -2;
}

bool cacheman::UpdateVolatileFiles()
{
	LOGSTARTFUNC

	string sErr; // for download error output

	// just reget them as-is and we are done. Also include non-index files, to be sure...
	if (m_bForceDownload)
	{
		Send("<b>Bringing index files up to date...</b><br>\n"sv);
		for (auto& f: m_metaFilesRel)
		{
			auto notIgnorable = ! GetFlags(f.first).forgiveDlErrors;

			// tolerate or not, it depends
			switch(Download(f.first))
			{
			case eDlResult::OK: continue;
			case eDlResult::GONE:
				m_nErrorCount += (notIgnorable && !cfg::follow404);
				break;
			case eDlResult::FAIL_LOCAL:
			case eDlResult::FAIL_REMOTE:
				m_nErrorCount += notIgnorable;
				break;
			}
		}
		ERRMSGABORT;
		LOGRET(false);
	}
	dbgline;
	tFileGroups idxGroups;

	auto dbgState = [&]() {
#ifdef DEBUGSPAM
	for (auto& f: m_metaFilesRel)
		SendFmt << "State of " << f.first << ": "
			<< f.second.toString();
#endif
	};

#ifdef DEBUG
		auto dbgDump = [&](const char *msg, int pfx)
		{
			tFmtSendTempRaii<mainthandler, bSS> printer(*this);
			printer.GetFmter() << "#########################################################################<br>\n"sv
					<< "## " <<  msg  << sBRLF
					<< "#########################################################################<br>\n"sv;
			for(const auto& cp : idxGroups)
			{
				printer.GetFmter() << pfx << ": cKEY: " << cp.first.toString() << hendl
						<< pfx << ": idxKey:" <<cp.second.diffIdxId.toString() << hendl
						<< pfx << ": Paths:" << hendl;
				for(const auto& path : cp.second.paths)
				{
					printer.GetFmter() << pfx << ":&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;"
					<< path << "&lt;=&gt;" << GetFlags(path).toString()
					<< hendl;
				}
			}
		};
#else
#define dbgDump(x,y)
#endif

	dbgState();

	MTLOGDEBUG("<br><br><b>STARTING ULTIMATE INTELLIGENCE</b><br><br>");

	if(m_bSkipIxUpdate)
	{
		SendFmt << "<span class=\"ERROR\">"
				"Warning: Online Activity is disabled, some update errors might be not recoverable without it"
				"<br></span>\n";
	}

	// this runs early with the state that is present on disk, before updating any file,
	// since it deals with the "reality" in the cache

	Send("<b>Checking by-hash stored files...</b><br>"sv);

	/*
	 * Update all Release files
	 *
	 * Prefer the InRelease version where it has been observed
	 */

	unordered_map<string_view,string_view> uniqueReleaseFiles; // key=folder, value=rel-file-path
	for (auto it = m_metaFilesRel.begin(); it != m_metaFilesRel.end(); ++it)
	{
		size_t len;

		if(endsWith(it->first, inRelKey))
			len = it->first.length() - inRelKey.length();
		else if (endsWith(it->first, relKey))
			len = it->first.length() - relKey.length();
		else
			continue;

		// InRelease will come first through sorting, next emplace skips it
		auto considered = uniqueReleaseFiles.emplace(it->first.substr(0, len), it->first);
		if (!considered.second)
			continue;
		auto sPathRel = it->first;

		if(!RestoreFromByHash(it, false))
		{
			m_nErrorCount++;
			if(sErr.empty())
				sErr = Concat("ByHash error at ", sPathRel);
			continue;
		}
		// okay, not restored from by-hash folder, download the original volatile version
		const auto& fl = GetFlags(sPathRel);
		if (eDlResult::OK != Download(sPathRel, tDlOpts()
									 .IgnErr(fl.hideDlErrors)
									 .GuessReplacement(true)))
		{
			if (!fl.hideDlErrors)
			{
				m_nErrorCount++;
				if(sErr.empty())
					sErr = "DL error at " + sPathRel;
			}

			if (CheckStopSignal())
			{
				SendFmt << "Operation canceled."sv << hendl;
				return false;
			}
			continue;
		}
	}

	Send("<b>Bringing index files up to date...</b><br>\n"sv);

	if(!FixMissingOriginalsFromByHashVersions())
	{
		SendFmt << "Error fixing by-hash links"sv << hendl;
		m_nErrorCount++;
		LOGRET(false);
	}
	for (const auto& kv: uniqueReleaseFiles)
		ExtractReleaseDataAndAutofixPatchIndex(idxGroups, kv.second);

	dbgDump("After group building:", 0);

	if(CheckStopSignal())
		LOGRET(false);

	// OK, the equiv-classes map is built, now post-process the knowledge
	FilterGroupData(idxGroups);
	ERRMSGABORT;
	dbgDump("Refined (1):", 1);
	dbgState();

	for(auto& it: idxGroups)
	{
		auto& paths(it.second.paths);
		sort(paths.begin(), paths.end(), lessByBestCompression);
#warning keep the uncompressed variant? like if(FindCompIdx(*jit) < 0)	jit++;
		remove_if(paths.begin(), paths.end(), [&](const auto& el){ return !GetFlags(el).vfile_ondisk; });

		BuildSingleLinkedBroList(it.second.paths);

		// also sync the context among the identical ones
		for(auto& p: it.second.paths)
		{
			if(GetFlags(p).uptodate)
				SyncSiblings(p, it.second.paths);
		}
	}

	dbgDump("Refined (5):", 5);
	dbgState();
//	DelTree(SABSPATH("_actmp")); // do one to test the permissions

	// now do patching where possible
	for(auto& groupKV: idxGroups)
	{
		if(!groupKV.second.diffIdxId.valid())
			continue;
		// any of them should do the job
		auto& ipath = GetFirstPresentPath(idxGroups, groupKV.second.diffIdxId);
		if(ipath.empty())
			continue;
		PatchOne(ipath, groupKV.second.paths);
	}
	dbgState();

	// semi-smart download of remaining files
	for(auto& groupKV: idxGroups)
	{
		bool groupHasOneFresh = false;

		for(auto& pathRel: groupKV.second.paths)
		{
			if (groupHasOneFresh)
				break;

			auto &fl = SetFlags(pathRel);
#ifdef DEBUG
			SendFmt << "Considering flags: " << pathRel << " " << fl.toString() << hendl;
#endif
			if (!fl.uptodate)
			{
				// okay, then try to download a fresh version
				groupHasOneFresh = eDlResult::OK == Download(pathRel);
				if (!groupHasOneFresh)
				{
					// a failed download will be caught separately but for now, another download attempt is pointless
					m_nErrorCount += !fl.forgiveDlErrors;
					continue;
				}
			}
			if (fl.uptodate)
				SyncSiblings(pathRel, groupKV.second.paths);
		}
	}
	dbgline;
	MTLOGDEBUG("<br><br><b>NOW GET THE REST</b><br><br>");

	// fetch all remaining stuff, at least the relevant parts and which we also can parse
	for(auto& idx2att : m_metaFilesRel)
	{
		if (idx2att.second.uptodate
				|| !idx2att.second.vfile_ondisk
				|| idx2att.second.eIdxType <= EIDX_UNKNOWN
				|| eDlResult::OK == Download(idx2att.first, tDlOpts()
											 .GuessReplacement(true)
											 .IgnErr(idx2att.second.hideDlErrors)))
		{
			continue;
		}
		m_nErrorCount += (!idx2att.second.forgiveDlErrors);
	}
	LOGRET(true);
}

void cacheman::BuildSingleLinkedBroList(tStrDeq& paths)
{
	if (paths.size() < 2)
		return;
	const string_view *pLast = nullptr;
	tMetaMap::iterator itFirst;
	for(auto &path : paths)
	{
		auto it = m_metaFilesRel.find(path);
		if (pLast)
			it->second.bro = pLast;
		else
			itFirst = it;
		pLast = & it->first;
	}
	itFirst->second.bro = pLast;
#if 0
	// build a daisy chain for later equivalence detection
	tMetaMap::iterator itPrev(m_metaFilesRel.end()),
			itFirst(m_metaFilesRel.end()),
			mit(m_metaFilesRel.end());
	for(auto &path : paths)
	{
		mit = m_metaFilesRel.find(path);
		if (mit == m_metaFilesRel.end())
			continue;
		if (itFirst == m_metaFilesRel.end())
			itFirst = itPrev = mit;
		else
		{
			itPrev->second.bro = mit;
			itPrev = mit;
		}
	}
	if (itFirst != m_metaFilesRel.end())
		mit->second.bro = itFirst;
#endif
}

void cacheman::SyncSiblings(cmstring &srcPathRel,const tStrDeq& targets)
{
	auto srcDirFile = SplitDirPath(srcPathRel);
	//auto srcType = GetTypeSuffix(srcDirFile.second);
	for(const auto& tgt: targets)
	{
		auto& flags = GetFlags(tgt);
		// not valid or it's us?
		if(!flags.vfile_ondisk || tgt == srcPathRel)
			continue;

		auto tgtDirFile = SplitDirPath(tgt);
		bool sameFolder = tgtDirFile.first == srcDirFile.first;

		if(!sameFolder && srcDirFile.second == tgtDirFile.second)
		{
			Inject(srcPathRel, tgt, true, -1, tHttpDate(1), "");
		}
	}
}

cacheman::enumMetaType cacheman::GuessMetaTypeFromPath(string_view sPath)
{
	auto sPureIfileName = GetBaseName(sPath);

	for (auto e : sfxXzBz2GzLzma)
	{
		if (!endsWith(sPureIfileName, e)) continue;
		sPureIfileName.remove_suffix(e.size());
		break;
	}

	if (sPureIfileName == "Packages") // Debian's Packages file
		return EIDX_PACKAGES;

	if (endsWithSzAr(sPureIfileName, ".db") || endsWithSzAr(sPureIfileName, ".db.tar"))
		return EIDX_ARCHLXDB;

	if (sPureIfileName == "setup")
		return EIDX_CYGSETUP;

	if (sPureIfileName == "repomd.xml")
		return EIDX_SUSEREPO;

	if (sPureIfileName.length() > 50
		&& endsWithSzAr(sPureIfileName, ".xml")
		&& sPureIfileName[40] == '-')
	{
		return EIDX_XMLRPMLIST;
	}

	if (sPureIfileName == "Sources")
		return EIDX_SOURCES;

	if (sPureIfileName == "Release" || sPureIfileName == "InRelease")
		return EIDX_RELEASE;

	if (sPureIfileName == sIndex)
		return endsWithSzAr(sPath, "i18n/Index") ? EIDX_TRANSIDX : EIDX_DIFFIDX;

	if (sPureIfileName == "MD5SUMS" && StrHas(sPath, "/installer-"))
		return EIDX_MD5DILIST;

	if (sPureIfileName == "SHA256SUMS" && StrHas(sPath, "/installer-"))
		return EIDX_SHA256DILIST;

	return EIDX_NEVERPROCESS;
}

bool cacheman::ParseAndProcessMetaFile(tCbReport cbReportOne, tMetaMap::iterator idxFile, int8_t runGroupAndTag)
{
	LOGSTARTFUNC;

	// huh?
	if (idxFile == m_metaFilesRel.end())
		return false;

	if (runGroupAndTag && idxFile->second.passId == runGroupAndTag)
		return true;

	const auto& sPath = idxFile->first;
	auto& idxType = idxFile->second.eIdxType;

	tRemoteFileInfo info;

	if(idxType < enumMetaType::EIDX_UNKNOWN) // ignored file
		return true;
	if(idxType == enumMetaType::EIDX_UNKNOWN) // default?
		idxType = GuessMetaTypeFromPath(idxFile->first);
	if(idxType <= enumMetaType::EIDX_UNKNOWN) // still unknown/unsupported... Just ignore.
		return true;
	m_currentlyProcessedIfile = sPath;

#ifdef DEBUG_FLAGS
	bool bNix=StrHas(sPath, "/i18n/");
#endif

	struct tRewriteHint
	{
		// full path of the directory of the processed index file with trailing slash
		string sBaseDir;

		// base folder which the package paths do refer to as root
		string sPkgBaseDir;
		/**
		 * @brief configure calculates and remembers the base directory of the packages and normalizes that path
		 * @param idxPathRel
		 * @param idxType
		 * @return
		 */
		bool configure(string_view idxPathRel, enumMetaType idxType)
		{
			// for some file types the main directory that parsed entries refer to
			// may differ, for some Debian index files for example
			if (!CalculateBaseDirectories(idxPathRel, idxType, sBaseDir, sPkgBaseDir))
				return false;

			if (!endsWithSzAr(sPkgBaseDir, SZPATHSEPUNIX))
				sPkgBaseDir += SZPATHSEPUNIX;

			return true;
		}
		bool setPath(tRemoteFileInfo& info, string_view pkgPath) const
		{
			trimFront(pkgPath, "\\/");
			info.path = sPkgBaseDir;
			info.path += pkgPath;
			return SimplifyPathInplace(info.path);
		}
	};
	deque<tRewriteHint> targets;
	if (!targets.emplace_back().configure(sPath, idxType))
	{
		m_nErrorCount++;
		SendFmt << "Unexpected index file without subdir found: " << sPath;
		return false;
	}
	if (runGroupAndTag) // let's add the doppelgängers
	{
		auto it = idxFile;
		do
		{
			const auto* nextName = it->second.bro;
			if (!nextName || nextName == & it->first)
				break;
			it = m_metaFilesRel.find(* nextName);
			if (it == m_metaFilesRel.end())
				return false; // heh?
			if (!targets.emplace_back().configure(*nextName, idxType))
			{
				m_nErrorCount++;
				SendFmt << "Unexpected index file without subdir found: " << *nextName;
				return false;
			}
		} while (true);
	}

	unsigned progHintX = 0;
#define SPAM_RED_LEVEL 8
	// full mask of ...1111
	constexpr unsigned progTrigger( (1u << SPAM_RED_LEVEL) - 1);

	auto report = [&](string_view pkgPath)
	{
		for(const auto& tgt : targets)
		{
			tgt.setPath(info, pkgPath);
			cbReportOne(info);
			if (0 == (++progHintX & progTrigger))
				ReportCont(".");
		}
	};

	// some common variables
	string_view sLine, key, val;

#if 0
	/*

	  They all might be the same origin and pointing to equal relative locations all over the place

pub/foo/debian/dists/abc/IDX
pub/foo/debian/pool/ddir/xxx

foo/debian/dists/abc/IDX
foo/debian/pool/ddir/xxx

xy/foo/debian/dists/abc/IDX
xy/foo/debian/pool/ddir/xxx

pub/foo/debian/dists/abc/IDX
pub/foo/debian/pool/ddir/xxx

ff/sadf/dists/IDX
ff/sads/pool/ddir/xxx

WTF/sadf/dists/IDX.bz2
WTF/sads/pool/ddir/xxx
	*
	*/
#endif

	filereader reader;
	if (!reader.OpenFile(SABSPATH(sPath), false, 1))
	{
		if (!GetFlags(sPath).forgiveDlErrors) // that would be ok (added by ignorelist), don't bother
		{
			tErrnoFmter err;
			SendFmt << "<span class=\"WARNING\">WARNING: unable to open "<<sPath
					<<"(" << err << ")</span>\n<br>\n";
		}
		return false;
	}

	tStrVec vsMetrics;
	string sStartMark;
	string path;
	string subDir;

	auto submitAndCheckAborted = [&]()
	{
		if(info.fpr.IsValid() && !path.empty())
			report(path);

		info.fpr.Invalidate();
		path.clear();
		subDir.clear();

		return CheckStopSignal();
	};

	switch(idxType)
	{
	case EIDX_PACKAGES:
	{
		LOG("filetype: Packages file");
		constexpr string_view headMD5sum("MD5sum"), headFilename("Filename"), headSize("Size");

		while (reader.GetOneLine(sLine))
		{
			trimBack(sLine);

			if (sLine.empty())
			{
				if (submitAndCheckAborted())
					return true;
			}
			else if (ParseKeyValLine(sLine, key, val))
			{
				// not looking for data we already have
				if (key == headMD5sum)
					info.fpr.SetCs(val, CSTYPE_MD5);
				else if(key == headSize)
					info.fpr.SetSize(val);
				else if(key == headFilename)
					UrlUnescapeAppend(val, (path.clear(), path));
			}
		}

		break;
	}
	case EIDX_ARCHLXDB:
	{
		LOG("assuming Arch Linux package db");
		unsigned nStep = 0;
		enum tExpData
		{
			_fname, _csum, _csize, _nthng
		} typehint(_nthng);

		while (reader.GetOneLine(sLine)) // last line doesn't matter, contains tar's padding
		{
			trimLine(sLine);

			if (nStep >= tExpData::_csize)
			{
				nStep = tExpData::_fname;
				if (submitAndCheckAborted())
					return true;
			}

			if (endsWithSzAr(sLine, "%FILENAME%"))
				typehint = _fname;
			else if (endsWithSzAr(sLine, "%CSIZE%"))
				typehint = _csize;
			else if (endsWithSzAr(sLine, "%MD5SUM%"))
				typehint = _csum;
			else
			{
				switch (typehint)
				{
				case _fname:
					path = sLine;
					nStep = 0;
					break;
				case _csum:
					info.fpr.SetCs(sLine, CSTYPE_MD5);
					nStep++;
					break;
				case _csize:
					info.fpr.SetSize(sLine);
					nStep++;
					break;
				default:
					continue;
				}
				// next line is ignored for now
				typehint = _nthng;
			}
		}
	}
		break;
	case EIDX_CYGSETUP:
		LOG("assuming Cygwin package setup listing");

		while (reader.GetOneLine(sLine))
		{
			if(CheckStopSignal())
				return true;

			string_view input(sLine);
			if(startsWithSz(sLine, "install: "))
				input.remove_prefix(9);
			else if(startsWithSz(sLine, "source: "))
				input.remove_prefix(8);
			else
				continue;

			tSplitWalk split(input, SPACECHARS);
			if(split.Next() && (path = split, !path.empty())
					&& split.Next() && info.SetSize(split.view())
					&& split.Next() && info.fpr.SetCs(split.view()))
			{
				if (submitAndCheckAborted())
					return true;
			}
		}
		break;
	case EIDX_SUSEREPO:
		LOG("SUSE pkg list file, entry level");
		while(reader.GetOneLine(sLine))
		{
			if(CheckStopSignal())
				return true;

			for(tSplitWalk split(sLine, "\"'><=/"); split.Next(); )
			{
				cmstring tok(split);
				LOG("testing filename: " << tok);
				// XXX: something else? Hint to ignore or hint to skip the line?
				if(!endsWithSzAr(tok, ".xml.gz"))
					continue;
				LOG("index basename: " << tok);
				path = tok;
				if (submitAndCheckAborted())
					return true;
			}
		}
		break;
	case EIDX_XMLRPMLIST:
		LOG("XML based file list, pickup any valid filename ending in .rpm");
		while(reader.GetOneLine(sLine))
		{
			if(CheckStopSignal())
				return true;

			for(tSplitWalk split(sLine, "\"'><=/"); split.Next(); )
			{
				cmstring tok(split);
				LOG("testing filename: " << tok);
				if (endsWithSzAr(tok, ".rpm")
						|| endsWithSzAr(tok, ".drpm")
						|| endsWithSzAr(tok, ".srpm"))
				{
					LOG("RPM basename: " << tok);
					path = tok;
					if (submitAndCheckAborted())
						return true;
				}
			}
		}
		break;
		// like http://ftp.uni-kl.de/debian/dists/jessie/main/installer-amd64/current/images/SHA256SUMS
	case EIDX_MD5DILIST:
	case EIDX_SHA256DILIST:
		LOG("Plain list of filenames and checksums");
		while(reader.GetOneLine(sLine))
		{
			tSplitWalk split(sLine, SPACECHARS);
			if( split.Next() && info.fpr.SetCs(split.view(),
					idxType == EIDX_MD5DILIST ? CSTYPE_MD5 : CSTYPE_SHA256)
					&& split.Next() && (path = split, !path.empty()))
			{
				if (submitAndCheckAborted())
					return true;
			}
			else if(CheckStopSignal())
				return true;
		}
		break;
	case EIDX_SOURCES:
	case EIDX_DIFFIDX:
	case EIDX_TRANSIDX:
	case EIDX_RELEASE:
	{
		bool reading = false;
		string_view fname;
		constexpr string_view headCs256("Checksums-Sha256:"sv), headDirectory("Directory:"sv), head256Dl("SHA256-Download:"sv),
				headMd5("MD5Sum:"sv), headSha256("SHA256:");
		CSTYPES relSec = CSTYPES::CSTYPE_UNSET;

		while(reader.GetOneLine(sLine))
		{
			trimBack(sLine);
			if (sLine.empty())
			{
				subDir.clear();
				reading = false;
				continue;
			}
			if (!reading)
			{
				if (sLine.starts_with(headDirectory))
				{
					sLine.remove_prefix(headDirectory.length());
					trimBoth(sLine);
					subDir = sLine;
					continue;
				}
				if (sLine.starts_with(headCs256) || sLine.starts_with(head256Dl))
					reading = true, relSec = CSTYPES::CSTYPE_UNSET;
				else if (idxType == EIDX_RELEASE && sLine.starts_with(headMd5))
					reading = true, relSec = CSTYPES::CSTYPE_MD5;
				else if (idxType == EIDX_RELEASE && sLine.starts_with(headSha256))
					reading = true, relSec = CSTYPES::CSTYPE_SHA256;
				if (reading)
					continue;
			}

			if (reading)
			{
				// content section finished?
				if (sLine.front() > ' ')
				{
					reading = false;
					relSec = CSTYPES::CSTYPE_UNSET;

					if (idxType == EIDX_DIFFIDX || idxType == EIDX_TRANSIDX)
						break; // taking only one single section here

					continue;
				}
				tSplitWalk split(sLine, SPACECHARS);
				if (!split.Next())
					continue;

				// okay, different handling for release files (picking varying checksum types) and Sources&*.diff/Index files (with predefined checksum type)
				if (relSec == CSTYPES::CSTYPE_UNSET)
				{
					if(info.fpr.SetCs(split.view(), CSTYPES::CSTYPE_SHA256)
							&& split.Next() && info.SetSize(split.view())
							&& split.Next() && (fname = split.view(), !fname.empty()))
					{
						path = PathCombine(subDir, fname);
						if (submitAndCheckAborted())
							return true;
					}
				}
				else
				{
					string_view csumStr;
					if(info.fpr.SetCs(csumStr = split.view())
							&& (info.fpr.GetCsType() == CSTYPES::CSTYPE_MD5 || CSTYPES::CSTYPE_SHA256 == info.fpr.GetCsType())
							&& split.Next() && info.SetSize(split.view())
							&& split.Next() && (fname = split.view(), !fname.empty()))
					{
						path = PathCombine(subDir, fname);
						if (submitAndCheckAborted())
							return true;
						// extra handling for the by-hash variants
						path = PathCombine("by-hash"sv, GetCsNameReleaseStyle(relSec), csumStr);
						if (submitAndCheckAborted())
							return true;
					}
				}
			}
		}
		break;
	}
	default:
		Send("<span class=\"WARNING\">"
				"WARNING: unable to read this file (unsupported format)</span>\n<br>\n");
		return false;
	}
	return reader.CheckGoodState(false);
}

bool cacheman::CalculateBaseDirectories(string_view sPath, enumMetaType idxType, mstring& sBaseDir, mstring& sPkgBaseDir)
{
	sBaseDir = GetDirPart(sPath);

	// does this look like a Debian archive structure? i.e. paths to other files have a base
	// directory starting in dists/?
	// The assumption doesn't however apply to the d-i checksum
	// lists, those refer to themselves only.
	//
	// similar considerations for Cygwin setup
	string::size_type pos;
	if (idxType != EIDX_MD5DILIST
			&& idxType != EIDX_SHA256DILIST
			&& stmiss != (pos = sBaseDir.rfind("/dists/")))
	{
		sPkgBaseDir = sPkgBaseDir.assign(sBaseDir, 0, pos + 1);
	}
	else if (idxType == EIDX_CYGSETUP
			 && stmiss != (pos = sBaseDir.rfind("/cygwin/")))
	{
		sPkgBaseDir = sPkgBaseDir.assign(sBaseDir, 0, pos + 8);
	}
	else
		sPkgBaseDir = sBaseDir;

	if (idxType == EIDX_PACKAGES)
		UrlUnescape(sPkgBaseDir);

	return SimplifyPathInplace(sBaseDir) && SimplifyPathInplace(sPkgBaseDir);
}

void cacheman::ParseGenericRfc822File(filereader& reader,
		cmstring& sExtListFilter,
		map<string, deque<string> >& contents)
{
	string_view key, val, lastKey, sLine;
	deque<string>* pLastVal(nullptr);
	while (reader.GetOneLine(sLine))
	{
		if (sLine.empty())
			continue;

		if (isspace((unsigned) (sLine[0])))
		{
			if (!pLastVal)
				continue;

			// also skip if a filter is set for extended lists on specific key
			if (!sExtListFilter.empty() && sExtListFilter != lastKey)
				continue;

			trimBoth(sLine);
			pLastVal->emplace_back().assign(sLine);
		}
		else if (ParseKeyValLine(sLine, key, val))
		{
			lastKey = key;
			pLastVal = & contents[mstring(key)];
			// we don't merge
			pLastVal->clear();
			pLastVal->emplace_back(val);
		}
	}
}

void cacheman::ProcessSeenIndexFiles(std::function<void(tRemoteFileInfo)> pkgHandler)
{
	LOGSTARTFUNC;
	auto tag = GenGroupTag();

	for(auto path2att = m_metaFilesRel.begin(); path2att != m_metaFilesRel.end(); ++path2att)
	{
		if(CheckStopSignal())
			return;

		auto& att = path2att->second;

		if(!att.vfile_ondisk && !att.uptodate)
			continue;

		ReportMisc(MsgFmt << "Parsing metadata in "sv << path2att->first);

		if (ParseAndProcessMetaFile(pkgHandler, path2att, tag))
			continue;

		if(!GetFlags(path2att->first).forgiveDlErrors)
		{
			m_nErrorCount++;
			Send("<span class=\"ERROR\">An error occurred while reading this file, some contents may have been ignored.</span>\n");
			ReportAdminAction(path2att->first, "Index data processing error");
			Send(sBRLF);
		}
	}
}

void cacheman::ReportAdminAction(string_view sFileRel, string_view reason, bool bAlsoHasNativeFile, eDlMsgSeverity reportLevel)
{
	auto id = Add2KillBill(sFileRel, bAlsoHasNativeFile ?
							   Concat(reason, " (incl. alternativ version)"sv) :
							   reason);

	tSelfClearingFmter x(g_fmtBuf);
	//tSendFmtRaii x(*this);
	x.GetFmter() << reason << "<label>(<input type=\"checkbox\" name=\"kf\" value=\""sv
			 << id <<"\">Tag";
	if (bAlsoHasNativeFile)
		x.GetFmter() << ", incl. alternativ version";
	x.GetFmter() << ")</label>";

	ReportCont(g_fmtBuf, reportLevel);
}

void cacheman::PrintAdminFileActions()
{
	SendFmt << "FIXME: print checkboxes for " << m_adminActionList.size() << " entries";
}

void cacheman::TellCount(unsigned nCount, off_t nSize)
{
	SendFmt << sBRLF << offttosH(m_nSpaceReleased) << " freed. " <<sBRLF;
	SendFmt << sBRLF << nCount <<" package file(s) marked "
			"for removal in few days. Estimated disk space to be released: "
			<< offttosH(nSize) << "." << sBRLF << sBRLF;
}

void cacheman::PrintStats(cmstring &title)
{
#warning restoreme, as part of cacheman::PrintAdminFileActions
#if 0
	multimap<off_t, string_view> sorted;
	off_t total = 0;
	for(auto &f: m_metaFilesRel)
	{
		total += f.second.usedDiskSpace;
		if(f.second.usedDiskSpace)
			sorted.emplace(f.second.usedDiskSpace, f.first);
	}
	if(!total)
		return;
	int nMax = std::min(int(sorted.size()), int(MAX_TOP_COUNT));

	// this formats directly to the buffer, flushing it later on

	g_msgFmtBuf << "<br>\n<table name=\"shorttable\"><thead>"
				   "<tr><th colspan=2>"sv << title;
	if(sorted.size() > MAX_TOP_COUNT)
		g_msgFmtBuf << " (Top " << nMax << "<span name=\"noshowmore\">,"
										   " <a href=\"javascript:show_rest();\">show more / cleanup</a></span>)"sv;
	g_msgFmtBuf << "</th></tr></thead>\n<tbody>"sv;
	for(auto it = sorted.rbegin(); it!=sorted.rend(); ++it)
	{
		g_msgFmtBuf << "<tr><td><b>"sv
					<< offttosH(it->first) << "</b></td><td>"sv
					<< it->second << "</td></tr>\n"sv;
		if(nMax--<=0)
			break;
	}
	Send("</tbody></table><div name=\"bigtable\" class=\"xhidden\">"sv);

	g_msgFmtBuf << "<br>\n<table><thead>"
				   "<tr><th colspan=1><input type=\"checkbox\" onclick=\"copycheck(this, 'xfile');\"></th>"
				   "<th colspan=2>"sv
				<< title
				<< "</th></tr></thead>\n<tbody>"sv;
	for(auto it = sorted.rbegin(); it != sorted.rend(); ++it)
	{
		g_msgFmtBuf << "<tr><td><input type=\"checkbox\" class=\"xfile\""sv
					<< AddLookupGetKey(it->second, "") << "></td><td><b>"sv << html_sanitize(offttosH(it->first)) << "</b></td><td>"sv
					<< it->second << "</td></tr>\n"sv;
	}
	SendFmt << "</tbody></table>";
#warning FIXME, check output format, proper syntax?

#endif
}

void cacheman::ProgTell()
{
	if (++m_nProgIdx == m_nProgTell)
	{
		ReportMisc(MsgFmt<<"Scanning, found " << m_nProgIdx << " file" << (m_nProgIdx>1?"s":""),
				   eDlMsgSeverity::INFO);
		m_nProgTell *= 2;
	}
}

bool cacheman::_checkSolidHashOnDisk(cmstring& hexname,
		const tRemoteFileInfo &entry,
		cmstring& privCacheSubdir
		)
{

	auto solidPath = PathCombine(CACHE_BASE, privCacheSubdir, GetDirPart(entry.path),
								 "by-hash"sv,
								 GetCsNameReleaseStyle(entry.fpr.GetCsType()),
								 hexname);
	return ! ::access(solidPath.c_str(), F_OK);
}

void cacheman::BuildCacheFileList()
{
	//dump_proc_status();
	IFileHandler::DirectoryWalk(cfg::cachedir, this);
	//dump_proc_status();
}

bool cacheman::RestoreFromByHash(tMetaMap::iterator relFile, bool bRestoreFromOldRefs)
{
	int errors = 0;
	auto basePath = GetDirPart(relFile->first);

	if (bRestoreFromOldRefs)
		basePath.remove_prefix(cfg::privStoreRelSnapSufixWithSlash.length());

	auto cb = [&](const tRemoteFileInfo &entry) -> void
	{
		// ignore, those files are empty and are likely to report false positives
		if(entry.fpr.GetSize() < 29)
			return;

		// safe output path building
		auto wantedPathRel = SimplifyPathChecked(PathCombine(basePath, entry.path));
		if (!wantedPathRel.second)
			return;

		auto& fl = SetFlags(wantedPathRel.first);
		if (fl.uptodate)
			return;

		auto hexname(entry.fpr.GetCsAsString());

		auto bhPathAbs = PathCombine(CACHE_BASE, GetDirPart(entry.path),
									 "by-hash"sv,
									 GetCsNameReleaseStyle(entry.fpr.GetCsType()),
									 hexname);

		Cstat stBH(bhPathAbs);
		if (!stBH)
			return;

		ReportExtraEntry(bhPathAbs, entry.fpr);

		if (entry.fpr.GetSize() != stBH.size())
		{
			ReportMisc(MsgFmt << bhPathAbs << " is incomplete, ignoring as candidate");
			return;
		}

		auto origPathAbs = PathCombine(CACHE_BASE, entry.path);

		string calculatedOrigin; // needing to rewrite the DL url
		header h;
		// load by-hash header, check URL, rewrite URL, copy the stuff over
		if(!h.LoadFromFile(Concat(bhPathAbs, ".head")) || ! h.h[header::XORIG])
		{
			ReportMisc(MsgFmt << "Couldn't read " << CACHE_BASE << bhPathAbs << ".head");
			return;
		}

		calculatedOrigin = h.h[header::XORIG];
		auto pos = calculatedOrigin.rfind("by-hash/");
		if (pos == stmiss)
			ReportMisc(MsgFmt << CACHE_BASE << bhPathAbs << " is not from by-hash folder", SEV_DBG);
		calculatedOrigin.erase(pos);
		calculatedOrigin += GetBaseName(entry.path);

		auto bhPathRel = string_view(bhPathAbs.data() + CACHE_BASE_LEN, bhPathAbs.length() - CACHE_BASE_LEN);

		if(!Inject(bhPathRel, wantedPathRel.first,
				   false, -1, tHttpDate(h.h[header::LAST_MODIFIED]), calculatedOrigin))
		{
			return ReportMisc(MsgFmt << CACHE_BASE << bhPathRel << " : Couldn't install", SEV_DBG);
		}
		fl.uptodate = fl.vfile_ondisk = true;
	};

	relFile->second.eIdxType = EIDX_RELEASE;

	return ParseAndProcessMetaFile(cb, relFile, GenGroupTag()) && errors == 0;
}

bool cacheman::FixMissingOriginalsFromByHashVersions()
{
	bool ret = true;
	// uses negated values to prefer the newest versions of those file which would pick the latest versions more likely
	map<time_t,string> oldStuff;

	auto pickFunc = [&](cmstring &sPathAbs, const struct stat &info)
	{
		oldStuff.emplace( - info.st_mtim.tv_sec, sPathAbs.substr(CACHE_BASE_LEN));
		return true;
	};
	IFileHandler::FindFiles(SABSPATH(cfg::privStoreRelSnapSufix), pickFunc);

	for (const auto& kv: oldStuff)
	{
		const auto& oldRelPathRel(kv.second);
		bool isNew(false);
		auto flagsIter = SetFlags(oldRelPathRel, isNew);
		// path relative to cache folder
		if (!RestoreFromByHash(flagsIter, true))
		{
			ReportMisc(MsgFmt << "There were error(s) processing "sv << oldRelPathRel << ", ignoring..."sv);
			ReportMisc("Enable verbosity to see more"sv, SEV_DBG, true);
			SetFlags(flagsIter->first).vfile_ondisk = false;
			return ret;
		}
#ifdef DEBUG
		ReportMisc(MsgFmt << "Purging "sv  << oldRelPathRel, SEV_DBG);
#endif
		if (DeleteAndAccount(SABSPATH(oldRelPathRel)) == YesNoErr::ERROR)
			ReportMisc(MsgFmt << "Error removing file, check state of "sv << oldRelPathRel);
		SetFlags(flagsIter->first).vfile_ondisk = false;
	}
	return ret;
}

bool cacheman::IsInternalItem(cmstring &sPathAbs, bool inDoubt)
{
	if (sPathAbs.length() <= CACHE_BASE_LEN)
		return inDoubt;
	return sPathAbs[CACHE_BASE_LEN] == '_';
}

#ifdef DEBUG
void cacheman::DumpInfo(Dumper &dumper)
{
	if (m_dler)
	{
#warning restore
#if 0
		DUMPFMT << m_dler->callID << ", "
				<< m_dlCtx->dler.get();
		for(auto& p: m_dlCtx->states)
		{
			DUMPFMT << (p->pResolvedDirectUrl ? p->pResolvedDirectUrl->ToURI(false) : se);
		}
#endif
	}
}
#endif



#ifdef DEBUG
void tBgTester::Action()
{
	tHttpUrl url;
	url.SetHttpUrl("http://ftp2.de.debian.org/debian/dists/unstable/InRelease");
	Download("debrep/dists/unstable/InRelease", tDlOpts().ForceUrl(move(url)).ForceRedl(true));
	for (int i = 0; i < 10 && !CheckStopSignal(); i++, sleep(1))
	{
		timespec tp;
		clock_gettime(CLOCK_MONOTONIC, &tp);
		SendFmt << tp.tv_sec << "." << tp.tv_nsec << "<br>\n";
	}
}

bool tBgTester::ProcessRegular(const std::string &, const struct stat &)
{
	return !CheckStopSignal();
}

#endif // DEBUG

}
