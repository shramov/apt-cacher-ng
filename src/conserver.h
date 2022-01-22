#ifndef CONSERVER_H_
#define CONSERVER_H_

#include "fileio.h"
#include "acres.h"
#include "dumper.h"

namespace acng
{
class IConnBase;

class conserver : public Dumpable
{
public:
	virtual ~conserver() = default;
	virtual bool Setup() = 0;
	virtual void ReleaseConnection(IConnBase*) =0;
	static conserver* Create(acres& res);
};

}

#endif /*CONSERVER_H_*/
