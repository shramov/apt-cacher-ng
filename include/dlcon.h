#ifndef _DLCON_H
#define _DLCON_H

#include "tcpconnect.h"

namespace acng
{
class IDlConFactory;
class fileitem;

/**
 * dlcon is a basic connection broker for download processes.
 * It's defacto a slave of the conn class, the active thread is spawned by conn when needed
 * and it's finished by its destructor. However, the life time is prolonged if the usage count
 * is not down to zero, i.e. when there are more users registered as reader for the file
 * downloaded by the agent here then it will continue downloading and block the conn dtor
 * until that download is finished or the other client detaches. If a download is active and parent
 * conn object calls Stop... then the download will be aborted ASAP.
 *
 * Internally, a queue of download job items is maintained. Each contains a reference either to
 * a full target URL or to a tupple of a list of mirror descriptions (url prefix) and additional
 * path suffix for the required file.
 *
 * In addition, there is a local blacklist which is applied to all download jobs in the queue,
 * i.e. remotes marked as faulty there are no longer considered by the subsequent download jobs.
 */

struct dlrequest;
struct tDlJob;
class fileitem;

class ACNG_API dlcon
{
	friend struct ::acng::tDlJob;
	class Impl;
	Impl *_p;

    public:
        dlcon(cmstring& sClientsHostname, const IDlConFactory &pConFactory = g_tcp_con_factory);
        ~dlcon();

        void WorkLoop();
        void SignalStop();
        bool AddJob(const SHARED_PTR<fileitem> &fi, const dlrequest& dlrq);
};

/**
 * @brief Parameter struct and fluent-friendly builder for download requests.
 */
struct dlrequest
{
	const tHttpUrl *pForcedUrl = nullptr;
	cfg::tRepoResolvResult repoSrc;
	LPCSTR reqHead = nullptr;
	LPCSTR szHeaderXff = nullptr;
    bool isPassThroughRequest = false;

	dlrequest& setSrc(const tHttpUrl& url) { pForcedUrl=&url; return *this;}
	dlrequest& setSrc(cfg::tRepoResolvResult repoRq) { repoSrc = std::move(repoRq); return *this; }
	dlrequest& setRqHeadString(LPCSTR rh) { reqHead = rh; return *this;}
	dlrequest& setXff(LPCSTR xff) { szHeaderXff = xff; return *this;}
};

}

#endif
