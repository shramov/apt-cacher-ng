#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "config.h"
#include <memory>
#include <functional>
#include <event.h>

struct evdns_base;

namespace acng
{

struct CDnsBase
{
	evdns_base* get() { return m_base; }
  void shutdown();
	~CDnsBase();
private:
	friend class evabase;
	evdns_base *m_base = nullptr;
	CDnsBase(evdns_base *pBase) : m_base(pBase) {}
};

struct t_event_desctor {
	evutil_socket_t fd;
	event_callback_fn callback;
	void *arg;
};

/**
 * This class is an adapter for general libevent handling, roughly fitting it into conventions of the rest of ACNG.
 * Partly static and partly dynamic, for pure convenience! Expected to be a singleton anyway.
 */
class ACNG_API evabase
{
public:
static event_base *base;
static std::atomic<bool> in_shutdown;

static std::shared_ptr<CDnsBase> GetDnsBase();
static void CheckDnsChange();

/**
 * Runs the main loop for a program around the event_base loop.
 * When finished, clean up some resources left behind (fire off specific events
 * which have actions that would cause blocking otherwise).
 */
int MainLoop();

static void SignalStop();

using tCancelableAction = std::function<void(bool)>;

/**
 * Push an action into processing queue. In case operation is not possible, runs the action with the cancel flag (bool argument set to true)
 */
static void Post(tCancelableAction&&);

static void addTeardownAction(event_callback_fn matchedCback, std::function<void(t_event_desctor)> action);

evabase();
~evabase();
};

}

#endif
