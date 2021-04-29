#ifndef ACREGISTRY_H
#define ACREGISTRY_H

#include "fileitem.h"

namespace acng {

// "owner" of a file item, cares about sharing instances between multiple agents
class TFileItemHolder
{
        friend class TFileItemRegistry;

public:
        // when copied around, invalidates the original reference
        ~TFileItemHolder();
        inline tFileItemPtr get() { return m_ptr; }
        // invalid dummy constructor
        inline TFileItemHolder() {}

        TFileItemHolder& operator=(const TFileItemHolder &src) = delete;
        TFileItemHolder& operator=(TFileItemHolder &&src) { m_ptr.swap(src.m_ptr); return *this; }
        TFileItemHolder(TFileItemHolder &&src) { m_ptr.swap(src.m_ptr); };

private:
        tFileItemPtr m_ptr;
        explicit TFileItemHolder(const tFileItemPtr& p) : m_ptr(p) {}
};

class IFileItemRegistry : public base_with_mutex
{
        public:

        // public constructor wrapper, create a sharable item with storage or share an existing one
        virtual TFileItemHolder Create(cmstring &sPathUnescaped, ESharingHow how, const fileitem::tSpecialPurposeAttr& spattr) WARN_UNUSED =0;

        // related to GetRegisteredFileItem but used for registration of custom file item
        // implementations created elsewhere (which still need to obey regular work flow)
        virtual TFileItemHolder Create(tFileItemPtr spCustomFileItem, bool isShareable) WARN_UNUSED =0;

        //! @return: true iff there is still something in the pool for later cleaning
        virtual time_t BackgroundCleanup() =0;

        virtual void dump_status() =0;

        virtual void AddToProlongedQueue(TFileItemHolder&&, time_t expTime) =0;

        virtual void Unreg(fileitem& ptr) =0;
};

std::shared_ptr<IFileItemRegistry> MakeRegularItemRegistry();

}

#endif // ACREGISTRY_H
