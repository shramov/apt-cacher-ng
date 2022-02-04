#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "fileio.h"
#include "acbuf.h"
#include "filereader.h"
#include <fcntl.h>

using namespace acng;

#define PERMS 00664

TEST(fileio,fsattr)
{
	Cstat st("/bin/sh");
	Cstat st2("/etc/resolv.conf");
	ASSERT_TRUE(st);
	ASSERT_EQ(st.info().st_dev, st2.info().st_dev);
}

TEST(fileio, operations)
{
	ASSERT_EQ(0, FileCopy("/etc/passwd", "passwd_here").value());
}

TEST(fileio, ut_acbuf)
{
	tSS x;
	auto path = "dummy.file";
	x << "y";
	ASSERT_EQ(1, x.dumpall(path, O_CREAT, PERMS, INT_MAX, true));
	x << "zzz";
	ASSERT_EQ(3, x.dumpall(path, O_CREAT, PERMS, INT_MAX, false));
	x << "y";
	ASSERT_EQ(1, x.dumpall(path, O_CREAT, PERMS, INT_MAX, true));
	Cstat st(path);
	ASSERT_EQ(1, st.size());
}

TEST(filereader, zcat)
{
	LPCSTR input(nullptr);
	for (auto path: {"testblob", "../tests/testblob", "../../apt-cacher-ng/tests/testblob"})
	{
		if (access(path, R_OK) == 0)
		{
			input = path;
			break;
		}
	}
	ASSERT_TRUE(input);
	filereader r;

	for (int extra = 0; extra < 3; ++extra)
	{
		for (LPCSTR sfx: {"", ".gz"})
		{
			ASSERT_TRUE(r.OpenFile(mstring(input) + sfx, false, extra));
			auto n = 0;
			mstring line;
			while (r.GetOneLine(line))
			{
				EXPECT_TRUE(r.CheckGoodState(false));
				++n;
			}
			EXPECT_TRUE(r.CheckGoodState(false));
			ASSERT_EQ(n, 32 + extra);
		}
	}
}
