
#include "debug.h"
#include "meta.h"
#include "fileitem.h"
#include "header.h"
#include "acfg.h"
#include "acbuf.h"
#include "fileio.h"
#include "aevutil.h"
#include "evabase.h"

#include <algorithm>

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

using namespace std;

namespace acng
{

// this is kept here as global anchor but it can be never set in special setups!
ACNG_API std::shared_ptr<IFileItemRegistry> g_registry;

void fileitem::NotifyObservers()
{
	if (!m_notifier)
		return;
	// XXX: keep it or release ASAP since all observers have left?
	if (!m_notifier->hasObservers())
		m_notifier.reset();
	else
		m_notifier->notify();
}

aobservable::subscription fileitem::Subscribe(const tAction &pokeAction)
{
	if (!m_notifier)
		m_notifier.spawn();
	return m_notifier->subscribe(pokeAction);
}

fileitem::fileitem(string_view sPathRel) :
	m_sPathRel(sPathRel)
{
}

void fileitem::DlRefCountAdd()
{
	ASSERT_IS_MAIN_THREAD;
	m_nDlRefsCount++;
}

void fileitem::DlRefCountDec(int code, string_view reason)
{
	ASSERT_IS_MAIN_THREAD;
	LOGSTARTFUNC;

	m_nDlRefsCount--;
	if (m_nDlRefsCount > 0)
		return; // someone will care...

	// ... otherwise: the last downloader disappeared, needing to tell observers
	NotifyObservers();
	if (m_status < FIST_COMPLETE)
	{
		DlSetError({code, to_string(reason)}, m_eDestroy);
		USRDBG("Download of " << m_sPathRel << " aborted");
	}
}

uint64_t fileitem::TakeTransferCount()
{
	uint64_t ret = m_nIncommingCount;
	m_nIncommingCount = 0;
	return ret;
}

fileitem::FiStatus TFileitemWithStorage::Setup()
{
	LOGSTARTFUNC;

	ASSERT_IS_MAIN_THREAD;

	if (m_status > FIST_FRESH)
		return m_status;

	auto error_clean = [this]()
	{
		m_nSizeCachedInitial = m_nSizeChecked = m_nContentLength = -1;
		m_bWriterMustReplaceFile = true;
		return m_status = FIST_INITED;
	};

	m_status = FIST_INITED;

	cmstring sPathAbs(CACHE_BASE + m_sPathRel);
	m_nSizeCachedInitial = GetFileSize(sPathAbs, -1);
	m_nSizeChecked = -1;

	if (!ParseHeadFromStorage(sPathAbs + ".head", &m_nContentLength, &m_responseModDate, &m_responseOrigin))
	{
		return error_clean();
	}

	LOG("good head");
	// report this for all good loading; for volatile items, only becomes relevant when volatile check is performed
	m_responseStatus = { 200, "OK" };

	if (!IsVolatile())
	{
		if (m_spattr.bHeadOnly)
		{
			return m_status = FIST_DLGOTHEAD;
		}

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
			// no content length known, let's check the remote size
			m_spattr.bVolatile = true;
		}
	}
	LOG("resulting status: " << (int) m_status);
	return m_status;
}

void fileitem::UpdateHeadTimestamp()
{
	if(m_sPathRel.empty())
		return;
	utimes(SZABSPATH(m_sPathRel + ".head"), nullptr);
}

#if 0
std::pair<fileitem::FiStatus, tRemoteStatus> fileitem::WaitForFinish()
{
	lockuniq g(this);
	while (m_status < FIST_COMPLETE)
		wait(g);
	return std::pair<fileitem::FiStatus, tRemoteStatus>(m_status, m_responseStatus);
}

std::pair<fileitem::FiStatus, tRemoteStatus>
fileitem::WaitForFinish(unsigned timeout, const std::function<bool()> &waitInterrupted)
{
	lockuniq g(this);
	while (m_status < FIST_COMPLETE)
	{
		if(wait_for(g, timeout, 1)) // on timeout
		{
			if (waitInterrupted)
			{
				if(waitInterrupted())
					continue;
				return std::pair<fileitem::FiStatus, tRemoteStatus>(FIST_DLERROR,  {500, "E_TIMEOUT"});
			}
		}
	}
	return std::pair<fileitem::FiStatus, tRemoteStatus>(m_status, m_responseStatus);
}
#endif

void TFileitemWithStorage::LogSetError(string_view message, fileitem::EDestroyMode destruction)
{
	LOGSTARTFUNCx(message, (int) destruction);
	USRERR(m_sPathRel << " storage error [" << message << "], check file AND directory permissions, last errno: " << tErrnoFmter());
	DlSetError({500, "Cache Error, check apt-cacher.err"}, destruction);
}

bool TFileitemWithStorage::SaveHeader(bool truncatedKeepOnlyOrigInfo)
{
	auto headPath = SABSPATHEX(m_sPathRel, ".head");
	if (truncatedKeepOnlyOrigInfo)
		return StoreHeadToStorage(headPath, -1, nullptr, &m_responseOrigin);
	return StoreHeadToStorage(headPath, m_nContentLength, &m_responseModDate, &m_responseOrigin);
};

bool fileitem::DlStarted(evbuffer*, size_t, const tHttpDate& modDate, cmstring& origin, tRemoteStatus status, off_t bytes2seek, off_t bytesAnnounced)
{
	LOGSTARTFUNCxs( modDate.view(), status.code, status.msg, bytes2seek, bytesAnnounced);
	ASSERT_IS_MAIN_THREAD;

	NotifyObservers();

	if(m_status >= FIST_DLGOTHEAD)
	{
		dbgline;
		// what's happening? Hot restart? Can only jump to previous position of state appears sane.
		if (m_nContentLength != bytesAnnounced && m_nContentLength != -1)
			return false;
		dbgline;
		if (modDate != m_responseModDate)
			return false;
		dbgline;
		if (bytes2seek > m_nSizeChecked)
			return false;
	}
	else
	{
		m_nContentLength = -1;
	}
	dbgline;

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

ssize_t TFileitemWithStorage::DlConsumeData(evbuffer* src, size_t maxTake)
{
	LOGSTARTFUNC;
	ASSERT_IS_MAIN_THREAD;

	// something might care, most likely... also about BOUNCE action
	NotifyObservers();

	LOG("adding chunk of " << maxTake << " bytes at " << m_nSizeChecked);

	// is this the beginning of the stream?
	if(m_filefd == -1 && !SafeOpenOutFile())
		return RX_ERROR;

	if (AC_UNLIKELY(m_filefd == -1 || m_status < FIST_DLGOTHEAD))
		return LogSetError("Suspicious fileitem status"), RX_ERROR;

	if (m_status > FIST_COMPLETE) // DLSTOP, DLERROR
		return RX_ERROR;

	auto ret = eb_dump_chunks(src, m_filefd, maxTake);
	if (ret > 0)
	{
		m_nSizeChecked += ret;
		m_nIncommingCount += ret;
	}
	else
	{
		ldbg("Dump error?");
	}
	return ret;
}


class TSimpleSender : public fileitem::ICacheDataSender
{
	// ICacheDataSender interface
	unique_eb m_buf = unique_eb(evbuffer_new());
	off_t m_nCursor = 0;
public:
	/**
	 * @brief TSimpleSender takes ownershop of this file descriptor
	 * @param fd
	 * @param fileSize
	 */
	TSimpleSender(int fd, off_t fileSize)
	{
		CHECK_ALLOCATED(*m_buf);
		ASSERT(fileSize > 0);
		ASSERT(fd != -1);
		if (evbuffer_add_file(*m_buf, fd, 0, fileSize))
		{
			justforceclose(fd);
			m_buf.reset();
		}
	}
	ssize_t SendData(bufferevent *target, off_t& callerSendPos, size_t maxTake) override
	{
		if (!m_buf.valid())
			return -1;

		// requested range starts after current offset? drain whatever is needed
		if (callerSendPos < m_nCursor)
			return -1; // user's cursor jumped back?
		if (callerSendPos > m_nCursor)
		{
			if (evbuffer_drain(*m_buf, (callerSendPos - m_nCursor)))
				return -1;
			m_nCursor = callerSendPos;
		}

		auto ret = eb_move_range(*m_buf, besender(target), maxTake, callerSendPos);
		if (ret > 0)
			m_nCursor += ret;
		return ret;
	}
};

/**
 * @brief The tSharedFd struct helps to track the last use of the file in the file segments
 * Since libevent destroys them voluntarily, we need to know when the last was gone.
 */
struct tSharedFd : public tLintRefcounted
{
	int fd = -1;
	tSharedFd(int n) : fd(n) {}
	~tSharedFd() { checkforceclose(fd); }
	static void cbRelease(struct evbuffer_file_segment const *, int, void *arg)
	{
		ASSERT_IS_MAIN_THREAD;
		((tSharedFd*)arg)->__dec_ref();
	}
};

class ACNG_API TSegmentSender : public fileitem::ICacheDataSender
{
	lint_ptr<tSharedFd> m_fd;
	using unique_segment = auto_raii<evbuffer_file_segment*, evbuffer_file_segment_free, nullptr>;
	ev_off_t m_segPos;
	ev_off_t m_segLen;
	unique_segment m_seg;

public:
	TSegmentSender(int fd) : m_fd(make_lptr<tSharedFd>(fd))
	{
	};

	ssize_t SendData(bufferevent *target, off_t& callerSendPos, size_t len) override
	{
		// how much can we actually squeeze from the current segment?
		if (m_seg.valid() && callerSendPos >= m_segPos && callerSendPos < m_segPos)
		{
			auto sendLen = len;
			auto innerOffset = callerSendPos - m_segPos;
			if (callerSendPos + off_t(len) > m_segPos + m_segLen)
				sendLen = m_segPos + m_segLen - callerSendPos;
			if (evbuffer_add_file_segment(besender(target), *m_seg, innerOffset, sendLen))
				return -1;
			len -= sendLen;
			callerSendPos += sendLen;
			// are we good here?
			if (len == 0)
				return len;
		}
		m_segPos = callerSendPos;
		m_segLen = len;
		m_seg.reset(evbuffer_file_segment_new(m_fd->fd, m_segPos, m_segLen, EVBUF_FS_DISABLE_LOCKING));
		if (!m_seg.valid())
			return -1;
		// otherwise release the file when it's finished
		m_fd->__inc_ref();
		evbuffer_file_segment_add_cleanup_cb(*m_seg, tSharedFd::cbRelease, m_fd.get());
		if (evbuffer_add_file_segment(besender(target), *m_seg, 0, len))
			return -1;
		callerSendPos += len;
		return len;
	};
};

std::unique_ptr<fileitem::ICacheDataSender> GetStoredFileSender(cmstring& sPathRel, off_t knownSize, bool considerComplete)
{
	ASSERT_IS_MAIN_THREAD;

	int fd = open(SZABSPATH(sPathRel), O_RDONLY);

	if (fd == -1)
		return std::unique_ptr<fileitem::ICacheDataSender>();

#ifdef HAVE_FADVISE
	posix_fadvise(fd, 0, knownSize, POSIX_FADV_SEQUENTIAL);
#endif

	if (knownSize > 0 && considerComplete)
		return make_unique<TSimpleSender>(fd, knownSize);

	return make_unique<TSegmentSender>(fd);
}

std::unique_ptr<fileitem::ICacheDataSender> TFileitemWithStorage::GetCacheSender()
{
	LOGSTARTFUNC;
	ASSERT_IS_MAIN_THREAD;
	USRDBG("Opening " << m_sPathRel);
	return GetStoredFileSender(m_sPathRel, m_nSizeChecked, m_status == FIST_COMPLETE);
}

bool TFileitemWithStorage::SafeOpenOutFile()
{
	LOGSTARTFUNC;
	checkforceclose(m_filefd);

	if (AC_UNLIKELY(m_spattr.bNoStore))
		return false;

	MoveRelease2Sidestore();

	auto sPathAbs(SABSPATH(m_sPathRel));

	int flags = O_WRONLY | O_CREAT | O_BINARY;

	mkbasedir(sPathAbs);

	auto replace_file = [&]()
	{
		dbgline;
		checkforceclose(m_filefd);
		// special case where the file needs be replaced in the most careful way
		m_bWriterMustReplaceFile = false;
		auto dir = GetDirPart(sPathAbs) + "./";
		auto tname = dir + ltos(rand()) + ltos(rand()) + ltos(rand());
		auto tname2 = dir + ltos(rand()) + ltos(rand()) +ltos(rand());
		// keep the file descriptor later if needed
		unique_fd tmp(open(tname.c_str(), flags, cfg::fileperms));
		if (tmp.m_p == -1)
			return LogSetError("Cannot create cache files"), false;
		dbgline;
		fdatasync(tmp.m_p);
		bool didNotExist = false;
		if (0 != rename(sPathAbs.c_str(), tname2.c_str()))
		{
			if (errno != ENOENT)
				return LogSetError("Cannot move cache files"), false;
			didNotExist = true;
		}
		if (0 != rename(tname.c_str(), sPathAbs.c_str()))
			return LogSetError("Cannot rename cache files"), false;
		if (!didNotExist)
			unlink(tname2.c_str());
		std::swap(m_filefd, tmp.m_p);
		return m_filefd != -1;
	};

	// if ordered or when acting on chunked transfers
	if (m_bWriterMustReplaceFile || m_nContentLength < 0)
	{
		if (!replace_file())
			return false;
		dbgline;
	}
	if (m_filefd == -1)
		m_filefd = open(sPathAbs.c_str(), flags, cfg::fileperms);
	// maybe the old file was a symlink pointing at readonly file
	if (m_filefd == -1 && ! replace_file())
		return false;

	ldbg(sPathAbs << " -- file opened?! returned: " << m_filefd);

	auto sizeOnDisk = lseek(m_filefd, 0, SEEK_END);
	if (sizeOnDisk == -1)
		return LogSetError("Cannot seek in cache files"), false;

	// remote files may shrink! We could write in-place and truncate later,
	// however replacing whole thing from the start seems to be safer option
	if (sizeOnDisk > m_nContentLength)
	{
		dbgline;
		if(! replace_file())
			return false;
		sizeOnDisk = 0;
	}

	ldbg("seek to " << m_nSizeChecked);

	// either confirm start at zero or verify the expected file state on disk
	if (m_nSizeChecked < 0)
		m_nSizeChecked = 0;

	// make sure to be at the right location, this sometimes could go wrong (probably via MoveRelease2Sidestore)
	lseek(m_filefd, m_nSizeChecked, SEEK_SET);

	// that's in case of hot resuming
	if(m_nSizeChecked > sizeOnDisk)
	{
		// hope that it has been validated before!
		return LogSetError("Checked size beyond EOF"), false;
	}

	auto sHeadPath(sPathAbs + ".head");
	ldbg("Storing header as " + sHeadPath);
	if (!SaveHeader(false))
		return LogSetError("Cannot store header"), false;

	// okay, have the stream open
	m_status = FIST_DLBODY;
	NotifyObservers();

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
	ASSERT_IS_MAIN_THREAD;
	DlSetError({500, "Bad Cache Item"}, killFile ? EDestroyMode::DELETE : EDestroyMode::TRUNCATE);
}

mstring TFileitemWithStorage::NormalizePath(cmstring &sPathRaw)
{
	return cfg::stupidfs ? DosEscape(sPathRaw) : sPathRaw;
}

TFileitemWithStorage::~TFileitemWithStorage()
{
	LOGSTARTFUNC;

	if (AC_UNLIKELY(m_spattr.bNoStore))
		return;

	checkforceclose(m_filefd);

	// done if empty, otherwise might need to perform pending self-destruction
	if (m_sPathRel.empty())
		return;

	mstring sPathAbs, sPathHead;
	auto calcPath = [&]() {
		sPathAbs = SABSPATH(m_sPathRel);
		sPathHead = SABSPATH(m_sPathRel) + ".head";
	};

	if (!m_bPureStreamNoStorage)
	{
		ldbg(int(m_eDestroy));

		switch (m_eDestroy)
		{
		case EDestroyMode::KEEP:
		{
			if(m_bPreallocated)
			{
				Cstat st(sPathAbs);
				if (st)
					ignore_value(truncate(sPathAbs.c_str(), st.size())); // CHECKED!
			}
			break;
		}
		case EDestroyMode::TRUNCATE:
		{
			calcPath();
			if (0 != ::truncate(sPathAbs.c_str(), 0))
				unlink(sPathAbs.c_str());
			TFileitemWithStorage::SaveHeader(true);
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
			TFileitemWithStorage::SaveHeader(true);
			break;
		}
		}
	}
}

// special file? When it's rewritten from start, save the old version aside
void TFileitemWithStorage::MoveRelease2Sidestore()
{
	LOGSTARTFUNC
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
		auto sideFileAbs = tgtDir;
		// appending a unique suffix. XXX: evaluate this, still useful?
		auto fpr = st.fpr();
		uint8_t buf[sizeof(fpr)]; // modern compiler should see it and type-pun as needed
		memcpy(buf, &fpr, sizeof(buf));
		sideFileAbs += BytesToHexString(buf, sizeof(buf));
		FileCopy(srcAbs, sideFileAbs);
	}
}


void fileitem::DlFinish(bool forceUpdateHeader)
{
	LOGSTARTFUNC;
	ASSERT_IS_MAIN_THREAD;

	NotifyObservers();

	if (AC_UNLIKELY(m_spattr.bNoStore))
		return;

	if (m_status > FIST_COMPLETE)
	{
		LOG("already completed");
		return;
	}

	// XXX: double-check whether the content length in header matches checked size?

	m_status = FIST_COMPLETE;

	if (cfg::debug & log::LOG_MORE)
		log::misc(tSS() << "Download of " << m_sPathRel << " finished");

	dbgline;
	// we are done! Fix header after chunked transfers?
	if (m_nContentLength < 0 || forceUpdateHeader)
	{
		if (m_nContentLength < 0)
			m_nContentLength = m_nSizeChecked;

		if (m_eDestroy == KEEP)
			SaveHeader(false);
	}
}

void fileitem::DlSetError(const tRemoteStatus& errState, fileitem::EDestroyMode kmode)
{
	ASSERT_IS_MAIN_THREAD;

	NotifyObservers();

	/*
	 * Maybe needs to fuse them, OTOH hard to tell which is the more severe or more meaningful
	 *
	if (m_responseStatus.code < 300)
		m_responseStatus.code = errState.code;
	if (m_responseStatus.msg.empty() || m_responseStatus.msg == "OK")
		m_responseStatus.msg = errState.msg;
		*
		*/
	m_responseStatus = errState;
	m_status = FIST_DLERROR;
	DBGQLOG("Declared FIST_DLERROR: " << m_responseStatus.code << " " << m_responseStatus.msg);
	if (kmode < m_eDestroy)
		m_eDestroy = kmode;
}

void fileitem::ManualStart(int statusCode, mstring statusMessage, mstring mimetype, mstring originOrRedirect, off_t contLen, time_t modTime)
{
	LOGSTARTFUNCs;
	auto q = [pin = as_lptr(this), statusCode, statusMessage, mimetype, originOrRedirect, contLen, modTime]()
	{
		LOGSTARTFUNCs
		ASSERT(!statusMessage.empty());
		pin->m_bLocallyGenerated = true;
		pin->NotifyObservers();
		ldbg(pin->m_status);
		if (pin->m_status > FIST_COMPLETE)
			return; // error-out already
		ASSERT(pin->m_status < FIST_DLGOTHEAD);
		pin->m_responseStatus = {statusCode, std::move(statusMessage)};
		ldbg(statusCode << " " << statusMessage)
		if (!mimetype.empty())
			pin->m_contentType = std::move(mimetype);
		pin->m_responseOrigin = originOrRedirect;
		if (contLen >= 0)
			pin->m_nContentLength = contLen;
		pin->m_responseModDate = tHttpDate(modTime != -1 ? modTime : GetTime());
		if (pin->m_status < FIST_DLGOTHEAD)
			pin->m_status = FIST_DLGOTHEAD;
	};
	if (evabase::IsMainThread())
	{
		dbgline;
		q();
	}
	else
	{
		dbgline;
		evabase::Post(q);
	}
}

TResFileItem::TResFileItem(string_view fileName, string_view mimetype)
	: fileitem("_internal_resource"sv)
{
	m_bLocallyGenerated = true;
	auto fd = open((cfg::confdir + SZPATHSEP).append(fileName).c_str(), O_RDONLY);
	if (fd == -1 && !cfg::suppdir.empty())
		fd = open((cfg::suppdir + SZPATHSEP).append(fileName).c_str(), O_RDONLY);
	if (fd == -1)
	{
		m_contentType = se;
		m_nContentLength = m_nSizeChecked = 0;
		m_status = FIST_COMPLETE;
		m_responseStatus = {404, string("Not Found"sv)};
		return;
	}
	Cstat st(fd);
	m_status = FIST_COMPLETE;
	m_contentType = string(mimetype);
	m_nContentLength = m_nSizeChecked = st.size();
	m_responseModDate = tHttpDate(st.msec());
	m_status = FIST_COMPLETE;
	m_responseStatus = {200, "OK"};
	if (st.size() > 0)
		m_sender = make_unique<TSimpleSender>(fd, st.size());
	else
		justforceclose(fd);
}

}
