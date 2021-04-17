#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "fileio.h"

using namespace acng;

TEST(fileio,fsattr)
{
	Cstat st("/bin/sh");
	Cstat st2("/etc/resolv.conf");
	ASSERT_TRUE(st);
	ASSERT_EQ(st.st_dev, st2.st_dev);
}

TEST(fileio,operations)
{
	ASSERT_EQ(0, FileCopy("/etc/passwd", "passwd_here").value());
}
