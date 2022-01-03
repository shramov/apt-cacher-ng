#include <cstring>

#include "bgtask.h"

#include "acfg.h"
#include "meta.h"
#include "filereader.h"
#include "evabase.h"

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

unsigned tExclusiveUserAction::Add2KillBill(cmstring &sPathRel)
{
	tSS nam;
	nam << CACHE_BASE << MJSTORE_SUBPATH << "/" << GetCacheKey() << ".kb"sv;
	if (!m_bHaveDeletionCandidates)
	{
		// XXX: error handling?
		mkdirhier(nam);
		m_bHaveDeletionCandidates = true;
	}
	nam << "/" << m_nKbInitVec;

	auto tgt = (string("../../..") + sPathRel);
	auto r = symlink(tgt.c_str(), nam.c_str());
	if (r)
	{
		USRDBG("SYMLINK FAIL! " << r << " to " << nam << " at " << tgt );
	}
	return m_nKbInitVec++;
}

void tExclusiveUserAction::SendProp(cmstring &key)
{
	if (key == "purgeactionmeta")
	{
		if (m_bHaveDeletionCandidates)
		{
			SendFmt << "<input type=\"hidden\" name=\"kbid\"\nvalue=\""sv << GetCacheKey() << "\">"sv;
		}
	}
	else
		return tMaintJobBase::SendProp(key);
}

}
