#ifndef _ACLOGGER_H
#define _ACLOGGER_H

#include "acbuf.h"

namespace acng
{

#ifdef DEBUG

struct ACNG_API t_logger
{
	t_logger(const char *szFuncName, const void * ptr, const char* szIndent="   "); // starts the logger, shifts stack depth
	~t_logger();
	tSS & GetFmter(const char *szPrefix = " - ");
	tSS & GetFmter4End();
	void WriteWithContext(const char *pSourceLocation);
	void Write();
private:
	tSS m_strm;
	const char * m_szName, *m_szIndentString;
	std::string m_threadNameBEGIN, m_objectIdEND;
	// don't copy
	t_logger(const t_logger&);
	t_logger operator=(const t_logger&);
};
#define USRDBG(msg) DBGQLOG(msg)
#define USRERR(msg) DBGQLOG(msg)
#else
// print some extra things when user wants debug with non-debug build
#define USRDBG(msg) { if(cfg::debug & log::LOG_DEBUG) {log::err( tSS()<<msg); } }
#define USRERR(msg) {log::err( tSS()<<msg); }
#endif

namespace log
{

extern bool logIsEnabled;

enum ETransferType
	: char
	{
		INDATA = 'I', OUTDATA = 'O', ERRORRQ = 'E'
};

enum ELogFlags : uint8_t
{
	///	@brief Flush log output after each line
	///
	LOG_FLUSH = 1,

	/// @brief Additional error information
	///
	LOG_MORE = 2,

	/// @brief Debug information (basic by default, excessive with -DDEBUG
	///
	LOG_DEBUG = 4,

	/// @brief Print debug information to console and not just apt-cacher.dbg
	///
	LOG_DEBUG_CONSOLE = 8
};

// access internal counters
std::pair<off_t, off_t> GetCurrentCountersInOut();
void ResetOldCounters();
std::pair<off_t, off_t> GetOldCountersInOut(bool calcIncomming = true, bool calcOutgoing = true);

mstring ACNG_API open();
void ACNG_API close(bool bReopen = false, bool truncateDebugLog = false);
void transfer(uint64_t bytesIn, uint64_t bytesOut, cmstring& sClient, cmstring& sPath,
		bool bAsError);

void ACNG_API err(const char *msg, size_t len);
inline void err(string_view msg) { if (logIsEnabled) return err(msg.data(), msg.length()); }
inline void err(LPCSTR msg) { return err(string_view(msg));}
//inline void err(cmstring& msg) { return err(string_view(msg));}

void ACNG_API dbg(const char *msg, size_t len);
inline void dbg(string_view msg)
{
	if(logIsEnabled) dbg(msg.data(), msg.length());
}

void misc(const mstring & sLine, const char cLogType = 'M');

void flush();

void GenerateReport(mstring &);

class tRowData
{
public:
	uint64_t byteIn, byteOut;
	unsigned long reqIn, reqOut;
	time_t from, to;
	tRowData() :
			byteIn(0), byteOut(0), reqIn(0), reqOut(0), from(0), to(0)
	{
	}
	;
	/*
	 tRowData(const tRowData &a) :
	 byteIn(a.byteIn), byteOut(a.byteOut),
	 reqIn(a.reqIn), reqOut(a.reqOut),
	 from(a.from), to(a.to)
	 {
	 };
	 */
private:
	// tRowData & operator=(const tRowData &a);
};

mstring GetStatReport();

}

//#define TIMEFORMAT "%a %d/%m"
#define TIMEFORMAT "%Y-%m-%d %H:%M"

}

#endif
