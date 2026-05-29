#ifndef RC4_H
#define RC4_H

#ifdef __cplusplus
extern "C" {
#endif

void set_key(const char* key);
int  init_file_crypto(const unsigned char* salt);  /* 0 = успех, -1 = ошибка */
void end_file_crypto();
void cleanup_key();
int  rc4_crypt(void* src, void* dst, int len);     /* 0 = успех, -1 = ошибка */

#ifdef __cplusplus
}
#endif

#endif
