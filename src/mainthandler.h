#ifndef MAINTHANDLER_H
#define MAINTHANDLER_H

#include "actypes.h"
#include "maintenance.h"
#include "sockio.h"
#include "acbuf.h"
#include "acres.h"
#include "astrop.h"

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

namespace acng
{
class mainthandler;
class IMaintJobItem;
struct tSpecialWorkDescription;

/**
 * @brief The IMaintJobItem class
 */
class IMaintJobItem : public fileitem
{
protected:
	std::unique_ptr<mainthandler> handler;

	mstring m_extraHeaders;

public:
	IMaintJobItem(std::unique_ptr<mainthandler>&& han, IMaintJobItem* owner);
	virtual ~IMaintJobItem() =default;

	virtual void Send(evbuffer *data) =0;
	virtual void Send(string_view sv) =0;
	/**
	 * @brief Eof signals the peer to stop processing, wherever it currently was.
	 */
	virtual void Eof() =0;

	mainthandler* GetHandler() { return handler.get(); }

	void AddExtraHeaders(mstring appendix);
	cmstring &GetExtraResponseHeaders() override;


	// tExtRefExpirer interface
public:
	void Abandon() override;

};

class ACNG_API mainthandler
		#ifdef DEBUG
		: public Dumpable
		#endif
{
protected:
	friend class BufferedPtItem;

public:
	// common data to be passed through constructors and kept in the base object
	struct tRunParms
	{
		EWorkType type;
		mstring cmd;
		int fd;
		// reference to the carrier item
		IMaintJobItem* owner;
		SomeData* arg;
		acres& res;
	} m_parms;

	/**
	 * @brief m_bItemIsHot flags the current activity on this item (i.e. Run method is being executed, probbly in background
	 */
	std::atomic_bool m_bItemIsHot = false;
	/**
	 * @brief g_sigTaskAbort shall be set to interrupt the thread ASAP
	 */
	std::atomic_bool m_bSigTaskAbort = false;

	// to be released AFTER executing thread is finished but on the main task
	lint_ptr<IMaintJobItem> m_itemLock;

	/*!
	 *  @brief Main execution method for maintenance tasks.
	 */
	virtual void Run() =0;

	virtual ~mainthandler() =default;

	mainthandler(tRunParms&& parms);

protected:
	struct timeval m_startTime;

	void Send(const char *data, size_t size)	{ return Send(string_view(data, size));	}
	void Send(ebstream& data) { return Send(data.be); }
	void Send(evbuffer *data) { m_parms.owner->Send(data); };
	void Send(string_view sv) { m_parms.owner->Send(sv); };

private:
	mainthandler(const mainthandler&);
	mainthandler& operator=(const mainthandler&);
	mstring m_sHostPort;

public:

	inline fileitem& item() { return * m_parms.owner; }
	const tSpecialWorkDescription& desc();

	cmstring & GetMyHostPort();
	/**
	 * @brief GetCacheKey formats something like <internalname>.<seconds>.<microseconds>
	 * @return
	 */
	tSS GetCacheKey();

	/**
	 * @brief Absolute location of the "kill bill".
	 * @return
	 */
	tSS GetKbLocation();

#define SendFmt tFmtSendTempRaii<mainthandler, bSS>(*this).GetFmter()
	bSS m_fmtHelper;
	void SendTempFmt() { Send(m_fmtHelper); m_fmtHelper.clear(); }
	bSS& GetTempFmt() { return m_fmtHelper; }
};

class ACNG_API DeleteHelper
{
protected:
	off_t m_nSpaceReleased = 0;
	YesNoErr DeleteAndAccount(cmstring& path, bool deleteOtherwiseTruncate = true, Cstat* st = nullptr);
};

mainthandler* creatorPrototype(mainthandler::tRunParms&& parms);

struct tSpecialWorkDescription
{
	string_view typeName;
	string_view title;
	string_view trigger;
	decltype(&creatorPrototype) creator;
	unsigned flags;
};

/**
 * @brief GetTaskName resolves a static name for the task type
 * @param type
 * @return Human readable name, guaranteed to be zero-terminated
 */
const tSpecialWorkDescription& GetTaskInfo(EWorkType type);
const unsigned BLOCKING = 0x1; // needs a detached thread
const unsigned FILE_BACKED = 0x2;  // output shall be returned through a tempfile, not directly via pipe
const unsigned EXCLUSIVE = 0x4; // shall be the only action of that kind active; output from an active sesion is shared; requires: FILE_BACKED

inline const tSpecialWorkDescription &mainthandler::desc() { return GetTaskInfo(m_parms.type); }

mainthandler* MakeMaintWorker(mainthandler::tRunParms&& parms);

std::string to_base36(unsigned int val);
static cmstring relKey("/Release"), inRelKey("/InRelease");
static cmstring sfxXzBz2GzLzma[] = { ".xz", ".bz2", ".gz", ".lzma"};
static cmstring sfxXzBz2GzLzmaNone[] = { ".xz", ".bz2", ".gz", ".lzma", ""};
static cmstring sfxMiscRelated[] = { "", ".xz", ".bz2", ".gz", ".lzma", ".gpg", ".diff/Index"};

/**
 * @brief GetTxBufferForBufferedItem Special-use thing to get access to the sending buffer directly
 * This assumes that the handler is made for Buffered threaded items only!
 * @param p
 * @return
 */
evbuffer* GetTxBufferForBufferedItem(fileitem& p);

}

#endif // MAINTHANDLER_H
