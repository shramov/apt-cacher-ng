#ifndef AHTTPURL_H
#define AHTTPURL_H

#include "actypes.h"

namespace acng
{

extern std::string sDefPortHTTP, sDefPortHTTPS;
extern cmstring PROT_PFX_HTTPS, PROT_PFX_HTTP;

class ACNG_API tHttpUrl
{

private:
        mstring sPort;

public:
        bool SetHttpUrl(cmstring &uri, bool unescape = true);
        mstring ToURI(bool bEscaped) const;
        mstring sHost, sPath, sUserPass;

        bool bSSL=false;
        inline cmstring & GetProtoPrefix() const
        {
                return bSSL ? PROT_PFX_HTTPS : PROT_PFX_HTTP;
        }
        tHttpUrl(const acng::tHttpUrl& a)
        {
                sHost = a.sHost;
                sPort = a.sPort;
                sPath = a.sPath;
                sUserPass = a.sUserPass;
                bSSL = a.bSSL;
        }
        tHttpUrl & operator=(const tHttpUrl &a)
        {
                if(&a == this) return *this;
                sHost = a.sHost;
                sPort = a.sPort;
                sPath = a.sPath;
                sUserPass = a.sUserPass;
                bSSL = a.bSSL;
                return *this;
        }
        bool operator==(const tHttpUrl &a) const
        {
                return a.sHost == sHost && a.sPort == sPort && a.sPath == sPath
                                && a.sUserPass == sUserPass && a.bSSL == bSSL;
        }
        ;bool operator!=(const tHttpUrl &a) const
        {
                return !(a == *this);
        }
        inline void clear()
        {
                sHost.clear();
                sPort.clear();
                sPath.clear();
                sUserPass.clear();
                bSSL = false;
        }
        inline cmstring& GetDefaultPortForProto() const {
                return bSSL ? sDefPortHTTPS : sDefPortHTTP;
        }
        inline cmstring& GetPort(cmstring& szDefVal) const { return !sPort.empty() ? sPort : szDefVal; }
        inline cmstring& GetPort() const { return GetPort(GetDefaultPortForProto()); }

        inline tHttpUrl(cmstring &host, cmstring& port, bool ssl) :
                        sPort(port), sHost(host), bSSL(ssl)
        {
        }
        inline tHttpUrl() =default;
};

}

#endif // AHTTPURL_H
