#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "httpdate.h"
#include "header.h"
#include "job.h"
#include "ebrunner.h"
#include "event2/bufferevent.h"

#include <conn.h>

using namespace acng;
using namespace std;

class t_conn_dummy : public IConnBase
{
    // ISharedConnectionResources interface
public:
    dlcontroller *SetupDownloader() override
    {
        return nullptr;
    }
	lint_ptr<IFileItemRegistry> GetItemRegistry() override
	{
		return lint_ptr<IFileItemRegistry>();
	}

	// IConnBase interface
public:
	void poke(uint_fast32_t) override
	{
	};
	cmstring &getClientName() override
	{
		static cmstring nix;
		return nix;
	};
} conn_dummy;

TEST(job, create)
{
	job j(conn_dummy);
    header h;
	evabaseFreeRunner ioApp(false);
	auto bev = bufferevent_socket_new(evabase::base, 0, 0);
	// void job::Prepare(const header &h, bufferevent* be, size_t headLen, cmstring& callerHostname)
	j.Prepare(h, bev, 0, "127.0.0.1");
	ASSERT_EQ(j.m_activity, job::STATE_SEND_BUF_NOT_FITEM);
	//beconsum bc(besender(bev));
	beconsum bc(j.m_preHeadBuf.get());
	auto omem = bc.linear();
	ASSERT_TRUE(omem.find("403 Invalid path") != stmiss);
	auto hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\n\r\n"sv;
	auto res = h.Load(hdata);
    ASSERT_GT(res, 0);
	j.Prepare(h, bev, hdata.size(), "127.0.0.1");
	ASSERT_TRUE(bc.linear().find("HTTP/1.1 403 Forbidden file type or location") != stmiss);
}
