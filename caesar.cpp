#include "caesar.h"

static char g_key = 0;

extern "C" void set_key(char key)
{
    g_key = key;
}

extern "C" void caesar(void* src, void* dst, int len)
{
    if (!src || !dst || len <= 0)
        return;

    unsigned char* s = (unsigned char*)src;
    unsigned char* d = (unsigned char*)dst;

    for (int i = 0; i < len; i++)
    {
        d[i] = s[i] ^ g_key;
    }
}