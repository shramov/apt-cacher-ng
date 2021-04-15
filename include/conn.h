#ifndef _CON_H
#define _CON_H

#include "actypes.h"
#include "actemplates.h"
#include "fileio.h"

namespace acng
{
class dlcon;

class ISharedConnectionResources
{
public:
    virtual dlcon* SetupDownloader() =0;
    virtual void LogDataCounts(cmstring & sFile, mstring xff, off_t nNewIn,
            off_t nNewOut, bool bAsError) =0;
};

class conn : public ISharedConnectionResources
{
	class Impl;
	Impl *_p;
public:
	conn(unique_fd fdId, const char *client);
	virtual ~conn();
	void WorkLoop();

    dlcon* SetupDownloader() override;
    void LogDataCounts(cmstring & sFile, mstring xff, off_t nNewIn,
            off_t nNewOut, bool bAsError) override;
private:
	conn& operator=(const conn&); // { /* ASSERT(!"Don't copy con objects"); */ };
	conn(const conn&); // { /* ASSERT(!"Don't copy con objects"); */ };
};

}

#endif
