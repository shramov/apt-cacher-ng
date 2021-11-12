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
	//auto iter = m_observers.rbegin().base(); // NO!! OH WTF, STL... https://stackoverflow.com/questions/16609041/c-stl-what-does-base-do/16609146#16609146
	auto iter = m_observers.end();
	--iter;
	//DBGQLOG("listeners: " << m_observers.size() << ", subiter: " << uintptr_t(& *iter));
	return TFinalAction([pin = as_lptr(this), iter]()
	{
		pin->unsubscribe(iter);
	});
}

void aobservable::unsubscribe(TActionList::iterator what)
{
	ASSERT_HAVE_MAIN_THREAD;
	// set a flag to remove the current item instead?
	if (what == m_currentlyProcessing)
	{
		//DBGQLOG("unsub: PROCESSED ITEM " << uintptr_t(& *what));
		m_currentlyProcessing = m_observers.end();
	}
	else if (what != m_observers.end())
	{
		//DBGQLOG("unsub: " << uintptr_t(& *what));
		m_observers.erase(what);
	}
}

void aobservable::doSchedule()
{
	ASSERT_HAVE_MAIN_THREAD;

	m_bNotifyPending = true;
	evabase::Post([me = as_lptr(this)] ()
	{
		me->doNotify();
	});
}

void aobservable::doNotify()
{
	ASSERT_HAVE_MAIN_THREAD;

	m_bNotifyPending = false;
	m_currentlyProcessing = m_observers.end();

	for (auto it = m_observers.begin(); it != m_observers.end(); )
	{
		if (!*it)
			continue;

		// set the flag
		m_currentlyProcessing = it;

		(*it)();

		if (m_currentlyProcessing == it)
			++it;
		else // hot removal was requested by unsubscribe?
			it = m_observers.erase(it);

		// and remove the flag again
		m_currentlyProcessing = m_observers.end();
	}

	if (m_bNotifyPending) // requested again?
		doSchedule();
}

}
