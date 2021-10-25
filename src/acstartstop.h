#ifndef TSTARTSTOP_H
#define TSTARTSTOP_H

#include "config.h"
#include "actemplates.h"

namespace acng
{

/**
 * @brief Global manager of a controlled shutdown, partly in the atexit(3) fashion but with more C++ context.
 */
class ACNG_API tStartStop
{
	tStartStop* g_instance;

public:
	tStartStop* getInstance() { return g_instance; }
	tStartStop() { g_instance = this; }
	~tStartStop();
	/**
	 * @brief atexit runs callables in reverse order on destruction
	 * @param act
	 */
	void atexit(tAction act);
};

}

#endif // TSTARTSTOP_H
