#ifndef ACKEEPALIVE_H
#define ACKEEPALIVE_H

#include "aobservable.h"
namespace acng
{

class tClock
{
public:
	tClock(const struct timeval& interval);
	virtual ~tClock();
	virtual void OnClockTimeout() = 0;
	struct XD;
	XD *m_data;
protected:
	void Suspend();
	void Resume();
};

/**
 * @brief Little helper which runs callbacks periodically, with an interval which fits a keepalive beat.
 */
class tBeatNotifier
{
public:
	virtual ~tBeatNotifier() =default;
	virtual aobservable::subscription AddListener(const tAction&) =0;
	static std::unique_ptr<tBeatNotifier> Create(const struct timeval&);
	//virtual void Modify(const struct timeval&) =0;
};

}

#endif // ACKEEPALIVE_H
