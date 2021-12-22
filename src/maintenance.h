#ifndef MAINTENANCE_H_
#define MAINTENANCE_H_

#include "config.h"
#include "ahttpurl.h"
#include "fileitem.h"

namespace acng
{
class acres;

enum class EWorkType : int8_t
{
	UNKNOWN = -1,
	REGULAR = 0,
	LOCALITEM,
	// expiration types
	EXPIRE,
	EXP_LIST,
	EXP_PURGE,
	EXP_LIST_DAMAGED,
	EXP_PURGE_DAMAGED,
	EXP_TRUNC_DAMAGED,
	USER_INFO,
	REPORT,
	AUT_REQ,
	AUTH_DENY,
	IMPORT,
	MIRROR,
	DELETE,
	DELETE_CONFIRM,
	COUNT_STATS,
	STYLESHEET,
	FAVICON,
	TRACE_START,
	TRACE_END,
	TRUNCATE,
	TRUNCATE_CONFIRM
#ifdef DEBUG
	, DBG_SLEEPER
	, DBG_BGSTREAM
#endif
};

EWorkType DetectWorkType(const tHttpUrl& reqUrl, string_view rawCmd, const char* auth);
/**
 * @brief Create a new "hot" fileitem (might have a thread attached already).
 * @param wType
 * @param url
 * @param refinedPath
 * @param reqHead
 * @return New fileitem object pointer, nullptr if the request is not supposed to be served by us
 */

tFileItemPtr Create(EWorkType jobType, bufferevent *bev, const tHttpUrl& url, SomeData* arg, acres& reso);

}

#endif /*MAINTENANCE_H_*/
