#include "aclock.h"
#include "aevutil.h"
#include "evabase.h"

namespace acng
{

void cbKaPing(evutil_socket_t, short, void *);

class ackeepaliveImpl : public tClock
{
	const struct timeval m_interval;
public:
	lint_ptr<aobservable> m_notifier;
	unique_event m_event;
	bool m_active = false;

	aobservable::subscription AddListener(const tAction& act) override
	{
		if (!m_notifier)
			m_notifier.reset(new aobservable);

		if (!m_event.valid())
			m_event.reset(event_new(evabase::base, -1, EV_TIMEOUT|EV_PERSIST, cbKaPing, this));

		Enable();

		return m_notifier->subscribe(act);
	}
	void Disable()
	{
		LOGSTARTFUNC;
		if (!m_active)
			return;
		m_active = false;

		event_del(m_event.get());
	}
	void Enable()
	{
		LOGSTARTFUNC;
		if (m_active)
			return;
		m_active = true;

		event_add(m_event.get(), &m_interval);
	}
	ackeepaliveImpl(const struct timeval& interval);
};

ackeepaliveImpl::ackeepaliveImpl(const timeval &interval)
	: m_interval(interval)
{
}

void cbKaPing(evutil_socket_t, short, void *arg)
{
	auto me = ((ackeepaliveImpl*)arg);
	if (me->m_notifier->notify())
		return;
	me->Disable();
}

std::unique_ptr<tClock> tClock::Create(const timeval &val)
{
	return std::unique_ptr<tClock>(new ackeepaliveImpl(val));
}

}
