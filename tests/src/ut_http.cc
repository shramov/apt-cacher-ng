#include "gtest/gtest.h"
#include "httpdate.h"
#include "header.h"
#include "ahttpurl.h"

#include "gmock/gmock.h"

#include <caddrinfo.h>

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
using namespace std;
TEST(http, status)
{
	tRemoteStatus a("200 OK", -1, false);
	ASSERT_EQ(a.code, 200);
	ASSERT_EQ(a.msg, "OK");
	tRemoteStatus b("200 OK  ", -1, false);
	ASSERT_EQ(b.code, 200);
	ASSERT_EQ(b.msg, "OK");
	tRemoteStatus c("  200 OK", -1, false);
	ASSERT_EQ(c.code, 200);
	ASSERT_EQ(c.msg, "OK");
	tRemoteStatus d("  200     OK Is IT  ", -1, false);
	ASSERT_EQ(d.code, 200);
	ASSERT_EQ(d.msg, "OK Is IT");

	tRemoteStatus e;
	ASSERT_EQ(e.code, 500);

	e = {1, "X"};
	ASSERT_EQ(e.code, 1);
	ASSERT_EQ(e.msg, "X");

	tRemoteStatus f("HTTP/1.0 200 OK", -1, false);
	ASSERT_NE(200, f.code);

	tRemoteStatus g("HTTP/1.0 200 OK", -1, true);
	ASSERT_EQ(200, g.code);
}

TEST(http, date)
{
	tHttpDate a;
	EXPECT_FALSE(a.isSet());
	EXPECT_EQ(a.view(), "");

	tHttpDate d(1);
	EXPECT_TRUE(d.isSet());
	ASSERT_EQ(d.view(), "Thu, 01 Jan 1970 00:00:01 GMT");

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
	ASSERT_EQ(e.view(), "");

	tHttpDate f(time_t(0));
	ASSERT_EQ(f, "Thu, 01 Jan 1970 00:00:00 GMT");

	auto shortBS = "Short&random BS";
	f = tHttpDate(shortBS);
	ASSERT_EQ(f.view(), shortBS);
}


TEST(http, cachehead)
{
		printf("%ld %ld %ld\n", sizeof(header), sizeof(std::vector<char>), sizeof(acbuf));

	mstring testHead = "foo.head";
	auto ok = StoreHeadToStorage(testHead, -1, nullptr, nullptr);
	ASSERT_TRUE(ok);

	{
		tHttpDate nix;
		mstring orig;
		ASSERT_TRUE(ParseHeadFromStorage(testHead, nullptr, &nix, &orig));
		ASSERT_FALSE(nix.isSet());
		ASSERT_TRUE(orig.empty());
	}

	mstring testOrig = "https://nix";
	off_t testSize = 12345;
	tHttpDate testDate(1);

	// play simple loader against the header processor, this should still be valid!
	header h;
	ASSERT_TRUE(h.LoadFromFile(testHead));
	ASSERT_EQ(h.getStatusCode(), 200);
	ASSERT_EQ(h.getStatusMessage(), "OK");
	ASSERT_FALSE(h.h[header::XORIG]);
	ASSERT_FALSE(h.h[header::LAST_MODIFIED]);
	ASSERT_TRUE(StoreHeadToStorage(testHead, testSize, &testDate, &testOrig));
    h.clear();
	ASSERT_TRUE(h.getStatusCode() < 0);
	ASSERT_TRUE(h.LoadFromFile(testHead));
	ASSERT_EQ(h.getStatusCode(), 200);
	ASSERT_EQ(h.getStatusMessage(), "OK");
	ASSERT_NE(h.proto, header::HTTP_10);
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
}

TEST(http, header)
{
    header h;
    ASSERT_TRUE(h.type == header::INVALID);
	string_view hdata = "GET /na/asdfasdfsadf\r\n\rfoo:bar\r";
	ASSERT_EQ(h.Load(hdata), -2); // bad continuation in first k=v line
	hdata = "GET /na/asdfasdfsadf\r\nfoo:bar\r";
	ASSERT_EQ(h.Load(hdata), 0);
	hdata = "GET /na/asdfasdfsadf\r\nfoo:bar\r\n\r\n";
    ASSERT_EQ(hdata.length(), h.Load(hdata));
	hdata = "GET /na/asdfasdfsadf HTTP/1.0\r\n\r\n";
    ASSERT_EQ(hdata.length(), h.Load(hdata));
	ASSERT_EQ(h.proto, header::HTTP_10);
    hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\nLast-Modified: Sunday, \r\n 06-Nov-94 08:49:37 GMT\r\n\r\n";
    auto l = h.Load(hdata);
    ASSERT_EQ(hdata.length(), l);
    string_view refDateS = "Sunday, 06-Nov-94 08:49:37 GMT";
    ASSERT_EQ(refDateS, h.h[header::LAST_MODIFIED]);
    tHttpDate refDate(refDateS, true);
    ASSERT_TRUE(refDate == h.h[header::LAST_MODIFIED]);

    hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\na: b\r\nc:d\r\ne:f\r\n\tffffuuuu\r\n\r\n";
    std::vector<std::pair<string_view,string_view> > unkollector;
    l = h.Load(hdata, &unkollector);
    ASSERT_EQ(hdata.length(), l);
    ASSERT_EQ(unkollector.size(), 4);
    ASSERT_EQ(unkollector[0].first, "a");
    ASSERT_EQ(unkollector[0].second, "b");
    ASSERT_EQ(unkollector[1].first, "c");
    ASSERT_EQ(unkollector[1].second, "d");
    ASSERT_EQ(unkollector[2].first, "e");
    ASSERT_EQ(unkollector[2].second, "f");
    ASSERT_EQ(unkollector[3].first, "");
    ASSERT_EQ(unkollector[3].second, "ffffuuuu");

    h.clear();
    auto extra = header::ExtractCustomHeaders(hdata, true);
    ASSERT_GT(extra.size(), 10);
    ASSERT_EQ(extra, "a: b\r\nc: d\r\ne: f ffffuuuu\r\n");

	hdata = "HTTP/1.1 ";
	ASSERT_EQ(0, h.Load(hdata));

	hdata = "HTTP/1.1 200 OK\r\nServer: nginx/1.14.2\r\nDate: Sun, 18 Apr 2021 16:17:39 GMT\r\nContent-Type";
	ASSERT_EQ(0, h.Load(hdata));

	hdata = "HTTP/1.1 200 OK\r\nServer: nginx/1.14.2\r\nDate: Sun, 18 Apr 2021 16:17:39 GMT\r\nContent-Type: foo";
	ASSERT_EQ(0, h.Load(hdata));

	hdata = "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\nAge: 230115\r\nContent-Type: application/x-debian-package\r\n"
"Date: Sun, 18 Apr 2021 16:29:32 GMT\r\nEtag: \"841c-5bbf2fe7c319f\"\r\nLast-Modified: Mon, 22 Feb 2021 20:53:29 GMT\r\n";
	ASSERT_EQ(207, hdata.length());
	ASSERT_EQ(0, h.Load(hdata));

	hdata = "GET /na/asdfasdfsadf"sv;
	auto r = h.Load(hdata);
	ASSERT_EQ(r, 0);
	hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\n";
	r = h.Load(hdata);
	ASSERT_EQ(r, 0);
	hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\n\r\n";
	r = h.Load(hdata);
	ASSERT_EQ(r, hdata.size());
}

TEST(http, misc)
{
	auto n = sizeof(acng::acng_addrinfo);
	ASSERT_GE(n, sizeof(::sockaddr_in6) + 3*sizeof(int));
}

TEST(http, url_url)
{
		tHttpUrl url;

		ASSERT_TRUE(url.SetHttpUrl("example.org"));
		ASSERT_EQ(url.sUserPass, "");
		ASSERT_EQ(url.sHost, "example.org");
		ASSERT_EQ(url.GetPort(), 80);
		ASSERT_EQ(url.sPath, "/");

		ASSERT_TRUE(url.SetHttpUrl("https://example.net"));
		ASSERT_EQ(url.sUserPass, "");
		ASSERT_EQ(url.sHost, "example.net");
		ASSERT_EQ(url.GetPort(), 443);
		ASSERT_EQ(url.sPath, "/");

		ASSERT_TRUE(url.SetHttpUrl("http://example.org"));
		ASSERT_EQ(url.sUserPass, "");
		ASSERT_EQ(url.sHost, "example.org");
		ASSERT_EQ(url.GetPort(), 80);
		ASSERT_EQ(url.sPath, "/");

		ASSERT_TRUE(url.SetHttpUrl("http://alice:sec::::ret@example.net:8080/foo/bar.html?abc=123&def=456"));
		ASSERT_EQ(url.sUserPass, "alice:sec::::ret");
		ASSERT_EQ(url.sHost, "example.net");
		ASSERT_EQ(url.GetPort(), 8080);
		ASSERT_EQ(url.sPath, "/foo/bar.html?abc=123&def=456");

		// Non-RFC3986-compliant case: undelimited IPv6 literal
		ASSERT_TRUE(url.SetHttpUrl("https://alice:se::cret@2001:db8::/bar"));
		ASSERT_EQ(url.sUserPass, "alice:se::cret");
		ASSERT_EQ(url.sHost, "2001:db8::");
		ASSERT_EQ(url.GetPort(), 443);
		ASSERT_EQ(url.sPath, "/bar");
}

TEST(http, url_host_port_ipv4)
{
		tHttpUrl url;
		uint16_t defaultPort = url.GetPort();

		ASSERT_TRUE(url.SetHttpUrl("0.0.0.0"));
		ASSERT_EQ(url.sHost, "0.0.0.0");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("127.0.0.1"));
		ASSERT_EQ(url.sHost, "127.0.0.1");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("10.20.30.40"));
		ASSERT_EQ(url.sHost, "10.20.30.40");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("255.255.255.255"));
		ASSERT_EQ(url.sHost, "255.255.255.255");
		ASSERT_EQ(url.GetPort(), defaultPort);
}

TEST(http, url_host_port_ipv4_port)
{
		tHttpUrl url;

		ASSERT_TRUE(url.SetHttpUrl("0.0.0.0:8000"));
		ASSERT_EQ(url.sHost, "0.0.0.0");
		ASSERT_EQ(url.GetPort(), 8000);

		ASSERT_TRUE(url.SetHttpUrl("127.0.0.1:8001"));
		ASSERT_EQ(url.sHost, "127.0.0.1");
		ASSERT_EQ(url.GetPort(), 8001);

		ASSERT_TRUE(url.SetHttpUrl("10.20.30.40:8002"));
		ASSERT_EQ(url.sHost, "10.20.30.40");
		ASSERT_EQ(url.GetPort(), 8002);

		ASSERT_TRUE(url.SetHttpUrl("255.255.255.255:8003"));
		ASSERT_EQ(url.sHost, "255.255.255.255");
		ASSERT_EQ(url.GetPort(), 8003);
}

TEST(http, url_host_port_hostname)
{
		tHttpUrl url;
		uint16_t defaultPort = url.GetPort();

		ASSERT_TRUE(url.SetHttpUrl("a"));
		ASSERT_EQ(url.sHost, "a");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("1"));
		ASSERT_EQ(url.sHost, "1");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("localhost"));
		ASSERT_EQ(url.sHost, "localhost");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("example.org"));
		ASSERT_EQ(url.sHost, "example.org");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("42.example.org"));
		ASSERT_EQ(url.sHost, "42.example.org");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("foo.bar.example.org"));
		ASSERT_EQ(url.sHost, "foo.bar.example.org");
		ASSERT_EQ(url.GetPort(), defaultPort);
}

TEST(http, url_host_port_hostname_port)
{
		tHttpUrl url;

		ASSERT_TRUE(url.SetHttpUrl("a:8000"));
		ASSERT_EQ(url.sHost, "a");
		ASSERT_EQ(url.GetPort(), 8000);

		ASSERT_TRUE(url.SetHttpUrl("1:8001"));
		ASSERT_EQ(url.sHost, "1");
		ASSERT_EQ(url.GetPort(), 8001);

		ASSERT_TRUE(url.SetHttpUrl("localhost:8002"));
		ASSERT_EQ(url.sHost, "localhost");
		ASSERT_EQ(url.GetPort(), 8002);

		ASSERT_TRUE(url.SetHttpUrl("example.org:8003"));
		ASSERT_EQ(url.sHost, "example.org");
		ASSERT_EQ(url.GetPort(), 8003);

		ASSERT_TRUE(url.SetHttpUrl("42.example.org:8003"));
		ASSERT_EQ(url.sHost, "42.example.org");
		ASSERT_EQ(url.GetPort(), 8003);

		ASSERT_TRUE(url.SetHttpUrl("foo.bar.example.org:8005"));
		ASSERT_EQ(url.sHost, "foo.bar.example.org");
		ASSERT_EQ(url.GetPort(), 8005);
}

TEST(http, url_host_port_ipv6)
{
		tHttpUrl url;
		uint16_t defaultPort = url.GetPort();

		ASSERT_TRUE(url.SetHttpUrl("[::]"));
		ASSERT_EQ(url.sHost, "::");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[::1]"));
		ASSERT_EQ(url.sHost, "::1");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[1::]"));
		ASSERT_EQ(url.sHost, "1::");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[1::1]"));
		ASSERT_EQ(url.sHost, "1::1");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[::a]"));
		ASSERT_EQ(url.sHost, "::a");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[a::]"));
		ASSERT_EQ(url.sHost, "a::");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[a::a]"));
		ASSERT_EQ(url.sHost, "a::a");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[1:2:3:4:5:6:7:8]"));
		ASSERT_EQ(url.sHost, "1:2:3:4:5:6:7:8");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[1111:2222:3333:4444:5555:6666:7777:8888]"));
		ASSERT_EQ(url.sHost, "1111:2222:3333:4444:5555:6666:7777:8888");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[a:a:b:b:c:d:e:f]"));
		ASSERT_EQ(url.sHost, "a:a:b:b:c:d:e:f");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[aaaa:aabb:bbaa:bbbb:cccc:dddd:eeee:ffff]"));
		ASSERT_EQ(url.sHost, "aaaa:aabb:bbaa:bbbb:cccc:dddd:eeee:ffff");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[1:a::b:2]"));
		ASSERT_EQ(url.sHost, "1:a::b:2");
		ASSERT_EQ(url.GetPort(), defaultPort);
}

TEST(http, url_host_port_ipv6_port)
{
		tHttpUrl url;

		ASSERT_TRUE(url.SetHttpUrl("[::]:8000"));
		ASSERT_EQ(url.sHost, "::");
		ASSERT_EQ(url.GetPort(), 8000);

		ASSERT_TRUE(url.SetHttpUrl("[::1]:8001"));
		ASSERT_EQ(url.sHost, "::1");
		ASSERT_EQ(url.GetPort(), 8001);

		ASSERT_TRUE(url.SetHttpUrl("[1::]:8002"));
		ASSERT_EQ(url.sHost, "1::");
		ASSERT_EQ(url.GetPort(), 8002);

		ASSERT_TRUE(url.SetHttpUrl("[1::1]:8003"));
		ASSERT_EQ(url.sHost, "1::1");
		ASSERT_EQ(url.GetPort(), 8003);

		ASSERT_TRUE(url.SetHttpUrl("[::a]:8004"));
		ASSERT_EQ(url.sHost, "::a");
		ASSERT_EQ(url.GetPort(), 8004);

		ASSERT_TRUE(url.SetHttpUrl("[a::]:8005"));
		ASSERT_EQ(url.sHost, "a::");
		ASSERT_EQ(url.GetPort(), 8005);

		ASSERT_TRUE(url.SetHttpUrl("[a::a]:8006"));
		ASSERT_EQ(url.sHost, "a::a");
		ASSERT_EQ(url.GetPort(), 8006);

		ASSERT_TRUE(url.SetHttpUrl("[1:2:3:4:5:6:7:8]:8007"));
		ASSERT_EQ(url.sHost, "1:2:3:4:5:6:7:8");
		ASSERT_EQ(url.GetPort(), 8007);

		ASSERT_TRUE(url.SetHttpUrl("[1111:2222:3333:4444:5555:6666:7777:8888]:8008"));
		ASSERT_EQ(url.sHost, "1111:2222:3333:4444:5555:6666:7777:8888");
		ASSERT_EQ(url.GetPort(), 8008);

		ASSERT_TRUE(url.SetHttpUrl("[a:a:b:b:c:d:e:f]:8009"));
		ASSERT_EQ(url.sHost, "a:a:b:b:c:d:e:f");
		ASSERT_EQ(url.GetPort(), 8009);

		ASSERT_TRUE(url.SetHttpUrl("[aaaa:aabb:bbaa:bbbb:cccc:dddd:eeee:ffff]:8010"));
		ASSERT_EQ(url.sHost, "aaaa:aabb:bbaa:bbbb:cccc:dddd:eeee:ffff");
		ASSERT_EQ(url.GetPort(), 8010);

		ASSERT_TRUE(url.SetHttpUrl("[1:a::b:2]:8011"));
		ASSERT_EQ(url.sHost, "1:a::b:2");
		ASSERT_EQ(url.GetPort(), 8011);
}

// RFC6874
TEST(http, url_host_port_ipv6_zone)
{
		tHttpUrl url;
		uint16_t defaultPort = url.GetPort();

		ASSERT_TRUE(url.SetHttpUrl("[fe80::1%eth0]", false));
		ASSERT_EQ(url.sHost, "fe80::1%eth0");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("[fe80::2%25eth1]", true));
		ASSERT_EQ(url.sHost, "fe80::2%eth1");
		ASSERT_EQ(url.GetPort(), defaultPort);
}

// Non-RFC3986-compliant case: undelimited IPv6 literal (always without a port)
TEST(http, url_host_port_ipv6_undelimited)
{
		tHttpUrl url;
		uint16_t defaultPort = url.GetPort();

		ASSERT_TRUE(url.SetHttpUrl("::"));
		ASSERT_EQ(url.sHost, "::");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("::1"));
		ASSERT_EQ(url.sHost, "::1");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("1::"));
		ASSERT_EQ(url.sHost, "1::");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("1::1"));
		ASSERT_EQ(url.sHost, "1::1");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("::a"));
		ASSERT_EQ(url.sHost, "::a");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("a::"));
		ASSERT_EQ(url.sHost, "a::");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("a::a"));
		ASSERT_EQ(url.sHost, "a::a");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("1:2:3:4:5:6:7:8"));
		ASSERT_EQ(url.sHost, "1:2:3:4:5:6:7:8");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("1111:2222:3333:4444:5555:6666:7777:8888"));
		ASSERT_EQ(url.sHost, "1111:2222:3333:4444:5555:6666:7777:8888");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("a:a:b:b:c:d:e:f"));
		ASSERT_EQ(url.sHost, "a:a:b:b:c:d:e:f");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("aaaa:aabb:bbaa:bbbb:cccc:dddd:eeee:ffff"));
		ASSERT_EQ(url.sHost, "aaaa:aabb:bbaa:bbbb:cccc:dddd:eeee:ffff");
		ASSERT_EQ(url.GetPort(), defaultPort);

		ASSERT_TRUE(url.SetHttpUrl("1:a::b:2"));
		ASSERT_EQ(url.sHost, "1:a::b:2");
		ASSERT_EQ(url.GetPort(), defaultPort);
}

TEST(http, url_host_port_invalid)
{
		tHttpUrl url;
		ASSERT_FALSE(url.SetHttpUrl(":foo]"));
		ASSERT_FALSE(url.SetHttpUrl(":1111]"));
		ASSERT_FALSE(url.SetHttpUrl("[1111"));
		ASSERT_FALSE(url.SetHttpUrl("[1111:f0"));
		ASSERT_FALSE(url.SetHttpUrl("[:::1]"));
		ASSERT_FALSE(url.SetHttpUrl(":::affe"));
		ASSERT_FALSE(url.SetHttpUrl("[:::1] :1234"));
		ASSERT_FALSE(url.SetHttpUrl("[:::1]lol:1234"));
		ASSERT_FALSE(url.SetHttpUrl("[:::1]lol?=asdf"));
		/* currently not catched because not validating in detail
		ASSERT_FALSE(url.SetHttpUrl("[1:2:3:4:5:6:7:8:9]"));
		ASSERT_FALSE(url.SetHttpUrl("1:2:3:4:5:6:7:8:9"));
		ASSERT_FALSE(url.SetHttpUrl("[1::2::3::4]"));
		ASSERT_FALSE(url.SetHttpUrl("1::2::3::4"));
		*/
}
