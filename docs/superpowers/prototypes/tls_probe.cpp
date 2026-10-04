// 单独的 TU：协程那边没法把这些折叠掉
#include <pthread.h>
#include <cstdio>
__thread int slot = 0;
void   tls_set(int v)       { slot = v; }
int    tls_get()            { return slot; }
void*  tls_addr()           { return &slot; }
unsigned long which_thread() { return (unsigned long)pthread_self(); }
