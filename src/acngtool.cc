#include "config.h"
#include "meta.h"
#include "acfg.h"
#include "aclogger.h"
#include "acbuf.h"
#include "aclogger.h"
#include "dirwalk.h"
#include "debug.h"
#include "dlcon.h"
#include "fileio.h"
#include "acregistry.h"
#include "sockio.h"
#include "bgtask.h"
#include "filereader.h"
#include "csmapping.h"
#include "aevutil.h"
#include "ebrunner.h"
#include "rex.h"
#include "acworm.h"

#include <functional>
#include <thread>
#include <iostream>
#include <fstream>
#include <string>
#include <list>
#include <queue>

#include <cstdbool>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstddef>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <regex.h>
#include <errno.h>

using namespace std;
using namespace acng;

using tCsDeq = deque<LPCSTR>;

#define SUICIDE_TIMEOUT 600
#define CURL_ERROR 1<<5

namespace acng
{
namespace log
{
	extern mstring g_szLogPrefix;
}
}

// from sockio.cc in more recent versions
bool isUdsAccessible(cmstring& path)
{
	Cstat s(path);
	return s && S_ISSOCK(s.info().st_mode) && 0 == access(path.c_str(), W_OK);
}

class tPrintItem : public fileitem
{
public:
	tPrintItem() : fileitem("<STREAM>")
	{
	}
	virtual FiStatus Setup() override
	{
		return m_status = FIST_INITED;
	}
	std::unique_ptr<ICacheDataSender> GetCacheSender() override
	{
		return std::unique_ptr<ICacheDataSender>();
	}
protected:
	ssize_t DlConsumeData(evbuffer *eb, size_t maxTake) override
	{
		return eb_dump_chunks(eb, fileno(stdout), maxTake);
	}
};

unique_ptr<acng::tMinComStack> g_comStack;
void delComStack() { g_comStack.reset(); }
acng::tMinComStack& GetComStack()
{
	if (!g_comStack)
	{
		g_comStack.reset(new acng::tMinComStack);
		atexit(delComStack);
	}
	return *g_comStack;
}

/**
 * Create a special processor which looks for error markers in the download stream and
 * reports the result afterwards.
 */

class tRepItem: public fileitem
{
	mstring m_buf;
	string m_key = maark;
	tStrDeq m_warningCollector;

public:
	tRepItem() : fileitem("<STREAM>")
	{
		m_nSizeChecked = m_nSizeCachedInitial = 0;
	}
	virtual FiStatus Setup() override
	{
		return m_status = FIST_INITED;
	}
protected:
	/*
	 *
<span class="ERROR">Found errors during processing, aborting as requested.</span>
<!--
41d_a6aeb8-26dfa2Errors found, aborting expiration...
-->
<br><b>End of log output. Please reload to run again.</b>
*/
	ssize_t DlConsumeData(evbuffer *eb, size_t maxTake) override
	{
		// glue with the old prefix if needed, later move remainder back the rest buffer if needed

		auto consumed = eb_dump_chunks(eb, m_buf, maxTake);
		string_view input(m_buf);
		while (true)
		{
			auto pos = input.find(m_key);
			if (pos == stmiss)
			{
				// drop all remaining input except for the unfinished line
				pos = input.rfind('\n');
				if (pos != stmiss)
					input.remove_prefix(pos + 1);
				return consumed;
			}
			input.remove_prefix(pos);
			auto nEnd = input.find('\n');
			// our message but unfinished?
			if (nEnd == stmiss)
				return consumed;
			// decode header and get a plain message view
			auto msgType = ControLineType(input[m_key.size()]);
			string_view msg(input.data() + m_key.size() + 1, nEnd - m_key.size() - 1);
			input.remove_prefix(msg.size());
			if (msg.empty())
				continue;
			if (msgType == ControLineType::BeforeError)
			{
				m_warningCollector.emplace_back(msg);
			}
			else if(msgType == ControLineType::Error)
			{
				for (auto l : m_warningCollector)
					cerr << l << endl;
				cerr << msg << endl;
			}
		}
		return consumed;
	}

	// fileitem interface
public:
	unique_ptr<ICacheDataSender> GetCacheSender() override
	{
		return unique_ptr<ICacheDataSender>();
	}
};

tAction GetLoopBreaker(lint_ptr<fileitem> fi)
{
	return [&]()
	{
		if (fi->GetStatus() < fileitem::FIST_COMPLETE)
			return;
		event_base_loopbreak(evabase::base);
	};
};

bool DownloadItem(tHttpUrl url, lint_ptr<fileitem> fi)
{
	if (!GetComStack().getBase())
		return false;

	auto subscrpt = fi->Subscribe(GetLoopBreaker(fi));
	GetComStack().getDownloader().AddJob(fi, &url, nullptr);
	event_base_loop(evabase::base, EVLOOP_NO_EXIT_ON_EMPTY);
	return fi->GetStatus() == fileitem::FIST_COMPLETE && fi->m_responseStatus.code == 200;
}

int wcat(LPCSTR url, LPCSTR proxy);

auto szHelp = R"(
USAGE: acngtool command parameter... [options]

command := { printvar, cfgdump, retest, patch, curl, encb64, maint, shrink }
parameter := (specific to command, or -h for extra help)
options := (see apt-cacher-ng options)
Standard options :=
-h|--help (before or after command type to get command specific help)
-i (ignore config errors)
)";
auto szHelpShrink = R"(
USAGE: acngtool shrink numberX [-f | -n] [-x] [-v] [variable assignments...]
-f: delete files
-n: dry run, display results
-v: more verbosity
-x: also drop index files (can be dangerous)
Suffix X can be k,K,m,M,g,G (for kb,KiB,mb,MiB,gb,GiB)
)";
auto szHelpMaint = R"(
USAGE: acngtool maint [-f]  [variable assignments...]
-f: force execution even if trade-off threshold not met
)";

auto szHelpNA = "Sorry, no extra help information for this command.\n";

int fin(int retCode, string_view what)
{
	auto& chan = (retCode ? cerr : cout);
	chan << what;
	if (what.back() > '\r')
		chan << endl;
	else
		chan.flush();
	exit(retCode);
	return EXIT_FAILURE;
}

struct pkgEntry
{
	std::string path;
	time_t lastDate;
	blkcnt_t blocks;
	// for prio.queue, oldest shall be on top
	bool operator<(const pkgEntry &other) const
	{
		return lastDate > other.lastDate;
	}
};

int shrink(off_t wantedSize, bool dryrun, bool apply, bool verbose, bool incIfiles)
{
	if (!dryrun && !apply)
	{
		cerr << "Error: needs -f or -n options" << endl;
		return 97;
	}
	if (dryrun && apply)
	{
		cerr << "Error: -f and -n are mutually exclusive" <<endl;
		return 107;
	}
//	cout << "wanted: " << wantedSize << endl;
	std::priority_queue<pkgEntry/*, vector<pkgEntry>, cmpLessDate */ > delQ;
	std::unordered_map<string, pair<time_t,off_t> > related;

	blkcnt_t totalBlocks = 0;
	rex matcher;

	IFileHandler::FindFiles(cfg::cachedir,
			[&](cmstring & path, const struct stat& finfo) -> bool
			{
		// reference date used in the prioqueue heap
		auto dateLatest = max(finfo.st_ctim.tv_sec, finfo.st_mtim.tv_sec);
		auto isHead = endsWithSzAr(path, ".head");
		string pkgPath, otherName;
		if(isHead)
		{
			pkgPath = path.substr(0, path.length()-5);
			otherName = pkgPath;
		}
		else
		{
			pkgPath = path;
			otherName = path + ".head";
		}
		auto ftype = matcher.GetFiletype(pkgPath);
		if((ftype==rex::FILE_SPECIAL_VOLATILE || ftype == rex::FILE_VOLATILE) && !incIfiles)
			return true;
		// anything else is considered junk

		auto other = related.find(otherName);
		if (other == related.end())
		{
			// the related file will appear soon
			related.insert(make_pair(path, make_pair(dateLatest, finfo.st_blocks)));
			return true;
		}
		// care only about stamps on .head files (track mode)
		// or ONLY about data file's timestamp (not-track mode)
		if( (cfg::trackfileuse && !isHead) || (!cfg::trackfileuse && isHead))
			dateLatest = other->second.first;

		auto bothBlocks = (finfo.st_blocks + other->second.second);
		related.erase(other);

		totalBlocks += bothBlocks;
		delQ.push({pkgPath, dateLatest, bothBlocks});

		return true;
			}
	, true, false);

	// there might be some unmatched remains...
	for(auto kv: related)
		delQ.push({kv.first, kv.second.first, kv.second.second});
	related.clear();

	auto foundSizeString = offttosHdotted(totalBlocks*512);
	blkcnt_t wantedBlocks = wantedSize / 512;

	if(totalBlocks < wantedBlocks)
	{
		if(verbose)
			cout << "Requested size smaller than current size, nothing to do." << endl;
		return 0;
	}

	if(verbose)
	{
		cout << "Found " << foundSizeString << " bytes of relevant data, reducing to "
		<< offttosHdotted(wantedSize) << " (~"<< (wantedBlocks*100/totalBlocks) << "%)"
		<< endl;
	}
	while(!delQ.empty())
	{
		bool todel = (totalBlocks > wantedBlocks);
		if(todel)
			totalBlocks -= delQ.top().blocks;
		const char *msg = 0;
		if(verbose || dryrun)
			msg = (todel ? "Delete: " : "Keep: " );
		auto& delpath(delQ.top().path);
		if(msg)
			cout << msg << delpath << endl << msg << delpath << ".head" << endl;
		if(todel && apply)
		{
			unlink(delpath.c_str());
			unlink(mstring(delpath + ".head").c_str());
		}
		delQ.pop();
	}
	if(verbose)
	{
		cout << "New size: " << offttosHdotted(totalBlocks*512) << " (before: "
		<< foundSizeString << ")" << endl;
	}
	return 0;
}

int do_shrink(tCsDeq& parms)
{
	bool dryrun(false), apply(false), verbose(false), incIfiles(false);
	off_t wantedSize(4000000000);

	for(auto p: parms)
	{
		if(*p > '0' && *p<='9')
			wantedSize = strsizeToOfft(p);
		else if(*p == '-')
		{
			for(++p;*p;++p)
			{
				apply |= (*p == 'f');
				dryrun |= (*p == 'n');
				verbose |= (*p == 'v');
				incIfiles |= (*p == 'x');
			}
		}
	}
	return shrink(wantedSize, dryrun, apply, verbose, incIfiles);
}

#if SUPPWHASH

int hashpwd()
{
#ifdef HAVE_SSL
	string plain;
	uint32_t salt=0;
	for(unsigned i=10; i; --i)
	{
		if(RAND_bytes(reinterpret_cast<unsigned char*>(&salt), 4) >0)
			break;
		else
			salt=0;
		sleep(1);
	}
	if(!salt) // ok, whatever...
	{
		uintptr_t pval = reinterpret_cast<uintptr_t>(&plain);
		srandom(uint(time(0)) + uint(pval) +uint(getpid()));
		salt=random();
		timespec ts;
		clock_gettime(CLOCK_BOOTTIME, &ts);
		for(auto c=(ts.tv_nsec+ts.tv_sec)%1024 ; c; c--)
			salt=random();
	}
	string crypass = BytesToHexString(reinterpret_cast<const uint8_t*>(&salt), 4);
#ifdef DEBUG
	plain="moopa";
#else
	cin >> plain;
#endif
	trimString(plain);
	if(!AppendPasswordHash(crypass, plain.data(), plain.size()))
		return EXIT_FAILURE;
	cout << crypass <<endl;
	return EXIT_SUCCESS;
#else
	cerr << "OpenSSL not available, hashing functionality disabled." <<endl;
	return EXIT_FAILURE;
#endif
}


bool AppendPasswordHash(string &stringWithSalt, LPCSTR plainPass, size_t passLen)
{
	if(stringWithSalt.length()<8)
		return false;

	uint8_t sum[20];
	if(1!=PKCS5_PBKDF2_HMAC_SHA1(plainPass, passLen,
			(unsigned char*) (stringWithSalt.data()+stringWithSalt.size()-8), 8,
			NUM_PBKDF2_ITERATIONS,
			sizeof(sum), (unsigned char*) sum))
		return false;
	stringWithSalt+=EncodeBase64((LPCSTR)sum, 20);
	stringWithSalt+="00";
#warning dbg
	// checksum byte
	uint8_t pCs=0;
	for(char c : stringWithSalt)
		pCs+=c;
	stringWithSalt+=BytesToHexString(&pCs, 1);
	return true;
}
#endif


int do_maint_job(tCsDeq& parms)
{
	if (cfg::reportpage.empty())
	{
		cerr << "ReportPage is not configured in the server config, aborting..." <<endl;
		return -1;
	}

	// base target URL, can be adapted for TCP requests
	tHttpUrl url;
	url.sUserPass = cfg::adminauth;
	LPCSTR req = getenv("ACNGREQ");
	url.sPath = Concat("/", cfg::reportpage, req ? req : "?doExpire=Start+Expiration&abortOnErrors=aOe");
	if (any_of(parms.begin(), parms.end(), [](LPCSTR s) { return 0 == strcmp("-f", s); }))
		url.sPath += "&force=1";

	auto isInsecForced = []() { auto se = getenv("ACNG_INSECURE"); return se && *se; };

	// by default, use the socket connection; if credentials require it -> enforce it
	bool have_cred = !url.sUserPass.empty(),
			have_uds = !cfg::adminpath.empty(),
			try_tcp = !have_cred;
	bool uds_ok = have_uds && isUdsAccessible(cfg::adminpath);

	if (have_cred)
	{
		if(isInsecForced()) // so try TCP anyway
		{
			try_tcp = true;
		}
		else if (have_uds && !uds_ok)
		{
			cerr << "This operation transmits credentials but the socket (" << cfg::adminpath
				 << ") is currently not accessible!" << endl;
			return EXIT_FAILURE;
		}
		else if(!have_uds)
		{
			cerr << "This operation transmits credentials but SocketPath is not configured to a safe location in the server configuration. "
					"Please set SocketPath to a safe location, or set ACNG_INSECURE environment variable to override this check."
					<<endl;
			return EXIT_FAILURE;
		}
		// ok, otherwise use Unix Domain Socket
	}

	bool response_ok = false;
	if (have_uds && uds_ok)
	{
		DBGQLOG("Trying UDS path");

		auto fi = make_lptr<tRepItem, fileitem>();
		url.sHost = cfg::adminpath;
		url.m_schema = tHttpUrl::EProtoType::UDS;
		url.SetPort(0);
		response_ok = DownloadItem(url, fi);
		DBGQLOG("UDS result: " << response_ok);
	}
	if (!response_ok && try_tcp)
	{
		DBGQLOG("Trying TCP path")
				// never use a proxy here (insecure?), those are most likely local IPs
				cfg::SetOption("Proxy=", nullptr);
		cfg::nettimeout = 30;
		vector<string> hostips;
		Tokenize(cfg::bindaddr, SPACECHARS, hostips, false);
		if (hostips.empty())
			hostips.emplace_back("127.0.0.1");
		for (const auto &tgt : hostips)
		{
			url.sHost = tgt;
			url.m_schema = tHttpUrl::EProtoType::HTTP;
			url.SetPort(cfg::port);
			auto fi = make_lptr<tRepItem, fileitem>();
			response_ok = DownloadItem(url, fi);
			if (response_ok)
				break;
		}
	}
	if (!response_ok)
	{
		cerr << "Could not make a valid request to the server. Please visit "
				<< url.ToURI(false) << " and check special conditions." <<endl;
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

tSS errorBuffer;
#define EFMT (errorBuffer.clean() << "ERROR: " )

typedef deque<acng::string_view> tStringViewList;

// sticky, needed in to remember last position in subsequent operations
unsigned long rangeLast(0), rangeStart(0);

inline void patchChunk(tStringViewList& data, string_view cmd, tStringViewList patch)
{
	if (AC_UNLIKELY(cmd.empty() || data.empty()))
		fin(21, EFMT << "Bad patch command");

	const char& code = cmd.back();
	bool append = false; // special, paste contents AFTER the line

	switch (code)
	{
	case 'a':
		append = true;
		__just_fall_through;
	case 'd':
	case 'c':
	{
		char *pEnd = nullptr;
		auto n = strtoul(cmd.data(), &pEnd, 10);
		if (!pEnd || cmd.data() == pEnd)
			fin(21, EFMT << "bad patch range");

		rangeLast = rangeStart = n;

		if (rangeStart > data.size())
			fin(21, EFMT << "bad range, start: " << rangeStart);

		if (*pEnd == ',')
		{
			n = strtoul(pEnd + 1, &pEnd, 10);
			// command code should follow after!
			if (!pEnd || & code != pEnd)
				fin(21, EFMT << "bad patch range");
			rangeLast = n;
			if (rangeLast > data.size() || rangeLast < rangeStart)
				fin(21, EFMT << "bad range, end: " << rangeLast);
		}
		break;
	}
	case '/':
		if (cmd != "s/.//"sv)
			fin(21, EFMT << "unsupported command: " << cmd);
		data[rangeStart] = ".\n"sv;
		return;
	default:
		fin(21, EFMT << "Unsupported command: " << cmd);
		break;
	}

#define DIT(offs) (data.begin() + offs)
	if (append)
		data.insert(DIT(rangeStart + 1), patch.begin(), patch.end());
	else
	{
		// non-moving pasting first
		size_t offset(0), pcount(patch.size());
		while(offset < pcount && rangeStart <= rangeLast)
			*(DIT(rangeStart++)) = patch[offset++];
		if (offset >= pcount && rangeStart > rangeLast)
			return;
		// otherwise extra stuff in new or old range
		if (offset < pcount)
			data.insert(DIT(rangeStart), patch.begin() + offset, patch.end());
		else
			data.erase(DIT(rangeStart), DIT(rangeLast + 1));
	}
}

/**
 * @brief patch_file
 * @param args Source path (can be compressed??), patch file (must be not uncompressed)
 * @return
 */
int patch_file(tCsDeq& args)
{
	filereader frBase, frPatch;
	if(!frBase.OpenFile(args.front()) || !frPatch.OpenFile(args[1], true))
		return -2;
	tStringViewList linesOrig, curHunkLines;

	linesOrig.emplace_back(se); // dummy entry to avoid -1 calculations because of ed numbering style

	tSplitWalkStrict origSplit(frBase.getView(), "\n");
	for(auto view : origSplit) // collect, including newline!
		linesOrig.emplace_back(view.data(), view.size() + 1);
	// if file was ended properly, drop the empty extra line, it contains an inaccessible range anyway
	if (frBase.getView().ends_with('\n'))
		linesOrig.pop_back();

	string_view curPatchCmd;
	tSplitWalkStrict patchSplit(frPatch.getView(), "\n");
	auto execute = [&]()
	{
		patchChunk(linesOrig, curPatchCmd, curHunkLines);
		curHunkLines.clear();
		curPatchCmd = se;
	};
	for(auto line : patchSplit)
	{
		if (!curPatchCmd.empty()) // collecting mode
		{
			if (line == "."sv)
				execute();
			else
				curHunkLines.emplace_back(line.data(), line.length() + 1); // with \n
			continue;
		}
		// okay, command, which kind?
		curPatchCmd = line;

		if (line.starts_with("w"sv)) // don't care about the name, though
			break;

		// single-line commands?
		if (line.ends_with('d') || line.starts_with('s'))
			execute();
	}

	ofstream res(args.back());
	if(!res.is_open())
		return -3;
	linesOrig.pop_front(); // dummy offset line
	for(const auto& kv : linesOrig)
		res.write(kv.data(), kv.size());
	res.flush();
	return res.good() ? 0 : -4;
}

int main(int argc, const char **argv)
{
	using namespace acng;

	cfg::g_bNoComplex = true; // no DB for just single variables
	cfg::minilog = true;	// no fancy timestamps and only STDERR output
	log::g_szLogPrefix = "acngtool";
	GetComStack();			// init the event base

	// maybe preset for the legacy mode
	LPCSTR mode = (string_view(argv[0]).ends_with("expire-caller.pl") ? "maint" : nullptr),
			szCfgDir = nullptr;
	LPCSTR *posMode(nullptr);
	tCsDeq xargs;
	bool wantCfgDir = false, ignoreCfgErrors = false, subHelp = false;

	auto warn_cfgdir = [&]()
	{
		if (!szCfgDir || access(szCfgDir, R_OK|X_OK))
		{
			cerr << "Warning: unknown or inaccessible config directory "
				 << (szCfgDir ? szCfgDir : "\n")
				 << endl;
		}
	};
	const char **argFirst = argv+1, **argEnd = argv+argc;
	// pick and process early/urgent options
	for (auto p = argFirst; p < argEnd; ++p)
	{
		if (wantCfgDir)
		{
			wantCfgDir = false;
			szCfgDir = *p;
		}
		else if (!strcmp(*p, "-c"))
			wantCfgDir = true;
		else if(!strcmp(*p, "-i"))
			ignoreCfgErrors = true;
		else
			continue;
		// blank it, it was consumed
		*p = nullptr;
	}

	if (wantCfgDir)
		fin(2, "-c requires a valid configuration directory");
	if (szCfgDir)
		cfg::ReadConfigDirectory(szCfgDir, !ignoreCfgErrors);

	// apply global options, collect mode name and its options
	cfg::g_bQuiet = true;
	for (auto p = argFirst; p < argEnd; ++p)
	{
		if (!*p || !**p)
			continue;
		if (!strncmp(*p, "-h", 2))
		{
			if (!posMode ||  p < posMode)
				fin(0, szHelp);
			subHelp = true;
		}
		else if (cfg::SetOption(*p, nullptr))
			continue;
		else if (!mode)
		{
			mode = *p;
			posMode = p;
		}
		else
			xargs.emplace_back(*p);
	}
	cfg::g_bQuiet = false;

	if (!mode)
		return fin(1, szHelp);

#define MODE(x) (strcmp(x, mode) == 0)
#define NA 42
#define CHECKARGS(n, m, mode, shelp) if (subHelp) fin(4, shelp); \
	if (m != NA && int(xargs.size()) < n) fin(3, "Insufficient options for command " mode); \
	if (m != NA && xargs.size() > m) fin(3, "Too many options for command " mode);

	if (MODE("maint"))
	{
		CHECKARGS(0, 1, "maint", szHelpMaint);
		warn_cfgdir();
		return do_maint_job(xargs);
	}
	if (MODE("shrink"))
	{
		CHECKARGS(1, NA, "shrink", szHelpShrink);
		return do_shrink(xargs);
	}
	if (MODE("patch"))
	{
		CHECKARGS(3, 3, "patch", "USAGE: ... shrink baseFile patchFile outFile"sv);
		return patch_file(xargs);
	}
	if (MODE("printvar"))
	{
		CHECKARGS(1, 1, "printvar", "USAGE: ... config-variable-name");
		warn_cfgdir();
		auto ps(cfg::GetStringPtr(xargs.front()));
		if(ps)
		{
			cout << *ps << endl;
			return 0;
		}
		else
		{
			auto pi(cfg::GetIntPtr(xargs.front()));
			if(pi)
			{
				cout << *pi << endl;
				return 0;
			}
		}
		return 42;
	}
	if (MODE("retest"))
	{
		CHECKARGS(1, 1, "retest", "USAGE: ... retest regular-expression\nNOTE: mind the shell expansion and escaping rules!"sv);
		warn_cfgdir();
		static auto matcher = make_shared<rex>();
		std::cout << ReTest(xargs.front(), *matcher) << std::endl;
		return 0;
	}
	if (MODE("curl"))
	{
		CHECKARGS(1, UINT_MAX, "curl", "USAGE: ... curl URLs..."sv);
		warn_cfgdir();
		int ret(0);
		for(auto p: xargs)
			ret |= wcat(p, getenv("http_proxy"));
		return ret;
	}
	if (MODE("cfgdump"))
	{
		warn_cfgdir();
		cfg::dump_config(false);
		return 0;
	}
	if (MODE("encb64"))
	{
		CHECKARGS(1, 1, "encb64", "USAGE: ... encb64 random-string"sv);
		std::cout << EncodeBase64Auth(xargs.front());
		return 0;
	}
	cerr << endl << "Unknown command: " << mode << endl;
	return fin(1, szHelp);
}

int wcat(LPCSTR surl, LPCSTR proxy)
{
	cfg::dnscachetime=0;
	cfg::persistoutgoing=0;
	cfg::badredmime.clear();
	cfg::redirmax=10;

	if(proxy)
	{
		if(cfg::SetOption(string("proxy:")+proxy, nullptr))
			return -1;
	}
	tHttpUrl url;
	if(!surl)
		return 2;
	string xurl(surl);
	if(!url.SetHttpUrl(xurl, false))
		return -2;

	auto fi = make_lptr<tPrintItem, fileitem>();
	if (DownloadItem(url, fi))
		return EXIT_SUCCESS;

	// don't reveal passwords
	auto xpos=xurl.find('@');
	if(xpos!=stmiss)
		xurl.erase(0, xpos+1);
	cerr << "Error: cannot fetch " << xurl <<" : "  << fi->m_responseStatus.msg << endl;
	if (fi->m_responseStatus.code >= 500)
		return EIO;
	if (fi->m_responseStatus.code >= 400)
		return EACCES;
	return CURL_ERROR;
}
