#include "ackeepalive.h"
#include "aevutil.h"

namespace acng
{

void cbKaPing(evutil_socket_t, short, void *);
const struct timeval keepAliveTimeout { 10, 0 };

class ackeepaliveImpl : public ackeepalive
{
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

		event_add(m_event.get(), &keepAliveTimeout);
	}
};

ackeepaliveImpl acimpl;

ackeepalive & ackeepalive::GetInstance()
{
	return acimpl;
}

void cbKaPing(evutil_socket_t, short, void *arg)
{
	auto me = ((ackeepaliveImpl*)arg);
	if (me->m_notifier->notify())
		return;
	me->Disable();
}

}
