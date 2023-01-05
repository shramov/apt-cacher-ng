#ifndef ACREGISTRY_H
#define ACREGISTRY_H

#include "fileitem.h"
#include "actypes.h"
#include "evabase.h"

namespace acng
{

using TFileItemHolder = lint_user_ptr<fileitem>;

class ACNG_API IFileItemRegistry : public tLintRefcounted
{
public:

	virtual ~IFileItemRegistry() =default;

	// public constructor wrapper, create a sharable item with storage or share an existing one
	virtual TFileItemHolder Create(cmstring &sPathUnescaped, ESharingHow how, const fileitem::tSpecialPurposeAttr& spattr) WARN_UNUSED =0;

	virtual TFileItemHolder Register(tFileItemPtr spCustomFileItem) WARN_UNUSED =0;

	//! @return: true iff there is still something in the pool for later cleaning
	virtual time_t BackgroundCleanup() =0;

	virtual void dump_status() =0;

	virtual void AddToProlongedQueue(TFileItemHolder&&, time_t expTime) =0;

	virtual void Unreg(fileitem& ptr) =0;
};

// global registry handling, used only in server
lint_ptr<IFileItemRegistry> ACNG_API SetupServerItemRegistry();
void ACNG_API TeardownServerItemRegistry();
extern ACNG_API lint_ptr<IFileItemRegistry> g_registry;

}

#endif // ACREGISTRY_H
