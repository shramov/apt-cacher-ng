#include "main.h"
#include "acfg.h"
#include "acsmartptr.h"
#include "../src/conserver.cc"

#include <unordered_map>

#include <sys/socket.h>

TEST(utserver, startup_ports)
{
	auto p = conserver::Create(*g_res);
	cfg::udspath.clear();
	cfg::adminpath.clear();
	cfg::port = find_free_port();
	ASSERT_TRUE(p->Setup());
	//ASSERT_EQ(p->)
	auto* q = static_cast<conserverImpl*>(p.get());
	//event_base_once(evabase::base)
	event_base_loop(evabase::base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
	vector<int> x;
	for(auto& el: q->m_listeners)
	{
		auto fd = event_get_fd(el.get());
		x.push_back(fd);
	}
	p->Abandon();
	event_base_loop(evabase::base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
	for(auto n: x)
	{
		EXPECT_NE(0, ::send(n, &q, sizeof(q), 0));
	}

	auto test_multi = make_fake_server(12);
	EXPECT_EQ(12, test_multi.ports.size());
	q = static_cast<conserverImpl*>(test_multi.serva.get());
	EXPECT_EQ(q->m_listeners.size(), 12);
}

