#ifndef MAINTENANCE_H_
#define MAINTENANCE_H_

#include "config.h"
#include "ahttpurl.h"
#include "fileitem.h"

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

ESpecialWorkType DetectWorkType(cmstring& cmd, cmstring& sHost, const char* auth);
/**
 * @brief Create a new "hot" fileitem (might have a thread attached already).
 * @param wType
 * @param url
 * @param refinedPath
 * @param reqHead
 * @return New fileitem object pointer, nullptr if the request is not supposed to be served by us
 */

tFileItemPtr Create(ESpecialWorkType jobType, bufferevent *bev, const tHttpUrl& url, cmstring& refinedPath, const header& reqHead);

}

#endif /*MAINTENANCE_H_*/
