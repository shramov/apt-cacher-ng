
#include "debug.h"

#include "pkgimport.h"
#include "header.h"
#include "dirwalk.h"
#include "meta.h"
#include "acfg.h"
#include "filereader.h"
#include "dlcon.h"
#include "csmapping.h"
#include "httpdate.h"

#include <iostream>
#include <fstream>

#include <cstdio>
#include <errno.h>

using namespace std;

#define SCACHEFILE (CACHE_BASE+"_impkeycache")
#define FMTSIG "FMT5"

namespace acng
{
/*
 * Algorithm:
 *
 * Scan the _import directory context and make a dictionary with
 * fingerprint(size, cs) -> (path, bUsed, mtime)
 * in the ProcessRegular callback function
 *
 * Parse the vol. files, do work, and mark used file with bUsed
 *
*/

/*
inline bool IsIndexDiff(const string & sPath)
{
	static tFingerprint fprBuf;
	static string sDiffHint(".diff" SZPATHSEP);
	tStrPos pos;
	return (endsWithSzAr(sPath,".gz") && (sPath.find(sDiffHint) != stmiss || (stmiss != (pos
			= sPath.rfind(SZPATHSEP "20")) && sPath.find_first_not_of("0123456789.-", pos + 3)
			== sPath.size() - 2)));
}
*/

bool pkgimport::ImportFileFound(const mstring &sPath, const struct stat &stinfo, tReporter& rep)
{
	if(CheckStopSignal())
		return false;

	if(endsWithSzAr(sPath, ".head"))
		return true;

	if(sPath.size()<=CACHE_BASE_LEN) // heh?
		return false;


	if(rex::FILE_INVALID == m_parms.res.GetMatchers().GetFiletype(sPath))
		return true;


	// get a fingerprint by checksumming if not already there from the fpr cache

	if(m_precachedList.find(sPath)!=m_precachedList.end())
		return true; // have that already, somewhere...

	for (CSTYPES ctp : { CSTYPE_MD5, CSTYPE_SHA1, CSTYPE_SHA512 } )
	{

		// get the most likely requested contents id
		tFingerprint fpr;
		/*	if ( (IsIndexDiff(sPath) && fpr.ScanFile(sPath, CSTYPE_SHA1, true))
			 || (!IsIndexDiff(sPath) && fpr.ScanFile(sPath, CSTYPE_MD5, false)))
			 */
		if (!fpr.ScanFile(sPath, ctp, false, nullptr))
		{
			rep.Warning() << "Error checking file contents";
			continue;
		}
		if (fpr.GetSize() < 50)
		{
			// ultra small files, looking like garbage (gz'ed empfy file, ...)
			continue;
		}

		SendFmt << "<font color=blue>Checked " << sPath << " (" << GetCsName(ctp)
				<< " fingerprint created)</font><br>\n";

		// add this entry immediately if needed and get its reference
		tImpFileInfo & node = m_importMap[fpr];
		if (node.sPath.empty())
		{ // fresh, just added to the map
			node.sPath = sPath;
			node.mtime = stinfo.st_mtime;
		}
		else if (node.sPath != sPath)
		{
			rep.Warning() << (MsgFmt << "Duplicate found, "sv << sPath << " vs. "
							  << node.sPath << ", ignoring new entry."sv);
			m_importRest.emplace_back(fpr, tImpFileInfo(sPath, stinfo.st_mtime));
		}
	}

	return true;
}

bool LinkOrCopy(const std::string &from, const std::string &to)
{
	mkbasedir(to);
	
	//ldbg("Moving " <<from<< " to " << to);
	
	// hardlinks don't work with symlinks properly, create a similar symlink then
	struct stat stinfo, toinfo;
	
	if(0 == lstat(from.c_str(), &stinfo) && S_ISLNK(stinfo.st_mode))
	{
		char buf[PATH_MAX+1];
		buf[PATH_MAX]=0x0;
		
		if (!realpath(from.c_str(), buf))
			return false;

		::unlink(to.c_str());
		return (0==symlink(buf, to.c_str()));		
	}
	else
	{
		if(0!=stat(from.c_str(), &stinfo))
				return false;
		
		if(0==stat(to.c_str(), &toinfo))
		{
			// same file, don't care
			if(stinfo.st_dev==toinfo.st_dev && stinfo.st_ino == toinfo.st_ino)
				return true;
			
			// unlink, this file must go. If that fails, we have weird permissions anyway.
			if(0!=unlink(to.c_str()))
				return false;
		}
		if (0==link(from.c_str(), to.c_str()))
			return true;

		if(!FileCopy(from,to))
			return false;
		
		// make sure the copy is ok
		return (0 == stat(from.c_str(), &stinfo) 
				&& 0 == stat(to.c_str(), &toinfo)
				&& toinfo.st_size==stinfo.st_size);
		
	}

	return false;
}

string svImportSfx("_import");
string svImportSfxSlash("_import/");
			
void pkgimport::Action()
{
	auto sSrcPath = SABSPATH(svImportSfx);
	
	tReporter rep(this, MsgFmt << "Importing from " << sSrcPath << " directory.",
				  eDlMsgSeverity::INFO, tReporter::SECTION);

	m_bByPath = true; // should act on all locations

	_LoadKeyCache();
	if(!m_precachedList.empty())
	{
		Send( tSS(100) << "Loaded " << m_importMap.size()
			  << (m_precachedList.size()==1 ? " entry" : " entries")
			  <<" from the fingerprint cache<br>\n");
	}

	IFileHandler::FindFiles(CACHE_BASE, [&](cmstring& s, const struct stat&)
	{
		if (CheckStopSignal()) return false;
		string_view rel(s);
		rel.remove_prefix(CACHE_BASE_LEN + 1);
		if (!IsInternalItem(s, true))
			AddIFileCandidate(rel);
		return true;
	});

	if(CheckStopSignal()) return;
	
	if (m_metaFilesRel.empty())
	{
		rep.Error("No index files detected. Unable to continue, cannot map files to internal locations."sv);
		return;
	}

	if (CheckStopSignal())
		return;
	if (!UpdateVolatileFiles() && m_bErrAbort)
	{
		rep.Error(sAbortMessage);
		return;
	}
	if (CheckStopSignal())
		return;

	DBGQLOG("building contents map for " << sSrcPath);
	IFileHandler::FindFiles(sSrcPath, [&](cmstring& s, const struct stat& stinfo)
	{
		return ImportFileFound(s, stinfo, rep);
	});

	if(CheckStopSignal())
		return;
	
	if(m_importMap.empty())
	{
		Send("No appropriate files found in the _import directory.<br>\nDone.<br>\n");
		return;
	}
	
	ProcessSeenIndexFiles([this](const tRemoteFileInfo &e) {
		HandlePkgEntry(e); });

	if(CheckStopSignal()) return;

	ofstream fList;
	fList.open(SCACHEFILE.c_str(), ios::out | ios::trunc);

	if(!fList.is_open())
	{
		unlink(SCACHEFILE.c_str());
		fList.open(SCACHEFILE.c_str(), ios::out| ios::trunc);
	}
	if(!fList.is_open())
		Send("Cannot save fingerprint cache, ignored");
	else
		fList << FMTSIG"\n";
	
	off_t remaining=0;

	for(const auto& fpr2info: m_importMap)
	{
		// delete imported stuff, and cache the fingerprint only when file was deleted
		
		if(fpr2info.second.bFileUsed && 0==unlink(fpr2info.second.sPath.c_str()))
			continue;
		if(!fList.is_open())
			continue;

#define svendl "\n"sv // don't flush all the time
		fList << fpr2info.first.GetSize() << svendl
				<< (int)fpr2info.first.GetCsType() << svendl
				<< fpr2info.first.GetCsAsString() << svendl
				<< fpr2info.second.sPath.substr(sSrcPath.size()+1) <<svendl
				<< fpr2info.second.mtime	<<svendl;

		remaining += fpr2info.first.GetSize();
	}

	// deal with the rest, never double-examine that stuff
	// copy&paste FTW
	for(const auto& fpr2info : m_importRest)
	{
		fList << fpr2info.first.GetSize() << svendl
				<< (int)fpr2info.first.GetCsType() << svendl
				<< fpr2info.first.GetCsAsString() << svendl
				<< fpr2info.second.sPath.substr(sSrcPath.size()+1) <<svendl
				<< fpr2info.second.mtime	<<svendl;
		remaining += fpr2info.first.GetSize();
	}
	fList.flush();
	fList.close();

	PrintStats("Imported data, per target");

	int n=(m_importMap.size()+m_importRest.size());
	if(n)
		SendFmt << "\n<br>" << n << (n==1 ? " file (" : " files (")
		<< offttosH(remaining) << ") left behind";
}

void pkgimport::HandlePkgEntry(const tRemoteFileInfo &entry)
{
	//typedef std::map<tFingerprint, tImpFileInfo, ltfingerprint> tImportMap;
	auto hit = m_importMap.find(entry.fpr);
	if (hit==m_importMap.end())
		return;
	
	string sDestAbs = SABSPATH(cfg::stupidfs ? DosEscape(entry.path) : entry.path);
#warning fixme, simplifypath?

	auto sDestHeadAbs = sDestAbs + ".head"sv;
	cmstring& sFromAbs=hit->second.sPath;

	SendFmt << "<font color=green>HIT: "sv  << sFromAbs
			<< "<br>\nDESTINATION: "sv << sDestAbs << "</font><br>\n"sv;

	// linking and moving would shred them when the link leads to the same target
	Cstat stDest(sDestAbs), stFrom(sFromAbs);

	if (stDest && stFrom
			&& stDest.info().st_ino == stFrom.info().st_ino
			&& stDest.info().st_dev == stFrom.info().st_dev)
	{
		//cerr << "Same target file, ignoring."<<endl;
		hit->second.bFileUsed=true;
		Send("<span class=\"WARNING\">Same file exists</span><br>\n");
		if(0!=access(sDestHeadAbs.c_str(), F_OK))
		{
			Send("<span class=\"WARNING\">Header is missing, will recreate...</span>\n<br>\n");
			goto gen_header;
		}
		return;
	}

	unlink(sDestAbs.c_str());

	// XXX: maybe use inject code instead

	if (!LinkOrCopy(sFromAbs, sDestAbs))
	{
		Send("<span class=\"ERROR\">ERROR: couldn't link or copy file.</span>\n<br>\n");
		return;
	}

	gen_header:

	unlink(sDestHeadAbs.c_str());
	if (!StoreHeadToStorage(sDestAbs+".head", entry.fpr.GetSize(), nullptr, nullptr))
	{
		log::err("Unable to store generated header");
		return; // junk may remain but that's a job for cleanup later
	}
	hit->second.bFileUsed = true;
	SetFlags(m_currentlyProcessedIfile).usedDiskSpace += entry.fpr.GetSize();
}

void pkgimport::_LoadKeyCache()
{
	std::ifstream in;

	tImpFileInfo info;
	tFingerprint fpr;
	
	in.open(SCACHEFILE.c_str());
	string cs;
	if(!in.is_open())
		return;

	std::getline(in, cs);
	if(cs!=FMTSIG)
		return;

/*	if(m_bVerbose)
		SendChunk("Loading fingerprints from key cache... \n");
		*/
	int csType(CSTYPE_INVALID);

	for(;;)
	{
		info.bFileUsed = false;

		std::getline(in, cs);
		if (fpr.SetSize(cs), fpr.GetSize() < 0)
			return;
		in>>csType;
		std::getline(in, cs); // newline

		std::getline(in, cs); // checksum line
		if(!fpr.SetCs(cs, (CSTYPES)csType))
			return;

		std::getline(in, info.sPath);
		info.sPath.insert(0, SABSPATH(svImportSfxSlash));

		in>>info.mtime;
		std::getline(in, cs); // newline

#ifdef DEBUG_FLAGS
		bool bNix=(stmiss != info.sPath.find("cabextract"));
#endif

		struct stat stbuf;
		if(0==stat(info.sPath.c_str(), &stbuf) 
				&& info.mtime==stbuf.st_mtime)
		{
			m_importMap[fpr]=info;
			m_precachedList.insert(info.sPath);
		}
		
		if(in.eof() || in.fail())
			break;
	}
	/*
	 *
	 if(m_bVerbose)
		SendChunk(ltos(m_importMap.size())+" entries<br>\n");
		*/
}

/*
bool pkgimport::ProcessOthers(const mstring& sPath, const struct stat&)
{
	if(S_ISLNK())
}

void tFInfo::Dump(FILE *fh, const string & second)
{
	fprintf(fh, "%s;%lu;%lu\n%s\n", sPath.c_str(), nSize, nTime, second.c_str());
}

void pkgimport::_GetCachedKey(const string & sPath, const struct stat &stinfo, string &out)
{
	char buf[PATH_MAX+5+3*sizeof(unsigned long)];
	sprintf(buf, "%s\n%lu\n%lu\n", sPath.c_str(), (unsigned long) stinfo.st_size,
			(unsigned long) stinfo.st_mtime);
	tStringMap::const_iterator it=m_cacheMap.find(buf);
	if(it==m_cacheMap.end())
		out.clear();
	else
		out=it->second;
}

 */

}
