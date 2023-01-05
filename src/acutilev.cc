#include "acutilev.h"
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

ssize_t eb_move_range(evbuffer *src, evbuffer *tgt, size_t len)
{
	auto have = evbuffer_get_length(src);
	if (have == 0)
		return 0;

	if (len > have)
		len = have;

	do {
		auto limited = len > INT_MAX;
		int limit = limited ? INT_MAX : len;
		auto n = evbuffer_remove_buffer(src, tgt, limit);
		// if error or not augmented -> pass through
		if (!limited || n <= limit)
			return n;
		len -= limit;
	}
	while (len > 0);
	return -1;
}

ssize_t eb_move_range(evbuffer *src, evbuffer *tgt, size_t maxTake, off_t &updatePos)
{
	auto n = eb_move_range(src, tgt, maxTake);
	if (n > 0)
		updatePos += n;
	return n;
}

string_view beconsum::front(ssize_t limit)
{
	auto len = evbuffer_get_contiguous_space(m_eb);
	if (limit >= 0 && size_t(limit) < len)
		len = limit;
	return string_view((const char*) evbuffer_pullup(m_eb, len), len);
}


}
