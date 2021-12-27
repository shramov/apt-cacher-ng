/*
 * bgtask.cpp
 *
 *  Created on: 18.09.2009
 *      Author: ed
 */

#include <cstring>

#include "bgtask.h"

#include "acfg.h"
#include "meta.h"
#include "filereader.h"
#include "evabase.h"

#include <condition_variable>

#include <limits.h>
#include <errno.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

using namespace std;

#define LOG_DECO_START "<html><head><style type=\"text/css\">" \
".WARNING { color: orange; }\n.ERROR { color: red; }\n" \
"</style></head><body>"
#define LOG_DECO_END "</body></html>"

namespace acng
{

#ifdef HAVE_ZLIB
mstring tExclusiveUserAction::BuildCompressedDelFileCatalog()
{
	mstring ret;
	tSS buf;

	// add the recent command, then the file records

	auto addLine = [&buf](unsigned id, cmstring& s)
		{
		unsigned len=s.size();
		buf.add((const char*) &id, sizeof(id))
				.add((const char*) &len, sizeof(len))
				.add(s.data(), s.length());
		};
	// don't care about the ID, compression will solve it
	addLine(0, m_parms.cmd);
	for(const auto& kv: m_pathMemory)
		addLine(kv.second.id, kv.first);

	unsigned uncompSize=buf.size();
	tSS gzBuf;
	uLongf gzSize = compressBound(buf.size())+32; // extra space for length header
	gzBuf.setsize(gzSize);
	// length header
	gzBuf.add((const char*)&uncompSize, sizeof(uncompSize));
	if(Z_OK == compress((Bytef*) gzBuf.wptr(), &gzSize,
			(const Bytef*)buf.rptr(), buf.size()))
	{
		ret = "<input type=\"hidden\" name=\"blob\"\nvalue=\"";
		ret += EncodeBase64(gzBuf.rptr(), (unsigned short) gzSize+sizeof(uncompSize));
		ret += "\">";
		return ret;
	}
	return "";
}

#endif

}
