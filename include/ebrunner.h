/*
 * ebrunner.h
 *
 *  Created on: 31 янв. 2021 г.
 *      Author: ed
 */

#ifndef INCLUDE_EBRUNNER_H_
#define INCLUDE_EBRUNNER_H_

#include "evabase.h"
#include "cleaner.h"
#include "dlcon.h"

#include <thread>

namespace acng
{

// tool helper class for acngtool and httpfs, runs event and download threads
class ACNG_API evabaseFreeFrunner : public evabase
{
public:
	dlcon dl;
	evabaseFreeFrunner(const IDlConFactory &pDlconFac);
	~evabaseFreeFrunner();
private:
	std::thread evthr, thr;
};

}


#endif /* INCLUDE_EBRUNNER_H_ */
