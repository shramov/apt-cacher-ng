
//#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"
#include "fileitem.h"
#include "header.h"
#include "acfg.h"
#include "acbuf.h"
#include "fileio.h"
#include "cleaner.h"

#include <errno.h>
#include <algorithm>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

using namespace std;

#warning Review all usages of Dl* methods, need to do locks properly

namespace acng
{
#define MAXTEMPDELAY acng::cfg::maxtempdelay // 27

static tFiGlobMap mapItems;
static acmutex mapItemsMx;

struct TExpiredEntry
{
	tFileItemPtr p;
	time_t timeExpired;
};

deque<TExpiredEntry> prolongedLifetimeQ;
static acmutex prolongMx;

header const & fileitem::GetHeaderUnlocked()
{
	return m_head;
}

string fileitem::GetHttpMsg()
{
	setLockGuard;
	if(m_head.frontLine.length()>9)
		return m_head.frontLine.substr(9);
	return m_head.frontLine;
}

fileitem::fileitem() :
			// good enough to not trigger the makeWay check but also not cause overflows
			m_nTimeDlStarted(END_OF_TIME-MAXTEMPDELAY*3),
			m_globRef(mapItems.end())
{
}

fileitem::~fileitem()
{
}


void fileitem::DlRefCountAdd()
{
	setLockGuard;
	m_nDlRefsCount++;
}

void fileitem::DlRefCountDec(mstring sReason)
{
	setLockGuard

	notifyAll();

	m_nDlRefsCount--;
	if (m_nDlRefsCount > 0)
		return; // someone will care...

	// ... otherwise: the last downloader disappeared, needing to tell observers

	if (m_status < FIST_COMPLETE)
	{
		m_status = FIST_DLERROR;
		m_head.clear();
		m_head.frontLine = "HTTP/1.1 ";
		m_head.frontLine += move(sReason);
		m_head.type = header::ANSWER;

		if (cfg::debug & log::LOG_MORE)
			log::misc(string("Download of ") + m_sPathRel + " aborted");
	}
}

uint64_t fileitem::TakeTransferCount()
{
	setLockGuard

	uint64_t ret = m_nIncommingCount;
	m_nIncommingCount = 0;
	return ret;
}

unique_fd fileitem::GetFileFd()
{
	LOGSTART("fileitem::GetFileFd");
	setLockGuard

	ldbg("Opening " << m_sPathRel);
	int fd = open(SZABSPATH(m_sPathRel), O_RDONLY);

#ifdef HAVE_FADVISE
	// optional, experimental
	if (fd != -1)
		posix_fadvise(fd, 0, m_nSizeChecked, POSIX_FADV_SEQUENTIAL);
#endif

	return unique_fd(fd);
}

off_t GetFileSize(cmstring &path, off_t defret)
{
	struct stat stbuf;
	return (0 == ::stat(path.c_str(), &stbuf)) ? stbuf.st_size : defret;
}
/*
void fileitem::ResetCacheState()
{
	setLockGuard;
	m_nSizeSeen = 0;
	m_nSizeChecked = 0;
	m_status = FIST_FRESH;
	m_bAllowStoreData = true;
	m_head.clear();
}
*/

fileitem::FiStatus fileitem::Setup(bool bCheckFreshness)
{
	LOGSTARTFUNCx(bCheckFreshness);

	setLockGuard;

	if (m_status > FIST_FRESH)
		return m_status;

	auto error_clean = [this]()
	{
		m_nSizeCachedInitial = m_nSizeChecked = m_nContLenInitial = -1;
#warning flag beachten!
		m_bWriterMustReplaceFile = true;
		return m_status = FIST_INITED;
	};

	m_status = FIST_INITED;
	m_bVolatile = bCheckFreshness;

	cmstring sPathAbs(CACHE_BASE + m_sPathRel);

	if (m_head.LoadFromFile(sPathAbs + ".head") > 0 && m_head.type == header::ANSWER)
	{
		if (200 != m_head.getStatus())
			return error_clean();

		LOG("good head");

		m_nSizeCachedInitial = GetFileSize(sPathAbs, 0);
		m_nContLenInitial = atoofft(m_head.h[header::CONTENT_LENGTH], -1);

		if (!m_bVolatile)
		{
			// non-volatile files, so could accept the length, do some checks first
			if (m_nContLenInitial >= 0)
			{
				// file larger than it could ever be?
				if (m_nContLenInitial < m_nSizeCachedInitial)
					return error_clean();

				// is it complete? and 0 value also looks weird, try to verify later
				if (m_nSizeCachedInitial == m_nContLenInitial)
				{
					m_nSizeChecked = m_nSizeCachedInitial;
					m_status = FIST_COMPLETE;
				}
				else
				{
					// otherwise wait for remote to confirm its presence too
					m_bVolatile = true;
				}
			}
			else
			{
				// no content length known, assume that it's ok
				m_nSizeChecked = m_nSizeCachedInitial;
			}
		}

		// some plausibility checks
		if (m_bVolatile)
		{
			// cannot resume if conditions not satisfied
			if (!m_head.h[header::LAST_MODIFIED])
				return error_clean();
		}
	}
	else // -> no .head file
	{
		// maybe there is some left-over without head file?
		// Don't thrust volatile data, but otherwise try to reuse?
		if (!m_bVolatile)
			m_nSizeCachedInitial = GetFileSize(sPathAbs, -1);
	}
	LOG("resulting status: " << (int) m_status);
	return m_status;
}

bool fileitem::CheckUsableRange_unlocked(off_t nRangeLastByte)
{
#warning wer braucht das?
	if (m_status == FIST_COMPLETE)
		return true;
	if (m_status < FIST_INITED || m_status > FIST_COMPLETE)
		return false;
	if (m_status >= FIST_DLGOTHEAD)
		return nRangeLastByte > m_nSizeChecked;

	// special exceptions for solid files
	return (m_status == FIST_INITED && !m_bVolatile
			&& m_nSizeCachedInitial>0 && nRangeLastByte >=0 && nRangeLastByte <m_nSizeCachedInitial
			&& atoofft(m_head.h[header::CONTENT_LENGTH], -255) > nRangeLastByte);
}

void fileitem::SetupComplete()
{
	setLockGuard;
	notifyAll();
	m_nSizeChecked = m_nSizeCachedInitial;
	m_status = FIST_COMPLETE;
}

void fileitem::UpdateHeadTimestamp()
{
	if(m_sPathRel.empty())
		return;
	utimes(SZABSPATH(m_sPathRel + ".head"), nullptr);
}

fileitem::FiStatus fileitem::WaitForFinish(int *httpCode)
{
	lockuniq g(this);
	while(m_status<FIST_COMPLETE)
		wait(g);
	if(httpCode)
		*httpCode=m_head.getStatus();
	return m_status;
}

fileitem::FiStatus fileitem::WaitForFinish(int *httpCode, unsigned check_interval, const std::function<void()> &check_func)
{
	lockuniq g(this);
	while(m_status<FIST_COMPLETE)
	{
		if(wait_for(g, 1, 1)) // on timeout
			check_func();
	}
	if(httpCode)
		*httpCode=m_head.getStatus();
	return m_status;
}

inline void _LogWithErrno(const char *msg, const string & sFile)
{
	tErrnoFmter f;
	log::err(tSS() << sFile <<
			" storage error [" << msg << "], last errno: " << f);
}

void fileitem_with_storage::SETERROR(string_view x)
{
	m_head.clear();
	m_head.frontLine = "HTTP/1.1 500 Cache Error, check apt-cacher.err";
	log::err(tSS() << m_sPathRel << " storage error [" << x
			<< "], last errno: " << tErrnoFmter());
};

bool fileitem_with_storage::withError(string_view message, fileitem::EDestroyMode destruction)
{
	SETERROR(message);
	//m_pStorage->DlStarted(move(h), rawHeader);
	DlSetError(destruction);
	return false;
};

bool fileitem_with_storage::DlStarted(acng::header h, acng::string_view rawHeader, off_t bytes2seek)
{

/*bool fileitem_with_storage::DownloadStartedStoreHeader(const header & h, size_t hDataLen,
		const char *pNextData,
		bool bForcedRestart, bool &bDoCleanRetry)
		*/

	LOGSTARTFUNC

	LOG(h.ToString());

	m_nTimeDlStarted = GetTime();
	m_nIncommingCount += rawHeader.size();
	notifyAll();

	USRDBG( "Download started, storeHeader for " << m_sPathRel << ", current status: " << (int) m_status);

	if(m_status >= FIST_DLGOTHEAD)
	{
		// cannot start again, what's happening?
		USRDBG( "FIXME, wild start");
		return false;
	}

	m_head = move(h);
	m_status=FIST_DLGOTHEAD;
	// if range was confirmed then can already start forwarding that much
	if (bytes2seek >= 0)
	{
		if (bytes2seek < m_nSizeChecked && m_nSizeChecked >= 0)
			return false;

		m_nSizeChecked = bytes2seek;
	}
	return true;
}

bool fileitem_with_storage::DlAddData(string_view chunk)
{
#warning really needs a lock here all time while doing write operations? or lock around metadata changes?
	// something might care, most likely... also about BOUNCE action
	notifyAll();

	m_nIncommingCount += chunk.size();

	// is this the beginning of the stream?
	if(m_status == FIST_DLGOTHEAD && !SafeOpenOutFile())
		return false;

	if (AC_UNLIKELY(m_filefd == -1 || m_status < FIST_DLGOTHEAD))
		return withError("Suspicious fileitem status");

	if (m_status > FIST_COMPLETE) // DLSTOP, DLERROR
		return false;

	while (!chunk.empty())
	{
		int r = write(m_filefd, chunk.data(), chunk.size());
		if (r == -1)
		{
			if (EINTR != errno && EAGAIN != errno)
				return withError("Write error");
		}
		m_nSizeChecked += r;
		chunk.remove_prefix(r);
	}
	return true;
}

bool fileitem_with_storage::SafeOpenOutFile()
{
	LOGSTARTFUNC
	checkforceclose(m_filefd);

	// using adaptive Delete-Or-Replace-Or-CopyOnWrite strategy

	MoveRelease2Sidestore();

	auto sPathAbs(SABSPATH(m_sPathRel));

	// First opening the file to be sure that it can be written. Header storage is the critical point,
	// every error after that leads to full cleanup to not risk inconsistent file contents

	int flags = O_WRONLY | O_CREAT | O_BINARY;

	Cstat stbuf;

	mkbasedir(sPathAbs);
	if (m_nSizeChecked <= 0)
	{
		// 0 might also be a sign of missing metadata while data file may exist
		// in that case use the other strategy of new file creation which does not crash mmap

		checkforceclose(m_filefd); // be sure about that
		mstring tname(sPathAbs + "-"), tname2(sPathAbs + "~");
		// keep the file descriptor later if needed
		unique_fd tmp(open(tname.c_str(), flags, cfg::fileperms | O_TRUNC));
		if (tmp.m_p == -1)
			return withError("Cannot create cache files");
		fdatasync(tmp.m_p);
		bool didExist = true;
		if (0 != rename(sPathAbs.c_str(), tname2.c_str()))
		{
			if (errno != ENOENT)
				return withError("Cannot move cache files");
			didExist = false;
		}
		if (0 != rename(tname.c_str(), sPathAbs.c_str()))
			return withError("Cannot rename cache files");
		if (!didExist)
			unlink(tname2.c_str());
		std::swap(m_filefd, tmp.m_p);
	}
	else
		m_filefd = open(sPathAbs.c_str(), flags, cfg::fileperms);

	ldbg("file opened?! returned: " << m_filefd);

	// self-recovery from cache poisoned with files with wrong permissions
	// we still want to recover the file content if we can
	if (m_filefd == -1)
	{
		if (m_nSizeChecked > 0) // OOOH CRAP! CANNOT APPEND HERE! Do what's still possible.
		{
			string temp = sPathAbs + ".tmp";
			if (!FileCopy(sPathAbs, temp, &errno))
				return withError("Cannot make file copies");

			if (0 != unlink(sPathAbs.c_str()))
				return withError("Cannot remove file in folder");

			if (0 != rename(temp.c_str(), sPathAbs.c_str()))
				return withError("Cannot rename files in folder");

			m_filefd = open(sPathAbs.c_str(), flags, cfg::fileperms);
		}
		else
		{
			unlink(sPathAbs.c_str());
			m_filefd = open(sPathAbs.c_str(), flags, cfg::fileperms);
		}
	}

	if (m_filefd == -1)
		return withError("Filesystem error");

#if 0 // do we care?
		if(0 != fstat(m_filefd, &stbuf) || !S_ISREG(stbuf.st_mode))
			return withError("Not a regular file", EDestroyMode::DELETE_KEEP_HEAD);
#endif

	auto sHeadPath(sPathAbs + ".head");

	ldbg("Storing header as " + sHeadPath);
	auto tempHead(sPathAbs + ".hea%");
	int count = m_head.StoreToFile(tempHead);

	if (count < 0)
	{
		errno = -count;
		return withError("Cannot store header");
	}

	// either confirm start at zero or verify the expected file state on disk

	if (m_nSizeChecked < 0)
		m_nSizeChecked = 0;
	else
	{
		if (m_nSizeChecked != lseek(m_filefd, 0, SEEK_END))
			return withError("Unexpected file change");
	}

	// this is not supposed to go wrong at this moment
	if (0 != rename(tempHead.c_str(), sHeadPath.c_str()))
		return withError("Renaming failure");

	// okay, have the stream open
	m_status = FIST_DLRECEIVING;
	return true;
}

void fileitem::MarkFaulty(bool killFile)
{
	setLockGuard
	DlSetError(killFile ? EDestroyMode::DELETE : EDestroyMode::TRUNCATE);
}

TFileItemHolder::~TFileItemHolder()
{
	LOGSTARTFUNC

	if (!m_ptr) // unregistered before? or not shared?
		return;

	lockguard managementLock(mapItemsMx);

	bool wasGloballyRegistered = m_ptr->m_globRef != mapItems.end();

	auto local_ptr(m_ptr); // might disappear
	lockguard fitemLock(*local_ptr);

	if ( -- m_ptr->usercount > 0)
		return; // still in active use

	m_ptr->notifyAll();

#ifdef DEBUG
	if (m_ptr->m_status > fileitem::FIST_INITED &&
			m_ptr->m_status < fileitem::FIST_COMPLETE)
	{
		log::err(mstring("users gone while downloading?: ") + ltos(m_ptr->m_status));
	}
#endif

	if (wasGloballyRegistered)
	{
		// some file items will be held ready for some time
		if (MAXTEMPDELAY && m_ptr->m_bVolatile && m_ptr->m_status == fileitem::FIST_COMPLETE)
		{
			auto when = m_ptr->m_nTimeDlStarted + MAXTEMPDELAY;
			if (when > GetTime())
			{
				AddToProlongedQueue(move(local_ptr), when);
				return;
			}
		}
	}
	// nothing, let's put the item into shutdown state
	if (m_ptr->m_status < fileitem::FIST_COMPLETE)
		m_ptr->m_status = fileitem::FIST_DLSTOP;
	m_ptr->m_head.frontLine = "HTTP/1.1 500 Cache file item expired";

	if (wasGloballyRegistered)
	{
		LOG("*this is last entry, deleting dl/fi mapping");
		mapItems.erase(m_ptr->m_globRef);
		m_ptr->m_globRef = mapItems.end();
	}
	// make sure it's not double-unregistered accidentally!
	m_ptr.reset();

}

void TFileItemHolder::AddToProlongedQueue(tFileItemPtr&& p, time_t expTime)
{
	lockguard g(prolongMx);
	// act like the item is still in use
	p->usercount++;
	prolongedLifetimeQ.emplace_back(TExpiredEntry {p, expTime});
	cleaner::GetInstance().ScheduleFor(prolongedLifetimeQ.front().timeExpired,
			cleaner::TYPE_EXFILEITEM);
}

TFileItemHolder TFileItemHolder::Create(cmstring &sPathUnescaped, ESharingHow how)
{
	LOGSTARTFUNCxs(sPathUnescaped);
#warning should have UTs for all combinations
	try
	{
		mstring sPathRel(fileitem_with_storage::NormalizePath(sPathUnescaped));
		lockguard lockGlobalMap(mapItemsMx);

		auto regnew = [&]()
		{
			LOG("Registering as NEW file item...");
			auto sp(make_shared<fileitem_with_storage>(sPathRel));
			sp->usercount++;
			auto res = mapItems.emplace(sPathRel, sp);
			ASSERT(res.second);
			sp->m_globRef = res.first;
			return TFileItemHolder(sp);
		};

		auto it = mapItems.find(sPathRel);
		if (it == mapItems.end())
			return regnew();

		auto &fi = it->second;

		auto share = [&]()
		{
			LOG("Sharing existing file item");
			it->second->usercount++;
			return TFileItemHolder(it->second);
		};

		lockguard g(*fi);

		if (how == ESharingHow::ALWAYS_TRY_SHARING)
			return share();

		// detect items that got stuck somehow and move it out of the way
		auto now(GetTime());
		auto makeWay = how == ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY ||
#warning range-limited item (only those starting from zero) shall set this!
#warning actually, bad idea, should carry rangelimit again in the item and verify against that in this method
				fi->m_bCreateItemMustDisplace ||
				(now > (fi->m_nTimeDlStarted + cfg::stucksecs));

		if (!makeWay)
			return share();

		// XXX: this is crap and cannot happen but better double-check!
		if (fi->m_sPathRel.empty())
			return TFileItemHolder();

		// okay, needing the evasive maneuver
		auto replPathRel = fi->m_sPathRel + "." + ltos(now);
		auto replPathAbs = SABSPATH(replPathRel);

		auto pathAbs = SABSPATH(fi->m_sPathRel);

		if (0 != link(pathAbs.c_str(), replPathAbs.c_str()) || 0 != unlink(pathAbs.c_str()))
		{
			// oh, that's bad, no permissions on the folder whatsoever?
			log::err(string("Failure to move file out of the way into ") + replPathAbs + " - errno: " + tErrnoFmter());
			return TFileItemHolder();
		}
		else
		{
			fi->m_sPathRel = replPathAbs;
			fi->m_eDestroy = fileitem::EDestroyMode::ABANDONED;
			fi->m_globRef = mapItems.end();
			mapItems.erase(it);
			return regnew();
		}
	} catch (std::bad_alloc&)
	{
		return TFileItemHolder();
	}
}

// make the fileitem globally accessible
TFileItemHolder TFileItemHolder::Create(tFileItemPtr spCustomFileItem, bool isShareable)
{
	LOGSTARTFUNCxs(spCustomFileItem->m_sPathRel);

	TFileItemHolder ret;

	if (!spCustomFileItem || spCustomFileItem->m_sPathRel.empty())
		return ret;


	if(!isShareable)
	{
		ret.m_ptr = spCustomFileItem;
	}

	lockguard lockGlobalMap(mapItemsMx);

	auto installed = mapItems.emplace(spCustomFileItem->m_sPathRel,
			spCustomFileItem);

	if(!installed.second)
		return ret; // conflict, another agent is already active

	spCustomFileItem->m_globRef = installed.first;
	spCustomFileItem->usercount++;
	ret.m_ptr = spCustomFileItem;
	return ret;
}

// this method is supposed to be awaken periodically and detects items with ref count manipulated by
// the request storm prevention mechanism. Items shall be be dropped after some time if no other
// thread but us is using them.
time_t TFileItemHolder::BackgroundCleanup()
{
	auto now = GetTime(), ret = END_OF_TIME;
	LOGSTARTFUNCsx(now);
	deque<tFileItemPtr> releasedQ;

	{
		lockguard g(prolongMx);
		while (!prolongedLifetimeQ.empty() && prolongedLifetimeQ.front().timeExpired <= now)
		{
			releasedQ.emplace_back(move(prolongedLifetimeQ.front().p));
			prolongedLifetimeQ.pop_front();
		}
		if (!prolongedLifetimeQ.empty())
			ret = prolongedLifetimeQ.front().timeExpired;
	}
	for(auto& item: releasedQ)
	{
		// run dtor just like as if it happened with a regular release would happen
		TFileItemHolder cleaner(item);
	}
	return ret;
}

ssize_t fileitem_with_storage::SendData(int out_fd, int in_fd, off_t &nSendPos, size_t count)
{
	if(out_fd == -1 || in_fd == -1)
		return -1;

#ifndef HAVE_LINUX_SENDFILE
	return sendfile_generic(out_fd, in_fd, &nSendPos, count);
#else
	ssize_t r=sendfile(out_fd, in_fd, &nSendPos, count);

	if(r<0 && (errno == ENOSYS || errno == EINVAL))
		return sendfile_generic(out_fd, in_fd, &nSendPos, count);

	return r;
#endif
}

void TFileItemHolder::dump_status()
{
	tSS fmt;
	log::err("File descriptor table:\n");
	for(const auto& item : mapItems)
	{
		fmt.clear();
		fmt << "FREF: " << item.first << " [" << item.second->usercount << "]:\n";
		if(! item.second)
		{
			fmt << "\tBAD REF!\n";
			continue;
		}
		else
		{
			fmt << "\t" << item.second->m_sPathRel
					<< "\n\tDlRefCount: " << item.second->m_nDlRefsCount
					<< "\n\tState: " << (int)  item.second->m_status
					<< "\n\tFilePos: " << item.second->m_nIncommingCount << " , "
					//<< item.second->m_nRangeLimit << " , "
					<< item.second->m_nSizeChecked << " , "
					<< item.second->m_nSizeCachedInitial
					<< "\n\tGotAt: " << item.second->m_nTimeDlStarted << "\n\n";
		}
		log::err(fmt);
	}
	log::flush();
}


fileitem_with_storage::~fileitem_with_storage()
{
	checkforceclose(m_filefd);

	// done if empty, otherwise might need to perform pending self-destruction
	if (m_sPathRel.empty())
		return;

	cmstring sPathAbs(SABSPATH(m_sPathRel));

	if (m_eDestroy)
	{
		cmstring sPathHead(sPathAbs + ".head");
		if (m_eDestroy >= EDestroyMode::DELETE)
		{
			unlink(sPathAbs.c_str());
			if (m_eDestroy == EDestroyMode::ABANDONED) // no head file there when moved to garbage
				unlink(sPathHead.c_str());
		}
		else
		{
			if (0 != ::truncate(sPathAbs.c_str(), 0))
				unlink(sPathAbs.c_str());
			// build a clean header where only the source can be remembered from
			header h;
			h.frontLine = m_head.frontLine;
			h.set(header::XORIG, h.h[header::XORIG]);
			h.StoreToFile(sPathHead);
		}
	}
	else if(m_bPreallocated)
	{
		Cstat st(sPathAbs);
		if (st)
			truncate(sPathAbs.c_str(), st.st_size); // CHECKED!
	}
}

// special file? When it's rewritten from start, save the old version aside
int fileitem_with_storage::MoveRelease2Sidestore()
{
	if(m_nSizeChecked)
		return 0;
	if(!endsWithSzAr(m_sPathRel, "/InRelease") && !endsWithSzAr(m_sPathRel, "/Release"))
		return 0;
	auto srcAbs = CACHE_BASE + m_sPathRel;
	Cstat st(srcAbs);
	if(st)
	{
		auto tgtDir = CACHE_BASE + cfg::privStoreRelSnapSufix + sPathSep + GetDirPart(m_sPathRel);
		mkdirhier(tgtDir);
		auto sideFileAbs = tgtDir + ltos(st.st_ino) + ltos(st.st_mtim.tv_sec)
				+ ltos(st.st_mtim.tv_nsec);
		return FileCopy(srcAbs, sideFileAbs);
		//return rename(srcAbs.c_str(), sideFileAbs.c_str());
	}
	return 0;
}


void fileitem_with_storage::DlFinish(bool asInCache)
{
	LOGSTARTFUNC

	notifyAll();

	if (m_status >= FIST_COMPLETE)
	{
		LOG("already completed");
		return;
	}

	if (asInCache)
	{
		m_nSizeChecked = m_nSizeCachedInitial;
	}

	// XXX: double-check whether the content length in header matches checked size?

	m_status = FIST_COMPLETE;

	if (cfg::debug & log::LOG_MORE)
		log::misc(tSS() << "Download of " << m_sPathRel << " finished");

	dbgline;

	// we are done! Fix header after chunked transfers?
	if (nullptr == m_head.h[header::CONTENT_LENGTH])
	{
		dbgline;

		if (m_nSizeChecked < 0)
			m_head.del(header::CONTENT_LENGTH);
		else
			m_head.set(header::CONTENT_LENGTH, m_nSizeChecked);

		// only update the file on disk if this item is still shared
		lockguard lockGlobalMap(mapItemsMx);
		if (m_globRef != mapItems.end())
			m_head.StoreToFile(SABSPATHEX(m_sPathRel, ".head"));
	}
}

void fileitem_with_storage::DlPreAlloc(off_t remoteSize)
{
	if (remoteSize < 0)
		return;
#warning restore me
#if false
	if (m_nSizeCachedInitial < 0)
		return;

	if (m_nSizeCachedInitial < (off_t) cfg::allocspace)
	{
		falloc_helper(m_filefd, 0, min(hint_start + hint_length, (off_t) cfg::allocspace));
		m_bPreallocated = true;
	}
#endif
}

void fileitem::DlSetError(acng::fileitem::EDestroyMode kmode)
{
	notifyAll();
	m_status = FIST_DLERROR;
	m_eDestroy = kmode;
}

}
