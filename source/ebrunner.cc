#include "ebrunner.h"
#include "evabase.h"
#include "cleaner.h"

namespace acng
{

evabaseFreeFrunner::evabaseFreeFrunner(const IDlConFactory &pDlconFac)
		: dl("INTERN", pDlconFac)
{
	m_eb = new evabase;
	evthr = std::thread([&]() { m_eb->MainLoop(); });
	thr = std::thread([&]() {dl.WorkLoop();});
}

evabaseFreeFrunner::~evabaseFreeFrunner()
{
	::acng::cleaner::GetInstance().Stop();
	dl.SignalStop();
	m_eb->SignalStop();
	thr.join();
	evthr.join();
	delete m_eb;
}

}
