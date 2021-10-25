#include "ptitem.h"
#include "fileio.h"
#include "debug.h"
#include "aevutil.h"

using namespace std;

#define PT_BUFFER_LIMIT (64*1024)

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
	NotifyObservers();
    m_status = FIST_COMPLETE;
}

ssize_t tPassThroughFitem::DlAddData(evbuffer *chunk, size_t maxTake)
{
    try
    {
        LOGSTARTFUNCx(maxTake, m_status);

		{
			auto in_buffer = evbuffer_get_length(m_q);
			off_t nAddLimit = PT_BUFFER_LIMIT - in_buffer;
			if (off_t(maxTake) > nAddLimit)
				maxTake = nAddLimit;
		}

		if (!maxTake)
			return 0;

		// something might care, most likely... also about BOUNCE action
		NotifyObservers();

		if (m_status > fileitem::FIST_COMPLETE)
			return -1;

        if (m_status < FIST_DLRECEIVING)
        {
            m_status = FIST_DLRECEIVING;
            m_nSizeChecked = 0;
        }

		auto ret = evbuffer_remove_buffer(chunk, m_q, maxTake);
		if (ret < 0)
			return ret;
		m_nIncommingCount += ret;
		m_nSizeChecked += ret;
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

class tPassThroughFitem::TSender : public fileitem::ICacheDataSender
{
	lint_ptr<tPassThroughFitem> parent;
public:
	TSender(tPassThroughFitem* p) : parent(p) {}
	ssize_t SendData(bufferevent* target, size_t maxTake) override
	{
		auto tocopy = std::min(evbuffer_get_length(parent->m_q), maxTake);
		auto howmuch = evbuffer_remove_buffer(parent->m_q, bufferevent_get_input(target), tocopy);
		// push the data source when needed
		//if (evbuffer_get_length(parent->m_q) >= PT_BUFFER_LIMIT)
		parent->NotifyObservers();
		return howmuch;
	}

	virtual off_t NewBytesAvailable() override
	{
		return evbuffer_get_length(parent->m_q);
	}
};

std::unique_ptr<fileitem::ICacheDataSender> tPassThroughFitem::GetCacheSender(off_t)
{
	return std::make_unique<TSender>(this);
}

}
