#include "dumper.h"

#ifdef DEBUG

#include <iostream>
using namespace std;

namespace acng
{

string Identer(PATH_MAX, '\t');

ACNG_API Dumper::Dumper()
{
}

Dumper ACNG_API &Dumper::operator<<(tSS &msg)
{
	cerr << string_view(Identer.data(), iLevel) << msg.view() << endl;
	return *this;
}

void ACNG_API Dumper::DumpFurther(Dumpable &next)
{
	++iLevel;
	next.DumpInfo(*this);
	--iLevel;
}

}

#endif
