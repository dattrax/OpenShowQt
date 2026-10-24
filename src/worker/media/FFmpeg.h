/*
 * The single include point for FFmpeg's C headers (task 1.5).
 *
 * FFmpeg's headers carry no extern "C" guards (recorded in PLAN.md task 0.5),
 * so every C++ translation unit that wants them must wrap them. Rather than
 * repeat the wrapper, code includes this header, and only media-module TUs
 * include it: the module's public types (MediaProbe, later the frame ring)
 * speak plain C++ so FFmpeg stays an implementation detail of the .cpp files
 * and the Qt-free worker keeps linking exactly one set of libav symbols.
 */

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
