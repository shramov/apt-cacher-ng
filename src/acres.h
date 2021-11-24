#ifndef ACRES_H
#define ACRES_H

#include "sut.h"

extern "C"
{
struct timeval;
}

namespace acng
{
class tClock;

/**
 * @brief The acres class provides access to certain shared resources
 * - configuration
 * - predefined notification clocks
 * - remote target database
 */
class acres
{
public:
	virtual ~acres() =default;
	static acres* Create();
	virtual tClock& GetKeepAliveBeat() =0;
	virtual tClock& GetIdleCheckBeat() =0;
	/**
	 * @brief GetCustomBeat register an own clock category, ID must be maintained manually
	 * @param id
	 * @return
	 */
	virtual tClock& GetCustomBeat(int id, const struct timeval& interval) =0;
};

}

#endif // ACRES_H
