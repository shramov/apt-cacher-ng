#include "csmapping.h"
#include "meta.h"

bool acng::tFingerprint::SetCs(string_view hexString, acng::CSTYPES eCstype)
{
	auto l = hexString.size();
	if(!l || l%2) // weird sizes...
		return false;
	if(eCstype == CSTYPE_INVALID)
	{
		eCstype = GuessCStype(hexString.size() / 2);
		if(eCstype == CSTYPE_INVALID)
			return false;
	}
	else if(l != 2 * GetCSTypeLen(eCstype))
		return false;

	csType=eCstype;
	return CsAsciiToBin(hexString.data(), csum, l/2);
}

bool acng::tFingerprint::Set(acng::tSplitWalk& splitInput, acng::CSTYPES wantedType)
{
	if(!splitInput.Next())
		return false;
	if(!SetCs(splitInput.view(), wantedType))
		return false;
	if(!splitInput.Next())
		return false;
	size = atoofft(splitInput.view(), -1);
	if(size < 0)
		return false;
	return true;
}

acng::mstring acng::tFingerprint::GetCsAsString() const
{
	return BytesToHexString(csum, GetCSTypeLen(csType));
}

acng::tFingerprint::operator mstring() const
{
	return GetCsAsString() + "_" + offttos(size);
}

bool acng::tFingerprint::CheckFile(acng::cmstring &sFile) const
{
	if(size != GetFileSize(sFile, -2))
		return false;
	tFingerprint probe;
	if(!probe.ScanFile(sFile, csType, false, nullptr))
		return false;
	return probe == *this;
}
