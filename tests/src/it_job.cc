#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "httpdate.h"
#include "header.h"
#include "job.h"

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
	bool poke(uint_fast32_t jobId) override
	{
		return true;
	};
	cmstring &getClientName() override
	{
		return sEmptyString;
	};
} conn_dummy;

#warning restore me
#if 0
TEST(job, create)
{
    job j(conn_dummy);
    header h;
	j.Prepare(h, ""sv, "127.0.0.1");
    ASSERT_TRUE(j.m_sendbuf.view().find("403 Invalid path") != stmiss);
	auto hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\n\r\n";
	auto res = h.Load(hdata);
    ASSERT_GT(res, 0);
	j.Prepare(h, hdata, "127.0.0.1");
    ASSERT_TRUE(j.m_sendbuf.view().find("HTTP/1.1 403 Forbidden file type or location") != stmiss);

#ifdef DEBUG
	j.Dispose();
#endif
}
#endif
