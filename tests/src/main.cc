#include "gtest/gtest.h"
#include "ac3rdparty.h"
#include "evabase.h"

using namespace acng;

void pushEvents(int secTimeout, bool* abortVar)
{
	for(auto dateEnd = time(0) + secTimeout;
		time(0) < dateEnd
		&& (abortVar == nullptr || !*abortVar)
		&& ! evabase::in_shutdown
		;)
	{
		event_base_loop(evabase::base, EVLOOP_NONBLOCK | EVLOOP_ONCE);
	}
}


int main(int argc, char **argv)
{
	acng::ac3rdparty_init();
	auto p = std::make_unique<acng::evabase>();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
