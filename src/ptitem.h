#ifndef PTITEM_H
#define PTITEM_H

#include "fileitem.h"
#include "aevutil.h"

namespace acng
{

/*
 * Unlike the regular store-and-forward file item handler, this ones does not store anything to
 * harddisk. Instead, it uses the download buffer and lets job object send the data straight from
 * it to the client.
 *
 * The header is put in its original raw format aside, and is reprocessed again by the job while filtering the offending header values ONLY.
 */
class tPassThroughFitem : public fileitem
{
protected:
		unique_eb m_q;
        std::string m_sHeader;
		class TSender;
		friend class TSender;
public:
		tPassThroughFitem(std::string s);
        virtual FiStatus Setup() override;

        const std::string& GetRawResponseHeader() override;

        void DlFinish(bool) override;

		ssize_t DlConsumeData(evbuffer* chunk, size_t maxTake) override;

        // fileitem interface
protected:
		bool DlStarted(evbuffer *rawData, size_t headerLen, const tHttpDate &modDate, string_view origin, tRemoteStatus status, off_t bytes2seek, off_t bytesAnnounced) override;
public:
		std::unique_ptr<ICacheDataSender> GetCacheSender() override;
};

}

#endif // PTITEM_H
