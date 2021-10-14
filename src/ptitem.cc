#include "ptitem.h"
#include "fileio.h"
#include "debug.h"
#include "aevutil.h"

using namespace std;

namespace acng
{

tPassThroughFitem::tPassThroughFitem(string s) : fileitem(s)
{
    LOGSTARTFUNC;
    if(!m_q)
        throw std::bad_alloc();
    m_nSizeChecked = m_nSizeCachedInitial = -1;
}

tPassThroughFitem::~tPassThroughFitem()
{
    evbuffer_free(m_q);
}

fileitem::FiStatus tPassThroughFitem::Setup()
{
    return m_status = FIST_INITED;
}

const string &tPassThroughFitem::GetRawResponseHeader() { return m_sHeader; }

void tPassThroughFitem::DlFinish(bool)
{
    LOGSTARTFUNC;
    notifyAll();
    m_status = FIST_COMPLETE;
}

ssize_t tPassThroughFitem::DlAddData(evbuffer *chunk, size_t maxTake)
{
    try
    {
        LOGSTARTFUNCx(maxTake, m_status);

        // something might care, most likely... also about BOUNCE action
        notifyAll();

		if (m_status > fileitem::FIST_COMPLETE)
			return -1;

        if (m_status < FIST_DLRECEIVING)
        {
            m_status = FIST_DLRECEIVING;
            m_nSizeChecked = 0;
        }
		auto ret = eb_move_atmost(m_q, chunk, maxTake);
		if (ret < 0)
			return ret;
		m_nIncommingCount += maxTake;
        m_nSizeChecked += maxTake;
		return ret;
    }
    catch (...)
    {
		m_status = FIST_DLERROR;
		return -1;
    }
}

bool tPassThroughFitem::DlStarted(evbuffer *rawData, size_t headerLen, const tHttpDate &,
								  cmstring &origin, tRemoteStatus status, off_t seekPos,
								  off_t bytesAnnounced)
{
//bool tPassThroughFitem::DlStarted(string_view rawHeader, const tHttpDate &, cmstring &origin, tRemoteStatus status, off_t seekPos, off_t bytesAnnounced)
    LOGSTARTFUNC;
    if (m_status > FIST_COMPLETE)
        return false;
    else if (m_status < FIST_DLGOTHEAD)
		m_status = FIST_DLGOTHEAD;
	else if (m_nSizeChecked > 0 && m_nSizeChecked != seekPos)
		return false;
	else if (m_nSizeChecked <=0 && seekPos > 0)
		return false;

	auto p = evbuffer_pullup(rawData, headerLen);
	m_sHeader.assign((LPCSTR) p, headerLen);
	m_responseOrigin = origin;
	m_responseStatus = status;
	m_nContentLength = bytesAnnounced;
	return true;
}

class tPassThroughFitem::TSender
{
public:
ssize_t SendData(bufferevent *target, evbuffer *, size_t maxTake)
{
#if 0
	LOGSTARTFUNC;
	notifyAll();
	if (m_status > FIST_COMPLETE || evabase::in_shutdown)
		return -1;
	return eb_move_atmost(bufferevent_get_input(target), m_q, maxTake);
#endif
	return -1;
#warning implementme
}

};

std::unique_ptr<fileitem::ICacheDataSender> tPassThroughFitem::GetCacheSender(off_t)
{
#warning fixme
	return std::unique_ptr<fileitem::ICacheDataSender>();
}

}
