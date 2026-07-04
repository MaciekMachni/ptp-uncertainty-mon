#ifndef PTP_UNC_BYTEORDER_H_
#define PTP_UNC_BYTEORDER_H_

#include <stdint.h>

#if defined(__linux__)
#include <endian.h>
#elif defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#define be16toh(x) OSSwapBigToHostInt16(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define htobe16(x) OSSwapHostToBigInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#else
#error "ptp-uncertainty requires Linux or macOS for building"
#endif

#endif /* PTP_UNC_BYTEORDER_H_ */
