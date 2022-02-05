#ifndef ACFGSHARED_H
#define ACFGSHARED_H

/**
  * Internal code shared between acfg and remotedb. Shall not be used by anyone else!
  */

#include "acfg.h"
#include "filereader.h"
#include "meta.h"

using namespace std;

namespace acng
{

namespace cfg
{

struct MapNameToString
{
	const char *name; mstring *ptr;
};

struct MapNameToInt
{
	const char *name; int *ptr;
	const char *warn; uint8_t base;
	uint8_t hidden;	// just a hint
};

extern MapNameToString n2sTbl[];
const extern unsigned n2sTblCount;
extern MapNameToInt n2iTbl[];
const extern unsigned n2iTblCount;

bool ParseOptionLine(const string &sLine, string &key, string &val);
void _FixPostPreSlashes(string &val);
tStrDeq ExpandFileTokens(cmstring &token);

// shortcut for frequently needed code, opens the config file, reads step-by-step
// and skips comment and empty lines
struct tCfgIter
{
        filereader reader;
        string sLine;
        string sFilename;
        tCfgIter(cmstring &fn);
        //inline operator bool() const { return reader.CheckGoodState(false, &sFilename); }
        bool Next();
};

extern string sPopularPath;

std::deque<std::string> ExpandFileTokens(cmstring &token);

}

}

#endif // ACFGSHARED_H
