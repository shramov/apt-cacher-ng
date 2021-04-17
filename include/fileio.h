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

namespace acng
{

int falloc_helper(int fd, off_t start, off_t len);
int fdatasync_helper(int fd);

ssize_t sendfile_generic(int out_fd, int in_fd, off_t *offset, size_t count);

class Cstat : public stat
{
	bool bResult;
public:
	Cstat() : bResult(false) {};
    Cstat(cmstring &s) : bResult(false) { update(s.c_str()); }
    Cstat(const char *sz) : bResult(false) { update(sz); }
	operator bool() const { return bResult; }
    bool update(const char *sz) { return (bResult = !::stat(sz, static_cast<struct stat*>(this))); }
};

bool FileCopy(cmstring &from, cmstring &to, int *pErrnoRet = nullptr);

bool LinkOrCopy(const mstring &from, const mstring &to);


void set_nb(int fd);
void set_block(int fd);

inline bool forceclose(int& fd) { bool ret = true; while(0 != ::close(fd)) { if(errno != EINTR) { ret = false; break; }}; fd=-1; return ret;}
inline void justforceclose(int fd) { while(0 != ::close(fd)) { if(errno != EINTR) break; }; }
inline void checkforceclose(int &fd)
{
	if (fd == -1)
		return;
	while (0 != ::close(fd))
	{
		if (errno != EINTR)
			break;
	};
	fd = -1;
}


inline void checkForceFclose(FILE* &fh)
{
	if (fh)
	{
		int fd = fileno(fh);
		if (0 != ::fclose(fh) && errno != EBADF)
		{
			forceclose(fd);
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

}

#endif /* FILEIO_H_ */
