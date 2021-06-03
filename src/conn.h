

#ifndef _CON_H
#define _CON_H

#include "actypes.h"
#include "actemplates.h"
#include "fileitem.h"

namespace acng
{

class dlcontroller;

/**
 * @brief Common functionality needed by the jobs.
 */
class IConnBase : public tLintRefcounted
{
public:
	virtual dlcontroller* SetupDownloader() =0;
	virtual lint_ptr<IFileItemRegistry> GetItemRegistry() =0;
	/**
	 * @brief Push the internal processing which is waiting for some notification
	 * @return true to keep calling, false to unregister the callback
	 */
	virtual bool poke(uint_fast32_t jobId) =0;
	virtual cmstring& getClientName() =0;
};

/**
 * @brief StartServing prepares the request serving stream and attached an appropriate handler to it
 * @param fd File descriptor, call of this method takes responsibility for it
 * @param clientName
 */
void StartServing(unique_fd fd, std::string clientName);

#if 0
class conn
{
public:
#error ney. brauche einen create mit dispatcher, gleich von anfang an verschiedene typen
	conn(unique_fd&& fd, mstring sClient, std::shared_ptr<IFileItemRegistry>);
	/**
	 * @brief Detach the object and run it's activities independently, eventually deleting itself.
	 */
	void Detach();

	std::shared_ptr<IFileItemRegistry> GetItemRegistry();
	dlcon* SetupDownloader();
	void LogDataCounts(cmstring & sFile, mstring xff, off_t nNewIn,
					   off_t nNewOut, bool bAsError);

private:
	conn& operator=(const conn&);
	conn(const conn&);
};

#endif

}

#endif
