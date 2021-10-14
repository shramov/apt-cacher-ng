#ifndef MAINTENANCE_H_
#define MAINTENANCE_H_

#include "config.h"

#include <future>

#include "meta.h"
#include "sockio.h"
#include "acbuf.h"
#include "job.h"
#include "ahttpurl.h"

static const std::string sBRLF("<br>\n");

#ifdef DEBUG
#define MTLOGDEBUG(x) { SendFmt << x << sBRLF; }
#define MTLOGASSERT(x, y) {if(!(x)) SendFmt << "<div class=\"ERROR\">" << y << "</div>\n" << sBRLF;}
//#define MTLOGVERIFY(x, y) MTLOGASSERT(x, y)
#else
#define MTLOGASSERT(x, y) {}
#define MTLOGDEBUG(x) {}
//#define MTLOGVERIFY(x, y) x
#endif

#define MAINT_PFX "maint_"

// inprecise maximum size of the send buffer when we start checking for backpressure
#define SENDBUF_TARGET_SIZE 30000

namespace acng
{
class IConnBase;

enum class ESpecialWorkType : int8_t
{
	workTypeDetect,

	// expiration types
	workExExpire,
	workExList,
	workExPurge,
	workExListDamaged,
	workExPurgeDamaged,
	workExTruncDamaged,
	//workBGTEST,
	workUSERINFO,
	workMAINTREPORT,
	workAUTHREQUEST,
	workAUTHREJECT,
	workIMPORT,
	workMIRROR,
	workDELETE,
	workDELETECONFIRM,
	workCOUNTSTATS,
	workSTYLESHEET,
	workTraceStart,
	workTraceEnd,
//		workJStats, // disabled, probably useless
	workTRUNCATE,
	workTRUNCATECONFIRM
};

class BackgroundThreadedItem;

class ACNG_API tSpecialRequest
{
	friend class BackgroundThreadedItem;

protected:

	// common data to be passed through constructors and kept in the base object
	struct tRunParms
	{
		ESpecialWorkType type;
		mstring cmd;
		bufferevent *bev;
		// notify when the sending can be started
		BackgroundThreadedItem* owner;
	};

	size_t m_curInBuf = 0;
	struct bufferevent *m_inOut[2];

public:
	/*!
	 *  @brief Main execution method for maintenance tasks.
	 */
	virtual void Run() =0;

	virtual ~tSpecialRequest();
	static fileitem* Create(ESpecialWorkType wType, const tHttpUrl& url, cmstring& refinedPath, const header& reqHead);

protected:
	tSpecialRequest(tRunParms&& parms);

//	inline void SendChunk(const mstring &x) { SendChunk(x.data(), x.size()); }

	// one of them needs to be set to start transmission
	void SetRawResponseHeader(std::string rawHeader);
	void SetMimeResponseHeader(int statusCode, string_view statusMessage, string_view mimetype);

	void SendChunk(const char *data, size_t size);
	void SendChunkRemoteOnly(const char *data, size_t size);
    void SendChunkRemoteOnly(string_view sv) { return SendChunkRemoteOnly(sv.data(), sv.size()); }
//	inline void SendChunk(const char *x) { SendChunk(x, x?strlen(x):0); }
	void SendChunk(string_view x) { SendChunk(x.data(), x.size()); }
	inline void SendChunk(const tSS &x){ SendChunk(x.data(), x.length()); }
	// for customization in base classes
	virtual void SendChunkLocalOnly(const char* /*data*/, size_t /*size*/) {};
	cmstring & GetMyHostPort();
	LPCSTR m_szDecoFile = nullptr;
	LPCSTR GetTaskName();
	tRunParms m_parms;

private:
	tSpecialRequest(const tSpecialRequest&);
	tSpecialRequest& operator=(const tSpecialRequest&);
	mstring m_sHostPort;

public:
	// dirty little RAII helper to send data after formating it, uses a shared buffer presented
	// to the user via macro
	class tFmtSendObj
	{
	public:
		inline tFmtSendObj(tSpecialRequest *p, bool remoteOnly)
		: m_parent(*p), m_bRemoteOnly(remoteOnly) { }
		inline ~tFmtSendObj()
		{
			if (!m_parent.m_fmtHelper.empty())
			{
				if(m_bRemoteOnly)
					m_parent.SendChunkRemoteOnly(m_parent.m_fmtHelper.data(), m_parent.m_fmtHelper.size());
				else
					m_parent.SendChunk(m_parent.m_fmtHelper);
				m_parent.m_fmtHelper.clear();
			}
		}
		tSpecialRequest &m_parent;
	private:
		tFmtSendObj operator=(const tSpecialRequest::tFmtSendObj&);
		bool m_bRemoteOnly;
	};

#define SendFmt tFmtSendObj(this, false).m_parent.m_fmtHelper
#define SendFmtRemote tFmtSendObj(this, true).m_parent.m_fmtHelper
#define SendChunkSZ(x) SendChunk(WITHLEN(x))

	tSS m_fmtHelper;
};

std::string to_base36(unsigned int val);
static cmstring relKey("/Release"), inRelKey("/InRelease");
static cmstring sfxXzBz2GzLzma[] = { ".xz", ".bz2", ".gz", ".lzma"};
static cmstring sfxXzBz2GzLzmaNone[] = { ".xz", ".bz2", ".gz", ".lzma", ""};
static cmstring sfxMiscRelated[] = { "", ".xz", ".bz2", ".gz", ".lzma", ".gpg", ".diff/Index"};
}

#endif /*MAINTENANCE_H_*/
