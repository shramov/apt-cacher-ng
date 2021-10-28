#include "aevutil.h"
#include "sockio.h"
#include "fileio.h"
#include "debug.h"

#include "event2/bufferevent.h"

namespace acng
{

void be_free_close(bufferevent *be)
{
	if (!be)
		return;
	auto fd = bufferevent_getfd(be);
	bufferevent_free(be);
	checkforceclose(fd);
}

void cbShutdownEvent(struct bufferevent *bev, short what, void *)
{
	LOGSTARTFUNCs;
	if (what & (BEV_EVENT_ERROR | BEV_EVENT_EOF | BEV_EVENT_TIMEOUT))
	{
		ldbg(what);
		return be_free_close(bev);
	}
}

void cbOutbufEmpty(bufferevent* pBE, void*)
{
	LOGSTARTFUNCs;
	if (evbuffer_get_length(besender(pBE)))
		return; // not done yet
	shutdown(bufferevent_getfd(pBE), SHUT_WR);
}

void cbDropInput(bufferevent* pBE, void*)
{
	LOGSTARTFUNCs;
	auto eb = bereceiver(pBE);
	evbuffer_drain(eb, evbuffer_get_length(eb));
}


void be_flush_free_close(bufferevent *be)
{
	LOGSTARTFUNCs;
//	bufferevent_disable(be, EV_READ | EV_WRITE);
	bufferevent_setwatermark(be, EV_WRITE, 0, INT_MAX);
	auto nRest = evbuffer_get_length(besender(be));
	bufferevent_setcb(be, cbDropInput, cbOutbufEmpty, cbShutdownEvent, be);
	bufferevent_enable(be, EV_WRITE | EV_READ);
	if (nRest == 0) // then just push the shutdown and wait for FIN,ACK
		cbOutbufEmpty(be, nullptr);
	else
		bufferevent_flush(be, EV_WRITE, BEV_NORMAL);
	// there is BEV_FLUSH but the source code looks shady, not clear if shutdown() is ever called
}


}
