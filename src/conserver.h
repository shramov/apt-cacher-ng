#ifndef CONSERVER_H_
#define CONSERVER_H_

#include "fileio.h"
#include "acres.h"
#include "dumper.h"

namespace acng
{
class IConnBase;

class conserver : public tLintRefcounted, public tExtRefExpirer
		#ifdef DEBUG
				, public Dumpable
		#endif
{
public:
	virtual ~conserver() = default;
	virtual bool Setup() = 0;
	virtual void ReleaseConnection(IConnBase*) =0;
	//virtual void ReplaceConnection(IConnBase *p, lint_user_ptr<IConnBase>(pNew)) =0;
	static lint_user_ptr<conserver> Create(acres& res);
};

}

#endif /*CONSERVER_H_*/
