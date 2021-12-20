#include "gtest/gtest.h"
#include "caddrinfo.h"
#include "ahttpurl.h"
#include "gmock/gmock.h"
#include "testcommon.h"
#include "aevutil.h"
#include "evabase.h"
#include "acfg.h"
#include "conserver.h"
#include "aconnect.h"

#include <thread>
#include <atomic>
#include <list>

#include <chrono>

#define TESTPORT 3141

using namespace acng;
using namespace std;

extern acres *g_res;

class TransportTest : public ::testing::Test
{
protected:
	bool server_okay = false;
	std::list<unique_fd> parkedFds;
	conserver *serva;

	void SetUp() override
	{
		acng::cfg::port = TESTPORT;
		serva = conserver::Create(*g_res);
		server_okay = serva->Setup();
	}
	void TearDown() override
	{
		evabase::SignalStop();
		pushEvents();
	}
};

TEST_F(TransportTest, server_started)
{
	// NOTE: expecting IPv6 functionality here, so two sockets
	ASSERT_TRUE(server_okay);
}

TEST_F(TransportTest, just_connect)
{
	/*
	auto msTimeout = std::chrono::seconds(2) + std::chrono::milliseconds(300);
	auto result = msTimeout.count();
	auto secs = std::chrono::duration_cast<std::chrono::seconds>(msTimeout);
	auto usecs = std::chrono::duration_cast<std::chrono::nanoseconds>(msTimeout - std::chrono::seconds(secs));
	*/
	//aconnector::Connect("localhost", TESTPORT, cfg::GetNetworkTimeout()->tv_sec, );
}
