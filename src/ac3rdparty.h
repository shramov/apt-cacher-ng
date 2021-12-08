#ifndef AC3RDPARTY_H
#define AC3RDPARTY_H

#include "config.h"
#include "actypes.h"

#ifdef HAVE_SSL
#include <openssl/ossl_typ.h>
#endif

namespace acng
{

void ACNG_API globalSslInit();
void ACNG_API globalSslDeInit();

class ACNG_API tSslConfig
{
public:

	tSslConfig();
	~tSslConfig();
	SSL_CTX* GetContext();
	cmstring& GetContextError() { return m_ctx_init_error; }

private:

	mstring m_ctx_init_error;
	SSL_CTX* m_ctx = nullptr;
};

}

#endif // AC3RDPARTY_H
