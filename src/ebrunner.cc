#include "ebrunner.h"
#include "evabase.h"
#include "dlcon.h"
#include <thread>


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
	evabase::in_shutdown = true;
	dler.reset();
	delete sharedResources;
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
