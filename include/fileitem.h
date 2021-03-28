
#ifndef _FILEITEM_H
#define _FILEITEM_H

#include <string>

#include "config.h"
#include "lockable.h"
#include "header.h"
#include "fileio.h"
#include <unordered_map>

namespace acng
{

class fileitem;
struct tDlJob;
typedef std::shared_ptr<fileitem> tFileItemPtr;
typedef std::unordered_map<mstring, tFileItemPtr> tFiGlobMap;


//! Base class containing all required data and methods for communication with the download sources
class ACNG_API fileitem : public base_with_condition
{
	friend struct tDlJob;
public:

	// Life cycle (process states) of a file description item
	enum FiStatus : uint8_t
	{

	FIST_FRESH, FIST_INITED, FIST_DLPENDING, FIST_DLGOTHEAD, FIST_DLRECEIVING,
	FIST_COMPLETE,
	// error cases: downloader reports its error or last user told downloader to stop
	FIST_DLERROR,
	FIST_DLSTOP // assumed to not have any users left
	};

	enum EDestroyMode : uint8_t
	{
		KEEP
		, TRUNCATE
		, DELETE
		, ABANDONED /* delete silently */
		, DELETE_KEEP_HEAD /* if damaged but maint. code shall find the traces */
	};

	virtual ~fileitem();
	
	// initialize file item, return the status
	virtual FiStatus Setup(bool bDynType);
	
	virtual unique_fd GetFileFd();
	uint64_t TakeTransferCount();
	uint64_t GetTransferCountUnlocked() { return m_nIncommingCount; }
	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow)=0;
	
	header const & GetHeaderUnlocked();
	inline header GetHeader() { setLockGuard; return m_head; }
	mstring GetHttpMsg();
	
	FiStatus GetStatus() { setLockGuard; return m_status; }
	FiStatus GetStatusUnlocked(off_t &nGoodDataSize) { nGoodDataSize = m_nSizeChecked; return m_status; }

	//! returns true if complete or DL not started yet but partial file is present and contains requested range and file contents is static
	bool CheckUsableRange_unlocked(off_t nRangeLastByte);

	// returns when the state changes to complete or error
	FiStatus WaitForFinish(int *httpCode=nullptr);

	FiStatus WaitForFinish(int *httpCode, unsigned check_interval, const std::function<void()> &check_func);
	
	/// mark the item as complete as-is, assuming that seen size is correct
	void SetupComplete();

	void UpdateHeadTimestamp();

	uint64_t m_nIncommingCount = 0;

	// whatever we found in the cached data initially
	off_t m_nSizeCachedInitial = -1, m_nContLenInitial = -1;

	bool m_bVolatile = true;

protected:

	bool m_bPreallocated = false;
	bool m_bReplaceOnOpen = false;

	unsigned m_nDlRefsCount = 0;

	fileitem();

	off_t m_nSizeChecked = -1;
	header m_head;
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
	 * @param rawHeader
	 * @return true when accepted
	 */
	virtual bool DlStarted(header h, string_view rawHeader, off_t bytes2seek) { return false;}
	/**
	* @return false to abort processing (report error)
	*
	* Fileitem must be locked before.
	*
	*/
	virtual bool DlAddData(string_view chunk)  { return false;};
	/**
	 * @brief Mark the download as finished, and verify that sizeChecked as sane at that moment or move to error state.
	 * @param asInCache Special case, if sizeInitial > sizeChecked, consider download done and sizeCheck equals sizeInitial
	 */
	virtual void DlFinish(bool asInCache = false)  {};

	/** Tweak FS to receive a file of remoteSize in one sequence,
	 * considering current m_nSizeChecked as well.
	 */
	virtual void DlPreAlloc(off_t remoteSize) {};


	virtual void DlRefCountAdd();
	virtual void DlRefCountDec(mstring sReasonStatusLine);

	/**
	 * @brief Mark this item as defect so its data will be invalidate in cache when released
	 *
	 * @param erase Delete the internal files if true, only truncate if false
	 */

	virtual void DlSetError(EDestroyMode);


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
	static TFileItemHolder Create(cmstring &sPathUnescaped, ESharingHow how =
			ESharingHow::ALWAYS_TRY_SHARING) WARN_UNUSED;

	// related to GetRegisteredFileItem but used for registration of custom file item
	// implementations created elsewhere (which still need to obey regular work flow)
	static TFileItemHolder Create(tFileItemPtr spCustomFileItem, bool isShareable)  WARN_UNUSED;

	//! @return: true iff there is still something in the pool for later cleaning
	static time_t BackgroundCleanup();

	static void dump_status();

	// when copied around, invalidates the original reference
	~TFileItemHolder();
	inline tFileItemPtr get() {return m_ptr;}
	// invalid dummy constructor
	inline TFileItemHolder() {}

	TFileItemHolder(const TFileItemHolder &src) = delete;
	TFileItemHolder& operator=(const TFileItemHolder &src) = delete;
	TFileItemHolder& operator=(TFileItemHolder &&src) { m_ptr.swap(src.m_ptr); return *this; }
	TFileItemHolder(TFileItemHolder &&src) { m_ptr.swap(src.m_ptr); };

private:

	tFileItemPtr m_ptr;
	explicit TFileItemHolder(tFileItemPtr p) : m_ptr(p) {}
	void AddToProlongedQueue(tFileItemPtr&& p, time_t expTime);
};

// dl item implementation with storage on disk
class fileitem_with_storage : public fileitem
{
public:
	inline fileitem_with_storage(cmstring &s) {m_sPathRel=s;};
	virtual ~fileitem_with_storage();
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
	void DlPreAlloc(off_t remoteSize) override;
	bool DlStarted(acng::header h, acng::string_view rawHeader, off_t bytes2seek) override;
	void DlFinish(bool asInCache) override;

	void SETERROR(string_view x);

	bool withError(string_view message, fileitem::EDestroyMode destruction
			= fileitem::EDestroyMode::KEEP);

private:
	bool SafeOpenOutFile();
};


}
#endif
