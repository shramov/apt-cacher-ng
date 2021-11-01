#ifndef ACLOCAL_H
#define ACLOCAL_H

#include "mainthandler.h"

namespace acng
{

class aclocal : public acng::tSpecialRequestHandler
{
public:

	struct TParms : public SomeData
	{
		mstring visPath;
		mstring fsBase;
		mstring fsSubpath;
		off_t offset;
	};

	aclocal(tSpecialRequestHandler::tRunParms&& parms);
	void Run() override;

private:
	void SetEarlySimpleResponse(int code, string_view msg);
	TParms m_extraParms;
};

}

#endif // ACLOCAL_H
