#include "config.h"
#include "ac3rdparty.h"
#include "acfg.h"
#include "debug.h"

#include <deque>

#include <event2/thread.h>
#include <event2/event.h>

#ifdef HAVE_SSL
#include <openssl/evp.h>
#include "openssl/ssl.h"
#include "openssl/err.h"
#include <openssl/crypto.h>
#endif

#include <ares.h>

namespace acng {

#ifdef HAVE_SSL

tSslConfig::tSslConfig()
{
	static bool inited=false;
	if(inited)
		return;
	inited = true;
	OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT, nullptr);
	ERR_load_BIO_strings();
	ERR_load_SSL_strings();
}

tSslConfig::~tSslConfig()
{
	if (m_ctx)
		SSL_CTX_free(m_ctx);
}

SSL_CTX *tSslConfig::GetContext()
{
	if (!m_ctx_init_error.empty())
		return nullptr;

	if (!m_ctx)
	{
		m_ctx = SSL_CTX_new(TLS_client_method());
		if (!m_ctx)
		{
			auto msg = ERR_reason_error_string(ERR_get_error());
			m_ctx_init_error = msg ? msg : "SSL context initialization failed";
			return nullptr;
		}

		if (! SSL_CTX_load_verify_locations(m_ctx,
				cfg::cafile.empty() ? nullptr : cfg::cafile.c_str(),
			cfg::capath.empty() ? nullptr : cfg::capath.c_str()))
		{
			auto msg = ERR_reason_error_string(ERR_get_error());
			m_ctx_init_error = msg ? msg : "Error loading local root certificates";
			SSL_CTX_free(m_ctx);
			m_ctx = nullptr;
		}

	}
	return m_ctx;
}

#else
#error Fix non-TLS build, this probably should setup a dummy of tSslConfig
void ACNG_API globalSslInit() {}
void ACNG_API globalSslDeInit() {}
#endif

void ACNG_API ac3rdparty_init()
{
	evthread_use_pthreads();
#ifdef DEBUG
	evthread_enable_lock_debugging();
#endif
	ares_library_init(ARES_LIB_INIT_ALL);
}

void ACNG_API ac3rdparty_deinit()
{
	libevent_global_shutdown();
	ares_library_cleanup();
}


}
