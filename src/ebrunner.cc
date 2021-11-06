#include "ebrunner.h"
#include "evabase.h"
#include "dlcon.h"
#include <thread>

using namespace std;

namespace acng
{
void SetupCleaner();

#warning FIXME, implement the abort timeout or maybe not, depending on the redesign
class evabaseFreeRunner::Impl
{
public:
	lint_ptr<dlcontroller> dl;
	thread evthr;
	unique_ptr<evabase> m_eb;

	Impl(bool withDownloader)
		:m_eb(new evabase)
	{
#warning why was cleaner needed?
//		SetupCleaner();
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
evabaseFreeRunner::evabaseFreeRunner(bool withDownloader)
{
	m_pImpl = new Impl(withDownloader);
}

evabaseFreeRunner::~evabaseFreeRunner()
{
	delete m_pImpl;
}

dlcontroller& evabaseFreeRunner::getDownloader()
{
	return * m_pImpl->dl;
}

event_base *evabaseFreeRunner::getBase()
{
	return m_pImpl->m_eb.get()->base;
}

}
