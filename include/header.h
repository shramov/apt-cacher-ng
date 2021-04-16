#ifndef _HEADER_H
#define _HEADER_H

#include <unordered_set>
#include <vector>
#include "acbuf.h"

namespace acng
{

/**
 * @brief ParseHeadFromStorage Reads cached items with basic validation
 * @param path Source file to read
 * @param contLen [OUT, OPTIONAL] Resulting content lenth
 * @param lastModified [OUT, OPTIONAL] Last modified date as timestamp
 * @param origSrc [OUT, OPTIONAL] Resulting content lenth
 * @return True if head appears valid (200 code answer)
 */
bool ParseHeadFromStorage(cmstring& path, off_t* contLen, time_t* lastModified, mstring* origSrc);

class ACNG_API header {
   public:
      enum eHeadType : char
	  {
         INVALID,
         HEAD,
         GET,
         POST,
         CONNECT,
         ANSWER
      };
      enum eHeadPos : char
	  {
    	  CONNECTION,			// 0
    	  CONTENT_LENGTH,
    	  IF_MODIFIED_SINCE,
    	  RANGE,
    	  IFRANGE,				// 4

    	  CONTENT_RANGE,
    	  LAST_MODIFIED,
    	  PROXY_CONNECTION,
    	  TRANSFER_ENCODING,
    	  XORIG,

    	  AUTHORIZATION,		// 10          
    	  XFORWARDEDFOR,
    	  LOCATION,
    	  CONTENT_TYPE,
    	  // unreachable entry and size reference
          HEADPOS_MAX,
          // special value, only a flag to remember that data is stored to external header
          HEADPOS_UNK_EXPORT
      };
#define ACNGFSMARK XORIG

      eHeadType type = INVALID;
      mstring frontLine;
      
      char *h[HEADPOS_MAX] = {0};

      inline header(){};
      ~header();
      header(const header &);
      header(header &&);
      header& operator=(const header&); 
      header& operator=(header&&);

      int LoadFromFile(const mstring & sPath);
      
      //! returns byte count or negative errno value
      int StoreToFile(cmstring &sPath) const;
      
      void set(eHeadPos, const mstring &value);
      void set(eHeadPos, const char *val);
      void set(eHeadPos, const char *s, size_t len);
      void set(eHeadPos, off_t nValue);
      void prep(eHeadPos, size_t length);
      void del(eHeadPos);
      inline void copy(const header &src, eHeadPos pos) { set(pos, src.h[pos]); };

      inline const char * getCodeMessage() const {
    	  return frontLine.length()>9 ? frontLine.c_str()+9 : "";
      }
      inline int getStatus() const { int r=atoi(getCodeMessage()); return r ? r : 500; }

      std::string getMessage() const;
      void clear();
      
      tSS ToString() const;

      /**
       * Read buffer to parse one string. Optional offset where to begin to
       * scan.
       *
       * @param src Pointer to raw input
       * @param length Maximum considered input length
       * @param unkHeaderMap Optional, series of string_view pairs containing key and values. If key is empty, record's value is a continuation of the previous value.
       * @return Length of processed data, 0: incomplete, needs more data, <0: error, >0: length of the processed data
       */
      int Load(string_view sv, std::vector<std::pair<string_view,string_view> > *unkHeaderMap = nullptr);

private:
      eHeadPos resolvePos(string_view key);
};

mstring ExtractCustomHeaders(string_view reqHead, bool isPassThrough);
}

#endif
