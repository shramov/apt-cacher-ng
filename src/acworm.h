#ifndef SCRATCHPAD_H
#define SCRATCHPAD_H

#include "actypes.h"
#include <list>
#include <vector>

namespace acng
{

/**
 * @brief Write Once, Read Many times, never destroy
 *
 * This is supposed to be one-way storage for persistent byte data, like strings for the database.
 */
class acworm
{
	struct Xdata;
	Xdata *m_pImpl;
public:
	acworm();
	~acworm();
	string_view Add(string_view src);

};

}

#endif // SCRATCHPAD_H
