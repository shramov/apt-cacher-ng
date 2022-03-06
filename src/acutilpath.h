#ifndef ACUTILPATH_H_
#define ACUTILPATH_H_

#include "actypes.h"

namespace acng
{

/**
 * @brief The eSlashMode enum
 */
enum class eSlashMode {
	NEVER, // trailing slash never included
	IF_SUBDIR_PRESENT, // append trailing slash if a subfolder is found, get an empty dirname otherwise
	ALWAYS // always have a trailing slash in the dirname, even a '/' as dirname if no subfolder found
};

string_view GetBaseName(string_view in);
string_view GetDirPart(string_view in, eSlashMode = eSlashMode::IF_SUBDIR_PRESENT);
std::pair<string_view, string_view> SplitDirPath(string_view in);
mstring SimplifyPath(string_view input);
// trade-off version: quick check for possible directory changes ("/."), then modify the string as needed
bool SimplifyPathInplace(mstring& input);
inline std::pair<mstring, bool> SimplifyPathChecked(string_view input)
{
	std::pair<mstring, bool> ret;
	try
	{
		ret.first = SimplifyPath(input);
		ret.second = true;
	}
	catch (...)
	{
		ret.second = false;
	}
	return ret;
}

std::string PathCombine(string_view a, string_view b);
std::string PathCombine(string_view a, string_view b, string_view c,
						string_view d = string_view(),
						string_view e = string_view(),
						string_view f = string_view());
}

#endif
