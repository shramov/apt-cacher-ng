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

#include <map>

namespace acng
{

class ACNG_API tExclusiveUserAction : public tMaintJobBase
{
public:
	using tMaintJobBase::tMaintJobBase;
	~tExclusiveUserAction();

protected:
	std::deque<std::pair<string_view,string_view>> m_adminActionList;
	unsigned Add2KillBill(string_view sPathRel, string_view reason);
};

enum class ControLineType
{
	NotForUs = 0,
	BeforeError = 1,
	Error = 2
};
#define maark "41d_a6aeb8-26dfa" // random enough to not match anything existing *g*

#define MJSTORE_SUBPATH "_xstore/maintjobs"sv
#define DIR_UP_3LVL "../../../"sv
#define DIR_UP_2LVL "../../"sv

}

#endif /* BGTASK_H_ */
