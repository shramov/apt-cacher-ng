#ifndef PORTUTILS_H
#define PORTUTILS_H

#include "actypes.h"
#include <cstdio>

#define DEFAULT_PORT_HTTP 80
#define DEFAULT_PORT_HTTPS 443
extern std::string sDefPortHTTP, sDefPortHTTPS;

namespace acng {

struct tPortAsString
{
		LPCSTR s;
		char buf[6];
		tPortAsString(uint16_t nPort)
		{
				if (nPort == 80)
						s = "80";
				else if (nPort == 443)
						s = "443";
				else
				{
						snprintf(buf, sizeof(buf), "%hi", nPort);
						s = buf;
				}
		}
};

std::string makeHostPortKey(const std::string & sHostname, uint16_t nPort);

}

#endif // PORTUTILS_H
