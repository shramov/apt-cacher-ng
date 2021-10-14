#ifndef PTITEM_H
#define PTITEM_H

#include "fileitem.h"

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
        evbuffer* m_q;
        std::string m_sHeader;
		class TSender;
		friend class TSender;
public:
		tPassThroughFitem(std::string s);
        ~tPassThroughFitem();
        virtual FiStatus Setup() override;

        const std::string& GetRawResponseHeader() override;

        void DlFinish(bool) override;

        ssize_t DlAddData(evbuffer* chunk, size_t maxTake) override;

        // fileitem interface
protected:
		bool DlStarted(evbuffer *rawData, size_t headerLen, const tHttpDate &modDate, cmstring &origin, tRemoteStatus status, off_t bytes2seek, off_t bytesAnnounced) override;
public:
		std::unique_ptr<ICacheDataSender> GetCacheSender(off_t startPos) override;
};

}

#endif // PTITEM_H
