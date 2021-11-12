#include "ackeepalive.h"
#include "aevutil.h"
#include "acstartstop.h"

namespace acng
{

void cbKaPing(evutil_socket_t, short, void *);
const struct timeval defaultKeepAliveTimeout { 10, 0 };

class ackeepaliveImpl : public ackeepalive
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

ackeepaliveImpl* acimpl;

ackeepaliveImpl::ackeepaliveImpl(const timeval &interval)
	: m_interval(interval)
{
	acimpl = this;
}

ackeepalive & ackeepalive::GetInstance()
{
	return *acimpl;
}

std::unique_ptr<ackeepalive> ackeepalive::SetupGlobalInstance(const timeval *interval)
{
	return std::make_unique<ackeepaliveImpl>(interval ? *interval : defaultKeepAliveTimeout);
//	tStartStop::getInstance()->atexit([&](){acimpl.reset();});
}

void cbKaPing(evutil_socket_t, short, void *arg)
{
	auto me = ((ackeepaliveImpl*)arg);
	if (me->m_notifier->notify())
		return;
	me->Disable();
}

}
