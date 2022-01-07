#ifndef AEVUTIL_H
#define AEVUTIL_H

#include "actypes.h"
#include "actemplates.h"

#include <event2/event.h>
#include <event2/buffer.h>
extern "C"
{
struct bufferevent;
void evbuffer_lock(evbuffer*);
void evbuffer_unlock(evbuffer*);
// this is to REMOVE data
struct evbuffer *bufferevent_get_input(struct bufferevent *bufev);
// this is to ADD data
struct evbuffer *bufferevent_get_output(struct bufferevent *bufev);
void bufferevent_free(struct bufferevent*);
}

#define CHECK_ALLOCATED(what) ASSERT(what); if (!what) throw std::bad_alloc();

namespace acng
{

/**
 * @brief evbuffer_dumpall - store all or limited range from the front to a file descriptor
 * This is actually evbuffer_write_atmost replacement without its sporadic abortion bug.
 */
ssize_t eb_dump_chunks(evbuffer* inbuf, int out_fd, size_t nMax2SendNow = -1);
ssize_t eb_dump_chunks(evbuffer* inbuf, std::function<void(string_view)>, size_t nMax2SendNow = -1);

inline evbuffer* besender(bufferevent* be) { return bufferevent_get_output(be); }
inline evbuffer* bereceiver(bufferevent* be) { return bufferevent_get_input(be); }
inline void send(evbuffer *dest, string_view sv) { if (evbuffer_add(dest, sv.data(), sv.length())) throw std::bad_alloc();}
inline void send(bufferevent *dest, string_view sv) { return send(besender(dest), sv); }

using unique_event = auto_raii<event*, event_free, nullptr>;
using unique_eb = auto_raii<evbuffer*, evbuffer_free, nullptr>;

/**
 * @brief bufferevent_free_fd_close destroys releases bufferevent AND closes the socket manually.
 * XXX: actually, send a shutdown command first?
 */
void be_free_close(bufferevent*);
void be_flush_free_close(bufferevent*);

#ifdef DEBUG
inline void BEV_FREE(bufferevent* bev)
{
	bufferevent_free(bev);
}
#else
#define BEV_FREE bufferevent_free
#endif

/**
 * This will simply finish the bufferent by freeing, closing of the FD must be handled by bufferevent (... CLOSE_ON_FREE)
 */
using unique_bufferevent = auto_raii<bufferevent*, BEV_FREE, nullptr>;
/**
 * Free the event when finishing and close its FD explicitly.
 */
using unique_bufferevent_fdclosing = auto_raii<bufferevent*, be_free_close, nullptr>;
/**
 * Flush the outgoing data by getting the confirmation from peer,
 * then free the event when finishing and close its FD explicitly.
 */
using unique_bufferevent_flushclosing = auto_raii<bufferevent*, be_flush_free_close, nullptr>;

// configure timeouts and enable a bufferent for bidirectional communication
void setup_be_bidirectional(bufferevent *be);

struct beconsum
{
    evbuffer *m_eb;
	beconsum(evbuffer* eb) : m_eb(eb) {}
	beconsum(bufferevent* eb) : m_eb(bufferevent_get_input(eb)) {}
    size_t size() { return evbuffer_get_length(m_eb); }
	beconsum& drop(size_t howMuch) { evbuffer_drain(m_eb, howMuch); return *this; }
	evbuffer* buf() { return m_eb; }
	/**
	 * @brief front_view gets contigous view on the first chunk in the buffer.
	 * @param limit if negative, grab the maximum size of the first chunk. If positive, then the size of the first chunk but not more than limit value
	 * @return string view structure with at most <limit> bytes
	 */
	string_view front(ssize_t limit = -1)
    {
            auto len = evbuffer_get_contiguous_space(m_eb);
			if (limit >= 0 && size_t(limit) < len)
				len = limit;
            return string_view((const char*) evbuffer_pullup(m_eb, len), len);
    }
	const string_view linear(size_t len) { return string_view(
					(const char*) evbuffer_pullup(m_eb, len), len); }
	// make the whole thing contiguous
	const string_view linear() { return linear(size()); }
};

inline ssize_t eb_move_range(evbuffer* src, evbuffer *tgt, size_t len)
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
/**
 * @brief eb_move_atmost little helper wrapper to increment one tracking position when needed
 * @param src
 * @param tgt
 * @param maxTake
 * @param updatePos
 * @return
 */
inline ssize_t eb_move_range(evbuffer* src, evbuffer *tgt, size_t maxTake, off_t& updatePos)
{
	auto n = eb_move_range(src, tgt, maxTake);
	if (n > 0)
		updatePos += n;
	return n;
}


}
#endif // AEVUTIL_H
