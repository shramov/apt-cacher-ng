#ifndef SHOWINFO_H_
#define SHOWINFO_H_

#include "maintenance.h"
#include <list>

namespace acng
{
class tMarkupFileSend : public tSpecialRequestHandler
{
public:
	virtual ~tMarkupFileSend() {};
	void Run() override;
protected:
	tMarkupFileSend(tRunParms&& parms,
			const char * filename,
			const char *mimetype,
					const tRemoteStatus& st);
	// presets some default properties for header/footer/etc.
	void SendRaw(const char *pBuf, size_t len);
	const char *m_sFileName, *m_sMimeType;
	tRemoteStatus m_httpStatus;

	bool m_bFatalError=false;

	// uses fallback lookup map, can be feed with data in subclass constructor
	virtual void SendProp(cmstring &key);
	virtual int CheckCondition(LPCSTR key, size_t len); // 0: true, 1: false, <0: unknown condition

private:
	tMarkupFileSend(const tMarkupFileSend&) =delete;
	tMarkupFileSend operator=(const tMarkupFileSend&)=delete;
	void SendIfElse(LPCSTR pszBeginSep, LPCSTR pszEnd);
};

struct tStyleCss : public tMarkupFileSend
{
	inline tStyleCss(tRunParms&& parms) :
	tMarkupFileSend(std::move(parms), "style.css", "text/css", "200 OK") {};
};

class tDeleter : public tMarkupFileSend
{
	std::set<unsigned> files;
	tSS sHidParms;
	mstring sVisualMode; // Truncat or Delet
	tStrDeq extraFiles;
public:
	tDeleter(tRunParms&& parms, cmstring& vmode);
	virtual void SendProp(cmstring &key) override;
	//virtual int CheckCondition(LPCSTR key, size_t len) override; // 0: true, 1: false, <0: unknown condition

};

struct tShowInfo : public tMarkupFileSend
{
	tShowInfo(const tRunParms& parms)
	:tMarkupFileSend(parms, "userinfo.html", "text/html", "406 Usage Information") {};
};

struct tMaintPage : public tMarkupFileSend
{
	tMaintPage(tRunParms&& parms);
	virtual void SendProp(cmstring &key) override;
};
}
#endif /*SHOWINFO_H_*/
