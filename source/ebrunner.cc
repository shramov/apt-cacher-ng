#include "ebrunner.h"
#include "evabase.h"
#include "cleaner.h"
#include "dlcon.h"
#include <thread>

namespace acng
{
void SetupCleaner();

class evabaseFreeFrunner::Impl
{
public:
	SHARED_PTR<dlcon> dl;
	std::thread thr, evthr;
	unique_ptr<evabase> m_eb;

	Impl(const IDlConFactory &pDlconFac)
		: dl(dlcon::CreateRegular(pDlconFac)),
		  m_eb(new evabase)
	{
		SetupCleaner();
		evthr = std::thread([&]() { m_eb->MainLoop(); });
		thr = std::thread([&]() {dl->WorkLoop();});
	}

	~Impl()
	{
		::acng::cleaner::GetInstance().Stop();
		dl->SignalStop();
		m_eb->SignalStop();
		thr.join();
		evthr.join();
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
	return * m_pImpl->dl;
}

}
