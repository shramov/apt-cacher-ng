#ifndef MAINTHANDLER_H
#define MAINTHANDLER_H

#include "actypes.h"
#include "maintenance.h"
#include "sockio.h"
#include "acbuf.h"
#include "acres.h"

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

class IMaintJobItem : public fileitem
{
public:
	using fileitem::fileitem;
	virtual evbuffer* PipeTx() =0;
	virtual evbuffer* PipeRx() =0;
	virtual void AddExtraHeaders(mstring appendix) =0;
};

class BufferedPtItemBase : public IMaintJobItem
{
protected:
	struct bufferevent *m_pipeInOut[2];
public:
	using IMaintJobItem::IMaintJobItem;
	struct evbuffer* PipeTx() override { return bufferevent_get_output(m_pipeInOut[0]); }
	struct evbuffer* PipeRx() override { return bufferevent_get_input(m_pipeInOut[1]); }
};

class ACNG_API tSpecialRequestHandler
{
protected:
	friend class BackgroundThreadedItem;

public:
	// common data to be passed through constructors and kept in the base object
	struct tRunParms
	{
		EWorkType type;
		mstring cmd;
		int fd;
		// reference to the carrier item
		fileitem* owner;
		SomeData* arg;
		acres& res;

		// provide access to BufferedPtItemBase typed jobs
		IMaintJobItem& bitem() { return * static_cast<BufferedPtItemBase*>(owner); }
	};

	/*!
	 *  @brief Main execution method for maintenance tasks.
	 */
	virtual void Run() =0;

	virtual ~tSpecialRequestHandler();

	tSpecialRequestHandler(tRunParms&& parms);

protected:

	evbuffer* PipeTx() { return m_parms.bitem().PipeTx(); }

	// for customization in base classes
	virtual void SendLocalOnly(const char* /*data*/, size_t /*size*/) {};
	virtual void SendLocalOnly(ebstream&) {};

	void SendRemoteOnly(const char *data, size_t size)
	{
		return SendRemoteOnly(string_view(data, size));
	}
	inline void SendRemoteOnly(ebstream& data) { return SendRemoteOnly(data.be);}
	void SendRemoteOnly(evbuffer *data);
	void SendRemoteOnly(string_view sv);

	void Send(const char *data, size_t len)
	{
		SendLocalOnly(data, len);
		SendRemoteOnly(data, len);
	}
	void Send(string_view x) { Send(x.data(), x.size()); }
	// this will eventually move data from there to output
	void Send(ebstream& x)
	{
		SendLocalOnly(x);
		SendRemoteOnly(x);
	}

	cmstring & GetMyHostPort();
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
				m_parent.SendRemoteOnly(m_parent.m_fmtHelper);
			else
				m_parent.Send(m_parent.m_fmtHelper);
			m_parent.m_fmtHelper.clear();
		}
		tSpecialRequestHandler &m_parent;
	private:
		tFmtSendObj operator=(const tSpecialRequestHandler::tFmtSendObj&) = delete;
		bool m_bRemoteOnly;
	};

#define SendFmt tFmtSendObj(this, false).m_parent.m_fmtHelper
#define SendFmtRemote tFmtSendObj(this, true).m_parent.m_fmtHelper
#define SendChunkSZ(x) Send(WITHLEN(x))

	bSS m_fmtHelper;
};


tSpecialRequestHandler* creatorPrototype(tSpecialRequestHandler::tRunParms&& parms);

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
const unsigned EXCLUSIVE = 0x4; // shall be the only action of that kind active; output from an active sesion is shared; requires: m_bFileBacked


std::string to_base36(unsigned int val);
static cmstring relKey("/Release"), inRelKey("/InRelease");
static cmstring sfxXzBz2GzLzma[] = { ".xz", ".bz2", ".gz", ".lzma"};
static cmstring sfxXzBz2GzLzmaNone[] = { ".xz", ".bz2", ".gz", ".lzma", ""};
static cmstring sfxMiscRelated[] = { "", ".xz", ".bz2", ".gz", ".lzma", ".gpg", ".diff/Index"};
}

#endif // MAINTHANDLER_H
