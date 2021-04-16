#include "gtest/gtest.h"
#include "httpdate.h"
#include "header.h"

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
	ASSERT_EQ(c.code, 200);
	ASSERT_EQ(c.msg, "OK");
	tRemoteStatus d("  200     OK Is IT  ");
	ASSERT_EQ(d.code, 200);
	ASSERT_EQ(d.msg, "OK Is IT");

	tRemoteStatus e;
	ASSERT_EQ(e.code, 500);

	e = {1, "X"};
	ASSERT_EQ(e.code, 1);
	ASSERT_EQ(e.msg, "X");

	tRemoteStatus f("HTTP/1.0 200 OK");
	ASSERT_EQ(200, f.code);
}

TEST(http, date)
{
	tHttpDate a;
	EXPECT_FALSE(a.isSet());
	EXPECT_EQ(a.any(), "");

	tHttpDate d(1);
	EXPECT_TRUE(d.isSet());
	ASSERT_EQ(d.any(), "Thu, 01 Jan 1970 00:00:01 GMT");

	/*
	 *         "%a, %d %b %Y %H:%M:%S GMT",
		"%A, %d-%b-%y %H:%M:%S GMT",
		"%a %b %d %H:%M:%S %Y"
		*/
	ASSERT_EQ(d, "Thu, 01 Jan 1970 00:00:01 GMT");
	ASSERT_EQ(d, tHttpDate("Thu, 01 Jan 1970 00:00:01 GMT"));
	ASSERT_EQ(d, tHttpDate(1));
	d.unset();
	EXPECT_FALSE(d.isSet());

	d = tHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");
	EXPECT_EQ(true, d.isSet());

	ASSERT_EQ(d, "Sunday, 06-Nov-94 08:49:37 GMT");
	ASSERT_EQ(d, "Sun Nov  6 08:49:37 1994");

	tHttpDate e(-1);
	ASSERT_FALSE(e.isSet());
	ASSERT_EQ(e.any(), "");

	tHttpDate f(time_t(0));
	ASSERT_EQ(f, "Thu, 01 Jan 1970 00:00:00 GMT");

	auto shortBS = "Short&random BS";
	f = tHttpDate(shortBS);
	ASSERT_EQ(f.any(), shortBS);
}


TEST(http, cachehead)
{
	mstring testHead = "foo.head", testOrig = "https://nix";
	off_t testSize = 12345;
	tHttpDate testDate(1);
	auto ok = StoreHeadToStorage(testHead, -1, nullptr, nullptr);
	ASSERT_TRUE(ok);
	{
		tHttpDate nix;
		mstring orig;
		ASSERT_TRUE(ParseHeadFromStorage(testHead, nullptr, &nix, &orig));
		ASSERT_FALSE(nix.isSet());
		ASSERT_TRUE(orig.empty());
	}
	// play simple loader against the header processor, this should still be valid!
	header h;
	ASSERT_TRUE(h.LoadFromFile(testHead));
	ASSERT_EQ(h.frontLine, "HTTP/1.1 200 OK");
	ASSERT_FALSE(h.h[header::XORIG]);
	ASSERT_FALSE(h.h[header::LAST_MODIFIED]);
	ASSERT_TRUE(StoreHeadToStorage(testHead, testSize, &testDate, &testOrig));
    h.clear();
    ASSERT_TRUE(h.frontLine.empty());
	ASSERT_TRUE(h.LoadFromFile(testHead));
	ASSERT_EQ(h.frontLine, "HTTP/1.1 200 OK");
	ASSERT_EQ(testOrig, h.h[header::XORIG]);
	ASSERT_EQ(testDate, h.h[header::LAST_MODIFIED]);
	{
		tHttpDate nix;
		mstring orig;
		off_t sz;
		ASSERT_TRUE(ParseHeadFromStorage(testHead, &sz, &nix, &orig));
		ASSERT_EQ(sz, testSize);
		ASSERT_EQ(nix, testDate);
		ASSERT_EQ(orig, testOrig);
	}

#warning TODO: write sample data to it, load it, unlink it, store sample data again with store method, load and compare
}

TEST(http, header)
{
    header h;
    ASSERT_TRUE(h.type == header::INVALID);
    string_view hdata = "GET /na/asdfasdfsadf\r\n\rfoo:bar\n";
    ASSERT_LT(h.Load(hdata), 0);
    hdata = "GET /na/asdfasdfsadf\r\nfoo:bar\r\n\r\n";
    ASSERT_EQ(hdata.length(), h.Load(hdata));
    hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\n\r\n";
    ASSERT_EQ(hdata.length(), h.Load(hdata));
    hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\nLast-Modified: Sunday, \r\n 06-Nov-94 08:49:37 GMT\r\n\r\n";
    auto l = h.Load(hdata);
    ASSERT_EQ(hdata.length(), l);
    string_view refDateS = "Sunday, 06-Nov-94 08:49:37 GMT";
    ASSERT_EQ(refDateS, h.h[header::LAST_MODIFIED]);
    tHttpDate refDate(refDateS, true);
    ASSERT_TRUE(refDate == h.h[header::LAST_MODIFIED]);
}
