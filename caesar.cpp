#include "caesar.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static char* g_key = nullptr;
static size_t g_page_size = 0;

static void segv_handler(int sig, siginfo_t *si, void *unused)
{
    if (g_key && (char*)si->si_addr >= g_key && (char*)si->si_addr < g_key + g_page_size)
    {
        fprintf(stderr, "\n[ОШИБКА БЕЗОПАСНОСТИ] Попытка несанкционированного доступа к памяти ключа!\n");
        exit(139);
    }
    
    fprintf(stderr, "\nSegmentation fault (standard)\n");
    exit(139);
}

extern "C" void set_key(char key)
{
    if (!g_key)
    {
        g_page_size = sysconf(_SC_PAGESIZE);
        g_key = (char*)mmap(NULL, g_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_flags = SA_SIGINFO;
        sa.sa_sigaction = segv_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
    }
    else
    {
        mprotect(g_key, g_page_size, PROT_READ | PROT_WRITE);
    }

    memcpy(g_key, &key, 1);

    mprotect(g_key, g_page_size, PROT_NONE);
}

extern "C" void cleanup_key()
{
    if (g_key)
    {
        mprotect(g_key, g_page_size, PROT_READ | PROT_WRITE);
        memset(g_key, 0, g_page_size);
        mprotect(g_key, g_page_size, PROT_NONE);
        munmap(g_key, g_page_size);
        g_key = nullptr;
    }
}

extern "C" void caesar(void* src, void* dst, int len)
{
    if (!src || !dst || len <= 0 || !g_key)
        return;

    mprotect(g_key, g_page_size, PROT_READ);

    unsigned char* s = (unsigned char*)src;
    unsigned char* d = (unsigned char*)dst;

    for (int i = 0; i < len; i++)
    {
        d[i] = s[i] ^ g_key[0];
    }

    mprotect(g_key, g_page_size, PROT_NONE);
}

extern "C" void test_security_violation()
{
    if (g_key)
    {
        g_key[0] = 'X'; 
    }
}