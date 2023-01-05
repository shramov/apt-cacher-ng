#include "ahttpurl.h"
#include "meta.h"
#include "acutilport.h"

namespace acng {

using namespace std;

// See RFC3986
bool tHttpUrl::SetHttpUrl(string_view sUrlRaw, bool unescape)
{
	clear();
	mstring url(unescape ? UrlUnescape(sUrlRaw) : sUrlRaw);

	trimBack(url);
	trimFront(url);

	if(url.empty())
		return false;

	tStrPos hStart(0), l=url.length(), hEndSuc(0), pStart(0), p;
	bool bCheckBrac=false, noPort = false;

	if (0 == strncasecmp(url.c_str(), "http://", 7))
		{
				hStart=7;
			m_schema = EProtoType::HTTP;
		}
	else if(0==strncasecmp(url.c_str(), "https://", 8))
	{
#ifndef HAVE_SSL
		log::err("E_NOTIMPLEMENTED: SSL");
		return false;
#else
				hStart=8;
				m_schema = EProtoType::HTTPS;
#endif
	}
	else if(isalnum((uint)url[0]))
				hStart=0;
	else if(url[0]=='[')
	{
				hStart=0;
				bCheckBrac=true; // must be closed
	}
	else if(startsWithSz(url, "::"))
	{
		hStart = 0;
		noPort = true;
	}
	else if(stmiss!=url.find("://"))
		return false; // other protocol or weird stuff

	// kill leading slashes in any case
	while(hStart<l && url[hStart]=='/') hStart++;
	if (hStart >= l)
		return false;

	hEndSuc=url.find_first_of("/?", hStart);
	if (stmiss == hEndSuc)
	{
		// okay, the hostname is all we have
		hEndSuc = l;
		goto extract_host_and_path_and_check_port;
	}
	if(url[hEndSuc] == '?') // matched http://foo?param=X
	{
		sPath = mstring("/") + url.substr(hEndSuc);
		goto extract_host_check_port;
	}

	pStart = hEndSuc;
	while(pStart<l && url[pStart]=='/')
		pStart++;
	pStart--;

extract_host_and_path_and_check_port:
	if(pStart==0)
		sPath="/";
	else
		sPath=url.substr(pStart);

extract_host_check_port:

	if(url[hStart]=='_') // those are reserved
		return false;

	sHost=url.substr(hStart, hEndSuc - hStart);

	// credentials might be in there, strip them off
	l=sHost.rfind('@');
	if(l!=mstring::npos)
	{
		sUserPass = UrlUnescape(sHost.substr(0, l));
		sHost.erase(0, l+1);
	}

	if (!bCheckBrac && 2 <= count(sHost.begin(), sHost.end(), ':'))
	{
		noPort=true;
	}

	l = sHost.size();
	if (!noPort)
	{
		if (bCheckBrac)
		{
			p = sHost.rfind(']');
			p = sHost.find(':', p+1);
		}
		else
			p=sHost.rfind(':');

		int pVal(0);
		if (p==stmiss)
			goto strip_ipv6_junk;
		else if(p==l-1)
			return false; // this is crap, http:/asdf?
		else for(tStrPos r=p+1;r<l;r++)
			if(! isdigit(sHost[r]))
				return false;
		pVal = atoi(sHost.data() + p + 1);
		if (pVal > MAX_VAL(uint16_t))
			return false;
		nPort = (uint16_t) pVal;
		sHost.erase(p);
	}

strip_ipv6_junk:

	bool host_appears_to_be_ipv6 = false;
	if(sHost[0]=='[')
	{
		host_appears_to_be_ipv6 = true;
		bCheckBrac=true;
		sHost.erase(0,1);
	}

	if (sHost[sHost.length()-1] == ']') {
		bCheckBrac = !bCheckBrac;
		sHost.erase(sHost.length()-1);
	}
	if (bCheckBrac) // Unmatched square brackets.
		return false;
	if (!host_appears_to_be_ipv6)
		sHost = UrlUnescape(sHost);

	// also detect obvious IPv6 misspelling ASAP
	return (sHost.find(":::"sv) == string_view::npos);
}

bool tHttpUrl::SetUnixUrl(cmstring &uri)
{
#warning implementme
	return false;
}

string tHttpUrl::ToURI(bool bUrlEscaped, bool hostOnly) const
{
	auto s(GetProtoPrefix());

	// if needs transfer escaping and is not internally escaped
	if (bUrlEscaped)
	{
		UrlEscapeAppend(sHost, s);
		if (nPort)
			s += se + ':' + tPortFmter().fmt(nPort) ;
		UrlEscapeAppend(sPath, s);
	}
	else
	{
		s += sHost;
		if (nPort)
			s += se + ':' + tPortFmter().fmt(nPort);
		if (!hostOnly)
			s += sPath;
	}
	return s;
}

string tHttpUrl::GetHostPortKey() const
{
	return HostPortKeyMaker(sHost, GetPort());
}

string tHttpUrl::GetHostPortProtoKey() const
{
	char sfx = 'a';
	sfx += (int) m_schema;
	return GetHostPortKey() + "_" + sfx;
}

}
