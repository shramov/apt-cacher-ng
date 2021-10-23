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
string_view GetTaskName(ESpecialWorkType type);

class BufferedPtItemBase : public fileitem
{
public:
	using fileitem::fileitem;

	struct bufferevent *m_pipeInOut[2];

	/**
	 * @brief SetHeader works similar to DlStarted but with more predefined (for local generation) parameters
	 * @param statusCode
	 * @param statusMessage
	 * @param mimetype
	 * @param originOrRedirect
	 * @param contLen
	 */
	virtual BufferedPtItemBase& ConfigHeader(int statusCode, string_view statusMessage, string_view mimetype = "", string_view originOrRedirect = "", off_t contLen = -1) =0;
	virtual BufferedPtItemBase& AddExtraHeaders(mstring appendix) =0;
};

class ACNG_API tSpecialRequestHandler
{
	friend class BackgroundThreadedItem;

public:
	// common data to be passed through constructors and kept in the base object
	struct tRunParms
	{
		ESpecialWorkType type;
		mstring cmd;
		int fd;
		// reference to the carrier item
		BufferedPtItemBase& output;
		lint_ptr<fileitem> pin();
	};

	/*!
	 *  @brief Main execution method for maintenance tasks.
	 */
	virtual void Run() =0;

	virtual ~tSpecialRequestHandler();

	virtual bool IsNonBlocking() const { return false; }

	tSpecialRequestHandler(tRunParms&& parms);

protected:

	evbuffer* PipeIn() { return bufferevent_get_input(m_parms.output.m_pipeInOut[0]); }
	evbuffer* PipeOut() { return bufferevent_get_output(m_parms.output.m_pipeInOut[1]); }

	// for customization in base classes
	virtual void SendChunkLocalOnly(const char* /*data*/, size_t /*size*/) {};
	virtual void SendChunkLocalOnly(beview&) {};

	void SendChunkRemoteOnly(const char *data, size_t size)
	{
		return SendChunkRemoteOnly(string_view(data, size));
	}
	void SendChunkRemoteOnly(beview& data);
	void SendChunkRemoteOnly(evbuffer *data);
	void SendChunkRemoteOnly(string_view sv);

	void SendChunk(const char *data, size_t len)
	{
		SendChunkLocalOnly(data, len);
		SendChunkRemoteOnly(data, len);
	}
	void SendChunk(string_view x) { SendChunk(x.data(), x.size()); }
	// this will eventually move data from there to output
	void SendChunk(beview& x)
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
			if (0 == evbuffer_get_length(m_parent.m_fmtHelper.be))
				return;
			if(m_bRemoteOnly)
				m_parent.SendChunkRemoteOnly(m_parent.m_fmtHelper.be);
			else
				m_parent.SendChunk(m_parent.m_fmtHelper);
			m_parent.m_fmtHelper.clear();
		}
		tSpecialRequestHandler &m_parent;
	private:
		tFmtSendObj operator=(const tSpecialRequestHandler::tFmtSendObj&);
		bool m_bRemoteOnly;
	};

#define SendFmt tFmtSendObj(this, false).m_parent.m_fmtHelper
#define SendFmtRemote tFmtSendObj(this, true).m_parent.m_fmtHelper
#define SendChunkSZ(x) SendChunk(WITHLEN(x))

	bSS m_fmtHelper;
};

std::string to_base36(unsigned int val);
static cmstring relKey("/Release"), inRelKey("/InRelease");
static cmstring sfxXzBz2GzLzma[] = { ".xz", ".bz2", ".gz", ".lzma"};
static cmstring sfxXzBz2GzLzmaNone[] = { ".xz", ".bz2", ".gz", ".lzma", ""};
static cmstring sfxMiscRelated[] = { "", ".xz", ".bz2", ".gz", ".lzma", ".gpg", ".diff/Index"};
}

#endif // MAINTHANDLER_H
