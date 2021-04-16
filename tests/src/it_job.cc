#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "httpdate.h"
#include "header.h"
#include "job.h"

#include <conn.h>

using namespace acng;

class t_conn_dummy : public ISharedConnectionResources
{
    // ISharedConnectionResources interface
public:
    dlcon *SetupDownloader() override
    {
        return nullptr;
    }
    void LogDataCounts(cmstring &, mstring , off_t , off_t , bool ) override
    {
    }
} conn_dummy;

TEST(job, create)
{
    job j(conn_dummy);
    header h;
    j.Prepare(h, ""sv);
    ASSERT_TRUE(j.m_sendbuf.view().find("403 Invalid path") != stmiss);
    auto hdata = "GET /na/asdfasdfsadf"sv;
    h.Load(hdata);
    j.Prepare(h, hdata);
    ASSERT_TRUE(j.m_sendbuf.view().find("403 Invalid path") != stmiss);
    hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\n";
    auto res = h.Load(hdata);
    ASSERT_LE(res, 0);
    hdata = "GET /na/asdfasdfsadf HTTP/1.1\r\n\r\n";
    res = h.Load(hdata);
    ASSERT_GT(res, 0);
    j.Prepare(h, hdata);
    ASSERT_TRUE(j.m_sendbuf.view().find("HTTP/1.1 403 Forbidden file type or location") != stmiss);
}
