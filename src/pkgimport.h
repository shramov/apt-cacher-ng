#ifndef PKGIMPORT_H_
#define PKGIMPORT_H_

//! Reuses lots of processing code
#include "expiration.h"

#include "fileio.h"
#include <cstring>
#include "csmapping.h"

namespace acng
{
class pkgimport : public cacheman
{

public:
	// XXX: c++11 using tCacheOperation::tCacheOperation;
	inline pkgimport(tRunParms&& parms)
		: cacheman(std::move(parms)) {};

	void Action() override;
	
protected:
	bool ImportFileFound(const mstring &sPath, const struct stat &, tReporter&);
	void _LoadKeyCache();
	void HandlePkgEntry(const tRemoteFileInfo &entry);

private:

	/*!
	 *  Two maps mapping in different directions:
		-	fingerprints pointing to file info description vector,
	 		created by file scan+identification
	 	-	when reusing old fingerprints, a file info description is mapped
	 		to stored fingerprint (cacheMap from tCacheProcessor)
	*/
	std::map<tFingerprint, tImpFileInfo> m_importMap;
	std::deque<std::pair<tFingerprint, tImpFileInfo> > m_importRest;
	std::set<mstring> m_precachedList;
	/* tFprCacheMap m_cacheMap;*/
		
	//void _ExtractLocationsFromVolFiles();
	void _GuessAndGetIfiles();
		
};

}
#endif /*PKGIMPORT_H_*/
