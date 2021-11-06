#include "acstartstop.h"

#include <deque>

namespace acng
{

std::deque<tAction> g_teardownlist;
tStartStop* tStartStop::g_instance = nullptr;

tStartStop::~tStartStop()
{
	while (!g_teardownlist.empty())
	{
		g_teardownlist.back()();
		g_teardownlist.pop_back();
	}
	g_instance = nullptr;
}

void tStartStop::atexit(tAction act)
{
	g_teardownlist.emplace_back(move(act));
}

}
