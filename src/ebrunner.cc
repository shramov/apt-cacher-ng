#include "ebrunner.h"
#include "evabase.h"
#include "dlcon.h"
#include <thread>

using namespace std;

namespace acng
{
void SetupCleaner();

#warning FIXME, implement the abort timeout or maybe not, depending on the redesign
class evabaseFreeFrunner::Impl
{
public:
	SHARED_PTR<dlcontroller> dl;
	thread evthr;
	unique_ptr<evabase> m_eb;

	Impl(bool withDownloader)
		:m_eb(new evabase)
	{
		SetupCleaner();
		if (withDownloader)
			dl = dlcontroller::CreateRegular();
		evthr = std::thread([&]() { m_eb->MainLoop(); });
	}

	~Impl()
	{
		if (dl)
			dl->Dispose();
		m_eb->SignalStop();
		evthr.join();
	}

};
evabaseFreeFrunner::evabaseFreeFrunner(bool withDownloader)
{
	m_pImpl = new Impl(withDownloader);
}

evabaseFreeFrunner::~evabaseFreeFrunner()
{
	delete m_pImpl;
}

dlcontroller& evabaseFreeFrunner::getDownloader()
{
	return * m_pImpl->dl;
}

event_base *evabaseFreeFrunner::getBase()
{
	return m_pImpl->m_eb.get()->base;
}

}
