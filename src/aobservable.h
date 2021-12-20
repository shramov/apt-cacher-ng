#ifndef AOBSERVABLE_H
#define AOBSERVABLE_H

#include "actypes.h"
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
	using subscription = TFinalAction;

	aobservable() =default;
	virtual ~aobservable()
	{

	};
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

	friend struct TFinalAction;
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

}

#endif // AOBSERVABLE_H
