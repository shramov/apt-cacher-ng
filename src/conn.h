

#ifndef _CON_H
#define _CON_H

#include "actypes.h"
#include "actemplates.h"
#include "fileitem.h"
#include "fileio.h"

namespace acng
{

class acres;
class dlcontroller;

/**
 * @brief Common functionality needed by the own jobs.
 */
class IConnBase : public tLintRefcounted
{
public:
	virtual dlcontroller* GetDownloader() =0;
	/**
	 * @brief Push the internal processing which is waiting for some notification
	 * @return true to keep calling, false to unregister the callback
	 */
	virtual void poke(uint_fast32_t dbgId) =0;
	virtual cmstring& getClientName() =0;
};

/**
 * @brief StartServing prepares the request serving stream and attached an appropriate handler to it
 * @param fd File descriptor, call of this method takes responsibility for it
 * @param clientName
 * @param isAdmin a special flog which creates a shortcut, running maintainenance task as the one and only job, and running already preauthorized
 */
lint_ptr<tLintRefcounted> ACNG_API StartServing(unique_fd&& fd, std::string clientName, acres&, bool isAdmin);

}

#endif
