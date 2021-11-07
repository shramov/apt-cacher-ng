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

#define TESTPORT 3141

using namespace acng;
using namespace std;

class TransportTest : public ::testing::Test
{
protected:
	int nSockets = 0;
	std::list<unique_fd> parkedFds;

	void SetUp() override
	{
		acng::cfg::port = TESTPORT;
		nSockets = conserver::Setup([&](unique_fd&&ufd, std::string) { parkedFds.emplace_back(move(ufd)); });
		evabase::CheckDnsChange();
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
	ASSERT_GT(nSockets, 1);
}

TEST_F(TransportTest, just_connect)
{
	//aconnector::Connect("localhost", TESTPORT, cfg::GetNetworkTimeout()->tv_sec, );
}
