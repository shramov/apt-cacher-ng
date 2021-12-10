#include "debug.h"
#include "acbuf.h"
#include "aclogger.h"
#include "config.h"
#include "meta.h"
#include "acfg.h"
#include "fileio.h"
#include "conserver.h"
#include "acregistry.h"
#include "acstartstop.h"
#include "filereader.h"
#include "csmapping.h"
#include "tpool.h"
#include "ac3rdparty.h"
#include "tpool.h"
#include "conn.h"
#include "acres.h"
#include "rex.h"

#ifdef DEBUG
#include <regex.h>
#endif

#include <iostream>

#include <cstdbool>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>

using namespace std;

namespace acng
{

static void usage(int nRetCode=0);
static void SetupCacheDir();
void term_handler(evutil_socket_t fd, short what, void *arg);
void log_handler(evutil_socket_t fd, short what, void *arg);
void dump_handler(evutil_socket_t fd, short what, void *arg);
void noop_handler(evutil_socket_t fd, short what, void *arg);
void handle_sigbus();
void check_algos();

void ac3rdparty_init();
void ac3rdparty_deinit();


//extern mstring sReplDir;

typedef struct sigaction tSigAct;

#ifdef HAVE_DAEMON
inline bool fork_away()
{
	return !daemon(0,0);
}
#else
inline bool fork_away()
{
	chdir("/");
	int dummy=open("/dev/null", O_RDWR);
	if(0<=dup2(dummy, fileno(stdin))
			&& 0<=dup2(dummy, fileno(stdout))
			&& 0<=dup2(dummy, fileno(stderr)))
	{
		switch(fork())
		{
			case 0: // this is child, good
				return true;
			case -1: // bad...
				return false;
			default: // in parent -> cleanup
				setsid();
				_exit(0);
		}
	}
	return false;
}
#endif

void parse_options(int argc, const char **argv)
{
	bool bExtraVerb=false;
	LPCSTR szCfgDir=nullptr;
	std::vector<LPCSTR> cmdvars;
	bool ignoreCfgErrors = false;

	for (auto p=argv+1; p<argv+argc; p++)
	{
		if (!strncmp(*p, "--", 2))
			break;
		if (!strncmp(*p, "-h", 2))
			usage();
		if (!strncmp(*p, "-i", 2))
			ignoreCfgErrors = true;
		else if (!strncmp(*p, "-v", 2))
			bExtraVerb = true;
		else if (!strcmp(*p, "-c"))
		{
			++p;
			if (p < argv + argc)
				szCfgDir = *p;
			else
				usage(2);
		}
		else if(**p) // not empty
			cmdvars.emplace_back(*p);
	}

	if(szCfgDir)
		cfg::ReadConfigDirectory(szCfgDir, !ignoreCfgErrors);

	for(auto& keyval : cmdvars)
		if(!cfg::SetOption(keyval, 0))
			usage(EXIT_FAILURE);

	cfg::PostProcConfig();

	if(bExtraVerb)
		cfg::debug |= (log::LOG_DEBUG|log::LOG_MORE);

}

void setup_sighandler()
{
	auto what = EV_SIGNAL|EV_PERSIST;
#define REGSIG(x,y) event_add(::event_new(evabase::base, x, what, & y, 0), nullptr);
	for(int snum : {SIGBUS, SIGTERM, SIGINT, SIGQUIT}) REGSIG(snum, term_handler);
	REGSIG(SIGUSR1, log_handler);
#warning restore dumper?
//	REGSIG(SIGUSR2, dump_handler);
	REGSIG(SIGPIPE, noop_handler);
#ifdef SIGIO
	REGSIG(SIGIO, noop_handler);
#endif
#ifdef SIGXFSZ
	REGSIG(SIGXFSZ, noop_handler);
#endif
}

static void usage(int retCode) {
	cout <<"Usage: apt-cacher-ng [options] [ -c configdir ] <var=value ...>\n\n"
		"Options:\n"
		"-h: this help message\n"
		"-c: configuration directory\n"
		"-i: ignore configuration loading errors\n"
		"-v: extra verbosity in logging\n"
#if SUPPWHASH
		"-H: read a password from STDIN and print its hash\n"
#endif
		"\n"
		"Most interesting variables:\n"
		"ForeGround: Don't detach (default: 0)\n"
		"Port: TCP port number (default: 3142)\n"
		"CacheDir: /directory/for/storage\n"
		"LogDir: /directory/for/logfiles\n"
		"\n"
		"See configuration examples for all directives or run:\n"
		"acngtool cfgdump\n\n";
	exit(retCode);
}


static void SetupCacheDir()
{
	using namespace cfg;

	if(cfg::cachedir.empty())
		return;	// warning was printed

	auto xstore(cacheDirSlash + cfg::privStoreRelSnapSufix);
	mkdirhier(xstore);
	if(!Cstat(xstore))
	{
		cerr << "Error: Cannot create any directory in " << cacheDirSlash << endl;
		exit(EXIT_FAILURE);
	}
	mkdirhier(cacheDirSlash + cfg::privStoreRelQstatsSfx + "/i");
	mkdirhier(cacheDirSlash + cfg::privStoreRelQstatsSfx + "/o");
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	tSS buf;
	buf << cacheDirSlash << "testfile." << tv.tv_usec * tv.tv_sec * (LPCSTR(buf.wptr()) - LPCSTR(&tv));
	mkbasedir(buf.c_str()); // try or force its directory creation
	int t=open( buf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 00644);
	if (t != -1)
	{
		checkforceclose(t);
		if(0==unlink(buf.c_str()))
			return;
	}
	cerr << "Failed to create cache directory or directory not writable." << endl
		<< "Check the permissions of " << cachedir << "!" << endl;

	exit(1);
}

void log_handler(evutil_socket_t, short, void*)
{
	log::close(true);
}

void noop_handler(evutil_socket_t, short, void*)
{
	//XXX: report weird signals?
}


void term_handler(evutil_socket_t signum, short, void*)
{
	DBGQLOG("caught signal " << signum);
	switch (signum) {
	case (SIGBUS):
		/* OH NO!
		 * Something going wrong with the mmaped files.
		 * Log the current state reliably.
		 * As long as there is no good recovery mechanism,
		 * just hope that systemd will restart the daemon.
		 */
		handle_sigbus();
		log::flush();
		__just_fall_through;
	case (SIGTERM):
	case (SIGINT):
	case (SIGQUIT):
	{
		evabase::in_shutdown.store(true);
		evabase::SignalStop();
		break;
	}
	default:
		return;
	}
}

void CloseAllCachedConnections();

std::unique_ptr<acres> sharedResources;

void daemon_init()
{
	auto lerr = log::open();
	if (!lerr.empty())
	{
		cerr
				<< "Problem creating log files in "
				<< cfg::logdir
				<< ". " << lerr << ".\n";

		exit(EXIT_FAILURE);
	}

	check_algos();

	setup_sighandler();

	SetupCacheDir();

	//DelTree(cfg::cacheDirSlash + sReplDir);
	SetupServerItemRegistry();
	sharedResources.reset(acres::Create());

	if (sharedResources->GetMatchers().HasErrors())
	{
		cerr << "An error occurred while compiling file type regular expression!" << endl;
		exit(EXIT_FAILURE);
	}


	auto nSockets = conserver::Setup([](unique_fd&& fd, std::string name) { StartServing(move(fd), name, *sharedResources); });

	if (nSockets <= 0)
	{
		cerr
				<< "No listening socket(s) could be created/prepared. "
				   "Check the network, check or unset the BindAddress directive.\n";
		exit(EXIT_FAILURE);
	}

	g_tpool = tpool::Create(300, 30);

	if (!cfg::foreground && !fork_away())
	{
		tErrnoFmter ef("Failed to change to daemon mode");
		cerr << ef << endl;
		exit(43);
	}

	if (!cfg::pidfile.empty())
	{
		mkbasedir(cfg::pidfile);
		FILE *PID_FILE = fopen(cfg::pidfile.c_str(), "w");
		if (PID_FILE != nullptr)
		{
			fprintf(PID_FILE, "%d", getpid());
			checkForceFclose(PID_FILE);
		}
	}
}

void daemon_deinit()
{
	evabase::in_shutdown = true;
	if (!cfg::pidfile.empty())
		unlink(cfg::pidfile.c_str());
	g_tpool->stop();
	conserver::Shutdown();
	sharedResources.reset();
	//		CloseAllCachedConnections();
#warning bring all users of itemregistry down!
	TeardownServerItemRegistry();
	log::close(false);
}

}

int main(int argc, const char **argv)
{
	using namespace acng;
	tStartStop g;

	ac3rdparty_init();
	g.atexit([](){ ac3rdparty_deinit(); });

	evabase dabase;
	parse_options(argc, argv);

	daemon_init();
	g.atexit([](){ daemon_deinit(); });

	return dabase.MainLoop();
}
