#ifndef DUMPER_H
#define DUMPER_H

#include "acbuf.h"
#include "astrop.h"

namespace acng
{
class Dumper;

class Dumpable
{
public:
	virtual void DumpInfo(Dumper& dumper) { (void) dumper; };
};

#ifdef DEBUG
/**
 * @brief Visitor object, holding state of the debug printing
 */
class Dumper
{
	unsigned iLevel = 0;
public:
	Dumper();
	void Start(Dumpable& subject);

	Dumper& operator<<(tSS&);
	/**
	 * @brief DumpFurther Descend one level and continue with this object there
	 */
	void DumpFurther(Dumpable &child);
	void DumpFurther(Dumpable *child) { return DumpFurther(*child); }

	void SendTempFmt() { *this << m_temp; m_temp.clear(); }
	tSS& GetTempFmt() { return m_temp; }
private:
	tSS m_temp;

};

#define DUMPFMT tFmtSendTempRaii<Dumper,tSS>(dumper).GetFmter()
#else
class Dumper
{
};
#endif

}
#endif // DUMPER_H
