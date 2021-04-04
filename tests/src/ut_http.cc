#include "gtest/gtest.h"
#include "httpdate.h"

#include "gmock/gmock.h"

#if 0 // those is all new stuff from the next branch - might restore when we use them
TEST(algorithms, checksumming)
{
	ASSERT_NO_THROW(acng::check_algos());

	acng::tChecksum cs, csmd(acng::CSTYPES::MD5);
	cs.Set("a26a96c0c63589b885126a50df83689cc719344f4eb4833246ed57f42cf67695a2a3cefef5283276cbf07bcb17ed7069de18a79410a5e4bc80a983f616bf7c91");

	auto cser2 = acng::csumBase::GetChecker(acng::CSTYPES::SHA512);
	cser2->add("bf1942");
	auto cs2=cser2->finish();
	ASSERT_EQ(cs2, cs);

	ASSERT_NE(csmd, cs2);
}
#endif
using namespace acng;

TEST(http, status)
{
	tRemoteStatus a("200 OK");
	ASSERT_EQ(a.code, 200);
	ASSERT_EQ(a.msg, "OK");
	tRemoteStatus b("200 OK  ");
	ASSERT_EQ(b.code, 200);
	ASSERT_EQ(b.msg, "OK");
	tRemoteStatus c("  200 OK");
	ASSERT_EQ(b.code, 200);
	ASSERT_EQ(b.msg, "OK");
	tRemoteStatus d("  200     OK Is IT  ");
	ASSERT_EQ(b.code, 200);
	ASSERT_EQ(b.msg, "OK Is IT");

}
