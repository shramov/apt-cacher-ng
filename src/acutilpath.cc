#include "acutilpath.h"
#include "meta.h"

namespace acng
{

using namespace std;

/*
void find_base_name(const char *in, const char * &pos, UINT &len)
{
	int l=strlen(in);
	if(l==0)
	{
		pos=in;
		len=0;
		return;
	}

		const char *p, *r;

		for(p=in+l-1;*p==cPathSep;p--)
		{
			if(p==in)
			{
				pos=in;
				len=1;
				return;
			}
		}
		for(r=p;r>=in && *p!=cPathSep;r--)
		{
			if(r==in)
			{
				pos=in;
				len=p-in+1;
			}
		}

}
*/


#warning add unit test
string_view GetBaseName(string_view in)
{
	//trimBack(in, SVANYSEP);

	if(in.empty())
		return se;

	auto end = in.find_last_not_of(CPATHSEP); // must be the last char of basename
	if(end == stmiss) // empty, or just a slash?
		return "/";

	auto start = in.rfind(CPATHSEP, end);
	if(stmiss == start)
		start = 0;
	else
		start++;

	return in.substr(start, end+1-start);
}

#warning inline this

string_view GetDirPart(string_view in, eSlashMode smode)
{
	if(in.empty())
		return smode == eSlashMode::ALWAYS ? SVPATHSEPUNIX : se;
	trimBack(in, SVANYSEP);
	auto end = in.find_last_of(SVANYSEP);
	if (end == stmiss) // none? don't care then
		return smode == eSlashMode::ALWAYS ? SVPATHSEPUNIX : se;
	return string_view(in.data(), end + (smode >= eSlashMode::IF_SUBDIR_PRESENT));
}

std::pair<mstring,mstring> retSlash = std::pair<mstring,mstring>("/", "");
std::pair<mstring,mstring> pse;

std::pair<string_view, string_view> SplitDirPath(string_view in /*, eSlashMode smode*/)
{
#if 0
	if(in.empty())
		return smode == eSlashMode::ALWAYS ? retSlash : pse;

	bool probablyDir = false;
	while (isAnyOf(in.back(), SVANYSEP))
	{
		probablyDir = true;
		in.remove_suffix(1);
	}

	auto end = in.find_last_of(SVANYSEP);
	if (end == stmiss) // none? don't care then
		return smode == eSlashMode::ALWAYS ? retSlash : ret;

	return string_view(in.data(), end + (smode >= eSlashMode::IF_SUBDIR_PRESENT));




	auto end = in.find_last_of(SVANYSEP);
	ret.first = in.substr(0, end);

#endif
	if (in.ends_with('/'))
		return make_pair(in, ""sv);

	auto dir = GetDirPart(in);
	return make_pair(dir, in.substr(dir.length()));
}

#if 0 // XXX: implement, with a ring buffer? Worth it?
template <typename T, unsigned scale = 3>
struct TLookBack
{
	std::array<T, scale> rbuf;
	size_t vsize = 0, validFrom = 0;
	enum Tres { OKAY, GONE, ERROR} error;
	void put(T val); // IMPLEMENT PROPERLY! { rbuf[++last] = val; };
	T pop(T badval)
	{
		if (last <= first)
			return error = ERROR, badval;
		if (last)

			? {T(), GONE }  : rbuf.at(last--); };
};

#endif

std::string PathCombine(string_view a, string_view b)
{
	std::string ret;
	ret.reserve(a.length() + b.length() + 1);
	trimBack(a, SZPATHSEP);
	trimBoth(b, SZPATHSEP);

	ret += a;

	if (!a.empty() && !b.empty())
	{
		ret += CPATHSEP;
		ret += b;
	}
	return ret;
}


std::string PathCombine(string_view a, string_view b, string_view c, string_view d, string_view e, string_view f)
{
	auto parms = {&a, &b, &c, &d, &e, &f};
	std::string ret;
	size_t pr(1);
	for (auto x: parms)
	{
		trimBoth(*x, SZPATHSEP);
		pr += (1 + x->length());
	}
	ret.reserve(pr);
	for (auto x: parms)
	{
		if (x->empty()) continue;
		if (!ret.empty()) ret += CPATHSEP;
		ret += *x;
	}
	return ret;
}



constexpr string_view dirEnds[] = {"/", "\\", "/.", "\\."};

mstring SimplifyPath(string_view input)
{
	mstring ret;
	if (!input.empty())
	{
		deque<string::size_type> lback;
		for (tSplitWalk split(input, SVANYSEP); split.Next();)
		{
			auto chunk = split.view();
			if (chunk == ".")
				continue;

			if (chunk != "..")
			{
				lback.push_back(ret.size());
				ret += CPATHSEPUNX;
				ret += chunk;
				continue;
			}

			// ugh, okay, step back?
			if (lback.empty())
				throw std::runtime_error("Directory traversal attempt");
			ret.erase(lback.back()+1);
			ret += chunk;
			lback.pop_back();
		}
		// ensure trailing delimiter if it was intended by caller that way
		for (auto sfx: dirEnds)
		{
			if (ret.ends_with(sfx))
			{
				ret += CPATHSEP;
				break;
			}
		}
	}
	return ret;
}

bool SimplifyPathInplace(mstring &input)
{
	if (stmiss == input.find("/.") && stmiss == input.find("//"))
		return true;
	auto res = SimplifyPathChecked(input);
	if (res.second)
		return input.swap(res.first), true;
	return false;
}


}
