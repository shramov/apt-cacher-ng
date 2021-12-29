#ifndef REMOTEDBTYPES_H
#define REMOTEDBTYPES_H

/**
  * Liteweight header used for shared functionality across remote communication code.
  */

#include "actypes.h"

namespace acng
{

struct tRepoData;

struct tRepoUsageHooks
{
        virtual void OnAccess()=0;
        virtual void OnRelease()=0;
        virtual ~tRepoUsageHooks() =default;
};

struct tRepoResolvResult
{
		string_view psRepoName; // backed by scratchpad memory
		string_view sRestPath; // backed by CALLER's MEMORY!
		const tRepoData* repodata = nullptr;
		bool valid() { return repodata && !psRepoName.empty(); }
};


}

#endif // REMOTEDBTYPES_H
