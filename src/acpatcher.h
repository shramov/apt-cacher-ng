#ifndef ACPATCHER_H
#define ACPATCHER_H

#include "actypes.h"
#include "actemplates.h"

namespace acng
{

class ACNG_API acpatcher
{
	struct Impl;
	Impl *m_pImpl;

public:
	acpatcher();
	~acpatcher();
	void Apply(cmstring& sOrig, cmstring& sPatch, cmstring& sResult) throw();
};

}

#endif // ACPATCHER_H
