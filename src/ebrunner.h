/*
 * ebrunner.h
 *
 *  Created on: 31 янв. 2021 г.
 *      Author: ed
 */

#ifndef INCLUDE_EBRUNNER_H_
#define INCLUDE_EBRUNNER_H_

#include "evabase.h"
#include "dlcon.h"

namespace acng
{
class dlcontroller;
class ebase;

// tool helper class for acngtool and httpfs, runs event and download threads
class ACNG_API tMinComStack
{
	acres* sharedResources;
	evabase* ebase;
	lint_user_ptr<dlcontroller> dler;
public:
	tMinComStack();
	~tMinComStack();
	dlcontroller& getDownloader();
	event_base* getBase();
private:
	class Impl;
	Impl *m_pImpl;
};

}


#endif /* INCLUDE_EBRUNNER_H_ */
