#include "gtest/gtest.h"
//#include "gmock/gmock.h"
#include "conserver.h"
#include "acfg.h"
#include <unordered_map>

#include <sys/socket.h>

using namespace acng;
using namespace std;

int find_free_port()
{
	while(true)
	{
		int a = random();
		if (a <=1024 || a >= 65535)
			continue;
		sockaddr_in test4;
		test4.sin_port = a;
		test4.sin_addr.s_addr = INADDR_ANY;
		test4.sin_family = AF_INET;
		int x = socket(AF_INET, SOCK_STREAM, 0);
		if (0 == ::bind(x, (struct sockaddr *) &test4, sizeof(test4)))
		{
			close(x);
			return a;
		}
	}
}

TEST(utserver, startup_ports)
{
	auto xres = acres::Create();
	auto p = conserver::Create(*xres);
	cfg::port = find_free_port();
	ASSERT_TRUE(p->Setup());
}

