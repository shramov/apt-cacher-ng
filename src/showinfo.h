#ifndef SHOWINFO_H_
#define SHOWINFO_H_

#include "mainthandler.h"
#include "meta.h"
#include "filereader.h"

#include <list>

namespace acng
{

extern tRemoteStatus stOK;

class tMarkupFileSend : public tSpecialRequestHandler
{
public:
	virtual ~tMarkupFileSend() {};
	void Run() override;

protected:
	void SendProcessedData(string_view);

	tMarkupFileSend(tRunParms&& parms,
					const char * outerDecoFilename,
					const char *mimetype,
					const tRemoteStatus& st);
	// presets some default properties for header/footer/etc.

	const char *m_sOuterDecoFile, *m_sMimeType;
	tRemoteStatus m_httpStatus;

	bool m_bFatalError = false;

	// uses fallback lookup map, can be feed with data in subclass constructor
	virtual void SendProp(cmstring &key);
	virtual int CheckCondition(string_view key); // 0: true, 1: false, <0: unknown condition

	struct tMarkupInput
	{
		filereader fr;
		bool bError = false;
		tMarkupInput(cmstring& fname, bool alreadyError = false);
		string_view view() { return fr.getView();}
	};

private:
	tMarkupFileSend(const tMarkupFileSend&) =delete;
	tMarkupFileSend operator=(const tMarkupFileSend&)=delete;
	void SendIfElse(LPCSTR pszBeginSep, LPCSTR pszEnd);
};

class tMaintJobBase : public tMarkupFileSend
{
public:
	//using tMarkupFileSend::tMarkupFileSend;
	tMaintJobBase(tRunParms&& parms);
protected:
	// to be implemented by subclasses to inject HTML into ${action}
	virtual void Action() =0;
	virtual void SendProp(cmstring &key) override;
	virtual int CheckCondition(string_view key) override; // 0: true, 1: false, <0: unknown condition

	void ProcessResource(cmstring sFilename);

	bool CheckStopSignal();
#warning FIXME: this needs to be static and coordinated with the Cancel code somehow
	std::atomic_bool g_sigTaskAbort;
	bool m_showCancel = false;
	time_t m_startTime;
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
};

struct tShowInfo : public tMarkupFileSend
{
	tShowInfo(tRunParms&& parms)
		:tMarkupFileSend(std::move(parms), "userinfo.html", "text/html", {406, "Usage Information"}) {};
};

struct tMaintOverview : public tMaintJobBase
{
	tMaintOverview(tRunParms&& parms);
	using tMaintJobBase::tMaintJobBase;
	virtual void SendProp(cmstring &key) override;
	virtual void Action() override;
};

}
#endif /*SHOWINFO_H_*/
