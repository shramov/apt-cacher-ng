#ifndef AOBSERVABLE_H
#define AOBSERVABLE_H

#include "actypes.h"
#include "evabase.h"
#include "debug.h"
#include "acsmartptr.h"
#include "actemplates.h"

#include <list>
#include <utility>
#include <thread>

namespace acng
{
class aobservable;
using TActionList = std::list<tAction>;

/*
 * This extends the classic observer pattern by adding delegates which actually do notifications,
 * where the internal rules are encapsulated in the agents, and the external rules only command
 * on the unsubscription.
 */
class aobservable : public SomeData, public tLintRefcounted
{
public:
	struct TUnsub
	{
		mutable lint_ptr<aobservable> us;
		mutable TActionList::iterator what;
		~TUnsub() { us->unsubscribe(what); }
		TUnsub(decltype(us) _us, decltype (what) _what) : us(_us), what(_what) {}
	};

	// XXX: replace with a auto_raii based (or similar) solution to avoid use of allocator
	using TUnsubKey = std::unique_ptr<TUnsub>;

	aobservable() =default;
	virtual ~aobservable() =default;
	using TNotifier = tAction;
	TUnsubKey subscribe(const TNotifier& newSubscriber) WARN_UNUSED;

private:
	friend class TUnsub;
	void unsubscribe(TActionList::iterator what);

public:
	/**
	 * @brief Returns a unsubkey which is not subscribed YET but contains a lint_ptr reference
	 */
	TUnsubKey getUnsubscribedKey();

protected:
	/**
	 * @brief notifyAll triggers deferred (!) execution of all subscriber callbacks and a cleanup of invalidates ones.
	 */
	void notifyAll();

	/**
	 * @brief zz_internalSubscribe is a special purpose installer of callbacks, with no lifecycle management!
	 * @param subscriberCode
	 */
	TActionList::iterator zz_internalSubscribe(const TNotifier& subscriberCode);

private:
	// list might be not the most efficient but it's good for iterator stability along with the ease of element removal
	TActionList m__subscribedObservers;
	std::vector<TActionList::iterator> m__observers2delete;
	bool m__bAtEventProcessing = false;
	void doEventNotify();
	void doEventCleanup();
};

// repurposed the old locking macro to ensure the correct thread context
#define setLockGuard ASSERT_HAVE_MAIN_THREAD

}

#endif // AOBSERVABLE_H
