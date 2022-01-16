#ifndef _JOB_H
#define _JOB_H

#include "config.h"
#include "acbuf.h"
#include "acregistry.h"
#include "sockio.h"
#include "debug.h"
#include "aevutil.h"

namespace acng
{

class IConnBase;
class header;
class acres;

extern uint_fast32_t g_genJobId;
class job
{
public:

	enum eJobResult : short
	{
		R_DONE = 0, R_DISCON = 2, R_WILLNOTIFY
    };
	job(IConnBase& parent) : m_parent(parent) {}

	~job();

	void Prepare(const header &h, bufferevent* be, cmstring& callerHostname, acres& res);
	void PrepareFatalError(string_view errorStatus);
	void PrepareAdmin(const header &h, bufferevent* be, acres& res);
	eJobResult Resume(bool canSend, bufferevent* be);

	uint_fast32_t GetId() { return IFDEBUGELSE(m_id, 0); }
	/** Send a keepalive header if we are still in preparation state */
	bool KeepAlive(bufferevent *bev);

    SUTPRIVATE:

    typedef enum : short
    {
        STATE_NOT_STARTED,
        STATE_SEND_DATA,
        STATE_SEND_CHUNK_HEADER,
        STATE_SEND_CHUNK_DATA,
		STATE_DONE,
		// special states for custom behavior notification
		STATE_DISCO_ASAP, // full failure
		STATE_SEND_BUF_NOT_FITEM // dummy state, only sending head buffer and finish
    } eActivity;

	IConnBase& m_parent;
	TFileItemHolder m_pItem;
	aobservable::subscription m_subKey;
	// std::shared_ptr<SomeData> m_tempData; // local state snapshot for delayed data retrieval
	std::unique_ptr<fileitem::ICacheDataSender> m_dataSender;
#ifdef DEBUG
	uint_fast32_t m_id = g_genJobId++;
#endif
    bool m_bIsHttp11 = true;
	bool m_bIsHeadOnly = false;

	enum EKeepAliveMode : uint8_t
	{
		CLOSE = 'c',
		KEEP,
		UNSPECIFIED
	} m_keepAlive = UNSPECIFIED;

    eActivity m_activity = STATE_NOT_STARTED;
	/**
	 * @brief m_preHeadBuf collects header data which shall be sent out ASAP.
	 *
	 * Initialized by GetFmtBuf, invalidated after sending the contents.
	 */
	unique_eb m_preHeadBuf;
    mstring m_sFileLoc; // local_relative_path_to_file
    mstring m_xff;

    tHttpDate m_ifMoSince;
    off_t m_nReqRangeFrom = -1, m_nReqRangeTo = -1;
    off_t m_nSendPos = 0;
    off_t m_nChunkEnd = -1;
    off_t m_nAllDataCount = 0;

	job(const job&);
	job& operator=(const job&);

	void SetEarlySimpleResponse(string_view message, bool nobody = false);

	inline bool ParseRangeAndIfMo(const header& h, acres& res);
	inline void CookResponseHeader();
	inline void AddPtHeader(cmstring& remoteHead);
	/**
	 * @brief HandleSuddenError sets the state to handle the errors
	 * @return True to continue on this job, false to disconnect ASAP
	 */
	inline bool HandleSuddenError();
	inline void AppendMetaHeaders();
	inline void PrependHttpVariant();
	inline eJobResult subscribeAndExit(int IFDEBUG(line));
	/**
	 * @brief GetBufFmter prepares the formatting buffer
	 * @return Format object usable for convenient data adding, which is sent ASAP in the next operation cycles
	 */
	inline ebstream GetBufFmter();
};

}

#endif
