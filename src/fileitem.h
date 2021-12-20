#ifndef _FILEITEM_H
#define _FILEITEM_H

#include "actypes.h"
#include "header.h"
#include "httpdate.h"
#include "evabase.h"
#include <map>

namespace acng
{
extern const std::string se;
class fileitem;
class cacheman;
class IFileItemRegistry;
class TFileItemRegistry;
struct tDlJob;

typedef lint_ptr<fileitem> tFileItemPtr;
typedef std::map<mstring, tFileItemPtr> tFiGlobMap;
struct tAppStartStop;

//! Base class containing all required data and methods for communication with the download sources
class ACNG_API fileitem : public tLintRefcounted, public tExtRefExpirer
{
	friend struct tDlJob;
	friend class cacheman;
	friend class TFileItemRegistry;

	lint_ptr<aobservable> m_notifier;

public:

	void NotifyObservers();
	aobservable::subscription Subscribe(const tAction& pokeAction);

	// items carrying those attributes might be shared under controlled circumstances only
	struct tSpecialPurposeAttr
	{
		bool bVolatile = false;
		bool bHeadOnly = false;
		/**
		 * @brief bNoStore Don't store metadata or attempt to touch cached data in the aftermath
		 * Most useful in combination with bHeadOnly
		 */
		bool bNoStore = false;
		off_t nRangeLimit = -1;
		mstring credentials;
	};

	// Life cycle (process states) of a file description item
	enum FiStatus : uint8_t
	{
		FIST_FRESH, // vanilla state
		FIST_INITED, // cache state was checked
		FIST_DLPENDING, // some download task for this item exists
		FIST_DLASSIGNED, // our downloader was assigned (optional)
		FIST_DLGOTHEAD,
		FIST_DLRECEIVING,
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

	/**
	 * @brief Implementation specific sender of (hot) cached data
	 *
	 * This remembers the position of the sent data stream, and resumes sending when pushed with SendData method.
	 */
	class ICacheDataSender
	{
	public:
		/**
		 * @brief SendData moves data from internal cached item descriptor to the target stream.
		 * @param target The bufferevent covering the receiver connection.
		 * @param maxTake Limits the data to be transfered
		 * @return How much data was added to target, -1 on error
		 */
		virtual ssize_t SendData(bufferevent* target, off_t& sendPos, size_t maxTake) { return -1; };
		virtual ~ICacheDataSender() = default;
	};
	/**
	 * @brief GetCacheSender prepares a cache helper object which tracks the progress of data creation
	 * @param mtReady True if the resulting sender agent needs to deliver to another thread (NOTIMPLEMENTED, expected to pass all data through main thread)
	 * @return Invalid pointer if the helper is not usable yet
	 */
	virtual std::unique_ptr<ICacheDataSender> GetCacheSender() =0;

	fileitem(string_view sPathRel);
	virtual ~fileitem() =default;
	
	// initialize file item, return the status
	virtual FiStatus Setup() { return FIST_DLERROR; };
	uint64_t TakeTransferCount();

	// mark the item as complete as-is, assuming that seen size is correct
	// void SetupComplete();

	void UpdateHeadTimestamp();

	uint64_t GetTransferCountUnlocked() { return m_nIncommingCount; }
	FiStatus GetStatus() { setLockGuard; return m_status; }
	off_t GetCheckedSize() { return m_nSizeChecked; }
	bool IsVolatile() { return m_spattr.bVolatile; }
	bool IsHeadOnly() { return m_spattr.bHeadOnly; }
	bool IsLocallyGenerated() { return m_bLocallyGenerated; }
	off_t GetRangeLimit() { return m_spattr.nRangeLimit; }
	/**
	 * @brief GetLastModified returns a sensible modification date if possible
	 * This is the actual value from remote if known and valid or a replacement otherwise
	 * @param unchecked Pass the raw reference even if it's invalid
	 * @return Modification timestamp
	 */
	const tHttpDate& GetLastModified(bool unchecked = false)
	{
		if (m_responseModDate.isSet() || unchecked)
			return m_responseModDate;
		return g_serverStartDate;
	}

	off_t m_nIncommingCount = 0;

	// whatever we found in the cached data initially
	off_t m_nSizeCachedInitial = -1;
	// initially from the file data, then replaced when download has started
	off_t m_nContentLength = -1;

	tRemoteStatus m_responseStatus;
	/** This member has multiple uses; for 302/304 codes, it contains the Location value */
	mstring m_responseOrigin;

	mstring m_contentType = "octet/stream";

protected:

	// trade-off between unneccessary parsing and on-the-heap storage
	tHttpDate m_responseModDate;

	bool m_bPreallocated = false;

	/** Relax the safety-checks on the remote body type */
	bool m_bLocallyGenerated = false;
	/**
	 * The item is usable but data might be fishy, so that file must be removed/replaced on opening.
	 */
	bool m_bWriterMustReplaceFile = false;
	/**
	 * Such item can exist as long as it's used by one client, new creators
	 * for this location must get it out of the way.
	 */
	bool m_bCreateItemMustDisplace = false;

	unsigned m_nDlRefsCount = 0;

	tSpecialPurposeAttr m_spattr;

	off_t m_nSizeChecked = -1;
	FiStatus m_status = FIST_FRESH;
	EDestroyMode m_eDestroy = EDestroyMode::KEEP;
	mstring m_sPathRel;
	time_t m_nTimeDlStarted = 0;

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
	virtual bool DlStarted(evbuffer* rawData, size_t headerLen, const tHttpDate& modDate, cmstring& origin, tRemoteStatus status, off_t bytes2seek, off_t bytesAnnounced);
	/**
	* @return false to abort processing (report error)
	*
	* Fileitem must be locked before by unique lock pointed by uli object.
	*
	* @return Number of bytes consumed, -1 on error
	*/
	virtual ssize_t DlConsumeData(evbuffer*, size_t maxTake) { (void) maxTake; return false; }
	/**
	 * @brief Mark the download as finished, and verify that sizeChecked as sane at that moment or move to error state.
	 */
	virtual void DlFinish(bool forceUpdateHeader);

public:
	/**
	 * @brief Mark this item as defect, optionally so that its data will be invalidated in cache when released
	 *
	 * @param destroyMode Decides the future of data existing in the cache
	 */

	virtual void DlSetError(const tRemoteStatus& errState, EDestroyMode destroyMode);

	/**
	 * @brief SetHeader works similar to DlStarted but with more predefined (for local generation) parameters
	 * @param statusCode
	 * @param statusMessage
	 * @param mimetype
	 * @param originOrRedirect
	 * @param contLen
	 */
	void ManualStart(int statusCode, mstring statusMessage, mstring mimetype = "octet/stream", mstring originOrRedirect = "", off_t contLen = -1, time_t modTime = -1);

protected:
	// flag for shared objects and a self-reference for fast and exact deletion, together with m_globRef
	IFileItemRegistry* m_owner = nullptr;
	tFiGlobMap::iterator m_globRef;

	// callback to store the header data on disk, if implemented
	virtual bool SaveHeader(bool) { return false; }

public:
	/// public proxy to DlSetError with truncation, locking!!
	void MarkFaulty(bool deleteItCompletely = false);
	/// optional method, returns raw header if needed in special implementations
	virtual const std::string& GetRawResponseHeader() { return se; }
	virtual const std::string& GetExtraResponseHeaders() { return se; }

	void DlRefCountAdd();
	void DlRefCountDec(int code, string_view reason);

	// tLintRefcountedIndexable interface
	void Abandon() override;

};

enum class ESharingHow
{
	ALWAYS_TRY_SHARING,
	AUTO_MOVE_OUT_OF_THE_WAY,
	FORCE_MOVE_OUT_OF_THE_WAY
};

// dl item implementation with storage on disk
class TFileitemWithStorage : public fileitem
{
public:
	inline TFileitemWithStorage(cmstring &s) : fileitem(s) {}
	virtual ~TFileitemWithStorage();

	FiStatus Setup() override;

	static mstring NormalizePath(cmstring &sPathRaw);

protected:
	void MoveRelease2Sidestore();
	int m_filefd = -1;

	void LogSetError(string_view message, fileitem::EDestroyMode destruction
				   = fileitem::EDestroyMode::KEEP);

	bool SaveHeader(bool truncatedKeepOnlyOrigInfo) override;
private:
	bool SafeOpenOutFile();

	// fileitem interface
protected:
	ssize_t DlConsumeData(evbuffer*, size_t maxTake) override;

	// fileitem interface
public:
	std::unique_ptr<ICacheDataSender> GetCacheSender() override;
};

class TResFileItem : public fileitem
{
public:
	TResFileItem(string_view fileName, string_view mimetype);
	// fileitem interface
	std::unique_ptr<ICacheDataSender> m_sender;
public:
	std::unique_ptr<ICacheDataSender> GetCacheSender() override { return std::move(m_sender); }
};

}
#endif
