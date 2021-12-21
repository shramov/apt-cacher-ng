#include "acforwarder.h"
#include "conn.h"
#include "meta.h"
#include "atransport.h"
#include "actypes.h"
#include "actemplates.h"
#include "rex.h"

namespace acng
{
using namespace std;

/**
 * @brief Vending point for CONNECT processing
 * Flushes and releases bufferevent in the end.
 */
struct TDirectConnector
{
	tHttpUrl url;
	unique_bufferevent_flushclosing m_be;
	lint_ptr<atransport> outStream;
	TFinalAction m_connBuilder;
	string_view m_httpProto;
	acres& m_res;
	aobservable::subscription m_baseSub;

	TDirectConnector(acres& res) : m_res(res)
	{
		// release from event base on shutdown
		m_baseSub = evabase::GetGlobal().subscribe([this]() { cbEvent(nullptr, BEV_EVENT_EOF, this);} );
	}
	~TDirectConnector()
	{
#warning implement data accounting
	}

	void Go(tHttpUrl&& url)
	{
		auto act = [this] (atransport::tResult&& rep)
		{
			if (!rep.err.empty())
			{
				ebstream(*m_be) << m_httpProto << " 502 CONNECT error: "sv << rep.err << "\r\n\r\n"sv;
				return delete this;
			}
			outStream = rep.strm;
			auto targetCon = outStream->GetBufferEvent();
			ebstream(*m_be) << m_httpProto << " 200 Connection established\r\n\r\n"sv;
			bufferevent_setcb(*m_be, cbClientReadable, cbClientWriteable, cbEvent, this);
			bufferevent_setcb(targetCon, cbClientWriteable, cbClientReadable, cbEvent, this);
			setup_be_bidirectional(*m_be);
			setup_be_bidirectional(targetCon);
			m_connBuilder.reset();
		};
		m_connBuilder = atransport::Create(move(url), move(act), m_res,
										   atransport::TConnectParms()
										   .SetDirectConnection(true)
										   .SetNoTlsOnTarget(true));
	}
	static void cbClientReadable(struct bufferevent*, void *ctx)
	{
		auto me = (TDirectConnector*)ctx;
		bufferevent_write_buffer(me->outStream->GetBufferEvent(), bereceiver(*me->m_be));
	};
	static void cbClientWriteable(struct bufferevent*, void *ctx)
	{
		auto me = ((TDirectConnector*)ctx);
		bufferevent_write_buffer(*me->m_be, bereceiver(me->outStream->GetBufferEvent()));
	}
	static void cbEvent(struct bufferevent *, short what, void *ctx)
	{
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))
			return delete ((TDirectConnector*)ctx);
	}
};


void PassThrough(unique_bufferevent_flushclosing&& xbe, cmstring& uri, const header& reqHead, acres& res)
{
	LOGSTARTFUNCs;
	try
	{
		bufferevent_disable(*xbe, EV_READ|EV_WRITE);
		// be sure about THAT
		bufferevent_setcb(*xbe, nullptr,nullptr,nullptr,nullptr);

		tHttpUrl url;
		if(res.GetMatchers().Match(uri, rex::PASSTHROUGH) && url.SetHttpUrl(uri))
		{
			auto hndlr = new TDirectConnector(res);
			hndlr->m_be.reset(move(xbe));
			hndlr->m_httpProto = reqHead.GetProtoView();
			return hndlr->Go(move(url));
		}
		else
		{
			ebstream(xbe.get()) <<  reqHead.GetProtoView() << " 403 CONNECT denied (ask the admin to allow HTTPS tunnels)\r\n\r\n"sv;
			return xbe.reset();
		}
	}
	catch (const std::bad_alloc&)
	{
		return xbe.reset();
	}
}

}
