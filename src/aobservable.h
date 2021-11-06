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
class aobservable : public tLintRefcounted
{
public:
	// move-only semantics
	class subscription
	{
		mutable lint_ptr<aobservable> observable;
		mutable TActionList::iterator what;
		subscription(const subscription&) =delete;
		friend class aobservable;
	public:
		subscription() =default;
		~subscription() { clear(); }
		subscription(decltype(observable) _us, decltype (what) _what) : observable(_us), what(_what) {}
		subscription& operator=(subscription&& src)
		{
			if (this != &src)
			{
				observable.swap(src.observable);
				what = src.what;
			}
			return *this;
		}
		subscription(subscription&& src)
		{
			*this = std::move(src);
		}
		bool valid() const { return observable.get(); }
		void clear()
		{
			if(valid())
			{
				observable->unsubscribe(what);
				observable.reset();
			}
		}
	};

	aobservable() =default;
	virtual ~aobservable() =default;
	using TNotifier = tAction;
	subscription subscribe(const TNotifier& newSubscriber) WARN_UNUSED;
	bool notify()
	{
		if (m_observers.empty() || m_bNotifyPending)
			return false;
		doSchedule();
		return true;
	}

	bool hasObservers() { return !m_observers.empty();}

	protected:

	friend class subscription;
	void unsubscribe(TActionList::iterator what);

private:
	// list might be not the most efficient but it's good for iterator stability along with the ease of element removal
	TActionList m_observers;
	// remember which element we are at. If reset while processing -> flag to remove the current element.
	TActionList::iterator m_currentlyProcessing;
	bool m_bNotifyPending = false;
	void doNotify();
	void doSchedule();
};

// repurposed the old locking macro to ensure the correct thread context
#define setLockGuard ASSERT_HAVE_MAIN_THREAD

}

#endif // AOBSERVABLE_H
