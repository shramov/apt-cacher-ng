
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"
#include "fileio.h"
#include "acbuf.h"
#include "acfg.h"
#include "meta.h"
#include "debug.h"
#include "acutilev.h"

#include <fcntl.h>
#ifdef HAVE_LINUX_FALLOCATE
#include <linux/falloc.h>
#endif
#include <unistd.h>

#if !defined(__clang__) && (__cplusplus < 201703L || (defined(__GNUC__) && __GNUC__ < 9)) // old GCC can be lying and STL might be not complete
#include <experimental/filesystem>
#define FSNS std::experimental::filesystem
#else
#include <filesystem>
#define FSNS std::filesystem
#endif

#ifndef BUFSIZ
#define BUFSIZ 8192
#endif

#include <event2/buffer.h>

using namespace std;

namespace acng
{

#define citer const_iterator

thread_local mstring fio_spr;

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
	FSNS::copy(from, to, FSNS::copy_options::overwrite_existing, ec);
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

bool ACNG_API optmkdir(LPCSTR path)
{
	return 0 == mkdir(path, cfg::dirperms) || EEXIST == errno;
}

bool ACNG_API mkdirhier(string_view path, bool tryOptimistic)
{
	fio_spr = path;

	// optimistic, should succeed in most cases
	if (tryOptimistic && optmkdir(fio_spr.c_str()))
		return true;

	// assuming the cache folder is the prefix, and skip any potential intermediate /s there
	auto it = fio_spr.begin();
	if (startsWith(path, cfg::cacheDirSlash))
		it += CACHE_BASE_LEN;
	while (it < fio_spr.end() && *it == '/')
		++it;
	// ok, try punching terminators into the string to mkdir on that
	for (;it < fio_spr.end(); ++it)
	{
		if (*it == '/')
		{
			*it = 0x0;
			if (0 == mkdir(fio_spr.data(), cfg::dirperms) || EEXIST == errno)
				*it = '/';
			else // error state, keep the errno. XXX: return some error code?
				return false;
		}
	}
	return optmkdir(fio_spr.c_str());
}

void set_block(int fd) {
	int flags = fcntl(fd, F_GETFL);
	//ASSERT(flags != -1);
	flags &= ~O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);
}

/**
 * @brief eb_dump_chunks - store all or limited range from the front to a file descriptor or similar sink.
 * For file descriptors, this is actually evbuffer_write_atmost replacement without its random aborting bug.
 */
template<typename Tstoreparam, ssize_t TSaveFunc(Tstoreparam, const struct iovec *__iovec, int __count)>
inline ssize_t ACNG_API eb_dump_chunks(evbuffer *inbuf, Tstoreparam saveParam, size_t nLenChecked)
{
	evbuffer_iovec ivs[64];
	off_t consumed = 0;
	int nErrno = 0;

	while (nLenChecked > 0)
	{
		auto nbufs = evbuffer_peek(inbuf, nLenChecked, nullptr, ivs, _countof(ivs));
		if (size_t(nbufs) > _countof(ivs))
			nbufs = _countof(ivs); // will come back here anyway
		ASSERT(nbufs > 0); // && unsigned(nbufs) <= _countof(ivs));
		if (nbufs <= 0)
			return -1;

		off_t bytesDeliverable = 0;
		// find the actual transfer length which we can actually serve with ivs array at once
		// OR make the last vector fit when we are at the end
		for (int i = 0; i < nbufs; ++i)
		{
			bytesDeliverable += ivs[i].iov_len;
			if (bytesDeliverable > (off_t) nLenChecked)
			{
				auto overshoot = bytesDeliverable - nLenChecked;
				ivs[i].iov_len -= overshoot;
				bytesDeliverable = nLenChecked;
				nbufs = i + 1;
				break;
			}
		}
		//auto nWritten = writev(out_fd, ivs, nbufs);
		auto nWritten = TSaveFunc(saveParam, ivs, nbufs);

		if (nWritten < 0)
			return nWritten;

		auto incomplete = nWritten != bytesDeliverable;
		nErrno = errno;

		evbuffer_drain(inbuf, nWritten);
		consumed += nWritten;

		nLenChecked -= nWritten;

		if (incomplete && nErrno != EAGAIN && nErrno != EINTR && nErrno != EWOULDBLOCK) // that is also an error!
			return consumed;
	}

	errno = nErrno;

	return consumed;
}

inline ssize_t writev2s(mstring* ret, const struct iovec *__iovec, int __count)
{
	ssize_t retlen(0);
	while (__count-- > 0)
	{
		ret->append((LPCSTR) __iovec->iov_base, __iovec->iov_len);
		retlen += __iovec->iov_len;
		__iovec++;
	}
	return retlen;
}

ssize_t ACNG_API eb_dump_chunks(evbuffer* inbuf, mstring& ret,  size_t nMax2SendNow)
{
	nMax2SendNow = std::min(nMax2SendNow, evbuffer_get_length(inbuf));
	ret.reserve(ret.size() + nMax2SendNow);
	eb_dump_chunks<mstring*, writev2s>(inbuf, &ret, nMax2SendNow);
	return nMax2SendNow;
};

ssize_t ACNG_API eb_dump_chunks(evbuffer* inbuf, int out_fd, size_t nMax2SendNow)
{
	auto xl(std::min(nMax2SendNow, evbuffer_get_length(inbuf)));
	return eb_dump_chunks<int, ::writev>(inbuf, out_fd, xl);
}

void ACNG_API event_and_fd_free(event *e)
{
	if (!e)
		return;
	auto fd = event_get_fd(e);
	event_free(e);
	checkforceclose(fd);
}

ssize_t dumpall(int fd, string_view data)
{
	ssize_t ret = data.size();
	while (data.size())
	{
		errno = 0;
		auto n = ::write(fd, data.data(), data.size());
		if (n <= 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		data.remove_prefix(n);
	}
	return ret;
}


}
