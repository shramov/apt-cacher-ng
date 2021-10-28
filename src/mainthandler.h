#ifndef MAINTHANDLER_H
#define MAINTHANDLER_H

#include "actypes.h"
#include "maintenance.h"
#include "sockio.h"
#include "acbuf.h"

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

/**
 * @brief GetTaskName resolves a static name for the task type
 * @param type
 * @return Human readable name, guaranteed to be zero-terminated
 */
string_view GetTaskName(EWorkType type);

class BufferedPtItemBase : public fileitem
{
public:
	using fileitem::fileitem;

	struct bufferevent *m_pipeInOut[2];	
	struct evbuffer* PipeTx() { return bufferevent_get_output(m_pipeInOut[0]); }
	struct evbuffer* PipeRx() { return bufferevent_get_input(m_pipeInOut[1]); }

	virtual void AddExtraHeaders(mstring appendix) =0;
};

class ACNG_API tSpecialRequestHandler
{
	friend class BackgroundThreadedItem;

public:
	// common data to be passed through constructors and kept in the base object
	struct tRunParms
	{
		EWorkType type;
		mstring cmd;
		int fd;
		// reference to the carrier item
		BufferedPtItemBase& output;
		void* arg;
		lint_ptr<fileitem> pin();
	};

	/*!
	 *  @brief Main execution method for maintenance tasks.
	 */
	virtual void Run() =0;

	virtual ~tSpecialRequestHandler();

	tSpecialRequestHandler(tRunParms&& parms);

protected:

	evbuffer* PipeTx() { return m_parms.output.PipeTx(); }

	// for customization in base classes
	virtual void SendChunkLocalOnly(const char* /*data*/, size_t /*size*/) {};
	virtual void SendChunkLocalOnly(ebstream&) {};

	void SendChunkRemoteOnly(const char *data, size_t size)
	{
		return SendChunkRemoteOnly(string_view(data, size));
	}
	inline void SendChunkRemoteOnly(ebstream& data) { return SendChunkRemoteOnly(data.be);}
	void SendChunkRemoteOnly(evbuffer *data);
	void SendChunkRemoteOnly(string_view sv);

	void SendChunk(const char *data, size_t len)
	{
		SendChunkLocalOnly(data, len);
		SendChunkRemoteOnly(data, len);
	}
	void SendChunk(string_view x) { SendChunk(x.data(), x.size()); }
	// this will eventually move data from there to output
	void SendChunk(ebstream& x)
	{
		SendChunkLocalOnly(x);
		SendChunkRemoteOnly(x);
	}

	cmstring & GetMyHostPort();
	LPCSTR m_szDecoFile = nullptr;
	tRunParms m_parms;

private:
	tSpecialRequestHandler(const tSpecialRequestHandler&);
	tSpecialRequestHandler& operator=(const tSpecialRequestHandler&);
	mstring m_sHostPort;

public:
	// dirty little RAII helper to send data after formating it, uses a shared
	// buffer presented to the user via macro. This two-stage design should
	// reduce needed locking operations on the output.
	class tFmtSendObj
	{
	public:
		inline tFmtSendObj(tSpecialRequestHandler *p, bool remoteOnly)
		: m_parent(*p), m_bRemoteOnly(remoteOnly) { }
		inline ~tFmtSendObj()
		{
			if(m_bRemoteOnly)
				m_parent.SendChunkRemoteOnly(m_parent.m_fmtHelper);
			else
				m_parent.SendChunk(m_parent.m_fmtHelper);
			m_parent.m_fmtHelper.clear();
		}
		tSpecialRequestHandler &m_parent;
	private:
		tFmtSendObj operator=(const tSpecialRequestHandler::tFmtSendObj&) = delete;
		bool m_bRemoteOnly;
	};

#define SendFmt tFmtSendObj(this, false).m_parent.m_fmtHelper
#define SendFmtRemote tFmtSendObj(this, true).m_parent.m_fmtHelper
#define SendChunkSZ(x) SendChunk(WITHLEN(x))

	bSS m_fmtHelper;

	bool m_bNeedsBgThread = true;
};

std::string to_base36(unsigned int val);
static cmstring relKey("/Release"), inRelKey("/InRelease");
static cmstring sfxXzBz2GzLzma[] = { ".xz", ".bz2", ".gz", ".lzma"};
static cmstring sfxXzBz2GzLzmaNone[] = { ".xz", ".bz2", ".gz", ".lzma", ""};
static cmstring sfxMiscRelated[] = { "", ".xz", ".bz2", ".gz", ".lzma", ".gpg", ".diff/Index"};
}

#endif // MAINTHANDLER_H
