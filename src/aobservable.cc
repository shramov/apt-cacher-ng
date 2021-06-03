#include "aobservable.h"
#include <memory>

using namespace std;

namespace acng
{


aobservable::TUnsubKey aobservable::subscribe(const aobservable::TNotifier &newSubscriber)
{
	ASSERT_HAVE_MAIN_THREAD;

	if (!newSubscriber) // XXX: not accepting invalid subscribers to avoid later checks, but what then?
		return TUnsubKey(new TUnsub { as_lptr(this), m__subscribedObservers.end() });
	m__subscribedObservers.emplace_back(newSubscriber);
	auto it = m__subscribedObservers.end();
	//evabase::Post([me = as_lptr(this)] () mutable { me->doNotify(); });
	return TUnsubKey(new TUnsub { as_lptr(this), --it });
}

void aobservable::unsubscribe(TActionList::iterator what)
{
	ASSERT_HAVE_MAIN_THREAD;
	if (!m__bAtEventProcessing)
		m__subscribedObservers.erase(what);
	else
	{
		m__observers2delete.emplace_back(move(what));
		evabase::Post([me = as_lptr(this)] () mutable { me->doEventCleanup(); });
	}
}

aobservable::TUnsubKey aobservable::getUnsubscribedKey()
{
	return make_unique<TUnsub>(as_lptr(this), m__subscribedObservers.end());
}

void aobservable::notifyAll()
{
#warning analyse performance, maybe use a dedicated event which manages the notifications, and which merges all the execution requests, maybe even from multiple observables
	evabase::Post([me = as_lptr(this)] () mutable { me->doEventNotify(); });
}

void aobservable::doEventNotify()
{
	ASSERT_HAVE_MAIN_THREAD;

	doEventCleanup();

	m__bAtEventProcessing = true;
	for(const auto& el: m__subscribedObservers)
	{
		try
		{
			if (el)
				(el)();
		}
		catch(...)
		{
			ASSERT(!"uncaught exception!");
		}
	}
	m__bAtEventProcessing = false;
	// do all deferred destruction orders which might have been collected throughout the callback execution
	doEventCleanup();
}

void aobservable::doEventCleanup()
{
	if (m__observers2delete.empty())
		return;

	//auto keepa(move(m_meDeleteKeeper));
	for (auto it: m__observers2delete)
		m__subscribedObservers.erase(it);

	if (m__subscribedObservers.empty())
		return; // exit stack ASAP and let keepa self-destruct

	m__observers2delete.clear();
}

}
