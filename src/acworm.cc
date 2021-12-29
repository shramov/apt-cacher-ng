#include "acworm.h"

#include <string.h>

namespace acng
{

// bucket sizes, choose something which does not cause much waste
#define START_SIZE 1024
#define MAX_SIZE 4096*2

struct acworm::Xdata
{
	std::list<std::vector<char>> data;
	size_t cursor = 0;

	void Expand(size_t newlen)
	{
		data.emplace_back(std::vector<char>());
		data.back().resize(newlen);
		cursor = 0;
	}

	string_view Add(string_view src)
	{
		if (data.empty())
			Expand(std::max(src.size(), (size_t) START_SIZE));
		if (src.length() > data.back().size() - cursor)
		{
			auto newlen = data.back().size()*2;
			if (newlen > MAX_SIZE)
				newlen = MAX_SIZE;
			if (src.length() > newlen)
				newlen = src.length();
			Expand(newlen);
		}
		memcpy(& data.back()[cursor], src.data(), src.size());
		string_view ret(& data.back()[cursor], src.size());
		cursor += src.size();
		return ret;
	}

};

acworm::acworm()
{
	m_pImpl = new Xdata();
}

acworm::~acworm()
{
	delete m_pImpl;
}

string_view acworm::Add(string_view src)
{
	return m_pImpl->Add(src);
}



}
