#ifndef CONSERVER_H_
#define CONSERVER_H_

#include "fileio.h"

namespace acng
{

namespace conserver {

using TAcceptor = std::function<void(unique_fd&&, std::string)>;

/*! Prepares the connection handlers and internal things, binds, listens, etc.
 * @return Number of created listeners.
 */
int Setup(TAcceptor);
/// Start the service
int Run();
/// Stop all running connections sanely and threads if possible
ACNG_API void Shutdown();

void HandleOverload();

/**
 * Perform a gracefull connection shutdown.
 */
ACNG_API void FinishConnection(int fd);

}

}

#endif /*CONSERVER_H_*/
