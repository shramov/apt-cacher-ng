#include <cstring>

#include "bgtask.h"

#include "acfg.h"
#include "meta.h"
#include "filereader.h"
#include "evabase.h"

#include <fstream>

#include <limits.h>
#include <errno.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

using namespace std;

namespace acng
{

tExclusiveUserAction::~tExclusiveUserAction()
{
	if (!m_adminActionList.empty())
	{
		try
		{
			ofstream f;
			f.open(GetKbLocation().c_str());
			for (unsigned i = 0; f.is_open() && i < m_adminActionList.size(); ++i)
				f << unsigned(i/2) << ":" << m_adminActionList[i].first << endl;
		}
		catch (...)
		{
		}
	}
}

int tExclusiveUserAction::Add2KillBill(string_view sPathRel, string_view reason)
{
	if (AC_UNLIKELY(sPathRel.find('\n')))
		return -1;

	m_adminActionList.emplace_back(m_stringStore.Add(sPathRel), m_stringStore.Add(reason));

	return m_adminActionList.size()/2 - 1;
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
