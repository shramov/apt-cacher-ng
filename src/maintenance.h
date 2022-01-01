#ifndef MAINTENANCE_H_
#define MAINTENANCE_H_

#include "config.h"
#include "ahttpurl.h"
#include "fileitem.h"

namespace acng
{
class acres;

enum EWorkType : unsigned
{
	REGULAR,
	LOCALITEM,
	// special job types with dedicated handlers, see Create(...)
	EXPIRE,
	EXP_LIST,
	EXP_PURGE,
	EXP_LIST_DAMAGED,
	EXP_PURGE_DAMAGED,
	EXP_TRUNC_DAMAGED,
	USER_INFO,
	REPORT,
	AUTH_REQ,
	AUTH_DENY,
	IMPORT,
	MIRROR,
	DELETE,
	DELETE_CONFIRM,
	COUNT_STATS,
#warning reoder, move dummies with no processing classes to the end
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
	, WORK_TYPE_MAX
};

EWorkType DetectWorkType(const tHttpUrl& reqUrl, const char* auth);

/**
 * @brief Create a new "hot" fileitem (might have a thread attached already).
 * @param wType
 * @param url
 * @param refinedPath
 * @param reqHead
 * @return New fileitem object pointer, nullptr if the request is not supposed to be served by us
 */

tFileItemPtr CreateSpecialWork(EWorkType jobType, bufferevent *bev, const tHttpUrl& url, SomeData* arg, acres& reso);

}

#endif /*MAINTENANCE_H_*/
