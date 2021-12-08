#include "acres.h"
#include "aclock.h"
#include "ac3rdparty.h"

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

public:
	acresImpl()
	{
		kaClock = tClock::Create(defaultKeepAliveTimeout);
		idleClock = tClock::Create(idleTimeout);
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
};

acres *acres::Create()
{
	return new acresImpl;
}

}
