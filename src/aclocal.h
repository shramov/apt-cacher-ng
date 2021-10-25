#ifndef ACLOCAL_H
#define ACLOCAL_H

#include "mainthandler.h"

namespace acng
{

class aclocal : public acng::tSpecialRequestHandler
{
	struct TParms : public SomeData
	{
		mstring visPath;
		mstring fsBase;
		mstring fsSubpath;
		off_t offset;
	};

public:
	aclocal(tSpecialRequestHandler::tRunParms&& parms);
	void Run() override;

private:
	void SetEarlySimpleResponse(int code, string_view msg);
};

}

#endif // ACLOCAL_H
