#ifndef ACKEEPALIVE_H
#define ACKEEPALIVE_H

#include "aobservable.h"
namespace acng
{

/**
 * @brief Little helper which runs callbacks periodically, with an interval which fits a keepalive beat.
 */
class tClock
{
public:
	virtual ~tClock() =default;
	virtual aobservable::subscription AddListener(const tAction&) =0;
	static std::unique_ptr<tClock> Create(const struct timeval&);
	//virtual void Modify(const struct timeval&) =0;
};

}

#endif // ACKEEPALIVE_H
