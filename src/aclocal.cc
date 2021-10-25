#include "aclocal.h"
#include "meta.h"
#include "acfg.h"

#include <queue>

#include <sys/types.h>
#include <dirent.h>

using namespace std;

namespace acng
{

aclocal::aclocal(tSpecialRequestHandler::tRunParms&& parms)
	: acng::tSpecialRequestHandler(move(parms))
{
	auto p = dynamic_cast<TParms*>(m_parms.arg);
	if (!p)
		throw std::bad_cast();
}

void aclocal::SetEarlySimpleResponse(int code, string_view msg)
{
	m_parms.output.ManualStart(code, msg);
}

void aclocal::Run()
{
	auto p = dynamic_cast<TParms*>(m_parms.arg);
	auto& fsBase = p->fsBase;
	auto& fsSubpath = p->fsSubpath;
	auto& visPath = p->visPath;

	mstring absPath = fsBase + SZPATHSEP + fsSubpath;
	Cstat stbuf(absPath);
	if (!stbuf)
	{
		switch(errno)
		{
		case EACCES:
			return SetEarlySimpleResponse(403, "Permission denied");
		case EBADF:
		case EFAULT:
		case ENOMEM:
		case EOVERFLOW:
		default:
			return SetEarlySimpleResponse(500, "Internal server error");
		case ELOOP:
			return SetEarlySimpleResponse(500, "Infinite link recursion");
		case ENAMETOOLONG:
			return SetEarlySimpleResponse(500, "File name too long");
		case ENOENT:
		case ENOTDIR:
			return SetEarlySimpleResponse(404, "File or directory not found");
		}
		return;
	}

	if(S_ISDIR(stbuf.st_mode))
	{
		// unconfuse the browser
		if (!endsWithSzAr(visPath, SZPATHSEPUNIX))
		{
			tFmtSendObj tx(this, true);
			m_fmtHelper << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>301 Moved Permanently</title></head><body><h1>Moved Permanently</h1>"
				 "<p>The document has moved <a href=\""sv << visPath << "/\">here</a>.</p></body></html>"sv;
			m_parms.output.ManualStart(301, "Moved Permanently"sv, "text/html"sv, visPath + "/", m_fmtHelper.size());
			return;
		}
		m_parms.output.ManualStart(200, "OK", "text/html"sv);
		SendFmt << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>Index of "sv
			 << visPath << "</title></head><body><h1>Index of "sv << visPath << "</h1>"sv
															   "<table><tr><th>&nbsp;</th><th>Name</th><th>Last modified</th><th>Size</th></tr>"sv
															   "<tr><th colspan=\"4\"><hr></th></tr>"sv;
		auto* dir = opendir(absPath.c_str());
		if (!dir) // weird, whatever... ignore...
			SendChunk("ERROR READING DIRECTORY"sv);
		else
		{
			// quick hack with sorting by custom keys, good enough here
			using tRecord = pair<string, evbuffer*>;
			priority_queue<tRecord, deque<tRecord>, greater<tRecord>> sortHeap;
			for(struct dirent *pdp(0);0!=(pdp=readdir(dir));)
			{
				if (0!=::stat(mstring(absPath+SZPATHSEP+pdp->d_name).c_str(), &stbuf))
					continue;

				bool bDir=S_ISDIR(stbuf.st_mode);
				tHttpDate date;
				bSS line;
				if(bDir)
					line << "[DIR]"sv;
				else if(startsWithSz(cfg::GetMimeType(pdp->d_name), "image/"))
					line << "[IMG]"sv;
				else
					line << "[&nbsp;&nbsp;&nbsp;]"sv;
				line << "</td><td><a href=\""sv
							<< pdp->d_name
						<< (bDir? "/\">"sv : "\">"sv )
						<< pdp->d_name
						<< "</a></td><td>"sv
						<< date.Set(stbuf.st_mtime).view()
						<< "</td><td align=\"right\">"sv;
				if (bDir)
					line << "-"sv;
				else
					line << offttosH(stbuf.st_size);
				sortHeap.push(make_pair(string(bDir?"a":"b") + pdp->d_name, line.release()));
				//dbgprint((mstring)line);
			}
			closedir(dir);
			while(!sortHeap.empty())
			{
				evbuffer *p = sortHeap.top().second;
				SendFmt << "<tr><td valign=\"top\">"sv << bSS::consume_buffers << p << "</td></tr>\r\n"sv;
				evbuffer_free(p);
				sortHeap.pop();
			}
		}
		SendFmt << "<tr><td colspan=\"4\">"sv << GetFooter() << "</td></tr></table></body></html>"sv;
		return;
	}
	if(!S_ISREG(stbuf.st_mode))
		return SetEarlySimpleResponse(403, "Unsupported data type");

	// OKAY, that's plain file delivery now

	int fd = open(absPath.c_str(), O_RDONLY);
	if (fd == -1)
		return SetEarlySimpleResponse(500, "IO error");
#ifdef HAVE_FADVISE
		posix_fadvise(fd, 0, stbuf.st_size, POSIX_FADV_SEQUENTIAL);
#endif

	cmstring &sMimeType=cfg::GetMimeType(absPath);
	m_parms.output.m_responseModDate = tHttpDate(stbuf.st_mtim.tv_sec);
	m_parms.output.ManualStart(200, "OK",
								sMimeType.empty() ? "octet/stream" : sMimeType,
								se, stbuf.st_size);
#warning check resuming after 3gb on 32bit build, and after 4.7 gb
	// let's mmap it and then drop the unneeded part on delivery from reader builder
	if (evbuffer_add_file(PipeTx(), fd, 0, stbuf.st_size))
	{
		checkforceclose(fd);
		return SetEarlySimpleResponse(500, "Internal error");
	}
}

}
