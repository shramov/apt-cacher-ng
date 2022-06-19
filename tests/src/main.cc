#include "gtest/gtest.h"
#include "acres.h"
#include "evabase.h"
#include <locale.h>

using namespace acng;

void pushEvents(int secTimeout, bool* abortVar)
{
	for(auto dateEnd = time(0) + secTimeout;
		time(0) < dateEnd
		&& (abortVar == nullptr || !*abortVar)
		&& ! evabase::GetGlobal().IsShuttingDown()
		;)
	{
		event_base_loop(evabase::base, EVLOOP_NONBLOCK | EVLOOP_ONCE);
	}
}

acres* g_res;

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "C");
	auto p = acng::evabase::Create();
	g_res = acres::Create();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

namespace acng
{
#ifdef DEBUG
void dbg_handler(evutil_socket_t, short, void*) {}
#endif
}
