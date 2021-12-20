#ifndef ACFORWARDER_H
#define ACFORWARDER_H

#include "actypes.h"
#include "header.h"
#include "acres.h"

namespace acng
{
void PassThrough(bufferevent* be, cmstring& uri, const header& reqHead, acres& res);
}
#endif // ACFORWARDER_H
