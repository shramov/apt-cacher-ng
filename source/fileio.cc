
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"
#include "fileio.h"
#include "acbuf.h"
#include "acfg.h"
#include <fcntl.h>
#ifdef HAVE_LINUX_FALLOCATE
#include <linux/falloc.h>
#endif
#include <unistd.h>

#if __cplusplus >= 201703L
#include <filesystem>
using namespace std::filesystem;
#else
#include <experimental/filesystem>
using namespace std::experimental::filesystem;
#endif

#ifndef BUFSIZ
#define BUFSIZ 8192
#endif

using namespace std;

namespace acng
{

#define citer const_iterator

#ifdef HAVE_LINUX_FALLOCATE

int falloc_helper(int fd, off_t start, off_t len)
{
   return fallocate(fd, FALLOC_FL_KEEP_SIZE, start, len);
}
#else
int falloc_helper(int, off_t, off_t)
{
   return 0;
}
#endif

int fdatasync_helper(int fd)
{
#if ( (_POSIX_C_SOURCE >= 199309L || _XOPEN_SOURCE >= 500) && _POSIX_SYNCHRONIZED_IO > 0)
	return fdatasync(fd);
#else
	return 0;
#endif
}

// linking not possible? different filesystems?
std::error_code FileCopy(cmstring &from, cmstring &to)
{
	std::error_code ec;
	copy(from, to, copy_options::overwrite_existing, ec);
	return ec;
}

ssize_t sendfile_generic(int out_fd, int in_fd, off_t *offset, size_t count)
{
	char buf[6*BUFSIZ];
	
	if(!offset)
	{
		errno=EFAULT;
		return -1;
	}
	if(lseek(in_fd, *offset, SEEK_SET)== (off_t)-1)
		return -1;

	ssize_t totalcnt=0;

	if(count > sizeof(buf))
		count = sizeof(buf);
	auto readcount=read(in_fd, buf, count);
	if(readcount<=0)
	{
		if(errno==EINTR || errno==EAGAIN)
			return 0;
		else
			return readcount;
	}
	for(decltype(readcount) nPos(0);nPos<readcount;)
	{
		auto r = write(out_fd, buf+nPos, readcount-nPos);
		if (r<0)
		{
			if(errno==EAGAIN || errno==EINTR)
				continue;
			return r;
		}
		nPos+=r;
		*offset+=r;
		totalcnt+=r;
	}
	return totalcnt;
}


bool xtouch(cmstring &wanted)
{
	mkbasedir(wanted);
	int fd = open(wanted.c_str(), O_WRONLY|O_CREAT|O_NOCTTY|O_NONBLOCK, cfg::fileperms);
	if(fd == -1)
		return false;
	checkforceclose(fd);
	return true;
}

void ACNG_API mkbasedir(cmstring & path)
{
	if(0==mkdir(GetDirPart(path).c_str(), cfg::dirperms) || EEXIST == errno)
		return; // should succeed in most cases

	// assuming the cache folder is already there, don't start from /, if possible
	unsigned pos=0;
	if(startsWith(path, cfg::cacheDirSlash))
	{
		// pos=acng::cfg:cachedir.size();
		pos=path.find("/", cfg::cachedir.size()+1);
	}
    for(; pos<path.size(); pos=path.find(SZPATHSEP, pos+1))
    {
        if(pos>0)
            mkdir(path.substr(0,pos).c_str(), cfg::dirperms);
    }
}

void ACNG_API mkdirhier(cmstring& path)
{
	if(0==mkdir(path.c_str(), cfg::dirperms) || EEXIST == errno)
		return; // should succeed in most cases
	if(path.empty())
		return;
	for(cmstring::size_type pos = path[0] == '/' ? 1 : 0;pos < path.size();pos++)
	{
		pos = path.find('/', pos);
		mkdir(path.substr(0,pos).c_str(), cfg::dirperms);
		if(pos == stmiss) break;
	}
}

void set_nb(int fd) {
	int flags = fcntl(fd, F_GETFL);
	//ASSERT(flags != -1);
	flags |= O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);
}
void set_block(int fd) {
	int flags = fcntl(fd, F_GETFL);
	//ASSERT(flags != -1);
	flags &= ~O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);
}

}
