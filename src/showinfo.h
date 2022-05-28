#ifndef SHOWINFO_H_
#define SHOWINFO_H_

#include "mainthandler.h"
#include "meta.h"
#include "filereader.h"
#include "acworm.h"

#include <list>

namespace acng
{

extern tRemoteStatus stOK;

class tMarkupFileSend : public mainthandler
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
	mstring m_tempString;
public:
	//using tMarkupFileSend::tMarkupFileSend;
	tMaintJobBase(tRunParms&& parms);
protected:
	// to be implemented by subclasses to inject HTML into ${action}
	virtual void Action() =0;
	// wrapper which ensures that we have proper logging system established
	//virtual void ActionWithLog() =0;
	virtual void SendProp(cmstring &key) override;
	virtual int CheckCondition(string_view key) override; // 0: true, 1: false, <0: unknown condition

	void ProcessResource(cmstring sFilename);

	bool CheckStopSignal();

	// this is the string memory dump, used in derived classes. No code in THIS class shall make use of it, to be sure about its destruction time vs. potential users.
	acworm m_stringStore;
	mstring& term(string_view s) { return m_tempString = s; }
};

class tDeleter : public tMarkupFileSend, public DeleteHelper
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
