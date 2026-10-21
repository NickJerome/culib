#pragma once

// 库里的 Windows 源文件都从这里包含 windows.h：不带 winsock 1，也不定义 min / max 宏。

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

// Windows 10 1803 起才有，旧一些的 SDK 和 MinGW 头文件里没有。
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
