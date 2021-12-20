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

	static void PassThrough(bufferevent* be, cmstring& uri, const header& reqHead, acres& res)
	{
		LOGSTARTFUNCs;

		TDirectConnector *hndlr = nullptr;
		// to be terminated here, in one way or another
		unique_bufferevent_flushclosing xbe(be);

		try
		{
			hndlr = new TDirectConnector(res);
			xbe.swap(hndlr->m_be);
			hndlr->m_httpProto = reqHead.GetProtoView();
		}
		catch (const std::bad_alloc&)
		{
			unique_bufferevent_fdclosing closer(be);
		}
		if(res.GetMatchers().Match(uri, rex::PASSTHROUGH) && hndlr->url.SetHttpUrl(uri))
			return hndlr->Go();
		ebstream(be) << hndlr->m_httpProto << " 403 CONNECT denied (ask the admin to allow HTTPS tunnels)\r\n\r\n"sv;
		return delete hndlr;
	}
	void Go()
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
		m_connBuilder = atransport::Create(url, move(act), m_res,
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

void PassThrough(bufferevent *be, cmstring &uri, const header &reqHead, acres &res)
{
	TDirectConnector::PassThrough(be, uri, reqHead, res);
}

}
