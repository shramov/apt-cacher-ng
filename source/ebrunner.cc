#include "ebrunner.h"
#include "evabase.h"
#include "cleaner.h"
#include "dlcon.h"
#include <thread>

namespace acng
{

class evabaseFreeFrunner::Impl
{
public:
	dlcon dl;
	std::thread thr, evthr;
	evabase *m_eb;

	Impl(const IDlConFactory &pDlconFac)
			: dl("INTERN", pDlconFac)
	{
		m_eb = new evabase;
		evthr = std::thread([&]() { m_eb->MainLoop(); });
		thr = std::thread([&]() {dl.WorkLoop();});
	}

	~Impl()
	{
		::acng::cleaner::GetInstance().Stop();
		dl.SignalStop();
		m_eb->SignalStop();
		thr.join();
		evthr.join();
		delete m_eb;
	}

};
evabaseFreeFrunner::evabaseFreeFrunner(const IDlConFactory &pDlconFac)
{
	m_pImpl = new Impl(pDlconFac);
}

evabaseFreeFrunner::~evabaseFreeFrunner()
{
	delete m_pImpl;
}

dlcon& evabaseFreeFrunner::getDownloader()
{
	return m_pImpl->dl;
}

}
