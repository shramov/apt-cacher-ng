
//#define LOCAL_DEBUG
#include "debug.h"

#include "meta.h"
#include "fileitem.h"
#include "header.h"
#include "acfg.h"
#include "acbuf.h"
#include "fileio.h"
#include "cleaner.h"
#include "evabase.h"

#include <errno.h>
#include <algorithm>
#include <list>

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
	TFileItemHolder hodler;
	time_t timeExpired;
};

std::list<TExpiredEntry> prolongedLifetimeQ;
static acmutex prolongMx;

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

void fileitem::DlRefCountDec(tRemoteStatus reason)
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
        m_responseStatus = move(reason);

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

fileitem::FiStatus fileitem_with_storage::Setup()
{
    LOGSTARTFUNC

	setLockGuard;

	if (m_status > FIST_FRESH)
		return m_status;

	auto error_clean = [this]()
	{
        m_nSizeCachedInitial = m_nSizeChecked = m_nContentLength = -1;
#warning Users must consider this flag and act accordingly
		m_bWriterMustReplaceFile = true;
		return m_status = FIST_INITED;
	};

	m_status = FIST_INITED;

	cmstring sPathAbs(CACHE_BASE + m_sPathRel);

    if (!ParseHeadFromStorage(SABSPATHEX(sPathAbs, ".head"), &m_nContentLength, &m_responseModDate, &m_responseOrigin))
    {
        if (IsVolatile()) //that's too risky
            return error_clean();
        // no head or missing? Whatever, assume it's ok for solid files for now
        m_nSizeCachedInitial = GetFileSize(sPathAbs, -1);
    }
    else
    {
        LOG("good head");
        if (!IsVolatile())
        {
#warning for range-limited, consider the range limit to be good enough to set FIST_COMPLETE
#warning that only works with not head-only
            // non-volatile files, so could accept the length, do some checks first
            if (m_nContentLength >= 0)
            {
                // file larger than it could ever be?
                if (m_nContentLength < m_nSizeCachedInitial)
                    return error_clean();

                // is it complete? and 0 value also looks weird, try to verify later
                if (m_nSizeCachedInitial == m_nContentLength)
                {
                    m_nSizeChecked = m_nSizeCachedInitial;
                    m_status = FIST_COMPLETE;
                }
                else
                {
                    // otherwise wait for remote to confirm its presence too
                    m_spattr.bVolatile = true;
                }
            }
            else
            {
                // no content length known, assume that it's ok
                m_nSizeChecked = m_nSizeCachedInitial;
            }
        }
    }
	LOG("resulting status: " << (int) m_status);
	return m_status;
}

#warning dead code?
#if 0
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
#endif

#if 0
void fileitem::SetupComplete()
{
	setLockGuard;
	notifyAll();
	m_nSizeChecked = m_nSizeCachedInitial;
	m_status = FIST_COMPLETE;
}
#endif

void fileitem::UpdateHeadTimestamp()
{
	if(m_sPathRel.empty())
		return;
	utimes(SZABSPATH(m_sPathRel + ".head"), nullptr);
}

std::pair<fileitem::FiStatus, tRemoteStatus> fileitem::WaitForFinish()
{
	lockuniq g(this);
    while (m_status < FIST_COMPLETE)
        wait(g);
    return std::pair<fileitem::FiStatus, tRemoteStatus>(m_status, m_responseStatus);
}

std::pair<fileitem::FiStatus, tRemoteStatus> fileitem::WaitForFinish(unsigned check_interval, const std::function<void()> &check_func)
{
	lockuniq g(this);
    while (m_status < FIST_COMPLETE)
	{
        if(wait_for(g, check_interval, 1)) // on timeout
			check_func();
	}
    return std::pair<fileitem::FiStatus, tRemoteStatus>(m_status, m_responseStatus);
}

inline void _LogWithErrno(const char *msg, const string & sFile)
{
	tErrnoFmter f;
	log::err(tSS() << sFile <<
			" storage error [" << msg << "], last errno: " << f);
}

bool fileitem_with_storage::withError(string_view message, fileitem::EDestroyMode destruction)
{
    log::err(tSS() << m_sPathRel << " storage error [" << message
            << "], last errno: " << tErrnoFmter());

    DlSetError({500, "Cache Error, check apt-cacher.err"}, destruction);
	return false;
}

bool fileitem_with_storage::SaveHeader(bool truncatedKeepOnlyOrigInfo)
{
	auto headPath = SABSPATHEX(m_sPathRel, ".head");
	if (truncatedKeepOnlyOrigInfo)
		return StoreHeadToStorage(headPath, -1, nullptr, &m_responseOrigin);
	return StoreHeadToStorage(headPath, m_nContentLength, &m_responseModDate, &m_responseOrigin);
};

bool fileitem::DlStarted(string_view rawHeader, const tHttpDate& modDate, cmstring& origin, tRemoteStatus status, off_t bytes2seek, off_t bytesAnnounced)
{
    LOGSTARTFUNCxs( modDate.any(), status.code, status.msg, bytes2seek, bytesAnnounced);

    m_nContentLength = -1;
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

    m_status=FIST_DLGOTHEAD;
	// if range was confirmed then can already start forwarding that much
	if (bytes2seek >= 0)
	{
        if (m_nSizeChecked >= 0 && bytes2seek < m_nSizeChecked)
			return false;
		m_nSizeChecked = bytes2seek;
	}

    m_responseStatus = move(status);
    m_responseOrigin = move(origin);
    m_responseModDate = modDate;
    m_nContentLength = bytesAnnounced;
	return true;
}

bool fileitem_with_storage::DlAddData(string_view chunk)
{
#warning check all users to have the lock set!
	// something might care, most likely... also about BOUNCE action
	notifyAll();

	m_nIncommingCount += chunk.size();

	// is this the beginning of the stream?
	if(m_filefd == -1 && !SafeOpenOutFile())
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
	LOGSTARTFUNC;
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
			auto err = FileCopy(sPathAbs, temp);
			if (err)
				return withError(err.message());

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
	if (!SaveHeader(false))
		return withError("Cannot store header");

	// either confirm start at zero or verify the expected file state on disk

	if (m_nSizeChecked < 0)
		m_nSizeChecked = 0;
	else
	{
		if (m_nSizeChecked != lseek(m_filefd, 0, SEEK_END))
			return withError("Unexpected file change");
	}

	// okay, have the stream open
	m_status = FIST_DLRECEIVING;

	/** Tweak FS to receive a file of remoteSize in one sequence,
	 * considering current m_nSizeChecked as well.
	 */
	if (cfg::allocspace > 0 && m_nContentLength > 0)
	{
		// XXX: we have the stream size parsed before but storing that member all the time just for this purpose isn't exactly great
		auto preservedSequenceLen = m_nContentLength - m_nSizeChecked;
		if (preservedSequenceLen > (off_t) cfg::allocspace)
			preservedSequenceLen = cfg::allocspace;
		if (preservedSequenceLen > 0)
		{
			falloc_helper(m_filefd, m_nSizeChecked, preservedSequenceLen);
			m_bPreallocated = true;
		}
	}

	return true;
}

void fileitem::MarkFaulty(bool killFile)
{
	setLockGuard
    DlSetError({500, "Bad Cache Item"}, killFile ? EDestroyMode::DELETE : EDestroyMode::TRUNCATE);
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

	// some file items will be held ready for some time
	if (wasGloballyRegistered
			&& !evabase::in_shutdown
			&& MAXTEMPDELAY
            && m_ptr->IsVolatile()
			&& m_ptr->m_status == fileitem::FIST_COMPLETE)
	{
		auto when = m_ptr->m_nTimeDlStarted + MAXTEMPDELAY;
		if (when > GetTime())
		{
			local_ptr->usercount++;
			AddToProlongedQueue(TFileItemHolder(local_ptr), when);
			return;
		}
	}

	// nothing, let's put the item into shutdown state
	if (m_ptr->m_status < fileitem::FIST_COMPLETE)
		m_ptr->m_status = fileitem::FIST_DLSTOP;
    m_ptr->m_responseStatus.msg = "Cache file item expired";
    m_ptr->m_responseStatus.code = 500;

	if (wasGloballyRegistered)
	{
		LOG("*this is last entry, deleting dl/fi mapping");
		mapItems.erase(m_ptr->m_globRef);
		m_ptr->m_globRef = mapItems.end();
	}
	// make sure it's not double-unregistered accidentally!
	m_ptr.reset();
}

void TFileItemHolder::AddToProlongedQueue(TFileItemHolder&& p, time_t expTime)
{
	lockguard g(prolongMx);
	// act like the item is still in use
	prolongedLifetimeQ.emplace_back(TExpiredEntry {move(p), expTime});
	cleaner::GetInstance().ScheduleFor(prolongedLifetimeQ.front().timeExpired,
			cleaner::TYPE_EXFILEITEM);
}

TFileItemHolder TFileItemHolder::Create(cmstring &sPathUnescaped, ESharingHow how, const fileitem::tSpecialPurposeAttr& spattr)
{
    LOGSTARTFUNCxs(sPathUnescaped, int(how));
#warning should have UTs for all combinations
	try
	{
		mstring sPathRel(fileitem_with_storage::NormalizePath(sPathUnescaped));
		lockguard lockGlobalMap(mapItemsMx);
        LOG("Normalized: " << sPathRel );
		auto regnew = [&]()
		{
			LOG("Registering as NEW file item...");
			auto sp(make_shared<fileitem_with_storage>(sPathRel));
			sp->usercount++;
            sp->m_spattr = spattr;
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

        if (how == ESharingHow::ALWAYS_TRY_SHARING || fi->m_bCreateItemMustDisplace)
			return share();

		// detect items that got stuck somehow and move it out of the way
		auto now(GetTime());
        auto makeWay = false;
        if (how == ESharingHow::FORCE_MOVE_OUT_OF_THE_WAY)
            makeWay = true;
        else
        {
            dbgline;
#warning does this only apply to volatile items? is this value updated while downloading?
            makeWay = now > (fi->m_nTimeDlStarted + cfg::stucksecs);
            // check the additional conditions for being perfectly identical
            if (!makeWay && fi->IsVolatile() != spattr.bVolatile)
            {
                // replace if previous was working in solid mode because it does less checks
                makeWay = ! fi->IsVolatile();
            }
            if (!makeWay && spattr.bHeadOnly != fi->IsHeadOnly())
            {
                dbgline;
                makeWay = true;
            }
            if (!makeWay && spattr.nRangeLimit != fi->GetRangeLimit())
            {
                dbgline;
                makeWay = true;
            }
#warning add validation when remote credentials are supported
        }
		if (!makeWay)
            return share();

        // okay, have to move a probably existing cache file out of the way,
        // therefore needing this evasive maneuver
		auto replPathRel = fi->m_sPathRel + "." + ltos(now);
		auto replPathAbs = SABSPATH(replPathRel);
		auto pathAbs = SABSPATH(fi->m_sPathRel);

        // XXX: this check is crap and cannot happen but better double-check!
        if (AC_UNLIKELY(fi->m_sPathRel.empty()))
            return TFileItemHolder();

		auto abandon_replace = [&]() {
			fi->m_sPathRel = replPathAbs;
			fi->m_eDestroy = fileitem::EDestroyMode::ABANDONED;
			fi->m_globRef = mapItems.end();
			mapItems.erase(it);
			return regnew();
		};

		if(0 == link(pathAbs.c_str(), replPathAbs.c_str()))
		{
			// only if it was actually there!
			if (0 == unlink(pathAbs.c_str()) || errno != ENOENT)
				return abandon_replace();
			else // unlink failed but file was there
				log::err(string("Failure to erase stale file item for ") + pathAbs + " - errno: " + tErrnoFmter());
		}
		else
		{
			if (ENOENT == errno) // XXX: replPathAbs doesn't exist but ignore for now
				return abandon_replace();

			log::err(string("Failure to move file out of the way into ")
					+ replPathAbs + " - errno: " + tErrnoFmter());
		}
	}
	catch (std::bad_alloc&)
	{
	}
	return TFileItemHolder();
}

// make the fileitem globally accessible
TFileItemHolder TFileItemHolder::Create(tFileItemPtr spCustomFileItem, bool isShareable)
{
	LOGSTARTFUNCxs(spCustomFileItem->m_sPathRel);

	TFileItemHolder ret;

	if (!spCustomFileItem || spCustomFileItem->m_sPathRel.empty())
		return ret;

    dbgline;
	if(!isShareable)
	{
		ret.m_ptr = spCustomFileItem;
	}

	lockguard lockGlobalMap(mapItemsMx);

    dbgline;
    auto installed = mapItems.emplace(spCustomFileItem->m_sPathRel,
			spCustomFileItem);

	if(!installed.second)
		return ret; // conflict, another agent is already active
    dbgline;
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
	auto now = GetTime();
	LOGSTARTFUNCsx(now);
	// where the destructors eventually do their job on stack unrolling
	decltype(prolongedLifetimeQ) releasedQ;
	lockguard g(prolongMx);
	if (prolongedLifetimeQ.empty())
		return END_OF_TIME;
	auto notExpired = std::find_if(prolongedLifetimeQ.begin(), prolongedLifetimeQ.end(),
			[now](const TExpiredEntry &el) {	return el.timeExpired > now;});
	// grab all before expired element, or even all
	releasedQ.splice(releasedQ.begin(), prolongedLifetimeQ, prolongedLifetimeQ.begin(), notExpired);
	return prolongedLifetimeQ.empty() ? END_OF_TIME : prolongedLifetimeQ.front().timeExpired;
}

ssize_t fileitem_with_storage::SendData(int out_fd, int in_fd, off_t &nSendPos, size_t count)
{
	if(out_fd == -1 || in_fd == -1)
		return -1;

#ifndef HAVE_LINUX_SENDFILE
	return sendfile_generic(out_fd, in_fd, &nSendPos, count);
#else
    auto r = sendfile(out_fd, in_fd, &nSendPos, count);

	if(r<0 && (errno == ENOSYS || errno == EINVAL))
		return sendfile_generic(out_fd, in_fd, &nSendPos, count);

	return r;
#endif
}

mstring fileitem_with_storage::NormalizePath(cmstring &sPathRaw)
{
	return cfg::stupidfs ? DosEscape(sPathRaw) : sPathRaw;
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

    mstring sPathAbs, sPathHead;
    auto calcPath = [&]() {
        sPathAbs = SABSPATH(m_sPathRel);
        sPathHead = SABSPATH(m_sPathRel) + ".head";
    };

    switch (m_eDestroy)
    {
    case EDestroyMode::KEEP:
    {
        if(m_bPreallocated)
        {
            Cstat st(sPathAbs);
            if (st)
                truncate(sPathAbs.c_str(), st.st_size); // CHECKED!
            break;
        }
    }
    case EDestroyMode::TRUNCATE:
    {
        calcPath();
        if (0 != ::truncate(sPathAbs.c_str(), 0))
            unlink(sPathAbs.c_str());
        SaveHeader(true);
        break;
    }
    case EDestroyMode::ABANDONED:
    {
        calcPath();
        unlink(sPathAbs.c_str());
        break;
    }
    case EDestroyMode::DELETE:
    {
        calcPath();
        unlink(sPathAbs.c_str());
        unlink(sPathHead.c_str());
        break;
    }
    case EDestroyMode::DELETE_KEEP_HEAD:
    {
        calcPath();
        unlink(sPathAbs.c_str());
        SaveHeader(true);
        break;
    }
    }
}

// special file? When it's rewritten from start, save the old version aside
void fileitem_with_storage::MoveRelease2Sidestore()
{
	if(m_nSizeChecked)
		return;
	if(!endsWithSzAr(m_sPathRel, "/InRelease") && !endsWithSzAr(m_sPathRel, "/Release"))
		return;
	auto srcAbs = CACHE_BASE + m_sPathRel;
	Cstat st(srcAbs);
	if(st)
	{
		auto tgtDir = CACHE_BASE + cfg::privStoreRelSnapSufix + sPathSep + GetDirPart(m_sPathRel);
		mkdirhier(tgtDir);
		auto sideFileAbs = tgtDir + ltos(st.st_ino) + ltos(st.st_mtim.tv_sec)
				+ ltos(st.st_mtim.tv_nsec);
		FileCopy(srcAbs, sideFileAbs);
	}
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
        m_nSizeChecked = m_nContentLength = m_nSizeCachedInitial;
	}

	// XXX: double-check whether the content length in header matches checked size?

	m_status = FIST_COMPLETE;

	if (cfg::debug & log::LOG_MORE)
		log::misc(tSS() << "Download of " << m_sPathRel << " finished");

	dbgline;

	// we are done! Fix header after chunked transfers?
    if (m_nContentLength < 0)
	{
        m_nContentLength = m_nSizeChecked;
        if (m_eDestroy == KEEP)
            SaveHeader(false);
	}
}

void fileitem::DlSetError(const tRemoteStatus& errState, fileitem::EDestroyMode kmode)
{
	notifyAll();
    m_responseStatus = errState;
	m_status = FIST_DLERROR;
    if (kmode < m_eDestroy)
        m_eDestroy = kmode;
}

}
