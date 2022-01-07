#include "acres.h"
#include "aclock.h"
#include "ac3rdparty.h"
#include "rex.h"
#include "acregistry.h"

#include <map>



#warning implement all extra functionality and migrage from the creepy global singletons into it

namespace acng
{

#warning make idle shorter, 3s
const struct timeval defaultKeepAliveTimeout { 10, 0 }, idleTimeout { 30, 1};

class ACNG_API acresImpl : public acres
{
	// acres interface

	std::unique_ptr<tBeatNotifier> kaClock, idleClock;
	std::map<int, std::unique_ptr<tBeatNotifier>> customClocks;
	tSslConfig m_ssl_setup;
	rex *rx = nullptr;
	aobservable::subscription m_shutdownSub;

public:
	acresImpl()
	{
		kaClock = tBeatNotifier::Create(defaultKeepAliveTimeout);
		idleClock = tBeatNotifier::Create(idleTimeout);
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

	tBeatNotifier &GetKeepAliveBeat() override
	{
		return *kaClock;
	}
	tBeatNotifier &GetIdleCheckBeat() override
	{
		return *idleClock;
	}
	tBeatNotifier &GetCustomBeat(int id, const struct timeval& interval) override
	{
		auto& ret = customClocks[id];
		if (!ret)
			ret = tBeatNotifier::Create(interval);
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
