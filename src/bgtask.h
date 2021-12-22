/*
 * bgtask.h
 *
 *  Created on: 18.09.2009
 *      Author: ed
 */

#ifndef BGTASK_H_
#define BGTASK_H_

#include "mainthandler.h"

namespace acng
{

class ACNG_API tExclusiveUserAction : public tSpecialRequestHandler
{
public:
	// forward all constructors, no specials here
	// XXX: oh please, g++ 4.7 is not there yet... using tSpecialRequest::tSpecialRequest;
	tExclusiveUserAction(tSpecialRequestHandler::tRunParms&& parms);

	virtual ~tExclusiveUserAction();

	 /*!
	  * This execution implementation makes sure that only one task runs
	  * in background, it sends regular header/footer printing and controls termination.
	  *
	  * Work is to be done the Action() method implemented by the subclasses.
	  */
	virtual void Run() override;

protected:
	bool CheckStopSignal();

	// to be implemented by subclasses
	virtual void Action() =0;

	void SendLocalOnly(const char *data, size_t size) override;

	void DumpLog(time_t id);

	time_t GetTaskId();

private:

	std::ofstream m_reportStream;

	// XXX: this code sucks and needs a full review. It abuses shared_ptr as stupid reference
	// counter. Originally written with some uber-protective considerations in mind like
	// not letting a listener block the work of an operator by any means.

	protected:
	// value is an ID number assigned to the string (key) in the moment of adding it
	struct pathMemEntry { mstring msg; unsigned id;};
	std::map<mstring,pathMemEntry> m_pathMemory;
	// generates a lookup blob as hidden form parameter
	mstring BuildCompressedDelFileCatalog();

	//static base_with_condition g_StateCv;
	static bool g_sigTaskAbort;
	// to watch the log file
	int m_logFd = -1;
};

enum ControLineType : uint8_t
{
	NotForUs = 0,
	BeforeError = 1,
	Error = 2
};
#define maark "41d_a6aeb8-26dfa" // random enough to not match anything existing *g*

}

#endif /* BGTASK_H_ */
