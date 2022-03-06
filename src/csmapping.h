#ifndef CSMAPPING_H_
#define CSMAPPING_H_

#include "actypes.h"
#include "filereader.h"

#include <map>
#include <cstdint>

// XXX: allocate this dynamically?
#define MAXCSLEN 64

namespace acng
{

// from astrop.h
off_t atoofft(string_view s, off_t nDefVal = 0);

template<bool StringSeparator, bool Strict>
class tSplitWalkBase;

typedef enum : char
{
	CSTYPE_UNSET = -1, // only a placeholder for special purposes
	CSTYPE_INVALID = 0,
	CSTYPE_MD5=1,
	CSTYPE_SHA1=2,
	CSTYPE_SHA256=3,
	CSTYPE_SHA512=4
} CSTYPES;

inline CSTYPES GuessCStype(unsigned short len)
{
	switch(len)
	{
	case 16: return CSTYPE_MD5;
	case 20: return CSTYPE_SHA1;
	case 32: return CSTYPE_SHA256;
	case 64: return CSTYPE_SHA512;
	default: return CSTYPE_INVALID;
	}
}
inline constexpr unsigned short GetCSTypeLen(CSTYPES t)
{
	switch(t)
	{
	case CSTYPE_MD5: return 16;
	case CSTYPE_SHA1: return 20;
	case CSTYPE_SHA256: return 32;
	case CSTYPE_SHA512: return 64;
	default: return 0;
	}
}
inline LPCSTR GetCsName(CSTYPES csType)
{
	switch(csType)
	{
	case CSTYPE_MD5: return "Md5";
	case CSTYPE_SHA1: return "Sha1";
	case CSTYPE_SHA256: return "Sha256";
	case CSTYPE_SHA512: return "Sha512";
	default: return "Other";
	}
}
inline LPCSTR GetCsNameReleaseStyle(CSTYPES csType)
{
	switch(csType)
	{
	case CSTYPE_MD5: return "MD5Sum";
	case CSTYPE_SHA1: return "SHA1";
	case CSTYPE_SHA256: return "SHA256";
	case CSTYPE_SHA512: return "SHA512";
	default: return "Other";
	}
}

class csumBase
{
public:
	virtual ~csumBase() {};
	virtual void add(const char *data, std::size_t size) = 0;
	virtual void add(const string_view view) { return add(view.data(), view.size()); }
	/**
	 * @brief finish
	 * @param ret Output blob
	 * @param bufLen Size of the output
	 * @return Length of the copied result blob, -1 if too small
	 */
	virtual int finish(uint8_t* ret, size_t bufLen) = 0;
	static std::unique_ptr<csumBase> GetChecker(CSTYPES);
};

// kind of file identity, compares by file size and checksum (MD5 or SHA*)
class ACNG_API tFingerprint
{
	SUTPROTECTED:
	off_t size = -1;
	CSTYPES csType = CSTYPE_INVALID;
	uint8_t csum[MAXCSLEN];
public:
	tFingerprint() =default;
	tFingerprint(const tFingerprint &a) : size(a.size),
	csType(a.csType)
	{
		memcpy(csum, a.csum, sizeof(csum));
	};
	tFingerprint & operator=(const tFingerprint& a)
	{
		if(this == &a) return *this;
		size = a.size;
		csType = a.csType;
		memcpy(csum, a.csum, sizeof(csum));
		return *this;
	}
	
	bool SetCs(string_view hexString, CSTYPES eCstype = CSTYPE_INVALID);
	void Set(uint8_t *pData, CSTYPES eCstype, off_t newsize)
	{
		size = newsize;
		csType = eCstype;
		if(pData)
			memcpy(csum, pData, GetCSTypeLen(eCstype));
	}

	inline bool Set(string_view hexString, CSTYPES eCstype, off_t newsize)
	{
		if (!SetCs(hexString, eCstype))
			return false;
		size = newsize;
		return true;
	}
	inline void SetSize(string_view sz) { size = atoofft(sz, -1); }
	inline void SetSize(off_t sz) { size = sz; }
	inline off_t GetSize() const { return size; }
	CSTYPES GetCsType() const { return csType; }
	inline bool IsValid() const { return size >= 0 && csType > CSTYPE_INVALID; }

	/**
	 * Extracts just first two tokens from splitter, first considered checksum, second the size.
	 * @return false if data is crap or wantedType was set but does not fit what's in the first token.
	 */
    bool Set(tSplitWalkBase<false, false>& splitInput, CSTYPES wantedType = CSTYPE_INVALID);
	// c'mon C++, I need to pass lvalues when I explicitly want it
    inline bool Set(tSplitWalkBase<false, false>&& splitInput, CSTYPES wantedType = CSTYPE_INVALID)
	{
		return Set(splitInput, wantedType);
	}

	bool ScanFile(const mstring & path, const CSTYPES eCstype, bool bUnpack = false, FILE *fDump = nullptr)
	{
		if(! GetCSTypeLen(eCstype))
			return false; // unsupported
		csType=eCstype;
		return filereader::GetChecksum(path, eCstype, csum, bUnpack, size, fDump, MAXCSLEN);
	}
	void Invalidate()
	{
		csType = CSTYPE_INVALID;
		size = -1;
	}
	mstring GetCsAsString() const;
	operator mstring() const;
	inline bool csEquals(const tFingerprint& other) const
	{
		return 0==memcmp(csum, other.csum, GetCSTypeLen(csType));
	}
	inline bool operator==(const tFingerprint & other) const
	{
		if(other.csType!=csType || size!=other.size)
			return false;
		return csEquals(other);
	}
	inline bool operator!=(const tFingerprint & other) const
	{
		return !(other == *this);
	}
	bool CheckFile(cmstring & sFile) const;
	bool operator<(const tFingerprint & other) const
	{
		if(other.csType!=csType)
			return csType<other.csType;
		if(size!=other.size)
			return size<other.size;
		return memcmp(csum, other.csum, GetCSTypeLen(csType)) < 0;
	}
};

struct ACNG_API tRemoteFileInfo
{
	tFingerprint fpr;
	mstring path;

	bool IsUsable()
	{
		return !path.empty() && fpr.GetCsType() != CSTYPE_INVALID && fpr.GetSize() > 0;
	}
	bool SetSize(string_view szSize) { return fpr.SetSize(szSize), fpr.GetSize() >= 0; }
};


/** For IMPORT:
 * Helper class to store attributes for different purposes. 
 * They need to be stored somewhere, either in fingerprint or
 * another struct including fingerprint, or just here :-(.
 */
struct tImpFileInfo
{
    mstring sPath;
    
    time_t mtime = 0;
    bool bFileUsed = false;
    
    inline tImpFileInfo(const mstring & s, time_t m) :
        sPath(s), mtime(m) {};
    tImpFileInfo() =default;
};
struct ltCacheKeyComp
{
  inline bool operator()(const tImpFileInfo &a, const tImpFileInfo &b) const
  {
	  return (a.mtime!=b.mtime) ? (a.mtime<b.mtime) :
#if defined(WINDOWS) || defined(WIN32)
			  (strcasecmp(a.sPath.c_str(), b.sPath.c_str()) < 0);
#else
			  (a.sPath<b.sPath);
#endif
  }
};

typedef std::map<tImpFileInfo, tFingerprint, ltCacheKeyComp> tFprCacheMap;

}

#endif /*CSMAPPING_H_*/
