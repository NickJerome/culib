#pragma once

// 套接字用到的系统头文件，平台差异只在这里判断一次。

#if defined(_WIN32)
#  include "platform/win32.h"
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mswsock.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#endif
