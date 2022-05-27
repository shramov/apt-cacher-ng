#ifndef EXPIRATION_H_
#define EXPIRATION_H_

#include "cacheman.h"

#include <list>
#include <fstream>
#include <unordered_map>

namespace acng
{

// caching all relevant file identity data and helper flags in such entries
struct tDiskFileInfo
{
	string_view folder;

	bool bHasBody = false; // only header if false
	bool bNoHeaderCheck = false;
	bool bNeedsStrictPathCheck = false;
#warning check alignment along with bools to avoid waste
	enum EStatus : uint8_t
	{
		FORCE_KEEP,
		FAKE, // actually not on disk
		UNDECIDED,
		FORCE_REMOVE
	} action = UNDECIDED;

	// this adds a couple of procent overhead so it's negligible considering
	// hashing or traversing overhead of a detached solution
	tFingerprint fpr;
};

class expiration : public cacheman
{
public:
	expiration(tRunParms&& parms);

protected:

	std::unordered_multimap<string_view,tDiskFileInfo> m_delCand;
	tStrVec m_oversizedFiles;
	tStrDeq m_emptyFolders;
	unsigned m_fileCur;
	time_t m_oldDate;

	tDiskFileInfo* PickCand(std::pair<decltype (m_delCand)::iterator, decltype (m_delCand)::iterator> range, string_view dir);
	tDiskFileInfo* PickCand(string_view folderName, string_view fileName);
	std::pair<tDiskFileInfo&,bool> PickOrAdd(string_view folderName, string_view fileName, bool dupData);
	std::pair<tDiskFileInfo&,bool> PickOrAdd(string_view svPathRel);

	void RemoveAndStoreStatus(bool purgeAll);

	// callback implementations
	virtual void Action() override;
	void HandlePkgEntry(const tRemoteFileInfo &entry);
	virtual void ReportExtraEntry(cmstring& /* sPathAbs */, const tFingerprint&) override;

	void PurgeMaintLogsAndObsoleteFiles();

	std::ofstream m_damageList;
	bool m_bIncompleteIsDamaged = false, m_bScanVolatileContents = false;

	void MarkObsolete(cmstring&) override;
	tStrVec m_obsoleteStuff;

	virtual bool _checkSolidHashOnDisk(cmstring& hexname, const tRemoteFileInfo &entry,
			cmstring& srcPrefix) override;
	virtual bool _QuickCheckSolidFileOnDisk(cmstring& /* sFilePathRel */) override;

	void DoStateChecks(tDiskFileInfo& infoLocal, const tRemoteFileInfo& infoRemote, bool bPathMatched);

	void LoadHints();

private:
	string_view m_lastDirCache = "";

	bool CheckAndReportError();

	void HandleDamagedFiles();
	void ListExpiredFiles(bool bPurgeNow);
	void TrimFiles();

	// IFileHandler interface
public:
	bool ProcessRegular(const mstring &sPath, const struct stat &) override;
	bool ProcessOthers(const std::string &sPath, const struct stat &) override;
	bool ProcessDirBefore(const std::string &sPath, const struct stat &) override;
	bool ProcessDirAfter(const std::string &sPath, const struct stat &) override;

	// tMarkupFileSend interface
protected:
	void SendProp(cmstring &key) override;


	virtual int CheckCondition(string_view key) override;


};

}

#endif /*EXPIRATION_H_*/
