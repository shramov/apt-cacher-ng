/*
 * astrop.h
 *
 *  Created on: 28.02.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_ASTROP_H_
#define INCLUDE_ASTROP_H_

#include "actypes.h"

#include <string>
#include <vector>
#include <deque>
#include <functional>

#include <strings.h> // strncasecmp

#define SPACECHARS " \f\n\r\t\v"

namespace acng
{

inline void trimFront(std::string &s, const char* junk=SPACECHARS)
{
	auto pos = s.find_first_not_of(junk);
	if(pos == std::string::npos)
		s.clear();
	else if(pos>0)
		s.erase(0, pos);
}

inline void trimFront(string_view& s, const char* junk=SPACECHARS)
{
	auto pos = s.find_first_not_of(junk);
	s.remove_prefix(pos == std::string::npos ? s.length() : pos);
}


inline void trimBack(std::string &s, const char* junk=SPACECHARS)
{
	auto pos = s.find_last_not_of(junk);
	if(pos == std::string::npos)
		s.clear();
	else if(pos>0)
		s.erase(pos+1);
}

inline void trimBack(string_view& s, const char* junk=SPACECHARS)
{
	auto pos = s.find_last_not_of(junk);
	s.remove_suffix(pos != std::string::npos ? s.size() - pos - 1 : s.length());
}

inline void trimBoth(std::string &s, const char* junk=SPACECHARS)
{
	trimBack(s, junk);
	trimFront(s, junk);
}

inline void trimBoth(string_view &s, const char* junk=SPACECHARS)
{
	trimBack(s, junk);
	trimFront(s, junk);
}


//! iterator-like helper for string splitting, for convenient use with for-loops
// Works exactly once!
class tSplitWalk
{
	string_view m_input;
	mutable std::string::size_type m_sliece_len;
	const char* m_seps;
	bool m_strict_delimiter, m_first;

public:
	/**
	 * @param line The input
	 * @param separators Characters which are considered delimiters (any char in that string)
	 * @param strictDelimiter By default, a sequence of separator chars are considered as one delimiter. This is normally good for whitespace but bad for specific visible separators. Setting this flag makes them be considered separately, returning empty strings as value is possible then.
	 */
#warning check all users, 3rd parameter changed meaning
        inline tSplitWalk(string_view line, const char* separators = SPACECHARS, bool strictDelimiter = false)
	: m_input(line), m_sliece_len(0), m_seps(separators), m_strict_delimiter(strictDelimiter), m_first(true)
	{}
	void reset(string_view line)
	{
		m_input=line;
		m_sliece_len=0;
		m_first=true;
	}
	inline bool Next()
	{
		if (m_input.length() == m_sliece_len)
			return false;
		m_input.remove_prefix(m_sliece_len);

		if (m_strict_delimiter)
		{
			if(!m_first)
				m_input.remove_prefix(1);
			else
				m_first = false;
			m_sliece_len = m_input.find_first_of(m_seps);
			// XXX: for the strict mode, the calculation of slice length could be made in lazy way. So, either in slice getter or here in Next but not beforehand.
			// However, it is quite likely that the next slice will also be needed, and additional branches here will cost more than they bring.
			if (m_sliece_len == std::string::npos)
				m_sliece_len = m_input.length();
			return true;
		}
		else
		{
			trimFront(m_input, m_seps);
			if(m_input.empty())
				return false;
			m_sliece_len = m_input.find_first_of(m_seps);
			if (m_sliece_len == std::string::npos)
				m_sliece_len = m_input.length();
			return true;
		}
	}

	// access operators for the current slice
	inline std::string str() const { return std::string(m_input.data(), m_sliece_len); }
	inline operator std::string() const { return str(); }
	inline string_view view() const { return string_view(m_input.data(), m_sliece_len); }
        /**
         * @brief Report the remaining part of the string after current token
         * Unless strict delimiter is specified, will also trim the end of the resulting string_view.
         * @return Right part of the string, might be empty (but invalid) string_view() if not found
         */
        string_view right(){
            if (m_input.length() == m_sliece_len)
                    return string_view();
            string_view ret(m_input.substr(m_sliece_len));

            if (m_strict_delimiter)
                return ret;

            auto cut = ret.find_first_not_of(m_seps);
            if (cut != std::string::npos)
                ret.remove_prefix(cut);
            cut = ret.find_last_not_of(m_seps);
            if (cut != std::string::npos)
                ret.remove_suffix(ret.size() - cut - 1);
            return ret;
        }

	struct iterator :
			public std::iterator<
			                        std::input_iterator_tag,   // iterator_category
			                        string_view,                      // value_type
			                        string_view,                      // difference_type
			                        const long*,               // pointer
			                        string_view                       // reference
			                                      >
	{
		tSplitWalk* _walker = nullptr;
		// default is end sentinel
		bool bEol = true;
		iterator() {}
		iterator(tSplitWalk& walker) : _walker(&walker) { bEol = !walker.Next(); }
		// just good enough for basic iteration and end detection
		bool operator==(const iterator& other) const { return (bEol && other.bEol); }
		bool operator!=(const iterator& other) const { return !(other == *this); }
		iterator& operator++() { bEol = !_walker->Next(); return *this; }
	    iterator operator++(int) {iterator retval = *this; ++(*this); return retval;}
		auto operator*() { return _walker->view(); }
	};
	iterator begin() {return iterator(*this); }
	iterator end() { return iterator(); }
	// little shortcut for collection creation; those methods are destructive to the parent, can only be used once
	std::vector<string_view> to_vector() { return std::vector<string_view>(begin(), end()); }
	std::vector<string_view> to_vector(unsigned nElementsToExpect)
		{
		std::vector<string_view> ret(nElementsToExpect);
		ret.assign(begin(), end());
		return ret;
		}
	std::deque<string_view> to_deque() { return std::deque<string_view>(begin(), end()); }

};

inline int strcasecmp(string_view a, string_view b)
{
	if(a.length() < b.length())
		return int(b.length()) +1;
	if(a.length() > b.length())
		return int(-a.length()) - 1;
	return strncasecmp(a.data(), b.data(), a.length());
}

#define trimLine(x) { trimFront(x); trimBack(x); }

#define startsWith(where, what) (0==(where).compare(0, (what).size(), (what)))
#define endsWith(where, what) ((where).size()>=(what).size() && \
		0==(where).compare((where).size()-(what).size(), (what).size(), (what)))
#define startsWithSz(where, what) (0==(where).compare(0, sizeof((what))-1, (what)))
#define endsWithSzAr(where, what) ((where).size()>=(sizeof((what))-1) && \
		0==(where).compare((where).size()-(sizeof((what))-1), (sizeof((what))-1), (what)))
#define stripSuffix(where, what) if(endsWithSzAr(where, what)) where.erase(where.size()-sizeof(what)+1);
#define stripPrefixChars(where, what) where.erase(0, where.find_first_not_of(what))

#define setIfNotEmpty(where, cand) { if(where.empty() && !cand.empty()) where = cand; }
#define setIfNotEmpty2(where, cand, alt) { if(where.empty()) { if(!cand.empty()) where = cand; else where = alt; } }

#define WITHLEN(x) x, (_countof(x)-1)
#define MAKE_PTR_0_LEN(x) x, 0, (_countof(x)-1)

std::string GetBaseName(const std::string &in);
std::string GetDirPart(const std::string &in);
std::pair<std::string,std::string> SplitDirPath(const std::string& in);
std::string PathCombine(string_view a, string_view b);


void fish_longest_match(string_view stringToScan, const char sep,
		std::function<bool(string_view)> check_ex);

void replaceChars(std::string &s, const char* szBadChars, char goodChar);
inline std::string to_string(string_view s) {return std::string(s.data(), s.length());}

// there is memchr and strpbrk but nothing like the last one acting on specified RAW memory range
inline LPCSTR mempbrk (LPCSTR  membuf, char const * const needles, size_t len)
{
#warning drop this, use plain stringview operation
   for(LPCSTR pWhere=membuf ; pWhere<membuf+len ; pWhere++)
      for(LPCSTR pWhat=needles; *pWhat ; pWhat++)
         if(*pWhat==*pWhere)
            return pWhere;
   return nullptr;
}


}


#endif /* INCLUDE_ASTROP_H_ */
