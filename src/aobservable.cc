#include "aobservable.h"
#include <memory>

using namespace std;

namespace acng
{

aobservable::subscription aobservable::subscribe(const aobservable::TNotifier &newSubscriber)
{
	ASSERT_HAVE_MAIN_THREAD;

	if (!newSubscriber) // XXX: not accepting invalid subscribers to avoid later checks, but what then?
		return subscription();
	m_observers.emplace_back(newSubscriber);
	return subscription(as_lptr(this), m_observers.end());
}

void aobservable::unsubscribe(TActionList::iterator what)
{
	ASSERT_HAVE_MAIN_THREAD;
	if (what == m_currentlyProcessing)
		m_currentlyProcessing = m_observers.end();
	m_observers.erase(what);
}

void aobservable::doSchedule()
{
	m_bNotifyPending = true;
	evabase::Post([me = as_lptr(this)] () mutable { me->notify(false); });
}

void aobservable::doNotify()
{	
	m_bNotifyPending = false;

	for (auto it = m_observers.begin(); it != m_observers.end(); )
	{
		m_currentlyProcessing = it;
		(*it)();
		if (m_currentlyProcessing == it)
			++it;
		else // removal was requested by unsubscribe?
			it = m_observers.erase(it);
	}
}

}
