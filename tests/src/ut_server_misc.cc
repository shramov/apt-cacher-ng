#include "gtest/gtest.h"
#include "acres.h"
#include "evabase.h"
#include "main.h"
#include "acfg.h"
#include "meta.h"
#include "conserver.h"

#include <locale.h>

using namespace acng;

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

TFakeServer make_fake_server(int portCount, conserver* pAltServer)
{
	TFakeServer ret;
	if (pAltServer)
		ret.serva = lint_user_ptr<conserver>(pAltServer);
	else
		ret.serva = conserver::Create(*g_res);
	for(int n = portCount; n-- > 0;)
	{
		cfg::bindaddr += " 127.0.0.1:";
		ret.ports.push_back(find_free_port());
		cfg::bindaddr += ltos(ret.ports.back());
	}
	ret.serva->Setup();
	return ret;
}
