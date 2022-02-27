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

namespace acng
{

unsigned tExclusiveUserAction::Add2KillBill(cmstring &sPathRel)
{
	if (!m_killBill.valid())
		m_killBill.reset(fopen(GetKbLocation().c_str(), "w"));
	if (!m_killBill.valid()) // XXX: error handling?
		return 0;
	m_bHaveDeletionCandidates = true;
	fprintf(m_killBill.get(), "%u:%s\n", m_nKbInitVec, sPathRel.c_str());
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

YesNoErr DeleteHelper::DeleteAndAccount(cmstring &path, bool deleteOrTruncate, Cstat* sb)
{
	Cstat localSB;
	if (!sb)
	{
		sb = &localSB;
		localSB.update(path.c_str());
	}

	if (!*sb)
		return YesNoErr::NO;
	auto res = deleteOrTruncate ? unlink(path.c_str()) : truncate(path.c_str(), 0);
	if (res)
		return YesNoErr::ERROR;
	m_nSpaceReleased += sb->size();
	return YesNoErr::YES;
}

}
