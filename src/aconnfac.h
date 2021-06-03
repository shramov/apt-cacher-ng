#ifndef ACONNFAC_H
#define ACONNFAC_H

#include "acsmartptr.h"
#include "atcpstream.h"

namespace acng {

class tRepoUsageHooks;

class IDlConFactory
{
public:
    /// Moves the connection handle to the reserve pool (resets the specified sptr).
    /// Should only be supplied with IDLE connection handles in a sane state.
    virtual void RecycleIdleConnection(lint_ptr<ahttpstream> & handle) const =0;
    virtual void Connect(const tHttpUrl& url,
                         bool *pbSecondHand,
                         tRepoUsageHooks *pStateTracker,
                         int timeout,
                         bool onlyFresh,
                         std::function<void(ahttpstream::tResult)> cbResult
                         ) const =0;

    virtual ~IDlConFactory() =default;
};


}
#endif // ACONNFAC_H
