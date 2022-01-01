#include "gtest/gtest.h"
#include "acres.h"
#include "evabase.h"

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
	auto p = acng::evabase::Create();
	g_res = acres::Create();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
