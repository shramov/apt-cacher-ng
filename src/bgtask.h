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
	unsigned m_nKbInitVec = 0;
public:
	using tMaintJobBase::tMaintJobBase;
protected:
	unsigned Add2KillBill(cmstring& sPathRel);
	// adds "purgeactionmeta"
	virtual void SendProp(cmstring &key) override;
};

enum ControLineType : uint8_t
{
	NotForUs = 0,
	BeforeError = 1,
	Error = 2
};
#define maark "41d_a6aeb8-26dfa" // random enough to not match anything existing *g*

#define MJSTORE_SUBPATH "_xstore/maintjobs"sv

}

#endif /* BGTASK_H_ */
