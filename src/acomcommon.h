#ifndef ACOMCOMMON_H
#define ACOMCOMMON_H

#include "actypes.h"

using tComError = unsigned;

/**
 * @brief The eTransErrors enum descries the nature of a fault occured somewhere.
 */
enum eTransErrors : tComError
{
TRANS_DNS_NOTFOUND=0x1,
TRANS_TIMEOUT = 0x2,
TRANS_WAS_USED = 0x4,
TRANS_INTERNAL_ERROR = 0x8,
TRANS_FAULTY_SSL_PEER = 0x10,
TRANS_STREAM_ERR_TRANSIENT = 0x20,
TRANS_STREAM_ERR_FATAL = 0x40
};

// codes which mean that the connection shall not be retried for that peer
#define IS_STREAM_FATAL_ERROR(n) (0 != (n & (TRANS_INTERNAL_ERROR|TRANS_DNS_NOTFOUND|TRANS_TIMEOUT|TRANS_STREAM_ERR_FATAL)))

#endif // ACOMCOMMON_H
