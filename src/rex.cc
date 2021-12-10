#include "rex.h"
#include "acfg.h"
#include "acfgshared.h"
#include "astrop.h"

#include <regex>
#include <iostream>

#define BADSTUFF_PATTERN "\\.\\.($|%|/)"

using namespace std;

namespace acng
{

struct rex::tImpl
{
	int errorCount = 0;
	// this has the exact order of the "regular" types in the enum
	std::array<std::vector<std::regex>, ematchtype_max+2> typeMatcher;
	std::vector<std::regex> vecReqPatters, vecTgtPatterns;

	int compile(unsigned pos, cmstring& ps)
	{
		if(ps.empty())
			return 0;
		try
		{
			typeMatcher[pos].emplace_back(ps, std::regex_constants::extended);
		}
		catch (const std::regex_error& ex)
		{
			std::cerr << "Regex error " << ex.code() << " in " << ps << " : " << ex.what() << std::endl;
			return 1;
		}
		catch (...)
		{
			return 2;
		}
		return 0;
	}
	void compileDefaultMatcher()
	{
		using namespace cfg;
		errorCount = compile(FILE_SOLID, pfilepat)
				+ compile(FILE_VOLATILE, vfilepat)
				+ compile(FILE_WHITELIST, wfilepat)
				+ compile(FILE_SOLID, pfilepatEx)
				+ compile(FILE_VOLATILE, vfilepatEx)
				+ compile(FILE_WHITELIST, wfilepatEx)
				+ compile(NASTY_PATH, BADSTUFF_PATTERN)
				+ compile(FILE_SPECIAL_SOLID, spfilepat)
				+ compile(FILE_SPECIAL_SOLID, spfilepatEx)
				+ compile(FILE_SPECIAL_VOLATILE, svfilepat)
				+ compile(FILE_SPECIAL_VOLATILE, svfilepatEx)
				+ (connectPermPattern == "~~~"
				   ? 0 : compile(PASSTHROUGH, connectPermPattern))
				+ CompileUncExpressions(rex::NOCACHE_REQ,
										tmpDontcacheReq.empty() ? tmpDontcache : tmpDontcacheReq)
				+ CompileUncExpressions(rex::NOCACHE_TGT,
										tmpDontcacheTgt.empty() ? tmpDontcache : tmpDontcacheTgt);
	}

	bool Match(cmstring &in, eMatchType type)
	{
		if(MatchType(in, type))
			return true;
		// very special behavior... for convenience
		return (type == FILE_SOLID && MatchType(in, FILE_SPECIAL_SOLID))
			|| (type == FILE_VOLATILE && MatchType(in, FILE_SPECIAL_VOLATILE));
	}

	// match the specified type by internal pattern PLUS the user-added pattern
	bool MatchType(cmstring &in, eMatchType type)
	{
		auto& matcher = typeMatcher.at(type);
		for(auto& el: matcher)
			if(regex_search(in, el))
				return true;
		return false;
	}

	ACNG_API eMatchType GetFiletype(const string & in)
	{
		if (MatchType(in, FILE_SPECIAL_VOLATILE))
			return FILE_VOLATILE;
		if (MatchType(in, FILE_SPECIAL_SOLID))
			return FILE_SOLID;
		if (MatchType(in, FILE_VOLATILE))
			return FILE_VOLATILE;
		if (MatchType(in, FILE_SOLID))
			return FILE_SOLID;
		return FILE_INVALID;
	}

	bool CompileUncachedRex(const string & token, NOCACHE_PATTYPE type, bool bRecursiveCall)
	{
		auto pos = ematchtype_max + (NOCACHE_TGT == type ? 0 : 1);

		if (0!=token.compare(0, 5, "file:")) // pure pattern
		{
			compile(pos, token);
			return !errorCount;
		}

		if(!bRecursiveCall) // don't go further than one level
		{
			auto srcs = cfg::ExpandFileTokens(token);
			for(const auto& src: srcs)
			{
				cfg::tCfgIter itor(src);
				if (!itor.reader.CheckGoodState(false, &src))
				{
					cerr << "Error opening pattern file: " << src <<endl;
					return false;
				}
				while(itor.Next())
				{
					if(!CompileUncachedRex(itor.sLine, type, true))
						return false;
				}
			}
			return true;
		}
		cerr << token << " is not supported here" <<endl;
		return false;
	}

	int CompileUncExpressions(NOCACHE_PATTYPE type, cmstring& pat)
	{
		for(tSplitWalk split(pat); split.Next(); )
			if(!CompileUncachedRex(split, type, false))
				return 1;
		return 0;
	}

	bool MatchUncacheable(const string & in, NOCACHE_PATTYPE type)
	{
		for(const auto& patre: (type == NOCACHE_REQ) ? vecReqPatters : vecTgtPatterns)
			if(regex_search(in, patre))
				return true;
		return false;
	}

};

rex::rex()
	: m_pImpl(new tImpl)
{
	using namespace cfg;
	m_pImpl->compileDefaultMatcher();
}

rex::~rex()
{
	delete m_pImpl;
}

bool rex::Match(cmstring &in, eMatchType type)
{
return m_pImpl->Match(in, type);
}

bool rex::HasErrors()
{
return m_pImpl->errorCount > 0;
}

rex::eMatchType rex::GetFiletype(const mstring &sPath)
{
return m_pImpl->GetFiletype(sPath);
}

bool rex::MatchUncacheable(const mstring &sPath, NOCACHE_PATTYPE patype)
{
return m_pImpl->MatchUncacheable(sPath, patype);
}

LPCSTR ReTest(LPCSTR s, rex& matcher)
{
	static LPCSTR names[rex::ematchtype_max] =
	{
				"FILE_SOLID", "FILE_VOLATILE",
				"FILE_WHITELIST",
				"NASTY_PATH", "PASSTHROUGH",
				"FILE_SPECIAL_SOLID"
	};
	auto t = matcher.GetFiletype(s);
	if(t<0 || t>=rex::ematchtype_max)
		return "NOMATCH";
	return names[t];
}


}
