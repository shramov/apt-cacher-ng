
#include "debug.h"

#include "acfg.h"
#include "meta.h"
#include "ahttpurl.h"
#include "filereader.h"
#include "fileio.h"
#include "sockio.h"
#include "lockable.h"
#include "cleaner.h"
#include "remotedb.h"
#include "acfgshared.h"

#include <regex.h>

#include <iostream>
#include <fstream>
#include <deque>
#include <algorithm>
#include <list>
#include <unordered_map>
#include <atomic>

using namespace std;

namespace acng
{
// hint to use the main configuration excluding the complex directives
//bool g_testMode=false;

bool bIsHashedPwd=false;

#define BARF(x) {if(!g_bQuiet) { cerr << x << endl;} exit(EXIT_FAILURE); }
#define BADSTUFF_PATTERN "\\.\\.($|%|/)"

namespace rex
{
bool CompileExpressions();
}


namespace cfg {

bool ACNG_API g_bQuiet=false, g_bNoComplex=false;

extern std::atomic_bool degraded;

// internal stuff:
string sPopularPath("/debian/");
string tmpDontcache, tmpDontcacheReq, tmpDontcacheTgt, optProxyCheckCmd;
int optProxyCheckInt = 99;

tStrMap localdirs;
// cached mime type strings, locked in RAM
static class : public base_with_mutex, public NoCaseStringMap {} mimemap;

std::bitset<TCP_PORT_MAX> *pUserPorts = nullptr;

tHttpUrl proxy_info;

// just the default, filled in by options
#define ALTERNATIVE_SPAWN_INTERVAL 2
struct timeval furtherConTimeout
{
	ALTERNATIVE_SPAWN_INTERVAL, 200000
};
struct timeval initialConTimeout
{
	ALTERNATIVE_SPAWN_INTERVAL, 200000
};
struct timeval networkTimeout
{
	17, 200000
};

struct MapNameToString
{
	const char *name; mstring *ptr;
};

struct MapNameToInt
{
	const char *name; int *ptr;
	const char *warn; uint8_t base;
	uint8_t hidden;	// just a hint
};

struct tProperty
{
	const char *name;
	std::function<bool(cmstring& key, cmstring& value)> set;
	std::function<mstring(bool superUser)> get; // returns a string value. A string starting with # tells to skip the output
};

// predeclare some
void _ParseLocalDirs(cmstring &value);

unsigned ReadBackendsFile(const string & sFile, const string &sRepName);
unsigned ReadRewriteFile(const string & sFile, cmstring& sRepName);

MapNameToString n2sTbl[] = {
		{  "CacheDir",                &cachedir}
		,{  "LogDir",                  &logdir}
		,{  "SupportDir",              &suppdir}
		,{  "SocketPath",              &udspath}
		,{  "PidFile",                 &pidfile}
		,{  "ReportPage",              &reportpage}
		,{  "VfilePattern",            &vfilepat}
		,{  "PfilePattern",            &pfilepat}
		,{  "SPfilePattern",           &spfilepat}
		,{  "SVfilePattern",           &svfilepat}
		,{  "WfilePattern",            &wfilepat}
		,{  "VfilePatternEx",          &vfilepatEx}
		,{  "PfilePatternEx",          &pfilepatEx}
		,{  "WfilePatternEx",          &wfilepatEx}
		,{  "SPfilePatternEx",         &spfilepatEx}
		,{  "SVfilePatternEx",         &svfilepatEx}
//		,{  "AdminAuth",               &adminauth}
		,{  "BindAddress",             &bindaddr}
		,{  "UserAgent",               &agentname}
		,{  "DontCache",               &tmpDontcache}
		,{  "DontCacheRequested",      &tmpDontcacheReq}
		,{  "DontCacheResolved",       &tmpDontcacheTgt}
		,{  "PrecacheFor",             &mirrorsrcs}
		,{  "RequestAppendix",         &requestapx}
		,{  "PassThroughPattern",      &connectPermPattern}
		,{  "CApath",                  &capath}
		,{  "CAfile",                  &cafile}
		,{  "BadRedirDetectMime",      &badredmime}
		,{	"OptProxyCheckCommand",	   &optProxyCheckCmd}
		,{  "BusAction",                &sigbuscmd} // "Special debugging helper, see manual!"
        ,{  "EvDnsResolvConf",		 	&dnsresconf}

};

MapNameToInt n2iTbl[] = {
		{   "Port",                              &port,             nullptr,    10, false}
		,{  "Debug",                             &debug,            nullptr,    10, false}
		,{  "OfflineMode",                       &offlinemode,      nullptr,    10, false}
		,{  "ForeGround",                        &foreground,       nullptr,    10, false}
		,{  "ForceManaged",                      &forcemanaged,     nullptr,    10, false}
		,{  "StupidFs",                          &stupidfs,         nullptr,    10, false}
		,{  "VerboseLog",                        &verboselog,       nullptr,    10, false}
		,{  "ExThreshold",                       &extreshhold,      nullptr,    10, false}
		,{  "ExTreshold",                        &extreshhold,      nullptr,    10, true} // wrong spelling :-(
		,{  "MaxStandbyConThreads",              &tpstandbymax,     nullptr,    10, false}
		,{  "MaxConThreads",                     &tpthreadmax,      nullptr,    10, false}
		,{  "DlMaxRetries",                      &dlretriesmax,     nullptr,    10, false}
		,{  "DnsCacheSeconds",                   &dnscachetime,     nullptr,    10, false}
		,{  "UnbufferLogs",                      &debug,            nullptr,    10, false}
		,{  "ExAbortOnProblems",                 &exfailabort,      nullptr,    10, false}
		,{  "ExposeOrigin",                      &exporigin,        nullptr,    10, false}
		,{  "LogSubmittedOrigin",                &logxff,           nullptr,    10, false}
		,{  "RecompBz2",                         &recompbz2,        nullptr,    10, false}
		,{  "NetworkTimeout",                    &nettimeout,       nullptr,    10, false}
		,{  "FastTimeout",                       &fasttimeout,      nullptr,    10, false}
		,{  "DisconnectTimeout",                 &discotimeout,     nullptr,    10, false}
		,{  "MinUpdateInterval",                 &updinterval,      nullptr,    10, false}
		,{  "ForwardBtsSoap",                    &forwardsoap,      nullptr,    10, false}
		,{  "KeepExtraVersions",                 &keepnver,         nullptr,    10, false}
		,{  "UseWrap",                           &usewrap,          nullptr,    10, false}
		,{  "FreshIndexMaxAge",                  &maxtempdelay,     nullptr,    10, false}
		,{  "RedirMax",                          &redirmax,         nullptr,    10, false}
		,{  "VfileUseRangeOps",                  &vrangeops,        nullptr,    10, false}
		,{  "ResponseFreezeDetectTime",          &stucksecs,        nullptr,    10, false}
		,{  "ReuseConnections",                  &persistoutgoing,  nullptr,    10, false}
		,{  "PipelineDepth",                     &pipelinelen,      nullptr,    10, false}
		,{  "ExSuppressAdminNotification",       &exsupcount,       nullptr,    10, false}
		,{  "OptProxyTimeout",                   &optproxytimeout,  nullptr,    10, false}
		,{  "MaxDlSpeed",                        &maxdlspeed,       nullptr,    10, false}
		,{  "MaxInresponsiveDlSize",             &maxredlsize,      nullptr,    10, false}
		,{  "OptProxyCheckInterval",             &optProxyCheckInt, nullptr,    10, false}
		,{  "TrackFileUse",		             	 &trackfileuse,		nullptr,    10, false}
		,{  "FollowIndexFileRemoval",            &follow404,		nullptr,    10, false}
        ,{  "ReserveSpace",                      &allocspace, 		nullptr ,   10, false}
        ,{  "EvDnsOpts",                	     &dnsopts,	 		nullptr ,   10, false}

        // octal base interpretation of UNIX file permissions
		,{  "DirPerms",                          &dirperms,         nullptr,    8, false}
		,{  "FilePerms",                         &fileperms,        nullptr,    8, false}

		,{ "Verbose", 			nullptr,		"Option is deprecated, ignoring the value." , 10, true}
		,{ "MaxSpareThreadSets",&tpstandbymax, 	"Deprecated option name, mapped to MaxStandbyConThreads", 10, true}
		,{ "OldIndexUpdater",	&oldupdate, 	"Option is deprecated, ignoring the value." , 10, true}
		,{ "Patrace",	&patrace, 				"Don't use in config files!" , 10, false}
		,{ "NoSSLchecks",	&nsafriendly, 		"Disable SSL security checks" , 10, false}
};


tProperty n2pTbl[] =
{
{ "Proxy", [](cmstring&, cmstring& value)
{
	if(value.empty()) proxy_info=tHttpUrl();
	else
	{
		if (!proxy_info.SetHttpUrl(value) || proxy_info.sHost.empty())
		BARF("Invalid proxy specification, aborting...");
	}
	return true;
}, [](bool superUser) -> string
{
	if(!superUser && !proxy_info.sUserPass.empty())
		return string("#");
	return proxy_info.sHost.empty() ? sEmptyString : proxy_info.ToURI(false);
} },
{ "LocalDirs", [](cmstring&, cmstring& value) -> bool
{
	if(g_bNoComplex)
	return true;
	_ParseLocalDirs(value);
	return !localdirs.empty();
}, [](bool) -> string
{
	string ret;
	for(auto kv : localdirs)
	ret += kv.first + " " + kv.second + "; ";
	return ret;
} },
{ "Remap-", [](cmstring& key, cmstring& value) -> bool
{
	if(g_bNoComplex)
	return true;

	string vname=key.substr(6, key.npos);
	if(vname.empty())
	{
		if(!g_bQuiet)
		cerr << "Bad repository name in " << key << endl;
		return false;
	}
	int type(-1); // nothing =-1; prefixes =0 ; backends =1; flags =2
		for(tSplitWalk split(value); split.Next();)
		{
			cmstring s(split);
			if(s.empty())
			continue;
			if(s.at(0)=='#')
			break;
			if(type<0)
			type=0;
			if(s.at(0)==';')
			++type;
			else if(0 == type)
			AddRemapInfo(false, s, vname);
			else if(1 == type)
			AddRemapInfo(true, s, vname);
			else if(2 == type)
			AddRemapFlag(s, vname);
		}
		if(type<0)
		{
			if(!g_bQuiet)
			cerr << "Invalid entry, no configuration: " << key << ": " << value <<endl;
			return false;
		}
		_AddHooksFile(vname);
		return true;
	}, [](bool) -> string
	{
		return "# mixed options";
	} },
{ "AllowUserPorts", [](cmstring&, cmstring& value) -> bool
{
	if(!pUserPorts)
	pUserPorts=new bitset<TCP_PORT_MAX>;
	for(tSplitWalk split(value); split.Next();)
	{
		cmstring s(split);
		const char *start(s.c_str());
		char *p(0);
		unsigned long n=strtoul(start, &p, 10);
		if(n>=TCP_PORT_MAX || !p || '\0' != *p || p == start)
		BARF("Bad port in AllowUserPorts: " << start);
		if(n == 0)
		{
			pUserPorts->set();
			break;
		}
		pUserPorts->set(n, true);
	}
	return true;
}, [](bool) -> string
{
	tSS ret;
	if(pUserPorts)
		{
	for(auto i=0; i<TCP_PORT_MAX; ++i)
	ret << (ret.empty() ? "" : ", ") << i;
		}
	return (string) ret;
} },
{ "ConnectProto", [](cmstring&, cmstring& value) -> bool
{
	int *p = conprotos;
	for (tSplitWalk split(value); split.Next(); ++p)
	{
		cmstring val(split);
		if (val.empty())
		break;

		if (p >= conprotos + _countof(conprotos))
		BARF("Too many protocols specified: " << val);

		if (val == "v6")
		*p = PF_INET6;
		else if (val == "v4")
		*p = PF_INET;
		else
		BARF("IP protocol not supported: " << val);
	}
	return true;
}, [](bool) -> string
{
	string ret(conprotos[0] == PF_INET6 ? "v6" : "v4");
	if(conprotos[0] != conprotos[1])
		ret += string(" ") + (conprotos[1] == PF_INET6 ? "v6" : "v4");
	return ret;
} },
{ "AdminAuth", [](cmstring&, cmstring& value) -> bool
{
	adminauth=value;
	adminauthB64=EncodeBase64Auth(value);
	return true;
}, [](bool) -> string
{
	return "#"; // TOP SECRET";
} }
,
{ "ExStartTradeOff", [](cmstring&, cmstring& value) -> bool
{
	exstarttradeoff = strsizeToOfft(value.c_str());
	return true;
}, [](bool) -> string
{
	return ltos(exstarttradeoff);
} }
	,
	{ "PermitCacheControl", [](cmstring&, cmstring& value) -> bool
	  {
		  ccNoCache = ccNoStore = false;
		  tSplitWalk spltr(value, "," SPACECHARS);
		  for(auto s: spltr)
		  {
			  if (s == "no-cache") ccNoCache = true;
			  else if (s == "no-store") ccNoStore = true;
		  }
		  return true;
	  }, [](bool) -> string
	  {
		  string ret;
		  if (ccNoCache)
		  ret += " no-cache";
		  if (ccNoStore)
		  ret += " no-store";
		  return ret;
	  } }
};

string * GetStringPtr(LPCSTR key) {
	for(auto &ent : n2sTbl)
		if(0==strcasecmp(key, ent.name))
			return ent.ptr;
	return nullptr;
}

int * GetIntPtr(LPCSTR key, int &base) {
	for(auto &ent : n2iTbl)
	{
		if(0==strcasecmp(key, ent.name))
		{
			if(ent.warn)
				cerr << "Warning, " << key << ": " << ent.warn << endl;
			base = ent.base;
			return ent.ptr;
		}
	}
	return nullptr;
}

tProperty* GetPropPtr(cmstring& key)
{
	auto sep = key.find('-');
	auto szkey = key.c_str();
	for (auto &ent : n2pTbl)
	{
		if (0 == strcasecmp(szkey, ent.name))
			return &ent;
		// identified as prefix, with matching length?
		if(sep != stmiss && 0==strncasecmp(szkey, ent.name, sep) && 0 == ent.name[sep+1])
			return &ent;
	}
	return nullptr;
}

int * GetIntPtr(LPCSTR key)
{
	for(auto &ent : n2iTbl)
		if(0==strcasecmp(key, ent.name))
			return ent.ptr;
	return nullptr;
}

inline bool qgrep(cmstring &needle, cmstring &file)
{
	for(cfg::tCfgIter itor(file); itor.Next();)
		if(StrHas(itor.sLine, needle))
			return true;
	return false;
}

bool DegradedMode()
{
	return degraded.load();
}

void DegradedMode(bool setVal)
{
	degraded.store(setVal);
}

void _FixPostPreSlashes(string &val)
{
	// fix broken entries

	if (val.empty() || val.at(val.length()-1) != '/')
		val.append("/");
	if (val.at(0) != '/')
		val.insert(0, "/", 1);
}

bool ReadOneConfFile(const string & szFilename, bool bReadErrorIsFatal=true)
{
	tCfgIter itor(szFilename);
	itor.reader.CheckGoodState(bReadErrorIsFatal, &szFilename);

	NoCaseStringMap dupeCheck;

	while(itor.Next())
	{
#ifdef DEBUG
		cerr << szFilename << " => " << itor.sLine <<endl;
#endif
		// XXX: To something about escaped/quoted version
		tStrPos pos=itor.sLine.find('#');
		if(stmiss != pos)
			itor.sLine.erase(pos);

		if(! SetOption(itor.sLine, &dupeCheck))
			BARF("Error reading main options, terminating.");
	}
	return true;
}


bool ParseOptionLine(const string &sLine, string &key, string &val)
{
	string::size_type posCol = sLine.find(":");
	string::size_type posEq = sLine.find("=");
	if (posEq==stmiss && posCol==stmiss)
	{
		if(!g_bQuiet)
			cerr << "Not a valid configuration directive: " << sLine <<endl;
		return false;
	}
	string::size_type pos;
	if (posEq!=stmiss && posCol!=stmiss)
		pos=min(posEq,posCol);
	else if (posEq!=stmiss)
		pos=posEq;
	else
		pos=posCol;

	key=sLine.substr(0, pos);
	val=sLine.substr(pos+1);
	trimBoth(key);
	trimBoth(val);
	if(key.empty())
		return false; // weird

	if(endsWithSzAr(val, "\\"))
		cerr << "Warning: multilines are not supported, consider using \\n." <<endl;

	return true;
}

tStrDeq ExpandFileTokens(cmstring &token)
{
	string sPath = token.substr(5);
	if (sPath.empty())
		BARF("Bad file spec for repname, file:?");
	bool bAbs = IsAbsolute(sPath);
	if (suppdir.empty() || bAbs)
	{
		if (!bAbs)
			sPath = confdir + sPathSep + sPath;
		return ExpandFilePattern(sPath, true);
	}
	auto pat = confdir + sPathSep + sPath;
	StrSubst(pat, "//", "/");
	auto res = ExpandFilePattern(pat, true);
	if (res.size() == 1 && !Cstat(res.front()))
		res.clear(); // not existing, wildcard returned
	pat = suppdir + sPathSep + sPath;
	StrSubst(pat, "//", "/");
	auto suppres = ExpandFilePattern(pat, true);
	if (suppres.size() == 1 && !Cstat(suppres.front()))
		return res; // errrr... done here
	// merge them
	tStrSet dupeFil;
	for(const auto& s: res)
		dupeFil.emplace(GetBaseName(s));
	for(const auto& s: suppres)
		if(!ContHas(dupeFil, GetBaseName(s)))
			res.emplace_back(s);
	return res;
}

inline void _ParseLocalDirs(cmstring &value)
{
	for(tSplitWalk splitter(value, ";"); splitter.Next(); )
	{
		mstring token=splitter.str();
		trimBoth(token);
		tStrPos pos = token.find_first_of(SPACECHARS);
		if(stmiss == pos)
		{
			cerr << "Cannot map " << token << ", needed format: virtualdir realdir, ignoring it";
			continue;
		}
		string from(token, 0, pos);
		trimBoth(from, "/");
		string what(token, pos);
		trimBoth(what, SPACECHARS "'\"");
		if(what.empty())
		{
			cerr << "Unsupported target of " << from << ": " << what << ", ignoring it" << endl;
			continue;
		}
		localdirs[from]=what;
	}
}


cmstring & GetMimeType(cmstring &path)
{
	{
		lockguard g(mimemap);
		static bool inited = false;
		if (!inited)
		{
			inited = true;
			for (tCfgIter itor("/etc/mime.types"); itor.Next();)
			{
				// # regular types:
				// text/plain             asc txt text pot brf  # plain ascii files

				tSplitWalk split(itor.sLine);
				if (!split.Next())
					continue;

				mstring mimetype = split;
				if (startsWithSz(mimetype, "#"))
					continue;

				while (split.Next())
				{
					mstring suf = split;
					if (startsWithSz(suf, "#"))
						break;
					mimemap[suf] = mimetype;
				}
			}
		}
	}

	tStrPos dpos = path.find_last_of('.');
	if (dpos != stmiss)
	{
        auto it = cfg::mimemap.find(path.substr(dpos + 1));
		if (it != cfg::mimemap.end())
			return it->second;
	}
	// try some educated guess... assume binary if we are sure, text if we are almost sure
	static cmstring os("application/octet-stream"), tp("text/plain");
	filereader f;
	if(f.OpenFile(path, true))
	{
        auto sv = f.getView().substr(0, 255);
        for(char c: sv)
		{
            if(!isascii(unsigned(c)))
				return os;
		}
		return tp;
	}
	return sEmptyString;
}

bool SetOption(const string &sLine, NoCaseStringMap *pDupeCheck)
{
	string key, value;

	if(!ParseOptionLine(sLine, key, value))
		return false;

	string * psTarget;
	int * pnTarget;
	tProperty * ppTarget;
	int nNumBase(10);

	if ( nullptr != (psTarget = GetStringPtr(key.c_str())))
	{

		if(pDupeCheck && !g_bQuiet)
		{
			mstring &w = (*pDupeCheck)[key];
			if(w.empty())
				w = value;
			else
				cerr << "WARNING: " << key << " was previously set to " << w << endl;
		}

		*psTarget=value;
	}
	else if ( nullptr != (pnTarget = GetIntPtr(key.c_str(), nNumBase)))
	{

		if(pDupeCheck && !g_bQuiet)
		{
			mstring &w = (*pDupeCheck)[key];
			if(w.empty())
				w = value;
			else
				cerr << "WARNING: " << key << " was already set to " << w << endl;
		}

		const char *pStart=value.c_str();
		if(! *pStart)
		{
			cerr << "Missing value for " << key << " option!" <<endl;
			return false;
		}
		
		errno=0;
		char *pEnd(nullptr);
		long nVal = strtol(pStart, &pEnd, nNumBase);

		if(RESERVED_DEFVAL == nVal)
		{
			cerr << "Bad value for " << key << " (protected value, use another one)" <<endl;
			return false;
		}

		*pnTarget=nVal;

		if (errno)
		{
			cerr << "Invalid number for " << key << " ";
			perror("option");
			return false;
		}
		if(*pEnd)
		{
			cerr << "Bad value for " << key << " option or found trailing garbage: " << pEnd <<endl;
			return false;
		}
	}
	else if ( nullptr != (ppTarget = GetPropPtr(key)))
	{
		return ppTarget->set(key, value);
	}
	else
	{
		if(!g_bQuiet)
			cerr << "Warning, unknown configuration directive: " << key <<endl;
		return false;
	}
	return true;
}


void ReadConfigDirectory(const char *szPath, bool bReadErrorIsFatal)
{
	dump_proc_status();
	char buf[PATH_MAX];
	if(!realpath(szPath, buf))
		BARF("Failed to open config directory");

	confdir=buf; // pickup the last config directory

#if defined(HAVE_WORDEXP) || defined(HAVE_GLOB)
	for(const auto& src: ExpandFilePattern(confdir+SZPATHSEP "*.conf", true))
		ReadOneConfFile(src, bReadErrorIsFatal);
#else
	ReadOneConfFile(confdir+SZPATHSEP"acng.conf", bReadErrorIsFatal);
#endif
}

void PostProcConfig()
{
	remotedb::GetInstance().PostConfig();

	if(!port) // heh?
		port=ACNG_DEF_PORT;

	if(connectPermPattern == "~~~")
	   connectPermPattern="^(bugs\\.debian\\.org|changelogs\\.ubuntu\\.com):443$";

	// let's also apply the umask to the directory permissions
	{
		mode_t mask = umask(0);
		umask(mask); // restore it...
		dirperms &= ~mask;
		fileperms &= ~mask;
	}

    // postprocessing

#ifdef FORCE_CUSTOM_UMASK
	if(!sUmask.empty())
	{
		mode_t nUmask=0;
		if(sUmask.size()>4)
			BARF("Invalid umask length\n");
		for(unsigned int i=0; i<sUmask.size(); i++)
		{
			unsigned int val = sUmask[sUmask.size()-i-1]-'0';
			if(val>7)
				BARF("Invalid umask value\n");
			nUmask |= (val<<(3*i));
		
		}
		//cerr << "Got umask: " << nUmask <<endl;
		umask(nUmask);
	}
#endif
	
   if(cachedir.empty() || cachedir[0] != CPATHSEP)
   {
	   if (!g_bQuiet)
		   cerr << "Warning: Cache directory unknown or not absolute, running in degraded mode!" << endl;
	   degraded=true;
   }
   if(!rex::CompileExpressions())
	   BARF("An error occurred while compiling file type regular expression!");
   
   if(cfg::tpthreadmax < 0)
	   cfg::tpthreadmax = MAX_VAL(int);
	   
   // get rid of duplicated and trailing slash(es)
	for(tStrPos pos; stmiss != (pos = cachedir.find(SZPATHSEP SZPATHSEP )); )
		cachedir.erase(pos, 1);

	cacheDirSlash=cachedir+CPATHSEP;

   if(!pidfile.empty() && pidfile.at(0) != CPATHSEP)
	   BARF("Pid file path must be absolute, terminating...");
   
   if(!cfg::agentname.empty())
	   cfg::agentheader=string("User-Agent: ")+cfg::agentname + "\r\n";

   stripPrefixChars(cfg::reportpage, '/');

   // user-owned header can contain escaped special characters, fixing them
   trimBoth(cfg::requestapx);
   if(!cfg::requestapx.empty())
   {
	   cfg::requestapx = unEscape(cfg::requestapx);
	   // and adding the final newline suitable for header!
	   trimBoth(cfg::requestapx);
	   if(!cfg::requestapx.empty())
		   cfg::requestapx += svRN;
	   // just make sure to not contain broken headers
	   if (cfg::requestapx.find(':') == stmiss)
		   cfg::requestapx.clear();
   }
   // create working paths before something else fails somewhere
   if(!udspath.empty())
	   mkbasedir(cfg::udspath);
   if(!cachedir.empty())
	   mkbasedir(cfg::cachedir);
   if(! pidfile.empty())
	   mkbasedir(cfg::pidfile);

   if(nettimeout < 5) {
	   cerr << "Warning: NetworkTimeout value too small, using: 5." << endl;
	   nettimeout = 5;
   }
   if(fasttimeout < 2)
   {
	   fasttimeout = 2;
   }

   if(RESERVED_DEFVAL == stucksecs)
	   stucksecs = 2 * (maxtempdelay + 3);

   if(RESERVED_DEFVAL == forwardsoap)
	   forwardsoap = !forcemanaged;

   if(RESERVED_DEFVAL == exsupcount)
	   exsupcount = (extreshhold >= 5);

#ifdef _SC_NPROCESSORS_ONLN
	numcores = (int) sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROC_ONLN)
	numcores = (int) sysconf(_SC_NPROC_ONLN);
#endif

   if(!rex::CompileUncExpressions(rex::NOCACHE_REQ,
		   tmpDontcacheReq.empty() ? tmpDontcache : tmpDontcacheReq)
   || !rex::CompileUncExpressions(rex::NOCACHE_TGT,
		   tmpDontcacheTgt.empty() ? tmpDontcache : tmpDontcacheTgt))
   {
	   BARF("An error occurred while compiling regular expression for non-cached paths!");
   }
   tmpDontcache.clear();
   tmpDontcacheTgt.clear();
   tmpDontcacheReq.clear();

   if(usewrap == RESERVED_DEFVAL)
   {
	   usewrap=(qgrep("apt-cacher-ng", "/etc/hosts.deny")
			   || qgrep("apt-cacher-ng", "/etc/hosts.allow"));
#ifndef HAVE_LIBWRAP
	   cerr << "Warning: configured to use libwrap filters but feature is not built-in." <<endl;
#endif
   }

   if(maxtempdelay<0)
   {
	   cerr << "Invalid maxtempdelay value, using default" <<endl;
	   maxtempdelay=27;
   }

   if(redirmax == RESERVED_DEFVAL)
	   redirmax = forcemanaged ? 0 : REDIRMAX_DEFAULT;

   if(!persistoutgoing)
	   pipelinelen = 1;

   if(!pipelinelen) {
	   cerr << "Warning, remote pipeline depth of 0 makes no sense, assuming 1." << endl;
	   pipelinelen = 1;
   }

	dump_proc_status();

	networkTimeout.tv_sec = nettimeout;

	initialConTimeout.tv_sec = fasttimeout;

	// find something sane
	furtherConTimeout.tv_sec = discotimeout / 8;
	if (furtherConTimeout.tv_sec >= fasttimeout - 1)
		furtherConTimeout.tv_sec = fasttimeout - 1;
	if (furtherConTimeout.tv_sec < 2)
		furtherConTimeout.tv_sec = 2;
} // PostProcConfig

void dump_config(bool includeDelicate)
{
	ostream &cmine(cout);

	for (auto& n2s : n2sTbl)
	{
		if (n2s.ptr)
			cmine << n2s.name << " = " << *n2s.ptr << endl;
	}

	if (cfg::debug >= log::LOG_DEBUG)
	{
		cerr << "escaped version:" << endl;
		for (const auto& n2s : n2sTbl)
		{
			if (!n2s.ptr)
				continue;

			cerr << n2s.name << " = ";
			for (const char *p = n2s.ptr->c_str(); *p; p++)
			{
				if ('\\' == *p)
					cmine << "\\\\";
				else
					cmine << *p;
			}
			cmine << endl;
		}
	}

	for (const auto& n2i : n2iTbl)
	{
		if (n2i.ptr && !n2i.hidden)
			cmine << n2i.name << " = " << *n2i.ptr << endl;
	}

	for (const auto& x : n2pTbl)
	{
		auto val(x.get(includeDelicate));
		if(startsWithSz(val, "#")) continue;
		cmine << x.name << " = " << val << endl;
	}

#ifndef DEBUG
	if (cfg::debug >= log::LOG_DEBUG)
		cerr << "\n\nAdditional debugging information not compiled in.\n\n";
#endif

#if 0 //def DEBUG
#warning adding hook control pins
	for(tMapString2Hostivec::iterator it = repoparms.begin();
			it!=repoparms.end(); ++it)
	{
		tHookHandler *p = new tHookHandler(it->first);
		p->downDuration=10;
		p->cmdCon = "logger wanna/connect";
		p->cmdRel = "logger wanna/disconnect";
		it->second.m_pHooks = p;
	}

	if(debug == -42)
	{
		/*
		 for(tMapString2Hostivec::const_iterator it=mapRepName2Backends.begin();
		 it!=mapRepName2Backends.end(); it++)
		 {
		 for(tRepoData::const_iterator jit=it->second.begin();
		 jit != it->second.end(); jit++)
		 {
		 cout << jit->ToURI() <<endl;
		 }
		 }

		 for(tUrl2RepIter it=mapUrl2pVname.begin(); it!=mapUrl2pVname.end(); it++)
		 {
		 cout << it->first.ToURI(false) << " ___" << *(it->second) << endl;
		 }

		 exit(1);
		 */
	}
#endif
}

acmutex authLock;

int CheckAdminAuth(LPCSTR auth)
{
	if(cfg::adminauthB64.empty())
		return 0;
	if(!auth || !*auth)
		return 1; // request it from user
	if(strncmp(auth, "Basic", 5))
		return -1; // looks like crap
	auto p=auth+5;
	while(*p && isspace((uint) *p)) ++p;

#ifndef SUPPWHASH
	return adminauthB64.compare(p) == 0 ? 0 : 1;

#else

	if(!bIsHashedPwd)
		return adminauth.compare(p) == 0 ? 0 : 1;

#ifndef HAVE_SSL
#warning You really want to add SSL support in order to support hashed passwords
	return -1;
#endif
	acbuf bufDecoded;
	if(!DecodeBase64(p, bufDecoded))
		return -1;
#if 0
	// there is always a char reserved
	cerr << "huhu, user sent: " << bufDecoded.c_str() <<endl;
#endif

	string usersauth(bufDecoded.rptr(), bufDecoded.size());
	auto poscol=usersauth.find(':');
	if(poscol==0 || poscol==stmiss || poscol+1==usersauth.size())
		return 1;

	// ok, try to match against our hash, first copy user and salt from config
	// always calculate the thing and compare the user and maybe hash later
	// attacker should not gain any knowledge from faster abort (side channel...)
	lockguard g(&authLock);
	string testHash=adminauth.substr(0, poscol+9);
	if(!AppendPasswordHash(testHash, usersauth.data()+poscol+1, usersauth.size()-poscol+1))
		return 1;
	if(testHash == adminauth)
	{
		// great! Cache it!
		adminauth = p;
		bIsHashedPwd = false;
		return 0;
	}
	return 1;
#endif
}

static bool proxy_failstate = false;
acmutex proxy_fail_lock;
ACNG_API const tHttpUrl* GetProxyInfo()
{
	if(proxy_info.sHost.empty())
		return nullptr;

	static time_t last_check=0;

	lockguard g(proxy_fail_lock);
	time_t now = time(nullptr);
	time_t sinceCheck = now - last_check;
	if(sinceCheck > optProxyCheckInt)
	{
		last_check = now;
		if(optProxyCheckCmd.empty())
			proxy_failstate = false;
		else
			proxy_failstate = (bool) system(optProxyCheckCmd.c_str());
	}

	return proxy_failstate ? nullptr : &proxy_info;
}

void MarkProxyFailure()
{
	lockguard g(proxy_fail_lock);
	if(optProxyCheckInt <= 0) // urgs, would never recover
		return;
	proxy_failstate = true;
}

const timeval* GetFirstConTimeout()
{
	return &initialConTimeout;
}
const timeval* GetFurtherConTimeout()
{
	return &furtherConTimeout;
}
const timeval* GetNetworkTimeout()
{
	return &networkTimeout;
}


tCfgIter::tCfgIter(cmstring &fn) : sFilename(fn)
{
	reader.OpenFile(fn, false, 1);
}

bool tCfgIter::Next()
{
	while(reader.GetOneLine(sLine))
	{
		trimFront(sLine);
		if(sLine.empty() || sLine[0] == '#')
			continue;
		return true;
	}
	return false;
}

} // namespace acfg

namespace rex
{
// this has the exact order of the "regular" types in the enum
struct { regex_t *pat=nullptr, *extra=nullptr; } rex[ematchtype_max];
vector<regex_t> vecReqPatters, vecTgtPatterns;

bool ACNG_API CompileExpressions()
{
	auto compat = [](regex_t* &re, LPCSTR ps)
	{
		if(!ps ||! *ps )
		return true;
		re=new regex_t;
		int nErr=regcomp(re, ps, REG_EXTENDED);
		if(!nErr)
		return true;

		char buf[1024];
		regerror(nErr, re, buf, sizeof(buf));
		delete re;
		re=nullptr;
		buf[_countof(buf)-1]=0; // better be safe...
			std::cerr << buf << ": " << ps << std::endl;
			return false;
		};
	using namespace cfg;
	return (compat(rex[FILE_SOLID].pat, pfilepat.c_str())
			&& compat(rex[FILE_VOLATILE].pat, vfilepat.c_str())
			&& compat(rex[FILE_WHITELIST].pat, wfilepat.c_str())
			&& compat(rex[FILE_SOLID].extra, pfilepatEx.c_str())
			&& compat(rex[FILE_VOLATILE].extra, vfilepatEx.c_str())
			&& compat(rex[FILE_WHITELIST].extra, wfilepatEx.c_str())
			&& compat(rex[NASTY_PATH].pat, BADSTUFF_PATTERN)
			&& compat(rex[FILE_SPECIAL_SOLID].pat, spfilepat.c_str())
			&& compat(rex[FILE_SPECIAL_SOLID].extra, spfilepatEx.c_str())
			&& compat(rex[FILE_SPECIAL_VOLATILE].pat, svfilepat.c_str())
			&& compat(rex[FILE_SPECIAL_VOLATILE].extra, svfilepatEx.c_str())
			&& (connectPermPattern == "~~~" ?
			true : compat(rex[PASSTHROUGH].pat, connectPermPattern.c_str())));
}

// match the specified type by internal pattern PLUS the user-added pattern
inline bool MatchType(cmstring &in, eMatchType type)
{
	if(rex[type].pat && !regexec(rex[type].pat, in.c_str(), 0, nullptr, 0))
		return true;
	if(rex[type].extra && !regexec(rex[type].extra, in.c_str(), 0, nullptr, 0))
		return true;
	return false;
}

bool Match(cmstring &in, eMatchType type)
{
	if(MatchType(in, type))
		return true;
	// very special behavior... for convenience
	return (type == FILE_SOLID && MatchType(in, FILE_SPECIAL_SOLID))
		|| (type == FILE_VOLATILE && MatchType(in, FILE_SPECIAL_VOLATILE));
}

ACNG_API eMatchType GetFiletype(const string & in)
{
	if (MatchType(in, FILE_SPECIAL_VOLATILE))
		return FILE_VOLATILE;
	if (MatchType(in, FILE_SPECIAL_SOLID))
		return FILE_SOLID;
	if (MatchType(in, FILE_VOLATILE))
		return FILE_VOLATILE;
	if (MatchType(in, FILE_SOLID))
		return FILE_SOLID;
	return FILE_INVALID;
}

#ifndef MINIBUILD

inline bool CompileUncachedRex(const string & token, NOCACHE_PATTYPE type, bool bRecursiveCall)
{
	auto & patvec = (NOCACHE_TGT == type) ? vecTgtPatterns : vecReqPatters;

	if (0!=token.compare(0, 5, "file:")) // pure pattern
	{
		unsigned pos = patvec.size();
		patvec.resize(pos+1);
		return 0==regcomp(&patvec[pos], token.c_str(), REG_EXTENDED);
	}
	else if(!bRecursiveCall) // don't go further than one level
	{
		tStrDeq srcs = cfg::ExpandFileTokens(token);
		for(const auto& src: srcs)
		{
			cfg::tCfgIter itor(src);
			if (!itor.reader.CheckGoodState(false, &src))
			{
				cerr << "Error opening pattern file: " << src <<endl;
				return false;
			}
			while(itor.Next())
			{
				if(!CompileUncachedRex(itor.sLine, type, true))
					return false;
			}
		}

		return true;
	}

	cerr << token << " is not supported here" <<endl;
	return false;
}


bool CompileUncExpressions(NOCACHE_PATTYPE type, cmstring& pat)
{
	for(tSplitWalk split(pat); split.Next(); )
		if (!CompileUncachedRex(split, type, false))
			return false;
	return true;
}

bool MatchUncacheable(const string & in, NOCACHE_PATTYPE type)
{
	for(const auto& patre: (type == NOCACHE_REQ) ? vecReqPatters : vecTgtPatterns)
		if(!regexec(&patre, in.c_str(), 0, nullptr, 0))
			return true;
	return false;
}

#endif //MINIBUILD


} // namespace rechecks


#ifndef MINIBUILD
LPCSTR ReTest(LPCSTR s)
{
	static LPCSTR names[rex::ematchtype_max] =
	{
				"FILE_SOLID", "FILE_VOLATILE",
				"FILE_WHITELIST",
				"NASTY_PATH", "PASSTHROUGH",
				"FILE_SPECIAL_SOLID"
	};
	auto t = rex::GetFiletype(s);
	if(t<0 || t>=rex::ematchtype_max)
		return "NOMATCH";
	return names[t];
}
#endif

}
