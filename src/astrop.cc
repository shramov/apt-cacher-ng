/*
 * astrop.cc
 *
 *  Created on: 10.03.2020
 *      Author: Eduard Bloch
 */

#include "astrop.h"
#include "meta.h"

#include <charconv>

namespace acng
{

using namespace std;

/**
 * Simple divide-n-conquer evaluation of the string. check_ex shall return true if the
 * offered string is acceptable. It is also the actual receiver of the result, i.e.
 * it shall remember the longest string which was successfully evaluated,
 * and other results which were found when the check was performed.
 *
 * Example: search for "foo/bar/oh/dear/more/stuff", will probably call check_ex with "foo/bar/oh", "foo/bar", "foo" (depending on the discovery results)
 */
void fish_longest_match(string_view stringToScan, const char sep,
		std::function<bool(string_view)> check_ex)
{
	if(stringToScan.empty())
	{
		check_ex(stringToScan);
		return;
	}
	size_t L = 0;
	// trim, pointing to the end() position
	auto R = stringToScan.find_last_not_of(sep) + 1;
	while (L < R)
	{
		auto m = (L + R) / 2;
		auto true_m=m;
		// tendency to move further right to test longer string
		while (stringToScan[m] != sep && m != R) ++m;

		if (check_ex(string_view(stringToScan.data(), m)))
			L = m;
		else
		{
			// if the longer one was tested, check the next possible which is shorter and is at a separator
			while(true_m>L && stringToScan[true_m] !=sep)
				--true_m;
			R = true_m;
		}
	}
}

bool strappend(char *&p, string_view appendix, string_view app2)
{
    auto l = strlen(p);
    auto xlen = appendix.size() + app2.size();
    auto pEx = (char*) realloc(p, l + xlen + 1);
    if (!pEx)
        return false;
    p = pEx;
    memcpy(pEx + l, appendix.data(), appendix.length());
    memcpy(pEx + l + appendix.length(), app2.data(), app2.length());
    pEx[l + xlen] = 0;
	return true;
}

template<typename Tresult>
Tresult aToSomething(string_view s, Tresult nDefVal)
{
	Tresult ret(nDefVal);
	auto pstart = s.find_first_not_of(SPACECHARSsv);
	if (pstart == stmiss)
		return nDefVal;

	auto ec(std::from_chars(s.data() + pstart, s.data() + s.size(), ret, 10).ec);
	switch (ec)
	{
	case std::errc::invalid_argument:
	case std::errc::result_out_of_range:
		return nDefVal;
	default:
		return ret;
	}
}

off_t atoofft(string_view s, off_t nDefVal)
{
	return aToSomething<off_t>(s, nDefVal);
}

}



