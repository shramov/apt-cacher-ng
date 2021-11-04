#ifndef ACKEEPALIVE_H
#define ACKEEPALIVE_H

#include "aobservable.h"
namespace acng
{

/**
 * @brief Little helper which runs callbacks periodically, with an interval which fits a keepalive beat.
 */
class ackeepalive
{
public:
	static ackeepalive& GetInstance();
	virtual aobservable::subscription AddListener(const tAction&) =0;
};

}

#endif // ACKEEPALIVE_H
