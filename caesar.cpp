#include "caesar.h"
#include <cstring>

// Ключ хранится внутри библиотеки
static char g_key = 0;

// Реализация set_key
extern "C" void set_key(char key) {
    g_key = key;
}

// Реализация caesar (XOR шифрование)
extern "C" void caesar(void* src, void* dst, int len) {
    if (len <= 0 || src == nullptr || dst == nullptr) {
        return;
    }

    unsigned char* s = static_cast<unsigned char*>(src);
    unsigned char* d = static_cast<unsigned char*>(dst);

    // XOR операция
    for (int i = 0; i < len; i++) {
        d[i] = s[i] ^ g_key;
    }
}