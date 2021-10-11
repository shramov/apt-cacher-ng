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
struct evbuffer *bufferevent_get_input(struct bufferevent *bufev);
void bufferevent_free(struct bufferevent*);
}

#define CHECK_ALLOCATED(what) ASSERT(what); if (!what) throw std::bad_alloc();

namespace acng
{

/**
 * @brief evbuffer_dumpall - store all or limited range from the front to a file descriptor
 * This is actually evbuffer_write_atmost replacement without its sporadic abortion bug.
 */
ssize_t eb_dump_atmost(evbuffer* inbuf, int out_fd, size_t nMax2SendNow);

/**
 * Move some data from begin of src to end of dest, but restrict the maximum length to maxLen.
 * @return -1 Fatal error occurred, state of src and dest are undefined
 */
ssize_t eb_move_atmost(evbuffer *dest, evbuffer *src, ssize_t maxLen);

struct TEbUniqueLock : public auto_raii<evbuffer*, evbuffer_unlock, nullptr>
{
        TEbUniqueLock(evbuffer* eb)
                : auto_raii<evbuffer*, evbuffer_unlock, nullptr>(eb)
        {
                evbuffer_lock(eb);
        }
};

inline evbuffer* input(bufferevent* be) { return bufferevent_get_input(be); }
inline void evbuffer_add(evbuffer *dest, string_view sv) { if (evbuffer_add(dest, sv.data(), sv.length())) throw std::bad_alloc();}

using unique_event = auto_raii<event*, event_free, nullptr>;
using unique_eb = auto_raii<evbuffer*, evbuffer_free, nullptr>;

/**
 * @brief bufferevent_free_fd_close destroys releases bufferevent AND closes the socket manually.
 * XXX: actually, send a shutdown command first?
 */
void be_free_fd_close(bufferevent*);
void be_flush_and_close(bufferevent*);
void be_flush_and_release_and_fd_close(bufferevent*);
/**
 * This will simply finish the bufferent by freeing, closing of the FD must be handled by bufferevent (... CLOSE_ON_FREE)
 */
using unique_bufferevent = auto_raii<bufferevent*, bufferevent_free, nullptr>;
/**
 * Free the event when finishing and close its FD explicitly.
 */
using unique_bufferevent_fdclosing = auto_raii<bufferevent*, be_free_fd_close, nullptr>;
/**
 * Flush the outgoing data by getting the confirmation from peer,
 * then free the event when finishing and close its FD explicitly.
 */
using unique_bufferevent_flushclosing = auto_raii<bufferevent*, be_flush_and_release_and_fd_close, nullptr>;

// configure timeouts and enable a bufferent for bidirectional communication
void setup_be_bidirectional(bufferevent *be);

struct beconsum
{
    evbuffer *m_eb;
	beconsum(evbuffer* eb) : m_eb(eb) {}
	beconsum(bufferevent* eb) : m_eb(bufferevent_get_input(eb)) {}
    size_t size() { return evbuffer_get_length(m_eb); }
	beconsum& drop(size_t howMuch) { evbuffer_drain(m_eb, howMuch); return *this; }
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
};

}
#endif // AEVUTIL_H
