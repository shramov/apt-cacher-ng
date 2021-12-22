#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "config.h"
#include "actemplates.h"
#include "aobservable.h"

#include <memory>
#include <thread>

#include <event.h>

#define ASSERT_HAVE_MAIN_THREAD ASSERT(std::this_thread::get_id() == evabase::GetMainThreadId())
// repurposed the old locking macro to ensure the correct thread context
#define setLockGuard ASSERT_HAVE_MAIN_THREAD

extern "C"
{
// XXX: forward-declaring only to avoid including ares.h right here; maybe can still do that and use ares_channel typedef directly.
struct ares_channeldata;
}

namespace acng
{
extern std::atomic_bool g_shutdownHint;

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

/**
 * This class is an adapter for general libevent handling, roughly fitting it into conventions of the rest of ACNG.
 * Partly static and partly dynamic, for pure convenience! Expected to be a singleton anyway.
 */
class ACNG_API evabase : public aobservable
{
	CDnsBase* m_cachedDnsBase = nullptr;
	void InitDnsOrCheckCfgChange();
public:
	static event_base *base;

	static evabase& GetGlobal();
	CDnsBase* GetDnsBase();

	static std::thread::id GetMainThreadId();

	/**
 * Runs the main loop for a program around the event_base loop.
 * When finished, clean up some resources left behind (fire off specific events
 * which have actions that would cause blocking otherwise).
 */
	int MainLoop();

	static void SignalStop();

	/**
 * @brief Post an action which will be run later (provided that the event loop is run), but the actions might be discarded in shutdown scenario.
 */
	static void Post(tAction&&);

	/**
 * @brief Execute in-place if on main thread, otherwise Post it
 */
	//static void PostOrRun(tCancelableAction&&);

	static inline bool IsMainThread() { return GetMainThreadId() == std::this_thread::get_id(); }

	/**
	 * @brief IsShuttingDown is non-binding information about ongoing shutdown phase.
	 * @return True if shutdown was requested.
	 */
	bool IsShuttingDown() { return g_shutdownHint; }

	~evabase();
	void PushLoop();
	// generic helper for BLOCKING method execution on the main thread
	uintptr_t SyncRunOnMainThread(std::function<uintptr_t()>, uintptr_t onRejection = 0);

	static lint_ptr<evabase> Create() { return lint_ptr<evabase>(new evabase); }

private:
	evabase();
};

}

#endif
