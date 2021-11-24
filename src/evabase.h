#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "config.h"
#include "actemplates.h"
#include <memory>
#include <thread>

#include <event.h>

#define ASSERT_HAVE_MAIN_THREAD ASSERT(std::this_thread::get_id() == evabase::GetMainThreadId())

extern "C"
{
// XXX: forward-declaring only to avoid including ares.h right here; maybe can still do that and use ares_channel typedef directly.
struct ares_channeldata;
}

namespace acng
{

struct CDnsBase : public std::enable_shared_from_this<CDnsBase>
{
	ares_channeldata* get() const { return m_channel; }
	void shutdown();
	~CDnsBase();
	bool Init();

	// ares helpers
	void sync();
	void dropEvents();
	void setupEvents();


private:
	friend class evabase;
	ares_channeldata* m_channel = nullptr;
	//	void Deinit();
	CDnsBase(decltype(m_channel) pBase) : m_channel(pBase) {}

	// activated when we want something from ares or ares from us
	event *m_aresSyncEvent = nullptr;
	std::vector<event*> m_aresEvents;
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
static void InitDnsOrCheckCfgChange();

static std::thread::id GetMainThreadId();

/**
 * Runs the main loop for a program around the event_base loop.
 * When finished, clean up some resources left behind (fire off specific events
 * which have actions that would cause blocking otherwise).
 */
int MainLoop();

static void SignalStop();

/**
 * Push an action into processing queue. In case of ongoing cancelation (like in shutdown case), the action is run with the bool argument set to true.
 * Method is reentrant and thread-safe - input from the IO thread gets higher processing priority.
 */
static void Post(tCancelableAction&&);

/**
 * @brief Post an action which will be run later (provided that the event loop is run), but the actions might be discarded in shutdown scenario.
 */
static void Post(tAction&&);

/**
 * @brief Execute in-place if on main thread, otherwise Post it
 */
//static void PostOrRun(tCancelableAction&&);

static inline bool IsMainThread() { return GetMainThreadId() == std::this_thread::get_id(); }


static void addTeardownAction(event_callback_fn matchedCback, std::function<void(t_event_desctor)> action);

evabase();
~evabase();

void PushLoop();
};

}

#endif
