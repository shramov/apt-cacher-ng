#include "ebrunner.h"
#include "evabase.h"
#include "dlcon.h"
#include "debug.h"

using namespace std;

namespace acng
{
void ac3rdparty_init();
void ac3rdparty_deinit();

tMinComStack::tMinComStack()
{
	ac3rdparty_init();
	ebase = new evabase;
	sharedResources = acres::Create();
	dler = dlcontroller::CreateRegular(*sharedResources);
}

tMinComStack::~tMinComStack()
{
	evabase::GetGlobal().SignalStop();
	ASSERT(1 == dler->__user_ref_cnt());
	dler.reset();
	delete sharedResources;
	// run last-minute termination actions
	evabase::GetGlobal().PushLoop();
	delete ebase;
	ac3rdparty_deinit();
}

dlcontroller &tMinComStack::getDownloader()
{
	return *dler;
}

event_base *tMinComStack::getBase()
{
	return ebase->base;
}

}
