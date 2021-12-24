#include "aclocal.h"
#include "meta.h"
#include "acfg.h"

#include <queue>

#include <sys/types.h>
#include <dirent.h>

using namespace std;

namespace acng
{

const string_view style = R"(
<style type="text/css">
body
{
   font-family: Verdana, Arial, sans-serif;
   font-size: small;
   color: black;
}

h1 {
  font-size: medium;
}

td
{
  padding: 1px;
  align-content: space-around;
}

tr
{
  vertical-align: middle;
}
</style>
)";

aclocal::aclocal(tSpecialRequestHandler::tRunParms&& parms)
	: acng::tSpecialRequestHandler(move(parms))
{
	auto* p = dynamic_cast<TParms*>(m_parms.arg);
	if (!p)
		throw std::bad_cast();
	m_extraParms = *p;
}

void aclocal::SetEarlySimpleResponse(int code, string_view msg)
{
	m_parms.bitem().ManualStart(code, to_string(msg), "text/plain");
}

void aclocal::Run()
{
	mstring absPath = m_extraParms.fsBase + SZPATHSEP + m_extraParms.fsSubpath;
	Cstat stbuf(absPath);
	if (!stbuf)
	{
		switch(errno)
		{
		case EACCES:
			return SetEarlySimpleResponse(403, "Permission denied"sv);
		case EBADF:
		case EFAULT:
		case ENOMEM:
		case EOVERFLOW:
		default:
			return SetEarlySimpleResponse(500, "Internal server error"sv);
		case ELOOP:
			return SetEarlySimpleResponse(500, "Infinite link recursion"sv);
		case ENAMETOOLONG:
			return SetEarlySimpleResponse(500, "File name too long"sv);
		case ENOENT:
		case ENOTDIR:
			return SetEarlySimpleResponse(404, "File or directory not found"sv);
		}
		return;
	}

	if(S_ISDIR(stbuf.st_mode))
	{
		// unconfuse the browser
		if (!endsWithSzAr(m_extraParms.visPath, SZPATHSEPUNIX))
		{
			tFmtSendObj tx(this, true);
			m_fmtHelper << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>301 Moved Permanently</title></head><body><h1>Moved Temporarily</h1>"
				 "<p>The document has moved <a href=\""sv << UrlEscape(m_extraParms.visPath) << "/\">here</a>.</p></body></html>"sv;
			m_parms.bitem().ManualStart(301, "Moved Permanently", "text/html", m_extraParms.visPath + "/", m_fmtHelper.size());
			return;
		}
		m_parms.bitem().ManualStart(200, "OK", "text/html");
		SendFmt << "<!DOCTYPE html>\n<html lang=\"en\"><head>"
				<< style << "<title>Index of "sv
			 << m_extraParms.visPath << "</title></head><body><h1>Index of "sv
			 << m_extraParms.visPath << "</h1>"sv
										"<table><tr><th>&nbsp;</th><th>Name</th><th>Last modified</th><th>Size</th></tr>"sv
										"<tr><th colspan=\"4\"><hr></th></tr>"sv;
		auto* dir = opendir(absPath.c_str());
		if (!dir) // weird, whatever... ignore...
			Send("ERROR READING DIRECTORY"sv);
		else
		{
			// quick hack with sorting by custom keys, good enough here
			using tRecord = pair<string, evbuffer*>;
			priority_queue<tRecord, deque<tRecord>, greater<tRecord>> sortHeap;
			for(struct dirent *pdp(0); 0 != (pdp=readdir(dir));)
			{
				if (0 != ::stat(mstring(absPath+SZPATHSEP+pdp->d_name).c_str(), &stbuf))
					continue;

				string_view nam(pdp->d_name);
				if (nam.empty())
					continue;

				bool bDir=S_ISDIR(stbuf.st_mode);
				tHttpDate date;
				auto *buf = evbuffer_new();
				if (!buf)
					continue;
				ebstream line(buf);
				if(bDir)
					line << "[DIR]"sv;
				else if(startsWithSz(cfg::GetMimeType(nam), "audio/"))
					line << "[AUD]"sv;
				else if(startsWithSz(cfg::GetMimeType(nam), "video/"))
					line << "[VID]"sv;
				else if(startsWithSz(cfg::GetMimeType(nam), "image/"))
					line << "[IMG]"sv;
				else
					line << "[&nbsp;&nbsp;&nbsp;]"sv;
				line << "</td><td><a href=\""sv
							<< UrlEscape(nam)
						<< (bDir? "/\">"sv : "\">"sv )
						<< nam
						<< "</a></td><td>"sv
						<< date.Set(stbuf.st_mtime).view()
						<< "</td><td align=\"right\">"sv;
				if (bDir)
					line << "-"sv;
				else
					line << offttosH(stbuf.st_size);
				string key(bDir?"a":"b");
				key += char(nam[0] == '.' ? 0x0 : MAX_VAL(char));
				key += nam;
				sortHeap.push(make_pair(move(key), buf));
				//dbgprint((mstring)line);
			}
			closedir(dir);
			while(!sortHeap.empty())
			{
				evbuffer *p = sortHeap.top().second;
				SendFmt << "<tr><td valign=\"top\">"sv << ebstream::bmode::buftake << p << "</td></tr>\r\n"sv;
				evbuffer_free(p);
				sortHeap.pop();
			}
		}
		SendFmt << "<tr><td colspan=\"4\">"sv << GetFooter() << "</td></tr></table></body></html>"sv;
		return;
	}
	if (!S_ISREG(stbuf.st_mode))
		return SetEarlySimpleResponse(403, "Unsupported data type"sv);

	cmstring &sMimeType = cfg::GetMimeType(absPath);

	// OKAY, that's plain file delivery now
	if (stbuf.st_size > 0)
	{
		int fd = open(absPath.c_str(), O_RDONLY);
		if (fd == -1)
			return SetEarlySimpleResponse(500, "IO error"sv);

#ifdef HAVE_FADVISE
		posix_fadvise(fd, 0, stbuf.st_size, POSIX_FADV_SEQUENTIAL);
#endif

#warning check resuming after 3gb on 32bit build, and after 4.7 gb
		// let's mmap it and then drop the unneeded part on delivery from reader builder
		if (0 != evbuffer_add_file(PipeTx(), fd, 0, stbuf.st_size))
		{
			checkforceclose(fd);
			return SetEarlySimpleResponse(500, "Internal error"sv);
		}
	}
	m_parms.bitem().ManualStart(200, "OK",
							   sMimeType.empty() ? "octet/stream" : sMimeType,
							   se, stbuf.st_size, stbuf.st_mtim.tv_sec);

}

}
