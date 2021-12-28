/*
 * fileio.h
 *
 *  Created on: 25.07.2010
 *      Author: ed
 */

#ifndef FILEIO_H_
#define FILEIO_H_

#include "actypes.h"
#include "actemplates.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstdio>
#include <unistd.h>

#include <inttypes.h>
#include <climits>
#include <memory>
#include <system_error>

#ifdef HAVE_LINUX_SENDFILE
#include <sys/sendfile.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ENEMIESOFDOSFS "?[]\\=+<>:;#"

#ifndef O_BINARY
#define O_BINARY 0 // ignore on Unix
#endif

extern "C"
{
struct evbuffer;
struct event;
void event_free(struct event*);
}
namespace acng
{

#define RX_ERROR -1
constexpr size_t MAX_IN_BUF = MAX_VAL(uint16_t);

int falloc_helper(int fd, off_t start, off_t len);
int fdatasync_helper(int fd);

ssize_t sendfile_generic(int out_fd, int in_fd, off_t *offset, size_t count);

class Cstat : public stat
{
	bool bResult;
public:
	Cstat() : bResult(false) {};
    Cstat(cmstring &s) : bResult(false) { update(s.c_str()); }
	Cstat(int fd) { bResult = ! fstat(fd, static_cast<struct stat*>(this)); }
    Cstat(const char *sz) : bResult(false) { update(sz); }
	operator bool() const { return bResult; }
    bool update(const char *sz) { return (bResult = !::stat(sz, static_cast<struct stat*>(this))); }
};

inline off_t GetFileSize(cmstring &path, off_t defret) { Cstat s(path); return s ? s.st_size : defret; }
std::error_code FileCopy(cmstring &from, cmstring &to);

bool LinkOrCopy(const mstring &from, const mstring &to);

void set_block(int fd);

inline void justforceclose(int fd)
{
	while(0 != ::close(fd))
	{
		if(errno != EINTR)
			break;
	};
}

inline void checkforceclose(int &fd)
{
	while (fd != -1)
	{
		if (0 == ::close(fd) || errno != EINTR)
			fd = -1;
	}
}


inline void checkForceFclose(FILE* &fh)
{
	if (fh)
	{
		int fd = fileno(fh);
		if (0 != ::fclose(fh) && errno != EBADF)
		{
			checkforceclose(fd);
		}
		fh = nullptr;
	}
}

// more efficient than tDtorEx with lambda
struct FILE_RAII
{
	FILE *p = nullptr;
	inline FILE_RAII() {};
	inline ~FILE_RAII() { close(); }
	operator FILE* () const { return p; }
	inline void close() { checkForceFclose(p); };
private:
	FILE_RAII(const FILE_RAII&);
	FILE_RAII operator=(const FILE_RAII&);
};

void mkdirhier(cmstring& path);
bool xtouch(cmstring &wanted);
void mkbasedir(const mstring & path);

ssize_t dumpall(int fd, string_view data);

/*
class tLazyStat
{
	LPCSTR path;
	struct stat stbuf;
	inline bool AccessFile() { if(path)
public:
	inline tLazyStat(LPCSTR p) : path(p) {};
	operator bool() const;
	off_t GetSize() const;
	off_t GetSpace() const;
};
*/

using unique_fd = auto_raii<int, justforceclose, -1>;

void event_and_fd_free(event*);
using unique_fdevent = auto_raii<event*, event_and_fd_free, nullptr>;

}

#endif /* FILEIO_H_ */
