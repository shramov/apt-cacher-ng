#ifndef _JOB_H
#define _JOB_H

#include "config.h"
#include "header.h"
#include "acbuf.h"
#include <sys/types.h>
#include "fileitem.h"
#include "maintenance.h"

namespace acng
{

class ISharedConnectionResources;
namespace rex {
enum eMatchType : int8_t;
}

class job
{
public:

    enum eJobResult : short
	{
		R_DONE = 0, R_AGAIN = 1, R_DISCON = 2, R_NOTFORUS = 3
    };

    job(ISharedConnectionResources &pParent) : m_pParentCon(pParent) {}
	~job();

    void Prepare(const header &h, string_view headBuf);

	/*
	 * Start or continue returning the file.
	 */
	eJobResult SendData(int confd, bool haveMoreJobs);

    SUTPRIVATE:

    typedef enum : short
    {
        STATE_NOT_STARTED,
        STATE_SEND_DATA,
        STATE_SEND_CHUNK_HEADER,
        STATE_SEND_CHUNK_DATA,
        STATE_DONE
    } eActivity;

	TFileItemHolder m_pItem;

	unique_fd m_filefd;    
    bool m_bIsHttp11 = true;
    ISharedConnectionResources &m_pParentCon;

	enum EKeepAliveMode : uint8_t
	{
		// stay away from boolean, for easy ORing
		CLOSE = 0x10,
		KEEP,
		UNSPECIFIED
	} m_keepAlive = UNSPECIFIED;

    eActivity m_activity = STATE_NOT_STARTED;

	tSS m_sendbuf;
    mstring m_sFileLoc; // local_relative_path_to_file
    mstring m_xff;
	tSpecialRequest::eMaintWorkType m_eMaintWorkType = tSpecialRequest::workNotSpecial;

    tHttpDate m_ifMoSince;
    off_t m_nReqRangeFrom = -1, m_nReqRangeTo = -1;
    off_t m_nSendPos = 0;
    off_t m_nChunkEnd = -1;
    off_t m_nAllDataCount = 0;
	rex::eMatchType m_type = (rex::eMatchType) -1;

	job(const job&);
	job& operator=(const job&);

    void CookResponseHeader();
    void AddPtHeader(cmstring& remoteHead);
	fileitem::FiStatus _SwitchToPtItem();
    void SetEarlyErrorResponse(string_view message);
	void PrepareLocalDownload(const mstring &visPath, const mstring &fsBase,
			const mstring &fsSubpath);

    bool ParseRange(const header& h);
	eJobResult HandleSuddenError();
    void AppendMetaHeaders();
    tSS& PrependHttpVariant();

};

class tTraceData: public tStrSet, public base_with_mutex
{
public:
	static tTraceData& getInstance();
};

}

#endif
