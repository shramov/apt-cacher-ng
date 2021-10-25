#include "tpool.h"
#include "meta.h"
#include "aclogger.h"

#include <mutex>
#include <condition_variable>
#include <thread>

using namespace std;

namespace acng {

class tpoolImpl : public tpool
{
	unsigned m_nMaxBacklog, m_nMaxActive, m_nMaxStandby;
	unsigned m_nCurStandBy = 0, m_nCurActive = 0;
	std::deque<std::function<void()>> m_backLog;
	bool m_shutdown = false;
	std::mutex mx;
	std::condition_variable cv;

public:

	tpoolImpl(unsigned maxBacklog, unsigned maxActive, unsigned maxStandby)
		: m_nMaxBacklog(maxBacklog), m_nMaxActive(maxActive), m_nMaxStandby(maxStandby)
	{
	}

	void ThreadAction()
	{
		std::unique_lock<std::mutex> g(mx);

		while (true)
		{
			if (m_shutdown)
			{
				m_nCurStandBy--;
				break;
			}
			if (m_backLog.empty())
			{
				cv.wait(g);
				continue;
			}
			auto c = move(m_backLog.front());
			m_backLog.pop_front();

			m_nCurStandBy--;
			m_nCurActive++;
			g.unlock();
			// run and release the work item, not in critical section!
			try
			{
				c();
			}
			catch (const std::exception& ex)
			{
				log::err(ex.what());
			}
			catch (...)
			{
				log::err("Unknown exception in background thread function"sv);
			}

			c = decltype (c)();
			g.lock();
			m_nCurActive--;
			if (m_nCurStandBy >= m_nMaxStandby || m_shutdown)
				break;
			m_nCurStandBy++;
		}
		cv.notify_all();
	};


	bool schedule(std::function<void ()> action) override
	{
		std::unique_lock<std::mutex> g(mx);
		if (m_backLog.size() > m_nMaxBacklog)
		{
			return false;
		}
		try
		{
			m_backLog.emplace_back(std::move(action));
			cv.notify_one();

			if (m_nCurStandBy < 1 && m_nCurActive < m_nMaxActive)
			{
				try
				{
					std::thread thr(&tpoolImpl::ThreadAction, this);
					thr.detach();
				}
				catch (...)
				{
					// can actually only happen in OOM condition and thread::detach should not fail, so just reject this request
					return false;
				}
				m_nCurStandBy++;
			}
		}
		catch (...)
		{
			return false;
		}
		return true;
	}

	void stop() override
	{
		std::unique_lock<std::mutex> g(mx);
		m_shutdown = true;
		cv.notify_all();
		while (m_nCurStandBy + m_nCurActive)
			cv.wait(g);
	}
};

#warning init this everywhere evabase init is performed
std::shared_ptr<tpool> tpool::Create(unsigned maxBacklog, unsigned maxActive, unsigned maxStandby)
{
	return std::make_shared<tpoolImpl>(maxBacklog, maxActive, maxStandby);
}

SHARED_PTR<tpool> ACNG_API g_tpool;

}
