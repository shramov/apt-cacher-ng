#include "gtest/gtest.h"
#include "cacheman.h"
#include "acfg.h"
#include "acregistry.h"
#include "gmock/gmock.h"

#include <unordered_map>

using namespace acng;

#define TEST_DIR "_tmp/"
#define IPATH TEST_DIR "Index"

#if 1
struct cachemanHandler : public cacheman
{
cachemanHandler(tRunParms&& p) : cacheman(std::move(p)) {}
bool ProcessRegular(const std::string &, const struct stat &) override {return true;}
bool ProcessOthers(const std::string &, const struct stat &) override {return true;}
bool ProcessDirAfter(const std::string &, const struct stat &) override {return true;}
protected:
	virtual void Action() override {}

// cacheman interface
public:
	eDlResult Download(cmstring &sFilePathRel, bool bIsVolatileFile, eDlMsgPrio msgLevel,
					   const tHttpUrl *pForcedURL, unsigned hints, cmstring *sGuessedFrom,
					   bool bForceReDownload) override
	{
		auto exBeg = mstring(TEST_DIR "T-20");
		auto beg = sFilePathRel.substr(0,  exBeg.size());
		EXPECT_EQ(beg, exBeg);
		EXPECT_TRUE(endsWithSzAr(sFilePathRel, ".gz"));
		auto cmd = cmstring("test -e ") + sFilePathRel + " || "
				"wget http://ftp.debian.org/debian/dists/sid/main/binary-amd64/Packages.diff/"
				+ sFilePathRel.substr(sizeof(TEST_DIR)-1)
					+ " -O " + sFilePathRel;
		auto dled = system(cmd.c_str());
		EXPECT_EQ(0, dled);
		return dled == 0 ? eDlResult::OK : eDlResult::FAIL_REMOTE;
	}
	bool Inject(cmstring &, cmstring &, bool , off_t , tHttpDate , string_view) override
	{
		return true;
	}
};

class TestPtItem : public IMaintJobItem
{
public:
	TestPtItem(std::unique_ptr<mainthandler> &&han)
		: IMaintJobItem(std::move(han), this) {}

	std::unique_ptr<ICacheDataSender> GetCacheSender() override
	{
		return std::unique_ptr<ICacheDataSender>();
	}
	void Send(evbuffer *) override {};
	void Send(string_view) override {};
	void Eof() override {};
};

std::string curDir()
{
	char pbuf[PATH_MAX];
	return getcwd(pbuf, _countof(pbuf));
}


TEST(cacheman, pdiff)
{
	using namespace acng;
	using namespace std;

	auto res = acres::Create();
	unique_ptr<mainthandler> handler;
	lint_ptr<IMaintJobItem> item;

	mainthandler::tRunParms opts
	{
		EWorkType::STYLESHEET,
				"?noop",
				-1,
				nullptr,
				nullptr,
				*res
	};

	auto p = new cachemanHandler(std::move(opts));
	auto& tm = *p;
	handler.reset(p);
	item.reset(new TestPtItem(move(handler)));

	tStrDeq input { "_tmp/base.doesntexist.Packages.xz" };
	cfg::suppdir = curDir();
	// those files are not registered, should bounce
	ASSERT_EQ(-1, tm.PatchOne(IPATH, input));
	cacheman::tIfileAttribs& setter1 = tm.SetFlags(input.front());
	setter1.vfile_ondisk = true;
	setter1.uptodate = false;
	ASSERT_NE(-1, tm.PatchOne(IPATH, input));
	ASSERT_EQ(0, system("mkdir -p _tmp"));
	// http://snapshot.debian.org/archive/debian/20201004T083822Z/dists/sid/main/binary-amd64/Packages.xz

	ASSERT_EQ(0, system("wget -O " IPATH " http://ftp.debian.org/debian/dists/sid/main/binary-amd64/Packages.diff/Index"));
	auto twoDaysAgo = GetTime() - 2 * 86400;
	twoDaysAgo -= twoDaysAgo % 86400;
	auto s2dago = ltos(twoDaysAgo);
	auto pbase = cmstring("_tmp/") + s2dago;
	auto cmd = cmstring("set -xe ; test -e ") + pbase + ".orig || wget $(date -d @" + s2dago
			+ " +http://snapshot.debian.org/archive/debian/"
			+ "%Y%m%dT%H%M%SZ/dists/sid/main/binary-amd64/Packages.xz) -O " + pbase + ".orig; "
			+ "xzcat " + pbase + ".orig > " + pbase;
	ASSERT_EQ(0, system(cmd.c_str()));
	{
		Cstat info(pbase);
		ASSERT_TRUE(info);
	}
	mstring pbase_sum, s;
	size_t pbase_len(0);

	input.emplace_back(pbase);
	ASSERT_EQ(0, tm.PatchOne(IPATH, input));

	{
		filereader rd;
		ASSERT_TRUE(rd.OpenFile(IPATH, true, 1));
		while (rd.GetOneLine(s))
		{
			if (s.size() > 80 && startsWithSz(s, "SHA256-Current: "))
			{
				pbase_len = atoi(s.c_str() + 80);
				pbase_sum = s.substr(16, 64);
				trimBoth(pbase_sum);
			}
		}
		ASSERT_EQ(pbase_sum.size(), 64);
		ASSERT_TRUE(pbase_len > 0);
	}
}
#endif
