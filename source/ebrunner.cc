#include "ebrunner.h"

namespace acng
{

evabaseFreeFrunner::evabaseFreeFrunner(const IDlConFactory &pDlconFac)
		: dl("INTERN", pDlconFac),
		  evthr([&]() { MainLoop(); }),
		  thr([&]() {dl.WorkLoop();})
{
}

evabaseFreeFrunner::~evabaseFreeFrunner()
{
	::acng::cleaner::GetInstance().Stop();
			dl.SignalStop();
			SignalStop();
			thr.join();
			evthr.join();
}

}
