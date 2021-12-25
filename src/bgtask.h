/*
 * bgtask.h
 *
 *  Created on: 18.09.2009
 *      Author: ed
 */

#ifndef BGTASK_H_
#define BGTASK_H_

#include "mainthandler.h"
#include "showinfo.h"

namespace acng
{

class ACNG_API tExclusiveUserAction : public tMaintJobBase
{
public:
	using tMaintJobBase::tMaintJobBase;

	virtual ~tExclusiveUserAction();

protected:

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
