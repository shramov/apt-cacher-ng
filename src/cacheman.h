#ifndef _CACHEMAN_H_
#define _CACHEMAN_H_

#include "config.h"
#include "acfg.h"
#include "dirwalk.h"
#include "mainthandler.h"
#include "csmapping.h"
#include "bgtask.h"
#include "conn.h"
#include "meta.h"
#include "rex.h"
#include "acworm.h"

#include <unordered_map>

namespace acng
{

class dlcontroller;

// XXX: specific declarations, maybe move to a namespace
class tDlJobHints;
class tFileGroups;
struct tContentKey;

static cmstring sAbortMsg("<span class=\"ERROR\">Found errors during processing, "
						  "aborting as requested.</span>");

static cmstring sIndex("Index");
static cmstring sslIndex("/Index");

bool CompDebVerLessThan(cmstring &s1, cmstring s2);
extern time_t m_gMaintTimeNow;

void DelTree(cmstring &what);

class TDownloadController;

enum class eDlMsgSeverity
{
	UNKNOWN,
	XDEBUG,	 // extra level for debugging
	VERBOSE, // only shown when verbosity wanted
	POTENTIAL_ERROR, // shown as error but only when debugging with verbosity
	INFO, // default level for the threshold if not quiet
	WARNING,
	NONFATAL_ERROR, // shown as error but shall not abort processing at state check
	ERROR, // count and print as error
	NEVER // filter value: always hide below
};

// extradebug build shall print that by default, regular build only in verbosity mode
#ifdef DEBUG
#define SEV_DBG eDlMsgSeverity::INFO
#else
#define SEV_DBG eDlMsgSeverity::VERBOSE
#endif

/**
 * @brief The cacheman class is a base for various cache maintainer operations.
 * It contains helper methods to assemble the list of relevant index files, and
 * ensuring that they are uptodate, while observing the existing data to get a
 * good idea of where the clients were acting at and which index data is
 * actually required.
 *
 * The later job is not trivial - the client maybe coming and going, may have part of its metadata already updated elsewhere, so that ACNG is kept blind about the remote changes.
 * On top of this, modern APT versions might
 * a) only fetch meta files from by-hash directories, never touching the original location
 * b) keep updating the local metadata from pdiff files, therefore only updating diff Index and fetching related patches, never the whole original description file.
 */
class ACNG_API cacheman :
		public IFileHandler,
		public tExclusiveUserAction,
		public DeleteHelper
{
	friend class TDownloadController;
	/** Little helper needed to deduplicate visiting of files with the same contents. */
	int8_t m_idxPassId = 1;

public:
	cacheman(tRunParms&& parms);
	virtual ~cacheman();

	enum enumMetaType : int8_t
	{
		EIDX_NEVERPROCESS = -2,
		//EIDX_ALREADYPROCESSED = -1, // parsed before and not subject to repeated processing
		EIDX_UNKNOWN = 0,
		EIDX_RELEASE,
		EIDX_PACKAGES,
		EIDX_SOURCES,
		EIDX_DIFFIDX,
		EIDX_ARCHLXDB,
		EIDX_CYGSETUP,
		EIDX_SUSEREPO,
		EIDX_XMLRPMLIST,
		EIDX_RFC822WITHLISTS,
		EIDX_TRANSIDX, // XXX: in the old times, there were special i18n/Index files, are they gone for good now?
		EIDX_MD5DILIST,
		EIDX_SHA256DILIST
	};
	struct tIfileAttribs;
	typedef std::map<string_view,tIfileAttribs> tMetaMap;
	typedef std::function<void(const tRemoteFileInfo&)> tCbReport;
	struct tIfileAttribs
	{
		bool vfile_ondisk = false, uptodate = false, hideDlErrors = false, forgiveDlErrors = false;
		int8_t passId = -1;
		enumMetaType eIdxType = EIDX_UNKNOWN; // lazy type analysis
		const string_view* bro = nullptr; // anchor to create a pseudo single list (circle), containing equivalent contents
		off_t usedDiskSpace = 0;
		inline tIfileAttribs() =default;

#if defined(DEBUG) || defined(DEBUGIDX)
		inline tSS toString() const
		{
			return tSS() << forgiveDlErrors << "|"
						 << hideDlErrors << "|"
						 << usedDiskSpace << "|"
						 << uptodate << "|"
						 << vfile_ondisk;
		}
#endif
	};

	// helpers to keep the code cleaner and more readable
	const tIfileAttribs &GetFlags(string_view sPathRel) const;

	enum class eDlResult
	{
		OK,
		GONE,
		FAIL_LOCAL,
		FAIL_REMOTE,
		//UNFINISHED
	};

private:

	struct printcfg
	{
		mstring curFileRel, buf;

		enum eState
		{
			BEGIN,
			PRINTING,
			COLLECTING
		} state = eState::BEGIN;
		enum ePrintFormat
		{
			DEV,
			WEB,
			//SIMPLEWEB,
			CRONJOB,
		} format = ePrintFormat::DEV;

		eDlMsgSeverity sevCur = eDlMsgSeverity::UNKNOWN, sevMax = eDlMsgSeverity::NEVER;
		unsigned fmtdepth = 0;
	} m_print;

	eDlMsgSeverity m_printSeverityMin = eDlMsgSeverity::INFO;

	void CloseLine();

	SUTPROTECTED:

	/**
	 * @brief ReportBegin Initiates the display or collection of a user message
	 * If the conditions are met in the beginning, start printing immediately, otherwise push the string to a background buffer
	 * @param what File name (relative path)
	 * @param sev Initial assumed severity
	 * @param opMode Start with collecting mode, regardless of severity conditions
	 * @
	 */
	void ReportBegin(string_view what, eDlMsgSeverity sev, bool bForceCollecting, bool bIgnoreErrors);
	void ReportCont(string_view msg, eDlMsgSeverity sev = eDlMsgSeverity::UNKNOWN);

#define REP_HINT_TAG_AS_NEEDED 0x2
#define REP_HINT_TAG_ALWAYS 0x4
#define REP_HINT_TAG_IS_NOT_ERROR 0x8

	void ReportEnd(string_view msg, eDlMsgSeverity sev = eDlMsgSeverity::UNKNOWN, unsigned hints = REP_HINT_TAG_AS_NEEDED);

	// print a single message line immediately, meaning can be inverted (i.e. print ONLY below threshold)
	void ReportMisc(string_view msg, eDlMsgSeverity sev = eDlMsgSeverity::VERBOSE, bool prioInverted = false)
	{
		if (!prioInverted ? sev < m_printSeverityMin : sev >= m_printSeverityMin)
			return;
		SendDecoratedComment(msg, sev);
	}

	void ReportSectionLabel(string_view label)
	{
		SendDecoratedComment(label, eDlMsgSeverity::INFO, 3);
	}

	/**
	 * @brief SendDecoratedComment
	 * If in the middle of active line -> send msg inline, otherwise: create a new single line
	 * @param msg
	 * @param colorHint
	 * @param heading If set to non-zero, print the msg as section heading line
	 */
	void SendDecoratedComment(string_view msg, eDlMsgSeverity colorHint, unsigned heading = 0);


	virtual int CheckCondition(string_view key) override;

	/**
	 * @brief ReportData Special variant for data processing, a crossover of above and doing flashy printing
	 * Will add action checkbox for message level WARNING and above.
	 * @param sev
	 * @param addPath
	 */
	void ReportData(eDlMsgSeverity sev, string_view path, string_view reason);

	// common helper variables
	bool m_bErrAbort, m_bForceDownload, m_bSkipIxUpdate = false;
	bool m_bScanInternals, m_bByPath, m_bByChecksum, m_bSkipHeaderChecks;
	bool m_bTruncateDamaged;
	int m_nErrorCount;
	unsigned int m_nProgIdx, m_nProgTell;

	mstring m_currentlyProcessedIfile;

	enumMetaType GuessMetaTypeFromPath(string_view sPath);

	// stuff in those directories must be managed by some top-level index files
	// whitelist patterns do not apply there!
	tStrSet m_managedDirs;

	// this is not unordered because sometimes we make use of iterator references while
	// doing modification of the map
	tMetaMap m_metaFilesRel;
	tIfileAttribs& SetFlags(string_view sPathRel);
	// slightly more versatile version
	tMetaMap::iterator SetFlags(string_view sPathRel, bool& reportCreated);
	bool UpdateVolatileFiles();
	void _BusyDisplayLogs();
	void _Usermsg(mstring m);
	bool AddIFileCandidate(string_view sFileRel);

	// IFileHandler interface
	bool ProcessDirBefore(const std::string &, const struct stat &) override
	{
		return true;
	}
	bool ProcessOthers(const mstring &, const struct stat &) override
	{
		return true;
	}
	bool ProcessDirAfter(const mstring &, const struct stat &) override
	{
		return true;
	}

	/*!
	 * As the name saids, processes all index files and calls a callback
	 * function maintenence::_HandlePkgEntry on each entry.
	 *
	 * If a string set object is passed then a little optimization might be
	 * enabled internally, which avoid repeated processing of a file when another
	 * one with the same contents was already processed. This is only applicable
	 * having strict path checking disabled, though.
	 *
	 * */

	void ProcessSeenIndexFiles(std::function<void(tRemoteFileInfo)> pkgHandler);

	std::shared_ptr<TDownloadController> m_dler;
	struct tDlOpts
	{
		tDlOpts() {} // GCC&CLANG bug, must stay here
		bool bIsVolatileFile = true;
		bool bForceReDownload = false;
		bool bGuessReplacement = false;
		bool bIgnoreErrors = false;
		eDlMsgSeverity msgVerbosityLevel = eDlMsgSeverity::INFO;
		tHttpUrl forcedURL;
		mstring sGuessedFrom, sFilePathRel;
		tDlOpts& Verbosity(eDlMsgSeverity level) { msgVerbosityLevel = level; return (*this); }
		tDlOpts& GuessedFrom(cmstring& path) { sGuessedFrom = path; return (*this); }
		tDlOpts& GuessReplacement(bool val) { bGuessReplacement = val; return (*this); }
		tDlOpts& ForceUrl(tHttpUrl&& val) { forcedURL = std::move(val); return (*this); }
		tDlOpts& SetVolatile(bool val) { bIsVolatileFile = val; return (*this); }
		tDlOpts& ForceRedl(bool val) { bForceReDownload = val; return (*this); }
		tDlOpts& IgnErr(bool val) { bIgnoreErrors = val; return (*this); }
	};

	virtual eDlResult Download(string_view sFilePathRel, tDlOpts opts = tDlOpts());
	//eDlResult Download(cmstring& sFilePathRel) { tDlOpts o; return Download(sFilePathRel, tDlOpts()); };

	void TellCount(unsigned nCount, off_t nSize);

	int8_t GenGroupTag() { m_idxPassId++; if (m_idxPassId) return m_idxPassId; return ++m_idxPassId; }

	/**
	 * @param collectAllCsTypes If set, will send callbacks for all identified checksum types. In addition, will set the value of Acquire-By-Hash to the pointed boolean.
	 */
	bool ParseAndProcessMetaFile(tCbReport output_receiver, tMetaMap::iterator idxFile, int8_t runGroupAndTag);

	bool GetAndCheckHead(cmstring & sHeadfile, cmstring &sFilePathRel, off_t nWantedSize);
	virtual bool Inject(string_view fromRel, string_view toRel, bool bSetIfileFlags, off_t contLen, tHttpDate lastModified, string_view forceOrig);

	/**
	 * @brief HandleExtraEntry will be called to declare additionally discovered references.
	 */
	virtual void ReportExtraEntry(cmstring& /* sPathAbs */, const tFingerprint& /* fpr*/) {};

	void PrintStats(cmstring &title);

	void ProgTell();
	void ReportAdminAction(string_view sFileRel, string_view reason, bool bExtraFile = false, eDlMsgSeverity reportLevel = eDlMsgSeverity::ERROR);

	// add certain files to the trash list, to be removed after the activity is done in case of the expiration task
	virtual void MarkObsolete(cmstring&) {};

	SUTPRIVATE:

	void ExtractReleaseDataAndAutofixPatchIndex(tFileGroups& ret, string_view sPathRel);
	void FilterGroupData(tFileGroups& idxGroups);
	void BuildSingleLinkedBroList(tStrDeq& paths);

	/**
	 * Adjust the configuration of related paths (relative to updatePath) to prevent
	 * smart downloads later, how exactly depends on current execution mode.
	 *
	 * If strict path checks are used the content may also be copied over.
	 */
	void SyncSiblings(cmstring &srcPathRel, const tStrDeq& targets);

	cacheman(const cacheman&);
	cacheman& operator=(const cacheman&);

	cmstring& GetFirstPresentPath(const tFileGroups& groups, const tContentKey& ckey);

	/*
	 * Analyze patch base candidate, fetch patch files as suggested by index, patch, distribute result.
	 *
	 * Returns 0 for success, -1 for "not needed", other values for errors
	 */
	int PatchOne(cmstring& pindexPathRel, const tStrDeq& patchBaseCandidates);
	static void ParseGenericRfc822File(filereader& reader, cmstring& sExtListFilter,
								std::map<mstring, std::deque<mstring> >& contents);
	static bool ParseDebianIndexLine(tRemoteFileInfo& info, cmstring& fline);

	SUTPROTECTED:
	static bool CalculateBaseDirectories(string_view sPath, enumMetaType idxType, mstring& sBaseDir, mstring& sBasePkgDir);
	bool IsDeprecatedArchFile(cmstring &sFilePathRel);

	tIfileAttribs attr_dummy;

	/* Little helper to check existence of specific name on disk, either in cache or replacement directory, depending on what the srcPrefix defines */
	virtual bool _checkSolidHashOnDisk(cmstring& hexname, const tRemoteFileInfo &entry,
									   cmstring& srcPrefix);

	// "can return false negatives" thing
	// to be implemented in subclasses
	virtual bool _QuickCheckSolidFileOnDisk(cmstring& /* sFilePathRel */) { return false; }
	void BuildCacheFileList();
	/**
	 * This is supposed to restore references to files that are no longer
	 * downloaded by apt directly but via semi-static files identified by hash
	 * value in their name.
	 *
	 * Without this link, the index processing would not be able to parse the
	 * lists correctly and expiration would eventually "expire" good data.
	 *
	 * The code identify the original location of the index
	 * file by Release file analysis. */
	bool FixMissingOriginalsFromByHashVersions();

	/**
	 * If the specified (In)Release file has By-Hash enabled, look for paths that match
	 * the hash reference and if found, restore the data contents on the location of the
	 * file that the by-hash blobs originated from.
	 * @param releasePathRel cache-relative location of InRelease file
	 * @param stripPrefix Optional suffix that was included in releasePathRel but shall be removed in new/referenced files
	 */
	bool RestoreFromByHash(tMetaMap::iterator relFile, bool bRestoreMissingMode);

	bool IsInternalItem(cmstring& sPathAbs, bool inDoubt);
#ifdef DEBUG
	// Dumpable interface
public:
	void DumpInfo(Dumper &dumper) override;
#endif
};

#ifdef DEBUG
class tBgTester : public cacheman
{
public:
	tBgTester(mainthandler::tRunParms&& parms)
		: cacheman(std::move(parms))
	{
		ASSERT(!"fixme");
		//		m_szDecoFile="maint.html";
	}
	void Action() override;

	// IFileHandler interface
public:
	bool ProcessRegular(const std::string &sPath, const struct stat &) override;
};
#endif // DEBUG
}

#endif /*_CACHEMAN_H_*/
