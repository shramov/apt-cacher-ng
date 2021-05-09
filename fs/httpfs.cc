//============================================================================
// Name        : acngfs.cpp
// Author      : Eduard Bloch
// Description : Simple FUSE-based filesystem for HTTP access (apt-cacher NG)
//============================================================================

#define FUSE_USE_VERSION 26

#include "debug.h"

#include "meta.h"
#include "header.h"
#include "caddrinfo.h"
#include "sockio.h"
#include "acbuf.h"
#include "acfg.h"
#include "lockable.h"
#include "cleaner.h"
#include "tcpconnect.h"
#include "ebrunner.h"
#include "fileitem.h"
#include "fileio.h"
#include "dlcon.h"
#include "acregistry.h"

#ifdef HAVE_SYS_MOUNT_H
#include <sys/param.h>
#include <sys/mount.h>
#endif
#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include <fuse.h>

#ifdef HAVE_DLOPEN
#include <dlfcn.h>
#endif

#include <list>
#include <unordered_map>

#define HEADSZ 5000
#ifndef MIN
#define MIN(a,b) ( (a<=b)?a:b)
#endif

#include <thread>

#define MAX_DL_STREAMS 8
#define MAX_DL_BUF_SIZE 64*1024

using namespace acng;
using namespace std;

//#define SPAM

#ifdef SPAM
#define _cerr(x) cerr << x
#warning printing spam all around
#else
#define _cerr(x)
#endif

#if !defined(BUFSIZ) || BUFSIZ > 8192
#define BUFSIZ 8192
#endif

#define HEAD_TRAILER_LEN BUFSIZ

// some globals, set only once
static struct stat statTempl;
static struct statfs stfsTemp;
static tHttpUrl baseUrl, proxyUrl;
static mstring altPath;
bool g_bGoodServer=true;

struct tDlStream
{
	TFileItemHolder hodler;
	thread runner;
	SHARED_PTR<dlcon> dler;
	enum eKind
	{
		HEADER, TRAILER, MID
	} kind;

	~tDlStream()
	{
		if(dler)
			dler->SignalStop();

		if(runner.joinable())
			runner.join();
	}

};

std::unordered_multimap<string, SHARED_PTR<tDlStream>> active_agents;
std::mutex mx_streams;
using mg = lock_guard<std::mutex>;


class tConsumingItem : public fileitem
{
	// fileitem interface
public:
	FiStatus Setup() override
	{
		return FIST_INITED;
	}
	tSS buf;
	std::atomic<off_t> bufStart = -1;

protected:
	bool DlAddData(string_view data, lockuniq &) override
	{
		m_status = FIST_DLRECEIVING;
		auto tooMuch = off_t(data.size()) + data.length() - MAX_DL_BUF_SIZE;
		if(tooMuch > 0)
		{
			buf.drop(tooMuch);
			bufStart += tooMuch;
		}
		buf << data;
		return true;
	}
	int fetchHot(char *retbuf, const char *, off_t pos, off_t len)
	{
		if (pos < bufStart)
			return -ECONNABORTED;
		auto inBuf = buf.size();
		// focus on the start and drop useless data
		if (pos >= bufStart + inBuf)
		{
			// outside current range?
			bufStart += inBuf;
			buf.clear();
		}
		else if (pos > bufStart && pos < bufStart + inBuf)
		{
			// part or whole chunk is available
			auto drop = pos - bufStart;
			buf.drop(drop);
			bufStart += drop;
		}
		if ( m_status != FIST_DLRECEIVING && pos+len > bufStart + off_t(buf.size()))
		{
			return -ECONNABORTED;
		}
		return -ECONNABORTED;
	}
};



/** @brief Deliver some agent from the pool which MAY have the data in the pipe which we need.
	 */
SHARED_PTR<tDlStream> getAgent(LPCSTR path, off_t pos)
{
	mg g(mx_streams);
	// if posisble, get one which fits
	auto its = active_agents.equal_range(path);
	auto spare_agent = active_agents.end();

	if (its.first != active_agents.end())
	{
		for (auto it = its.first; it != its.second; ++it)
		{
			auto fi = static_pointer_cast<tConsumingItem>(it->second->hodler.get());
			if (!fi)
				continue;
			if ( pos >= fi->bufStart)
			{
				SHARED_PTR<tDlStream> ret = move(it->second);
				active_agents.erase(it);
				return ret;
			}
			// can we recycle idle resources if we have to create a new one?

			// those are finalized so we can use them and they are most likely to still have a hot connection (FILO, not FIFO, XXX: check efficiency)
			if (it->second->kind != tDlStream::MID)
			{
				spare_agent = it;
			}
			// XXX: if not donor found in this range, check others too? Might cause pointless reconnects
		}
	}
	auto ret = make_shared<tDlStream>();
	if (spare_agent != active_agents.end())
	{
		ret->dler.swap(spare_agent->second->dler);
		ret->runner.swap(spare_agent->second->runner);
	}
	else
	{
		ret->dler = dlcon::CreateRegular();
		ret->runner = std::thread([&]() {ret->dler->WorkLoop();});
	}
	return ret;
}

void returnAgent(LPCSTR path, SHARED_PTR<tDlStream> && agent)
{
	mg g(mx_streams);
	if (active_agents.size() > MAX_DL_STREAMS)
		active_agents.erase(active_agents.begin()); // can purge anyone, should be random enough
	active_agents.insert(make_pair(path, move(agent)));
}

struct tDlDesc
{
	cmstring m_path;
	uint m_ftype;

	virtual int Read(char *retbuf, const char *path, off_t pos, size_t len) =0;
	virtual int Stat(struct stat &stbuf) =0;
	tDlDesc(cmstring &p, uint ftype) : m_path(p), m_ftype(ftype) {};
	virtual ~tDlDesc() {};
};

struct tDlDescLocal : public tDlDesc
{
	FILE *pFile;
	tDlDescLocal(cmstring &path, uint ftype) : tDlDesc(path, ftype), pFile(nullptr)
	{
	};

	int Stat(struct stat &stbuf) override
	{
		if(altPath.empty()) // hm?
			return -ENOENT;

		auto absPath = altPath + m_path;
		if (::stat(absPath.c_str(), &stbuf))
			return -errno;

		// verify the file state
		off_t cl = -2;
		if (!ParseHeadFromStorage(absPath, &cl, nullptr, nullptr) || cl != stbuf.st_size)
			return -EIO;

		return 0;
	}

	virtual ~tDlDescLocal()
	{
		if(pFile)
			fclose(pFile);
		pFile=nullptr;
	};

	int Read(char *retbuf, const char *, off_t pos, size_t len) override
	{
		if (!pFile)
		{
			struct stat stbuf;
			if(Stat(stbuf))
				return -EIO; // file incomplete or missing

			FILE *pf = fopen((altPath + m_path).c_str(), "rb");
			if (!pf)
				return -EIO;
			pFile = pf;
		}

		int copied=0;
		if(pFile && 0==fseeko(pFile, pos, SEEK_SET))
		{
			while(!feof(pFile) && !ferror(pFile) && len>0)
			{
				size_t r = ::fread(retbuf+copied, 1, len, pFile);
				copied+=r;
				len-=r;
			}
		}
		return ferror(pFile) ? -EIO : copied;
	}
};

struct tFileId
{
	off_t m_size = 0; tHttpDate m_ctime; time_t validAt = GetTime();
	tFileId() =default;
	tFileId(off_t a, mstring b) : m_size(a), m_ctime(b) {};
	bool operator!=(tFileId other) const { return m_size != other.m_size || m_ctime != other.m_ctime;}
};
static class tRemoteInfoCache : public base_with_mutex, public map<string, tFileId>
{} remote_info_cache;

struct tDlDescRemote : public tDlDesc
{
protected:

	tFileId fid;
	SHARED_PTR<tDlStream> m_agent;
	tHttpDate m_modDate;
	off_t m_contLen = -1;
	bool m_metaVerified = false;

public:
	tDlDescRemote(cmstring &p, uint n) : tDlDesc(p,n)
	{
		// good moment to expire the caches, every time, should not cost much anyway
		g_tcp_con_factory.BackgroundCleanup();
		lockguard g(remote_info_cache);
		map<string, tFileId>::const_iterator it = remote_info_cache.find(m_path);
		if (it != remote_info_cache.end())
		{
#define META_CACHE_TIME 120
			if (it->second.validAt < GetTime() - META_CACHE_TIME)
			{
				remote_info_cache.erase(it);
				return;
			}

			m_modDate = it->second.m_ctime;
			m_contLen = it->second.m_size;
		}
	};
	int Stat(struct stat &stbuf)
	{
		stbuf = statTempl;
		if (m_contLen >= 0)
		{
			stbuf.st_size = m_contLen;
			stbuf.st_mode &= ~S_IFDIR;
			stbuf.st_mode |= S_IFREG;
			stbuf.st_ctime = m_modDate.value(1);
			_cerr("Using precached\n");
			return 0;
		}
		else if (m_metaVerified) // ok, not existing?
		{
			return -EEXIST;
		}
	}

	int Read(char *retbuf, const char *path, off_t pos, size_t len)
	{

#if 0
		tHttpUrl uri = proxyUrl;
		uri.sPath += baseUrl.sHost
		// + ":" + ( baseUrl.sPort.empty() ? baseUrl.sPort : "80")
				+ baseUrl.sPath + m_path;

		class tFitem: public fileitem
		{
		public:
			char *pRet;
			size_t nRest, nGot;
			off_t skipBytes, m_nRangeLimit;
			int nErr;

#define SETERROR { nErr=__LINE__; return false;}
			bool &m_isFirst;

			ssize_t SendData(int, int, off_t&, size_t) override
			{
				return 0;
			} // nothing to send
			bool DlAddData(string_view chunk) override
			{
				auto count = chunk.size();
				auto p = chunk.data();

				if(skipBytes>0)
				{
					if(skipBytes>count)
					{
						skipBytes-=count;
						return true;
					}
					count-=skipBytes;
					p+=skipBytes;
					skipBytes=0;
				}

				if(!nRest)
				{
					m_status=FIST_COMPLETE;
					return false;
				}
				if(count>nRest)
					count=nRest;
				memcpy(pRet+nGot, p, count);
				nGot+=count;
				nRest-=count;
				return true;
			}

			void DlFinish(bool asInCache = false) override
			{
				notifyAll();
				m_status=FIST_COMPLETE;
			};

			virtual bool DlStarted(header head, string_view, off_t) override
			{
				_cerr(head.frontLine<<endl);
				m_head = head; // XXX: bloat, only status line and contlen required
				int st =head.getStatus();

				if(st == 416)
					return true; // EOF
#warning that all needs to be overhauled, old evil copy-paste code
				if(st != 200 && st != 206)
				{
					SETERROR;
				}

				// validation
				if (head.h[header::LAST_MODIFIED])
				{
					if (m_isFirst)
						fid.m_ctime = head.h[header::LAST_MODIFIED];
					else if (fid.m_ctime != head.h[header::LAST_MODIFIED])
						SETERROR;
				}

				off_t myfrom(0), myto(0), mylen(0);
				const char *p=head.h[header::CONTENT_RANGE];
				if(p)
				{
					int n=sscanf(p, "bytes " OFF_T_FMT "-" OFF_T_FMT "/" OFF_T_FMT, &myfrom, &myto, &mylen);
					if(n<=0)
						n=sscanf(p, "bytes=" OFF_T_FMT "-" OFF_T_FMT "/" OFF_T_FMT, &myfrom, &myto, &mylen);
					if(n!=3  // check for nonsense
							|| (m_nSizeCachedInitial>0 && myfrom != m_nSizeCachedInitial-1)
							|| (m_nRangeLimit>=0 && myto > m_nRangeLimit) // too much data?
							|| myfrom<0 || mylen<0
					)
					{
						SETERROR;
					}

				}
				else if(st == 200 && head.h[header::CONTENT_LENGTH])
					mylen = atoofft(head.h[header::CONTENT_LENGTH]);

				// validation
				if(m_isFirst)
					fid.m_size = mylen;
				else
					if(fid.m_size != mylen)
						SETERROR;

				skipBytes -= myfrom;
				if(skipBytes<0)
					SETERROR;
				return true;
			}
			tFileId &fid;
			tFitem(char *p, size_t size, off_t start, tFileId &fi, bool &isfirst)
			: fileitem("FIXME"), pRet(p), nRest(size),
					nGot(0), skipBytes(start), nErr(0), m_isFirst(isfirst), fid(fi)
			{
				//m_bVolatile = false;
				m_nSizeCachedInitial = start;
				m_nRangeLimit = g_bGoodServer ? start+size-1 : -1;
			}
		};

		{
			lockguard g(remote_info_cache);
			map<string, tFileId>::const_iterator it = remote_info_cache.find(path);
			if (it != remote_info_cache.end())
				fid = it->second;
		}
		tFileId fidOrig=fid;

		auto* pFi = new tFitem(retbuf, len, pos, fid, bIsFirst);
		tFileItemPtr spFi(static_cast<fileitem*>(pFi));
		//dler->AddJob(spFi, dlrequest().setSrc(uri).setRangeLimit(pFi->m_nRangeLimit));
		int nHttpCode(100);
		//pFi->WaitForFinish(&nHttpCode);
		bIsFirst=false;


		if (m_ftype == rex::FILE_SOLID && fidOrig != fid)
		{
			lockguard g(remote_info_cache);
			remote_info_cache[m_path] = fid;
		}

		if(nHttpCode==416)
			return 0; // EOF
		if(pFi->nErr || !pFi->nGot)
			return -EIO;
		return pFi->nGot;
#endif
	}

	int StatOld(struct stat &stbuf)
	{
#warning RESTOREME
#if 0
		stbuf = statTempl;
		{
			lockguard g(remote_info_cache);
			map<string, tFileId>::const_iterator it = remote_info_cache.find(m_path);
			if (it != remote_info_cache.end())
			{
				stbuf.st_size = it->second.m_size;
				stbuf.st_mode &= ~S_IFDIR;
				stbuf.st_mode |= S_IFREG;
				struct tm tmx;
				if(tHttpDate::ParseDate(it->second.m_ctime.c_str(), &tmx))
					stbuf.st_ctime = mktime(&tmx);
				_cerr("Using precached\n");
				return 0;
			}
		}
		// ok, not cached, do the hard way

		tHttpUrl uri = proxyUrl;
		uri.sPath += baseUrl.sHost
		// + ":" + ( baseUrl.sPort.empty() ? baseUrl.sPort : "80")
				+ baseUrl.sPath + m_path;
		class tFitemProbe: public fileitem
		{
		public:
			ssize_t SendData(int, int, off_t&, size_t) override
			{
				return 0;
			}
		protected:
			bool DlStarted(header head, string_view rawHeader, off_t) override {
				m_head = head; // XXX: bloat, only status line and contlen required
				m_status = FIST_COMPLETE;
				return true;
			}
		};
		auto probe(make_shared<tFitemProbe>());
		dler->AddJob(probe, dlrequest().setSrc(uri).setHeadOnly(true));
		int nHttpCode(100);
		fileitem::FiStatus res = probe->WaitForFinish(&nHttpCode);
		stbuf.st_size = atoofft(probe->GetHeaderUnlocked().h[header::CONTENT_LENGTH], 0);
		stbuf.st_mode &= ~S_IFDIR;
		stbuf.st_mode |= S_IFREG;

		if (res < fileitem::FIST_COMPLETE)
			return -EIO;
		else if (nHttpCode == 200)
		{
			if (m_ftype == rex::FILE_SOLID) // not caching volatile stuff
			{
				lockguard g(remote_info_cache);
				remote_info_cache[m_path] =
						tFileId(stbuf.st_size, probe->GetHeaderUnlocked().h[header::LAST_MODIFIED]);
			}
			struct tm tmx;
			if(header::ParseDate(probe->GetHeaderUnlocked().h[header::LAST_MODIFIED], &tmx))
				stbuf.st_ctime = mktime(&tmx);
			return 0;
		}
#endif
		return -ENOENT;
	}
};



/// If found as downloadable, present as a file, or become a directory otherwise.
static int acngfs_getattr(const char *path, struct stat *stbuf)
{
	if(!path)
		return -1;

	rex::eMatchType type = rex::GetFiletype(path);
	_cerr( "type: " << type);
	if (type == rex::FILE_SOLID || type == rex::FILE_VOLATILE)
	{
		if(0 == tDlDescLocal(path, type).Stat(*stbuf))
			return 0;
		if(0 == tDlDescRemote(path, type).Stat(*stbuf))
			return 0;
	}

	//ldbg("Be a directory!");
	memcpy(stbuf, &statTempl, sizeof(statTempl));
	stbuf->st_mode &= ~S_IFMT; // delete mode flags and set them as needed
	stbuf->st_mode |= S_IFDIR;
	stbuf->st_size = 4;
	return 0;
}

static int acngfs_fgetattr(const char *path, struct stat *stbuf,
	  struct fuse_file_info *)
{
	// FIXME: reuse the con later? or better not, size might change during operation
	return acngfs_getattr(path, stbuf);
}

static int acngfs_access(const char *, int mask)
{
	// non-zero (failure) when trying to write
   return mask&W_OK;
}

static int acngfs_opendir(const char *, struct fuse_file_info *)
{
	// let FUSE manage directories
	return 0;
}

static int acngfs_readdir(const char *, void *, fuse_fill_dir_t,
	  off_t, struct fuse_file_info *)
{   
	return -EPERM;
}

static int acngfs_releasedir(const char *, struct fuse_file_info *)
{
   return 0;
}

static int acngfs_open(const char *path, struct fuse_file_info *fi)
{
	//lockguard g(&mxTest);
	
	if (fi->flags & (O_WRONLY|O_RDWR|O_TRUNC|O_CREAT))
			return -EROFS;

	tDlDesc *p(nullptr);
	struct stat stbuf;
	rex::eMatchType ftype = rex::GetFiletype(path);

	try
	{
		// ok... if that is a remote object, can we still use local access instead?
		if(!altPath.empty() && rex::FILE_SOLID == ftype)
		{
				p = new tDlDescLocal(path, ftype);
				if(p)
				{
					if(0 == p->Stat(stbuf))
						goto desc_opened;
					delete p;
					p = nullptr;
				}
		}


		p = new tDlDescRemote(path, ftype);

		if (!p) // running exception-free?
			return -EIO;

		if (0 != p->Stat(stbuf))
		{
			delete p;
			return -EIO;
		}
	}
	catch(std::bad_alloc&)
	{
		return -EIO;
	}
	
	desc_opened:

	fi->fh = (uintptr_t) p;
	return 0;
}


static int acngfs_read(const char *path, char *buf, size_t size, off_t offset,
      struct fuse_file_info *fi)
{
	auto p=(tDlDesc*) fi->fh;
	return p->Read(buf, path, offset, size);
}

static int acngfs_statfs(const char *, struct statvfs *stbuf)
{
   memcpy(stbuf, &stfsTemp, sizeof(*stbuf));
	return 0;
}

static int acngfs_release(const char *, struct fuse_file_info *fi)
{
	if(fi->fh)
		delete (tDlDesc*)fi->fh;
	return 0;
}

struct fuse_operations acngfs_oper;

int my_fuse_main(int argc, char ** argv)
{
   return fuse_main(argc, argv, &acngfs_oper, nullptr);
}

void _ExitUsage() {
   cerr << "USAGE: acngfs BaseURL ProxyHost MountPoint [FUSE Mount Options]\n"
   << "examples:\n\t  acngfs http://ftp.uni-kl.de/debian cacheServer:3142 /var/local/aptfs\n"
   << "\t  acngfs http://ftp.uni-kl.de/debian localhost:3142 /var/cache/apt-cacher-ng/debrep /var/local/aptfs\n\n"
        << "FUSE mount options summary:\n\n";
    char *argv[] = {strdup("..."), strdup("-h")};
    my_fuse_main( _countof(argv), argv);
    exit(EXIT_FAILURE);
}

#define barf(x) { cerr << endl << "ERROR: " << x <<endl; exit(1); }
#define erUsage { _ExitUsage(); }


int main(int argc, char *argv[])
{
	using namespace acng;
	memset(&acngfs_oper, 0, sizeof(acngfs_oper));

	acngfs_oper.getattr	= acngfs_getattr;
	acngfs_oper.fgetattr	= acngfs_fgetattr;
	acngfs_oper.access	= acngfs_access;
	acngfs_oper.opendir	= acngfs_opendir;
	acngfs_oper.readdir	= acngfs_readdir;
	acngfs_oper.releasedir	= acngfs_releasedir;
	acngfs_oper.open	= acngfs_open;
	acngfs_oper.read	= acngfs_read;
	acngfs_oper.statfs	= acngfs_statfs;
	acngfs_oper.release	= acngfs_release;
	umask(0);

	for(int i = 1; i<argc; i++)
		if(argv[i] && 0==strcmp(argv[i], "--help"))
			erUsage;

	if(argc<4)
		barf("Not enough arguments, try --help.\n");

	cfg::agentname = "ACNGFS";
	cfg::agentheader="User-Agent: ACNGFS\r\n";
	cfg::requestapx = "User-Agent: ACNGFS\r\nX-Original-Source: 42\r\n";
#ifdef SPAM
	cfg::debug=0xff;
	cfg::verboselog=1;
#endif

	if(argv[1] && baseUrl.SetHttpUrl(argv[1]))
	{
#ifdef VERBOSE
		cout << "Base URL: " << baseUrl.ToString()<<endl;
#endif
	}
	else
	{
		cerr << "Invalid base URL, " << argv[1] <<endl;
		exit(EXIT_FAILURE);
	}
	// FUSE adds starting / already, drop ours if present
	trimBack(baseUrl.sPath, "/");

	if(argv[2] && proxyUrl.SetHttpUrl(argv[2]))
	{
		/*if(proxyUrl.GetPort().empty())
		   proxyUrl.sPort="3142";
		   */
	}
	else
	{
		cerr << "Invalid proxy URL, " << argv[2] <<endl;
		exit(EXIT_FAILURE);
	}

	// all parameters processed, forwarded to fuse call below

	acng::rex::CompileExpressions();

#if 0//def SPAM
	{
		fuse_file_info fi = {0};
		const char *dingsda="/dists/unstable/InRelease";
		acngfs_open(dingsda, &fi);
		char buf[165536];
		off_t pos=0;
		for(;0 < acngfs_read(dingsda, buf, sizeof(buf), pos, &fi); pos+=sizeof(buf)) ;
		return 0;
	}
#endif

	unsigned int nMyArgCount = 2; // base url, proxy host
	// alternative path supplied in the next argument?
	if(argc > 4 && argv[4] && argv[4][0] != '-' ) // 4th argument is not an option?
	{
		nMyArgCount=3;
		altPath = argv[3];
	}

	// test mount point
	char *mpoint = argv[nMyArgCount+1];
	if(!mpoint || stat(mpoint, &statTempl) || statfs(mpoint, &stfsTemp))
		barf(endl << "Cannot access " << mpoint);
	if(!S_ISDIR(statTempl.st_mode))
		barf(endl<< mpoint << " is not a directory.");

	// skip our arguments, keep those for fuse including mount point and argv[0] at the right place
	argv[nMyArgCount] = argv[0]; // application path
	argv = &argv[nMyArgCount];
	argc -= nMyArgCount;

	evabaseFreeFrunner eb(g_tcp_con_factory);
//	dler = &eb.getDownloader();

	//return my_fuse_main(argc, argv);
	int mt = 1;

	/** This is the part of fuse_main() before the event loop */
	auto fs = fuse_setup(argc, argv,
				&acngfs_oper, sizeof(acngfs_oper),
				&mpoint, &mt, nullptr);
	if (!fs)
		return 2;
	return fuse_loop_mt(fs);
}
