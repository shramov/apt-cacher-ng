#ifndef _DLCON_H
#define _DLCON_H

#include "remotedbtypes.h"
#include "ahttpurl.h"
#include "acsmartptr.h"
#include "acres.h"

#include <functional>

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


class ACNG_API dlcontroller : public tLintRefcounted, public tExtRefExpirer
{
public:
	static lint_user_ptr<dlcontroller> CreateRegular(acres& res);
	virtual ~dlcontroller() =default;
	/**
	 * Forced Shutdown. Unlike the lazy shutdown, it will terminate all pending jobs immediatelly and stop all idle streams.
	 * */
	virtual void TeardownASAP() =0;

	/**
	 * @brief AddJob
	 * @param fi
	 * @param src Either this or repoSrc must be set
	 * @param repoSrc Either this or src must be set
	 * @param isPT this influences Connection/Accept-Encoding fields, rely on what the requester gives us, XXX: is Connection not filtered?
	 * @param extraHeaders
	 * @return
	 */
	virtual bool AddJob(lint_ptr<fileitem> fi, const tHttpUrl* src, tRepoResolvResult* repoSrc, bool isPT = false, mstring extraHeaders = "") =0;
};

}

#endif
