/*
 * sut.h
 *
 *  Created on: 12.01.2020
 *      Author: ed
 */

#ifndef INCLUDE_SUT_H_
#define INCLUDE_SUT_H_

#ifdef UNDER_TEST
#define SUTPROTECTED public
#define SUTPRIVATE public
// this is evil - when mixed between lib and app, it can apparently cause a call of a wrong method
// XXX: disabling for now, maybe later align all compiler switches so that it never mismatched
//#define SUTVIRTUAL virtual
#else
#define SUTPROTECTED protected
#define SUTPRIVATE private
//#define SUTVIRTUAL
#endif

#endif /* INCLUDE_SUT_H_ */
