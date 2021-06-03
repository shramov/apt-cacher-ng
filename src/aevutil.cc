#include "aevutil.h"
#include "sockio.h"
#include "fileio.h"

#include "event2/bufferevent.h"

namespace acng
{

void be_free_fd_close(bufferevent *be)
{
	if (!be)
		return;
	auto fd = bufferevent_getfd(be);
	bufferevent_free(be);
	checkforceclose(fd);
}

void be_flush_and_close(bufferevent *be)
{
#warning needing to flush
	bufferevent_free(be);
}

void be_flush_free_fd_close(bufferevent *be)
{
#warning this requires a special statemachine, shortcut for now
	return be_free_fd_close(be);

}


}
