#ifndef ACFORWARDER_H
#define ACFORWARDER_H

#include "actypes.h"
#include "header.h"
#include "acres.h"
#include "aevutil.h"

namespace acng
{
void PassThrough(unique_bufferevent_flushclosing&& xbe, cmstring& uri, const header& reqHead, acres& res);
}
#endif // ACFORWARDER_H
