#ifndef _DLCON_H
#define _DLCON_H

#include "remotedbtypes.h"
#include "ahttpurl.h"
#include "acsmartptr.h"

namespace acng
{
class fileitem;

/**
 * dlcon is a basic connection broker for download processes.
 *
 * Internally, queues of download job items are maintained. Each contains a reference either to
 * a full target URL or to a tupple of a list of mirror descriptions (url prefix) and additional
 * path suffix for the required file.
 *
 * In addition, there is a local blacklist which is applied to all download jobs in the queue,
 * i.e. remotes marked as faulty there are no longer considered by the subsequent download jobs.
 */

struct dlrequest;
struct tDlJob;

class ACNG_API dlcontroller : public tLintRefcounted
{
public:
	static lint_ptr<dlcontroller> CreateRegular();
	virtual ~dlcontroller() =default;
	virtual void Dispose() =0;
	/**
	 * @brief AddJob
	 * @param fi
	 * @param src
	 * @param isPT this influences Connection/Accept-Encoding fields, rely on what the requester gives us, XXX: is Connection not filtered?
	 * @param extraHeaders
	 * @return
	 */
	virtual bool AddJob(lint_ptr<fileitem> fi, tHttpUrl src, bool isPT = false, mstring extraHeaders = "") =0;
	virtual bool AddJob(lint_ptr<fileitem> fi, tRepoResolvResult repoSrc, bool isPT = false, mstring extraHeaders = "") =0;
};

}

#endif
