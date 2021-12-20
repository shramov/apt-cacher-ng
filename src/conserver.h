#ifndef CONSERVER_H_
#define CONSERVER_H_

#include "fileio.h"
#include "acres.h"

namespace acng
{
class conserver
{
public:
	virtual ~conserver() = default;
	virtual bool Setup() = 0;
	static conserver* Create(acres& res);
};

}

#endif /*CONSERVER_H_*/
