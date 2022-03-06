#include "aclock.h"
#include "acutilev.h"
#include "evabase.h"

namespace acng
{

// XXX: legacy, maybe better remove it
class ackeepaliveImpl : public tBeatNotifier, public tClock
{
public:
	aobservable m_notifier;

	aobservable::subscription AddListener(const tAction& act) override
	{
		Resume();
		return m_notifier.subscribe(act);
	}
	// constructed in suspended state
	ackeepaliveImpl(const struct timeval& interval) : tClock(interval)
	{
		Suspend();
	}

	void OnClockTimeout() override
	{
		if (!m_notifier.notify())
			Suspend();
	}
};

std::unique_ptr<tBeatNotifier> tBeatNotifier::Create(const timeval &val)
{
	return std::unique_ptr<tBeatNotifier>(new ackeepaliveImpl(val));
}

struct tClock::XD
{
	struct timeval m_interval;
	bool m_bEnabled = false;
	unique_event m_event;
};
void cbClock(evutil_socket_t, short, void *arg)
{
	auto me = ((tClock*)arg);
	me->OnClockTimeout();
	if (me->m_data->m_bEnabled)
		event_add(me->m_data->m_event.get(), & me->m_data->m_interval);
}

tClock::tClock(const timeval &interval)
{
	m_data = new XD;
	memcpy(& m_data->m_interval, &interval, sizeof(interval));
	m_data->m_event.reset(event_new(evabase::base, -1, EV_TIMEOUT, cbClock, this));
}

tClock::~tClock()
{
	delete m_data;
}

void tClock::Suspend()
{
	if (!m_data->m_bEnabled)
		return;
	m_data->m_bEnabled = false;
	event_del(m_data->m_event.get());
}

void tClock::Resume()
{
	if (m_data->m_bEnabled)
		return;
	m_data->m_bEnabled = true;
	event_add(m_data->m_event.get(), & m_data->m_interval);
}

}
