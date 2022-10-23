#include "gtest/gtest.h"
#include "caddrinfo.h"
#include "ahttpurl.h"
#include "gmock/gmock.h"
#include "testcommon.h"
#include "acutilev.h"
#include "evabase.h"
#include "acfg.h"
#include "conserver.h"
#include "aconnect.h"
#include "main.h"

#include <thread>
#include <atomic>
#include <list>

#include <chrono>

//#define TESTPORT 3141

using namespace acng;
using namespace std;

extern acres *g_res;

class TransportTest : public ::testing::Test
{
public:
//	bool server_okay = false;
//	std::list<unique_fd> parkedFds;
//	lint_user_ptr<conserver> serva;
	TFakeServerData fake_server;

	TransportTest()
	{
		acng::cfg::udspath.clear(); // don't care for now
//		acng::cfg::port = TESTPORT;
//		serva = conserver::Create(*g_res);
//		server_okay = serva->Setup();
		fake_server = make_fake_server(1);
	}
	~TransportTest()
	{
		// shold not be here, it is part of the application infrastructure
		// evabase::SignalStop();
		pushEvents();
	}
};

TEST_F(TransportTest, server_started)
{
	ASSERT_FALSE(evabase::GetGlobal().IsShuttingDown());
	// NOTE: expecting IPv6 functionality here, hence two sockets
	ASSERT_TRUE(fake_server.ports.size() > 0);
}

TEST_F(TransportTest, just_connect)
{
	bool stopVar = false;
	/*
	auto msTimeout = std::chrono::seconds(2) + std::chrono::milliseconds(300);
	auto result = msTimeout.count();
	auto secs = std::chrono::duration_cast<std::chrono::seconds>(msTimeout);
	auto usecs = std::chrono::duration_cast<std::chrono::nanoseconds>(msTimeout - std::chrono::seconds(secs));
	*/
	{
		auto res = aconnector::Connect("localhost", fake_server.ports.front(), aconnector::tCallback(), cfg::GetNetworkTimeout()->tv_sec);
		ASSERT_TRUE(res.m_p);
		pushEvents();
		pushEvents();
		res.m_p();
		res.m_p = decltype(res.m_p)();
	}
	bool connected = false;
	{
		auto res = aconnector::Connect("localhost", fake_server.ports.front(),
									   [&] (aconnector::tConnResult ret)
		{
				   connected = true;
				   EXPECT_TRUE(ret.fd.valid());
				   EXPECT_EQ(ret.sError, "");
				   stopVar = true;
	}, cfg::GetNetworkTimeout()->tv_sec);
		ASSERT_TRUE(res.m_p);
		pushEvents(5, &stopVar);
		//pushEvents();
		ASSERT_TRUE(connected);
	}

}
