#ifndef _mpeg_ps_h_
#define _mpeg_ps_h_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#ifndef OS_INT64_TYPE
#if defined(_WIN32) || defined(_WIN64)
	typedef __int64				int64_t;	
	typedef unsigned __int64	uint64_t;
#else
	#include <stdint.h>
	typedef long long			int64_t;
	typedef unsigned long long	uint64_t;
#endif
#define OS_INT64_TYPE
#endif /* OS_INT64_TYPE */

enum
{
	STREAM_VIDEO_MPEG4	= 0x10,
	STREAM_VIDEO_H264	= 0x1b,
	STREAM_VIDEO_SVAC	= 0x80,
	STREAM_AUDIO_AAC	= 0x0f,
	STREAM_AUDIO_G711	= 0x90,
	STREAM_AUDIO_G722	= 0x92,
	STREAM_AUDIO_G723	= 0x93,
	STREAM_AUDIO_G729	= 0x99,
	STREAM_AUDIO_SVAC	= 0x9B,
};

struct mpeg_ps_func_t
{
	/// alloc new packet
	/// @param[in] param use-defined parameter(by mpeg_ps_create)
	/// @param[in] bytes alloc memory size in byte
	/// @return memory pointer
	void* (*alloc)(void* param, size_t bytes);

	/// free packet
	/// @param[in] param use-defined parameter(by mpeg_ps_create)
	/// @param[in] packet PS packet pointer(alloc return pointer)
	void (*free)(void* param, void* packet);

	/// callback on PS packet done
	/// @param[in] param use-defined parameter(by mpeg_ps_create)
	/// @param[in] packet PS packet pointer(alloc return pointer)
	/// @param[in] bytes packet size
	void (*write)(void* param, void* packet, size_t bytes);
};

void* mpeg_ps_create(const struct mpeg_ps_func_t *func, void* param);
int mpeg_ps_destroy(void* ps);
int mpeg_ps_add_stream(void* ps, int streamType, const void* info, size_t bytes);
int mpeg_ps_reset(void* ps);

/// input ES
/// @param[in] ps MPEG-2 Program Stream packer(mpeg_ps_create)
/// @param[in] streamType such as: STREAM_VIDEO_H264/STREAM_AUDIO_AAC
/// @param[in] pts presentation time stamp(in 90KHZ)
/// @param[in] dts decoding time stamp(in 90KHZ)
/// @param[in] data ES memory
/// @param[in] bytes ES length in byte
/// @return 0-ok, ENOMEM-alloc failed, <0-error
int mpeg_ps_write(void* ps, int streamType, int64_t pts, int64_t dts, const void* data, size_t bytes);

size_t mpeg_ps_packet_dec(const unsigned char* data, size_t bytes, const struct mpeg_ps_func_t *func, void* param);

#ifdef __cplusplus
}
#endif
#endif /* !_mpeg_ps_h_ */
