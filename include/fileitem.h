
#ifndef _FILEITEM_H
#define _FILEITEM_H

#include <string>

#include "config.h"
#include "lockable.h"
#include "header.h"
#include "fileio.h"
#include "httpdate.h"
#include <unordered_map>

namespace acng
{

class fileitem;
struct tDlJob;
class cacheman;
typedef std::shared_ptr<fileitem> tFileItemPtr;
typedef std::unordered_map<mstring, tFileItemPtr> tFiGlobMap;

//! Base class containing all required data and methods for communication with the download sources
class ACNG_API fileitem : public base_with_condition
{
	friend struct tDlJob;
    friend class cacheman;
public:

    // items carrying those attributes might be shared under controlled circumstances only
    struct tSpecialPurposeAttr
    {
        bool bVolatile = false;
        bool bHeadOnly = false;
        off_t nRangeLimit = -1;
        mstring credentials;
    };

	// Life cycle (process states) of a file description item
	enum FiStatus : uint8_t
	{

	FIST_FRESH, FIST_INITED, FIST_DLPENDING, FIST_DLGOTHEAD, FIST_DLRECEIVING,
	FIST_COMPLETE,
	// error cases: downloader reports its error or last user told downloader to stop
	FIST_DLERROR,
	FIST_DLSTOP // assumed to not have any users left
	};

    /**
     * @brief The EDestroyMode enum
     * Defines which data is needed to be deleted when this item terminates.
     * Ordered by "severity", including how much data will be lost afterwards.
     * @see DlSetError
     */
	enum EDestroyMode : uint8_t
	{
		KEEP
        , DELETE_KEEP_HEAD /* if damaged but maint. code shall find the traces laster */
        , TRUNCATE
        , ABANDONED /* similar to DELETE but head might be gone already might gone already */
        , DELETE
    };

	virtual ~fileitem();
	
	// initialize file item, return the status
    virtual FiStatus Setup() { return FIST_DLERROR; };
	
	virtual unique_fd GetFileFd();
	uint64_t TakeTransferCount();
	uint64_t GetTransferCountUnlocked() { return m_nIncommingCount; }
	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow)=0;

	FiStatus GetStatus() { setLockGuard; return m_status; }
	FiStatus GetStatusUnlocked(off_t &nGoodDataSize) { nGoodDataSize = m_nSizeChecked; return m_status; }
    FiStatus GetStatusUnlocked() { return m_status; }

//	//! returns true if complete or DL not started yet but partial file is present and contains requested range and file contents is static
//	bool CheckUsableRange_unlocked(off_t nRangeLastByte);

	// returns when the state changes to complete or error
    std::pair<FiStatus, tRemoteStatus> WaitForFinish();

    std::pair<FiStatus, tRemoteStatus> WaitForFinish(unsigned check_interval, const std::function<void()> &check_func);
	
	/// mark the item as complete as-is, assuming that seen size is correct
	void SetupComplete();

    void UpdateHeadTimestamp();

    bool IsVolatile() { return m_spattr.bVolatile; }
    bool IsHeadOnly() { return m_spattr.bHeadOnly; }
    off_t GetRangeLimit() { return m_spattr.nRangeLimit; }

	uint64_t m_nIncommingCount = 0;

	// whatever we found in the cached data initially
    off_t m_nSizeCachedInitial = -1;
    // initially from the file data, then replaced when download has started
    off_t m_nContentLength = -1;

    tRemoteStatus m_responseStatus;
    /** This member has multiple uses; for 302/304 codes, it contains the Location value */
    mstring m_responseOrigin;
    // trade-off between unneccessary parsing and on-the-heap storage
    tHttpDate m_responseModDate;

    string_view m_contentType = "octet/stream";

protected:

	bool m_bPreallocated = false;
	/**
	 * The item is usable but data file must be removed/replaced on opening.
	 */
	bool m_bWriterMustReplaceFile = false;
	/**
	 * Such item can exist as long as it's used by one client, new creators
	 * for this location must get it out of the way.
	 */
	bool m_bCreateItemMustDisplace = false;

	unsigned m_nDlRefsCount = 0;

    tSpecialPurposeAttr m_spattr;

	fileitem();

	off_t m_nSizeChecked = -1;
	std::atomic_int usercount = ATOMIC_VAR_INIT(0);
	FiStatus m_status = FIST_FRESH;
	EDestroyMode m_eDestroy = EDestroyMode::KEEP;
	mstring m_sPathRel;
	time_t m_nTimeDlStarted;

	/*************************************
	 *
	 * Dl* methods are used by the downloader. All access must happen in locked mode.
	 *
	 *************************************/

	/**
	 * Mark the beginning of a download, with a header to be consumed and
	 * stored. Responsible to update m_nSizeChecked and m_status accordingly.
	 *
	 * Fileitem must be locked before.
	 *
	 * @param h Already preprocessed header
     * @param rawHeader Original header contents as memory chunk
     * @param semiRawHeader The incoming processing header which was already partly processed for the remaining parameters
	 * @return true when accepted
	 */
    virtual bool DlStarted(string_view rawHeader, const tHttpDate& modDate, cmstring& origin, tRemoteStatus status, off_t bytes2seek, off_t bytesAnnounced);
	/**
	* @return false to abort processing (report error)
	*
	* Fileitem must be locked before.
	*
	*/
    virtual bool DlAddData(string_view)  { return false;};
	/**
	 * @brief Mark the download as finished, and verify that sizeChecked as sane at that moment or move to error state.
	 * @param asInCache Special case, if sizeInitial > sizeChecked, consider download done and sizeCheck equals sizeInitial
	 */
    virtual void DlFinish(bool asInCache = false)  { (void) asInCache; };

	virtual void DlRefCountAdd();
    virtual void DlRefCountDec(tRemoteStatus reason);

	/**
	 * @brief Mark this item as defect so its data will be invalidate in cache when released
	 *
	 * @param erase Delete the internal files if true, only truncate if false
	 */

    virtual void DlSetError(const tRemoteStatus& errState, EDestroyMode);

	// this is owned by TFileItemUser and covered by its locking; it serves as flag for shared objects and a self-reference for fast and exact deletion
	tFiGlobMap::iterator m_globRef;
	friend class TFileItemHolder;

public:
	/// public proxy to DlSetError with truncation, locking!!
	void MarkFaulty(bool deleteItCompletely = false);
	/// optional method, returns raw header if needed in special implementations
	virtual const std::string& GetRawResponseHeader() { return sEmptyString; }
};

enum class ESharingHow
{
	ALWAYS_TRY_SHARING,
	AUTO_MOVE_OUT_OF_THE_WAY,
	FORCE_MOVE_OUT_OF_THE_WAY
};

// "owner" of a file item, cares about sharing instances between multiple agents
class TFileItemHolder
{
public:
	// public constructor wrapper, create a sharable item with storage or share an existing one
    static TFileItemHolder Create(cmstring &sPathUnescaped, ESharingHow how, const fileitem::tSpecialPurposeAttr& spattr) WARN_UNUSED;

	// related to GetRegisteredFileItem but used for registration of custom file item
	// implementations created elsewhere (which still need to obey regular work flow)
	static TFileItemHolder Create(tFileItemPtr spCustomFileItem, bool isShareable)  WARN_UNUSED;

	//! @return: true iff there is still something in the pool for later cleaning
	static time_t BackgroundCleanup();

	static void dump_status();

	// when copied around, invalidates the original reference
	~TFileItemHolder();
	inline tFileItemPtr get() { return m_ptr; }
	// invalid dummy constructor
	inline TFileItemHolder() {}

	TFileItemHolder& operator=(const TFileItemHolder &src) = delete;
	TFileItemHolder& operator=(TFileItemHolder &&src) { m_ptr.swap(src.m_ptr); return *this; }
	TFileItemHolder(TFileItemHolder &&src) { m_ptr.swap(src.m_ptr); };

private:

	tFileItemPtr m_ptr;
	explicit TFileItemHolder(tFileItemPtr p) : m_ptr(p) {}
	void AddToProlongedQueue(TFileItemHolder&&, time_t expTime);
};

// dl item implementation with storage on disk
class fileitem_with_storage : public fileitem
{
public:
	inline fileitem_with_storage(cmstring &s) {m_sPathRel=s;};
    virtual ~fileitem_with_storage();

    FiStatus Setup() override;

	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow) override;

	inline static mstring NormalizePath(cmstring &sPathRaw)
	{
		return cfg::stupidfs ? DosEscape(sPathRaw) : sPathRaw;
    }

protected:
	int MoveRelease2Sidestore();
	int m_filefd = -1;

	bool DlAddData(string_view chunk) override;
	void DlFinish(bool asInCache) override;

	bool withError(string_view message, fileitem::EDestroyMode destruction
			= fileitem::EDestroyMode::KEEP);

private:
    bool SaveHeader(bool truncatedKeepOnlyOrigInfo);
    bool SafeOpenOutFile();
};


}
#endif
