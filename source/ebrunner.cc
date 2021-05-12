#include "ebrunner.h"
#include "evabase.h"
#include "cleaner.h"
#include "dlcon.h"
#include <thread>

using namespace std;

namespace acng
{
void SetupCleaner();

class evabaseFreeFrunner::Impl
{
public:
	SHARED_PTR<dlcon> dl;
	thread dlthr, evthr;
	unique_ptr<evabase> m_eb;

	Impl(const IDlConFactory &pDlconFac, bool withDownloader)
		:m_eb(new evabase)
	{
		SetupCleaner();
		if (withDownloader)
			dl = dlcon::CreateRegular(pDlconFac);
		evthr = std::thread([&]() { m_eb->MainLoop(); });
		dlthr = std::thread([&]() {dl->WorkLoop();});
	}

	~Impl()
	{
		::acng::cleaner::GetInstance().Stop();
		if (dl)
			dl->SignalStop();
		m_eb->SignalStop();

		if (dl)
			dlthr.join();
		evthr.join();
	}

};
evabaseFreeFrunner::evabaseFreeFrunner(const IDlConFactory &pDlconFac, bool withDownloader)
{
	m_pImpl = new Impl(pDlconFac, withDownloader);
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
