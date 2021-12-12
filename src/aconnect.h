#ifndef ACONNECT_H
#define ACONNECT_H

#include "fileio.h"

namespace acng
{
/**
 * @brief Short living object used to establish TCP connection to a target
 *
 * The object is self-destructing after execution.
 */
struct ACNG_API aconnector
{
	struct ACNG_API tConnResult
	{
		unique_fd fd;
		std::string sError;
		uint_fast16_t flags;
	};

	// file descriptor, error message, forcedSsl flag
	using tCallback = std::function<void (tConnResult)>;

    /**
         * @brief Start connection asynchronously and report result via callback
         * @param target Connection target
         * @param port
         * @param cbReport
         *
         * Thread context: ST, IO thread
         */
	static TFinalAction Connect(cmstring& target, uint16_t port, tCallback cbReport, int timeout = -1);
    /**
         * @brief Connect to a target in synchronous fashion
         * @param target
         * @param port
         * @return Connected file socket OR non-empty error string
         *
         * Thread context: not IO thread, reentrant, blocking!
         */
	static tConnResult Connect(cmstring& target, uint16_t port, int timeout = -1);
};
}
#endif // ACONNECT_H
