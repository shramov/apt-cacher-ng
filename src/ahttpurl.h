#ifndef AHTTPURL_H
#define AHTTPURL_H

#include "actypes.h"
#include "portutils.h"

namespace acng
{

extern std::string sDefPortHTTP, sDefPortHTTPS;
extern cmstring PROT_PFX_HTTPS, PROT_PFX_HTTP, PROT_PFX_UNIX;

#define DEFAULT_PORT_HTTP 80
#define DEFAULT_PORT_HTTPS 443

class ACNG_API tHttpUrl
{

private:
		uint16_t nPort = 0;

public:
		enum class EProtoType : decltype (nPort)
		{
			HTTP,
			HTTPS,
			UDS
		} m_schema = EProtoType::HTTP;

		bool SetHttpUrl(cmstring &uri, bool unescape = true);
		bool SetUnixUrl(cmstring &uri);
		mstring ToURI(bool bEscaped, bool hostOnly = false) const;
        mstring sHost, sPath, sUserPass;

		inline cmstring & GetProtoPrefix() const
		{
			switch(m_schema)
			{
			case EProtoType::HTTP: return PROT_PFX_HTTP;
			case EProtoType::HTTPS: return PROT_PFX_HTTPS;
			case EProtoType::UDS: return PROT_PFX_UNIX;
			}
			return PROT_PFX_HTTP;
		}
        tHttpUrl(const acng::tHttpUrl& a)
        {
                sHost = a.sHost;
				nPort = a.nPort;
                sPath = a.sPath;
                sUserPass = a.sUserPass;
				m_schema = a.m_schema;
        }
        tHttpUrl & operator=(const tHttpUrl &a)
        {
                if(&a == this) return *this;
                sHost = a.sHost;
				nPort = a.nPort;
                sPath = a.sPath;
                sUserPass = a.sUserPass;
				m_schema = a.m_schema;
                return *this;
        }
        bool operator==(const tHttpUrl &a) const
        {
				return a.sHost == sHost && a.nPort == nPort && a.sPath == sPath
								&& a.sUserPass == sUserPass && m_schema == a.m_schema;;
        }

		bool operator!=(const tHttpUrl &a) const
        {
                return !(a == *this);
        }
        inline void clear()
        {
                sHost.clear();
				nPort = 0;
                sPath.clear();
                sUserPass.clear();
				m_schema = EProtoType::HTTP;
        }
		uint16_t GetDefaultPortForProto() const
		{
			switch(m_schema)
			{
			case EProtoType::HTTP: return DEFAULT_PORT_HTTP;
			case EProtoType::HTTPS: return DEFAULT_PORT_HTTPS;
			default: return 0;
			}
        }
		uint16_t GetPort(uint16_t defVal) const { return nPort ? nPort : defVal; };
		uint16_t GetPort() const { return GetPort(GetDefaultPortForProto()); }
		void SetPort(uint16_t nPort) { this->nPort = nPort; }

		inline tHttpUrl(cmstring &host, uint16_t port, EProtoType schema) :
						nPort(port), sHost(host), m_schema(schema)
		{
		}
		inline tHttpUrl(cmstring &host, uint16_t port, bool ssl) :
			tHttpUrl(host, port, ssl ? EProtoType::HTTPS : EProtoType::HTTP)
        {
        }

        inline tHttpUrl() =default;
		// special short version with only hostname and port number
		std::string GetHostPortKey() const;
		// like above but also incorporates the schema
		std::string GetHostPortProtoKey() const;

		tHttpUrl& SetHost(string_view host) { sHost = host; return *this; }
		tHttpUrl& SetHost(mstring&& host) { sHost = move(host); return *this; }
};
/*
struct tHttpUrlAssignable : public tHttpUrl
{
	bool valid = false;
	using tHttpUrl :: tHttpUrl;
	tHttpUrlAssignable(cmstring& raw) : tHttpUrl(), valid(SetHttpUrl(raw)) {}
};
*/

}

#endif // AHTTPURL_H
