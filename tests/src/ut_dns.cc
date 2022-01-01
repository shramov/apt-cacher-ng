#include "gtest/gtest.h"
#include "caddrinfo.h"
#include "ahttpurl.h"
#include "gmock/gmock.h"
#include "testcommon.h"
#include "aevutil.h"
#include "evabase.h"

#include <thread>
#include <atomic>

#define TESTTARGET "localhost" //"www.example.com"

using namespace acng;

#if 0 // this is pointless since dnsbase is now initialized by resource provider on demand
TEST(caddrinfo, test_query_not_inited)
{
	// std::function<void(std::shared_ptr<CAddrInfo>)> tDnsResultReporter;
	CAddrInfoPtr result;
	bool fin(false);
	auto rep = [&](CAddrInfoPtr res) { fin = true; result = res; };
	CAddrInfo::Resolve(TESTTARGET, 443, rep);
	pushEvents(3, &fin);
	ASSERT_TRUE(result);
	EXPECT_EQ(result->getError(), "503 Bad DNS configuration");
	result.reset();
	fin = false;
	CAddrInfo::Resolve(TESTTARGET, 443, rep);
	pushEvents(3, &fin);
	ASSERT_TRUE(result);
	EXPECT_EQ(result->getError(), "");
	EXPECT_LT(0, result->getTargets().size());

}
#endif

TEST(caddrinfo, test_query_inited)
{
	CAddrInfoPtr result;
	bool fin(false);
	auto rep = [&](CAddrInfoPtr res) { fin = true; result = res; };
	result.reset();
	fin = false;
	CAddrInfo::Resolve(TESTTARGET, 443, rep);
	pushEvents(3, &fin);
	ASSERT_TRUE(result);
	EXPECT_EQ(result->getError(), "");
	EXPECT_LT(0, result->getTargets().size());
}

TEST(caddrinfo, blocking_query)
{
	CAddrInfoPtr result;
	std::atomic_bool fin(false);

	std::thread t([&]()
	{
		result = CAddrInfo::Resolve(TESTTARGET, 443);
		fin = true;
	});
	pushEvents(1);
	t.join();
	ASSERT_TRUE(result);
	EXPECT_EQ(result->getError(), "");
	EXPECT_LT(0, result->getTargets().size());
}
