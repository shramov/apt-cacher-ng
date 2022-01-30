#include "dlratelimiter.h"
#include "sockio.h"
#include "aevutil.h"
#include "evabase.h"
#include "acfg.h"

namespace acng
{

lint_ptr<dlratelimiter> g_cachedLimiter;

#define PRECISSION 8

const struct timeval tickInterval
{
	0,
	1000000/PRECISSION
};

class limiterImpl : public dlratelimiter
{
public:
	struct ev_token_bucket_cfg *m_cfg;
	struct bufferevent_rate_limit_group *m_group;

	limiterImpl()
	{
		auto spd = cfg::maxdlspeed * 1024 / PRECISSION;
		m_cfg = ev_token_bucket_cfg_new(spd,
										EV_RATE_LIMIT_MAX,
										EV_RATE_LIMIT_MAX,
										EV_RATE_LIMIT_MAX,
										&tickInterval
										);
		m_group = bufferevent_rate_limit_group_new(evabase::base, m_cfg);
	}
	~limiterImpl()
	{
		if (m_group)
			bufferevent_rate_limit_group_free(m_group);
		if (m_cfg)
			ev_token_bucket_cfg_free(m_cfg);
	}

	void Abandon() override
	{
		g_cachedLimiter.reset();
	}

	void AddStream(bufferevent *bev) override
	{
		bufferevent_add_to_rate_limit_group(bev, m_group);
	}
	void DetachStream(bufferevent *bev) override
	{
		bufferevent_remove_from_rate_limit_group(bev);
	}
};

lint_user_ptr<dlratelimiter> dlratelimiter::GetSharedLimiter()
{
	try
	{
		if (cfg::maxdlspeed <= 0)
			return tRateLimiterPtr();
		if (!g_cachedLimiter)
		{
			auto limiter = new limiterImpl;
			if (!limiter->m_cfg || !limiter->m_group)
			{
				delete limiter;
				return tRateLimiterPtr();
			}

			g_cachedLimiter = as_lptr<dlratelimiter>(limiter);
		}
		return tRateLimiterPtr(g_cachedLimiter);
	}
	catch (...)
	{
		return tRateLimiterPtr();
	}
}


}
