#ifndef REX_H
#define REX_H

#include "actypes.h"

namespace acng
{
class ACNG_API rex
{
public:
	enum NOCACHE_PATTYPE : bool
	{
		NOCACHE_REQ,
		NOCACHE_TGT
	};

	enum eMatchType : int8_t
	{
		FILE_INVALID = -1, // WARNING: this is forward-declared elsewhere!
		FILE_SOLID = 0, FILE_VOLATILE, FILE_WHITELIST,
		NASTY_PATH, PASSTHROUGH,
		FILE_SPECIAL_SOLID,
		FILE_SPECIAL_VOLATILE,
		ematchtype_max
	};

	rex();
	~rex();

	bool Match(cmstring &in, eMatchType type);
	bool HasErrors();
	eMatchType GetFiletype(const mstring &);
	bool MatchUncacheable(const mstring &, NOCACHE_PATTYPE);
	//bool CompileUncExpressions(NOCACHE_PATTYPE type, cmstring& pat);

private:
	struct tImpl;
	tImpl* m_pImpl;
};

}

#endif // REX_H
