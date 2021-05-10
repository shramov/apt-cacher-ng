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

#include <event2/buffer.h>

#define HEADSZ 5000
#ifndef MIN
#define MIN(a,b) ( (a<=b)?a:b)
#endif

#include <thread>

#define MAX_DL_STREAMS 8
#define MAX_DL_BUF_SIZE 1024*1024

using namespace acng;
using namespace std;


namespace acng
{
namespace log
{
	extern mstring g_szLogPrefix;
}
}

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

#define HEAD_TAIL_LEN BUFSIZ

// some globals, set only once
static struct stat statTempl;
static struct statfs stfsTemp;
tHttpUrl baseUrl;
cmstring& altPath = cfg::cachedir;

bool g_bGoodServer=true;

struct tDlAgent
{
	thread runner;
	SHARED_PTR<dlcon> dler;
	time_t createdAt = GetTime();
	enum eKind
	{
		HEADER, TRAILER, MID
	} kind;

	tDlAgent()
	{
		dler = dlcon::CreateRegular();
		runner = std::thread([&]() { dler->WorkLoop();} );
	}
	~tDlAgent()
	{
		if(dler)
			dler->SignalStop();

		if(runner.joinable())
			runner.join();
	}

};

//std::unordered_multimap<string, SHARED_PTR<tDlStream>> active_agents;
std::list<SHARED_PTR<tDlAgent>> spare_agents;
std::mutex mx_streams;
using mg = lock_guard<std::mutex>;

SHARED_PTR<tDlAgent> getAgent()
{
	mg g(mx_streams);
#if 0 // nay, will be reconnected, cheaper than restarting the thread
	// dump those who were proably disconnected anyway
	auto exTime = GetTime() - 15;
	spare_agents.remove_if([exTime](decltype (spare_agents)::iterator it)
	{
		return (**it).createdAt < exTime;
	});
#endif
	if (!spare_agents.empty())
	{
		auto ret = spare_agents.back();
		spare_agents.pop_back();
		return ret;
	}
	return make_shared<tDlAgent>();
}

void returnAgent(SHARED_PTR<tDlAgent> && agent)
{
	mg g(mx_streams);
	spare_agents.push_back(move(agent));
}

class tConsumingItem : public fileitem
{
public:
	evbuffer* buf;
	off_t pos;

	tConsumingItem(string_view path, off_t knownLength, tHttpDate knownDate, off_t rangeStart, off_t rangeLen)
		: fileitem(path),  pos(rangeStart)
	{
		m_status = FIST_INITED;
		m_nSizeCachedInitial = knownLength;
		m_responseModDate = knownDate;
		m_spattr.nRangeLimit = rangeStart + rangeLen - 1;
		m_spattr.bVolatile = true;
		buf = evbuffer_new();
	}
	~tConsumingItem()
	{
		evbuffer_free(buf);
	}

protected:
	bool DlAddData(string_view data, lockuniq &) override
	{
		m_status = FIST_DLRECEIVING;
		if (m_nSizeChecked < 0)
			m_nSizeChecked = 0;
		if (evbuffer_get_length(buf) >= MAX_DL_BUF_SIZE)
			return true;
		if (m_nSizeChecked < pos)
		{
			if (m_nSizeChecked + off_t(data.size()) < off_t(pos))
			{
				// no overlap, not reachable
				m_nSizeChecked += data.size();
				return true;
			}
			// reached, partial or full overlap
			auto drop = pos - m_nSizeChecked;
			data.remove_prefix(drop);
			m_nSizeChecked += drop;
		}
		auto avail = MAX_DL_BUF_SIZE - evbuffer_get_length(buf);
		auto toTake = min(avail, data.length());
		if (0 != evbuffer_add(buf, data.data(), toTake))
			return false;
		data.remove_prefix(toTake);
		// false to abort if too much data already
		return data.empty();
	}
};

#if 0
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
#endif

struct tDlDesc
{
	cmstring m_path;
	uint m_ftype;

	virtual int Read(char *retbuf, const char *path, off_t pos, size_t len) =0;
	virtual int Stat(struct stat *stbuf) =0;
	tDlDesc(cmstring &p, uint ftype) : m_path(p), m_ftype(ftype) {};
	virtual ~tDlDesc() {};
};

struct tDlDescLocal : public tDlDesc
{
	FILE *pFile;
	tDlDescLocal(cmstring &path, uint ftype) : tDlDesc(path, ftype), pFile(nullptr)
	{
	};

	int Stat(struct stat *stbuf) override
	{
		if (!stbuf)
			return -EIO;

		if(altPath.empty()) // hm?
			return -ENOENT;

		auto absPath = altPath + m_path;
		if (::stat(absPath.c_str(), stbuf))
			return -errno;

		// verify the file state
		off_t cl = -2;
		if (!ParseHeadFromStorage(absPath, &cl, nullptr, nullptr) || cl != stbuf->st_size)
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
			if(Stat(&stbuf))
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

using unique_evbuf = auto_raii<evbuffer*,evbuffer_free,nullptr>;
#define META_CACHE_EXP_TIME 120
struct tFileMeta
{
	off_t m_size = -1; tHttpDate m_ctime; time_t validAt = GetTime();
	SHARED_PTR<unique_evbuf> head, tail;
	tFileMeta() =default;
	tFileMeta(off_t a, mstring b) : m_size(a), m_ctime(b) {};
	bool operator!=(tFileMeta other) const { return m_size != other.m_size || m_ctime != other.m_ctime;}
};
static class tRemoteInfoCache : public base_with_mutex, public map<string, tFileMeta>
{} remote_info_cache;

struct tDlDescRemote : public tDlDesc
{
protected:

	tFileMeta m_meta;
	SHARED_PTR<tDlAgent> m_agent;
	bool m_bMetaVerified = false;
	tHttpUrl m_uri;
	bool m_bItemWasRead = false, m_bDataChangeError = false;

public:
	tDlDescRemote(cmstring &p, uint n) : tDlDesc(p,n)
	{
		// good moment to expire the caches, every time, should not cost much anyway
		g_tcp_con_factory.BackgroundCleanup();
		lockguard g(remote_info_cache);
		auto it = remote_info_cache.find(m_path);
		if (it != remote_info_cache.end())
		{
			if (GetTime() > it->second.validAt + META_CACHE_EXP_TIME)
			{
				remote_info_cache.erase(it);
				return;
			}
			m_meta = it->second;
			return;
		}

		m_uri = baseUrl;
		m_uri.sPath += m_path;

		// Stat(nullptr);
	};
	int Stat(struct stat *stbuf)
	{
		if (!m_bMetaVerified)
		{
			// string_view path, off_t knownLength, tHttpDate knownDate, off_t rangeStart, off_t rangeLen
			auto item = make_shared<tConsumingItem>(m_path, m_meta.m_size, m_meta.m_ctime, 0, HEAD_TAIL_LEN);
			pair<fileitem::FiStatus, tRemoteStatus> res;

			item->DlRefCountAdd();
			try
			{
				if (!m_agent)
					m_agent = getAgent();
				m_agent->dler->AddJob(item, tHttpUrl(m_uri));
				res = item->WaitForFinish(5, [](){ return false; });
			}
			catch (...)
			{
				// ignored, only needs to make sure to adjust user count safely
			}
			item->DlRefCountDec({500, "N/A"});

			if (res.first != fileitem::FIST_COMPLETE)
				return -EIO;
			auto changed = (item->m_responseModDate != m_meta.m_ctime || item->m_nContentLength != m_meta.m_size);
			if (changed)
			{
				m_meta.m_size = item->m_nContentLength;
				m_meta.m_ctime = item->m_responseModDate;
				{
					lockguard g(remote_info_cache);
					remote_info_cache[m_path] = m_meta;
				}
				if (m_bItemWasRead)
				{
					m_bDataChangeError = true;
					return -EIO;
				}
			}

			m_bMetaVerified = true;
		}
		if(stbuf)
			*stbuf = statTempl;
		if (m_meta.m_size >= 0)
		{
			stbuf->st_size = m_meta.m_size;
			stbuf->st_mode &= ~S_IFDIR;
			stbuf->st_mode |= S_IFREG;
			stbuf->st_ctime = m_meta.m_ctime.value(1);
			return 0;
		}
		return -ENOENT;
	}

	int Read(char *retbuf, const char *path, off_t pos, size_t len)
	{
		return -EIO;

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
		if(0 == tDlDescLocal(path, type).Stat(stbuf))
			return 0;
		if(0 == tDlDescRemote(path, type).Stat(stbuf))
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
					if(0 == p->Stat(&stbuf))
						goto desc_opened;
					delete p;
					p = nullptr;
				}
		}


		p = new tDlDescRemote(path, ftype);

		if (!p) // running exception-free?
			return -EIO;

		if (0 != p->Stat(&stbuf))
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
   cerr << "USAGE: acngfs BaseURL MountPoint [ACNG Configuration Assignments] [FUSE Mount Options]\n"
   << "Examples:\n\t  acngfs http://ftp.uni-kl.de/debian /var/local/aptfs proxy=cacheServer:3142"
   << "\n\t  acngfs http://ftp.uni-kl.de/debian /var/local/aptfs proxy=cacheServer:3142 cachedir=/var/cache/apt-cacher-ng/debrep\n\n"
        << "FUSE mount options summary:\n\n";
    char *argv[] = {strdup("..."), strdup("-h")};
    my_fuse_main( _countof(argv), argv);
    exit(EXIT_FAILURE);
}

#define barf(x) { cerr << endl << "ERROR: " << x <<endl; exit(1); }

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

	cfg::agentname = "ACNGFS";
	cfg::agentheader="User-Agent: ACNGFS\r\n";
	cfg::requestapx = "User-Agent: ACNGFS\r\nX-Original-Source: 42\r\n";
	cfg::cachedir.clear();
	cfg::cacheDirSlash.clear();
#ifdef DEBUG
	cfg::debug=0xff;
	cfg::verboselog=1;
	log::g_szLogPrefix = "acngfs";
	log::open();
#endif

	if(argc<3)
		barf("Not enough arguments, try --help.\n");

	vector<char*> fuseArgs(argv, argv + 1);
	fuseArgs.push_back(argv[2]);

	cfg::g_bQuiet = true; // STFU, we need to separate our options silently

	// check help request and weedout our arguments
	for(auto p=argv + 3; p < argv + argc; ++p)
	{
		if (0==strcmp(*p, "--help"))
			_ExitUsage();
		else if (!cfg::SetOption(*p, nullptr))
			fuseArgs.push_back(*p);
	}

	if(argv[1] && baseUrl.SetHttpUrl(argv[1]))
	{
		// FUSE adds starting / already, drop ours if present
		trimBack(baseUrl.sPath, "/");
#ifdef VERBOSE
		cout << "Base URL: " << baseUrl.ToString()<<endl;
#endif
	}
	else
	{
		cerr << "Invalid base URL, " << argv[1] << endl;
		exit(EXIT_FAILURE);
	}

	if (! cfg::GetProxyInfo() || cfg::GetProxyInfo()->sHost.empty())
	{
		cerr << "WARNING: proxy not specified or invalid, please set something like proxy=localserver:3142."
" Continuing without proxy server for now, this might cause severe performance degradation!" << endl;
		exit(EXIT_FAILURE);
	}

	cfg::PostProcConfig();

	// all parameters processed, forwarded to fuse call below

	acng::rex::CompileExpressions();

	// test mount point
	if(!argv[2] || stat(argv[2], &statTempl) || statfs(argv[2], &stfsTemp))
		barf(endl << "Cannot access directory " << argv[2]);
	if(!S_ISDIR(statTempl.st_mode))
		barf(endl<< argv[2] << " is not a directory.");

	// restore application arguments

#warning this carries a dlcon with a thread, do we need it? only evabase with one controlled thread is required
	evabaseFreeFrunner eb(g_tcp_con_factory);
	int mt = 1;
	/** This is the part of fuse_main() before the event loop */
	auto fs = fuse_setup(fuseArgs.size(), &fuseArgs[0],
				&acngfs_oper, sizeof(acngfs_oper),
				&argv[2], &mt, nullptr);
	if (!fs)
		return 2;
	auto ret = fuse_loop_mt(fs);
	// shutdown agents in reliable fashion
	spare_agents.clear();
	return ret;
}
