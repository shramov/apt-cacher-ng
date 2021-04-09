#include "httpdate.h"
#include "meta.h"
#include "filereader.h"
#include "acfg.h"

#include <regex>

#warning abasetypes.h should be used in the header, only for string_view

using namespace std;

namespace acng {

static const char* fmts[] =
{
        "%a, %d %b %Y %H:%M:%S GMT",
        "%A, %d-%b-%y %H:%M:%S GMT",
        "%a %b %d %H:%M:%S %Y"
};

string_view zeroDateBuf = "Do, 01 Jan 1970 01:00:00 GMT";

bool tHttpDate::ParseDate(const char *s, struct tm *tm)
{
    if(!s || !tm)
        return false;
    for(const auto& fmt : fmts)
    {
        if(::strptime(s, fmt, tm))
            return true;
    }
    return false;
}

time_t tHttpDate::ParseDate(const char *s, time_t onError)
{
    struct tm t;
    if (!ParseDate(s, &t))
        return onError;
    return mktime(&t);
}

unsigned tHttpDate::FormatTime(char *buf, size_t bufLen, const struct tm * src)
{
    if(bufLen < 26)
        return 0;
    //asctime_r(&src, buf);
    auto len = strftime(buf, sizeof(buf), fmts[0], src);
    if (len >= bufLen || len < 10)
    {
        buf[0] = 0;
        return 0;
    }
    buf[len] = '\0';
    return len;
}

unsigned tHttpDate::FormatTime(char *buf, size_t bufLen, const time_t cur)
{
    if(bufLen < 26)
        return 0;
    struct tm tmp;
    gmtime_r(&cur, &tmp);
    return FormatTime(buf, bufLen, &tmp);
}

tHttpDate::tHttpDate(time_t val) : tHttpDate()
{
    if (val)
        FormatTime(buf, sizeof(buf), val);
}
/*
acng::tHttpDate::operator mstring() const
{
    if (isnorm)
        return mstring(buf, length);
    else
    {
        return tHttpDate(buf, true);
    }
}
*/
tHttpDate::tHttpDate(const char* val, bool forceNorm) : tHttpDate()
{
    if (!val || !*val)
        return;

    struct tm tbuf;
    size_t srcLen(0);
    if (!forceNorm)
    {
        srcLen = strlcpy(buf, val, sizeof(buf));
        forceNorm = srcLen >= sizeof(buf);
    }

    if (!forceNorm)
    {
        length = srcLen;
        return;
    }

    // too long or forced to normalize :-(
    if (!ParseDate(val, &tbuf))
    {
        unset();
        return;
    }

    length = FormatTime(buf, sizeof(buf), &tbuf);
    if (length)
        isnorm = true;
    else
        unset();

}

bool tHttpDate::operator==(const tHttpDate &other) const {
    if (isSet() != other.isSet())
        return false;
    if (0 == strncmp(buf, other.buf, sizeof(buf)))
        return true;
    return value(-1) == other.value(-2);
}

bool tHttpDate::operator==(const char *other) const
{
    bool otherSet = other && *other;
    if (isSet())
    {
        if (!otherSet)
            return false;
        if (0 == strncmp(other, buf, sizeof(buf)))
            return true;
        return value(-1) == ParseDate(other, -2);
    }
    else // also equal if both are not set
        return !otherSet;
}

//string_view contLenPfx(WITHLEN("Content-Length: ")), laMoPfx(WITHLEN("Last-Modified: ")), origSrc(WITHLEN("X-Original-Source: "));

bool ParseHeadFromStorage(cmstring &path, off_t *contLen, tHttpDate *lastModified, mstring *origSrc)
{
    filereader reader;
    if (!reader.OpenFile(path, true, 0))
        return false;
    auto view = reader.getView();
    if (!view.starts_with("HTTP/1.1 200") || !view.ends_with('\n'))
        return false;
#warning optimize when view-based tokenizer is in place
    string sLine;
    auto needValueMask = (contLen != nullptr) + 2*(lastModified != nullptr) + 4*(origSrc != nullptr);

    while(reader.GetOneLine(sLine) && needValueMask)
    {
        trimFront(sLine);
        if (contLen && startsWithSz(sLine, "Content-Length:"))
        {
            *contLen = atoofft(sLine.data()+15, -1);
            needValueMask &= ~1;
        }
        else if (lastModified && startsWithSz(sLine, "Last-Modified: "))
        {
#warning optimize with view trimming
            sLine = sLine.substr(14);
            while (!sLine.empty() && isspace(unsigned(sLine.front())))
                sLine = sLine.substr(1);
            *lastModified = tHttpDate(sLine.c_str());
            needValueMask &= ~2;
        }
#warning this and other startsWith things shall be using astrop method - however the built-in method is only available since c++20
        else if (origSrc && startsWithSz(sLine, "X-Original-Source: "))
        {
            *origSrc = sLine.substr(14);
#warning optimize with view trimming
            trimFront(*origSrc);
            needValueMask &= ~4;
        }
    }
    return true;
}

bool StoreHeadToStorage(cmstring &path, off_t contLen, tHttpDate *lastModified, mstring *origSrc)
{
    if (path.empty())
        return false;
#if 0
    string temp1;
    temp1 = path;
    temp1[temp1.size()-1] = '-';
    auto temp2 = path;
    temp2[temp2.size()-1] = '+';
#endif
    tSS fmt(250);
    fmt << "HTTP/1.1 200 OK\r\n"sv;
	if (contLen >= 0)
		fmt << "Content-Length: "sv << contLen << svRN;
    if (lastModified && lastModified->isSet())
        fmt << "Last-Modified: "sv << lastModified->any() << svRN;
    if (origSrc && !origSrc->empty())
        fmt << "X-Original-Source: "sv << *origSrc << svRN;
    fmt << svRN;

    return fmt.dumpall(path.c_str(), O_CREAT, cfg::fileperms);

    // that above should be safe enough. The worst risk is that a head file will contain some trailing garbage when rewritten with shorter variant - which we don't really care about.

#if 0
    // exclusively saving to a new file
    if (fmt.dumpall(head.c_str(), O_CREAT | O_EXCL, cfg::fileperms))
    {
        return true;

    }

    if (link(path.c_str(), temp1.c_str()) != 0)
    {
        if (errno == EEXIST)
        return ;
        // ok, try safe swap
    {

    }
#endif
}

const std::regex reHttpStatus("(\\s*)(HTTP/1.?)?(\\s+)(.*?)(\\s*)");

tRemoteStatus::tRemoteStatus(string_view s)
{
	tSplitWalk split(s);
	if (split.Next())
	{
		char *pe;
		auto tok = split.view();
		if (!tok.empty())
		{
			code = strtol(tok.data(), &pe, 10);
			// pointer advanced?
			if (tok.data() != pe)
			{
				msg = split.right();
				if (!msg.empty())
					return;
			}
		}
	}
	code = 500;
	msg = "Invalid header line";
}

}
