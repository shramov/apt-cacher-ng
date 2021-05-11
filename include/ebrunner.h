/*
 * ebrunner.h
 *
 *  Created on: 31 янв. 2021 г.
 *      Author: ed
 */

#ifndef INCLUDE_EBRUNNER_H_
#define INCLUDE_EBRUNNER_H_

#include "config.h"

namespace acng
{
class dlcon;
class IDlConFactory;

// tool helper class for acngtool and httpfs, runs event and download threads
class ACNG_API evabaseFreeFrunner
{
	class Impl;
public:
	evabaseFreeFrunner(const IDlConFactory &pDlconFac, bool withDownloader);
	~evabaseFreeFrunner();
	dlcon& getDownloader();
private:
	Impl *m_pImpl;
};

}


#endif /* INCLUDE_EBRUNNER_H_ */
