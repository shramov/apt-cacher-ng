#include "acres.h"
#include "aclock.h"
#include "ac3rdparty.h"
#include "rex.h"
#include "acregistry.h"

#include <map>



#warning implement all extra functionality and migrage from the creepy global singletons into it

namespace acng
{

const struct timeval defaultKeepAliveTimeout { 10, 0 }, idleTimeout { 3, 1};

class acresImpl : public acres
{
	// acres interface

	std::unique_ptr<tClock> kaClock, idleClock;
	std::map<int, std::unique_ptr<tClock>> customClocks;
	tSslConfig m_ssl_setup;
	rex *rx = nullptr;
	aobservable::subscription m_shutdownSub;

public:
	acresImpl()
	{
		kaClock = tClock::Create(defaultKeepAliveTimeout);
		idleClock = tClock::Create(idleTimeout);
		m_shutdownSub = evabase::GetGlobal().subscribe([&]()
		{
			kaClock.reset();
			idleClock.reset();
			customClocks.clear();
			m_shutdownSub.reset();
		});
	}
	~acresImpl()
	{
		delete rx;
	}

	tClock &GetKeepAliveBeat() override
	{
		return *kaClock;
	}
	tClock &GetIdleCheckBeat() override
	{
		return *idleClock;
	}
	tClock &GetCustomBeat(int id, const struct timeval& interval) override
	{
		auto& ret = customClocks[id];
		if (!ret)
			ret = tClock::Create(interval);
		return *ret;
	}

public:
	tSslConfig &GetSslConfig() override
	{
		return m_ssl_setup;
	}

	// acres interface
public:
	rex &GetMatchers() override
	{
		if (!rx)
			rx = new rex;
		return *rx;
	}

	// acres interface
public:
	acng::lint_ptr<IFileItemRegistry> GetItemRegistry() override
	{
		return SetupServerItemRegistry();
	}
};

acres *acres::Create()
{
	return new acresImpl;
}

}
