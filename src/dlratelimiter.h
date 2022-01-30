#ifndef DLRATELIMITER_H
#define DLRATELIMITER_H

#include "acsmartptr.h"

namespace acng
{
class dlratelimiter : public tLintRefcounted, public tExtRefExpirer
{
public:
	static lint_user_ptr<dlratelimiter> GetSharedLimiter();
	virtual void AddStream(bufferevent* bev) =0;
	virtual void DetachStream(bufferevent* bev) =0;
};

using tRateLimiterPtr = lint_user_ptr<dlratelimiter>;

}

#endif // DLRATELIMITER_H
