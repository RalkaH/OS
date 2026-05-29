#include "rc4.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>

// Мастер-ключ в mmap-странице (mprotect работает только с целыми страницами)
struct MasterKey {
    char key[256];
    int len;
};

static MasterKey* g_master = nullptr;
static size_t g_master_page = 0;

// Мьютекс для ВСЕХ операций mprotect с g_master:
// - чтение в init_file_crypto()
// - запись в set_key()
// Без мьютекса один поток может закрыть PROT_NONE, пока другой читает → SIGSEGV
// Для S-box мьютекс НЕ нужен — он thread-local (__thread), каждый поток
// работает со своей страницей, конфликта mprotect между потоками нет.
static pthread_mutex_t key_mutex = PTHREAD_MUTEX_INITIALIZER;

// S-box + индексы RC4 — thread-local, у каждого потока свой экземпляр.
// Защита: mprotect(PROT_NONE) вне процедуры шифрования — требование ТЗ:
// "Внутреннее состояние шифра должно быть недоступно вне процедуры шифрования"
struct RC4State {
    unsigned char sbox[256];
    int i;
    int j;
};

static __thread RC4State* tls_rc4 = nullptr;
static __thread void* tls_rc4_page = nullptr;
static __thread size_t tls_rc4_page_size = 0;

// Обработчик SIGSEGV — ловит попытку чтения мастер-ключа пока PROT_NONE
// Используем только write() — async-signal-safe (fprintf НЕ safe → дедлок)
static void segv_handler(int sig, siginfo_t *si, void *unused)
{
    const char sec_msg[] = "\n[SECURITY] Unauthorized access to master key\n";
    const char seg_msg[] = "\nSegmentation fault\n";

    if (g_master && (char*)si->si_addr >= (char*)g_master &&
        (char*)si->si_addr < (char*)g_master + g_master_page)
    {
        (void)write(STDERR_FILENO, sec_msg, sizeof(sec_msg) - 1);
        _exit(139);
    }
    (void)write(STDERR_FILENO, seg_msg, sizeof(seg_msg) - 1);
    _exit(139);
}

// Вспомогательная: mprotect с проверкой возврата
static int mprotect_checked(void* addr, size_t len, int prot, const char* ctx)
{
    if (mprotect(addr, len, prot) != 0)
    {
        fprintf(stderr, "Ошибка mprotect(%s): не удалось установить права 0x%x на %p\n",
                ctx, prot, addr);
        return -1;
    }
    return 0;
}

// Сохранить мастер-ключ в mmap-страницу, закрыть PROT_NONE
extern "C" void set_key(const char* key)
{
    if (!g_master)
    {
        g_master_page = sysconf(_SC_PAGESIZE);
        g_master = (MasterKey*)mmap(NULL, g_master_page,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_master == MAP_FAILED)
        {
            fprintf(stderr, "Ошибка: mmap для мастер-ключа не удался\n");
            g_master = nullptr;
            exit(1);
        }
        if (mlock(g_master, g_master_page) != 0)
            fprintf(stderr, "Предупреждение: mlock мастер-ключа не удался (ключ может попасть в swap)\n");

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_flags = SA_SIGINFO;
        sa.sa_sigaction = segv_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
    }

    // Мьютекс: mprotect(PROT_READ|PROT_WRITE) влияет на все потоки.
    // Без мьютекса другой поток в init_file_crypto() может читать ключ,
    // а мы закроем PROT_NONE → SIGSEGV в том потоке.
    pthread_mutex_lock(&key_mutex);
    if (mprotect_checked(g_master, g_master_page, PROT_READ | PROT_WRITE, "set_key write") != 0)
    {
        pthread_mutex_unlock(&key_mutex);
        return;
    }

    strncpy(g_master->key, key, 255);
    g_master->key[255] = '\0';
    g_master->len = strlen(g_master->key);

    mprotect_checked(g_master, g_master_page, PROT_NONE, "set_key lock");
    pthread_mutex_unlock(&key_mutex);
}

// Инициализация RC4 для файла: full_key = пароль||соль, KSA
// После вызова страница S-box закрыта (PROT_NONE) —
// rc4_crypt() откроет её, выполнит PRGA и закроет обратно.
// Возвращает 0 при успехе, -1 при ошибке (mmap/mprotect не удался).
//
// ПОТОК: init_file_crypto() → rc4_crypt()×N → end_file_crypto()
// Каждое звено проверяет состояние. Если init не вызывался или уже был end —
// rc4_crypt вернёт -1, и вызывающий код обнаружит ошибку.
extern "C" int init_file_crypto(const unsigned char* salt)
{
    if (!g_master) return -1;

    // Выделить thread-local страницу под S-box (один раз за время работы потока)
    if (!tls_rc4_page)
    {
        tls_rc4_page_size = sysconf(_SC_PAGESIZE);
        tls_rc4_page = mmap(NULL, tls_rc4_page_size,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (tls_rc4_page == MAP_FAILED)
        {
            fprintf(stderr, "Ошибка: mmap для S-box не удался\n");
            tls_rc4_page = nullptr;
            tls_rc4_page_size = 0;
            return -1;
        }
        if (mlock(tls_rc4_page, tls_rc4_page_size) != 0)
            fprintf(stderr, "Предупреждение: mlock S-box не удался (состояние шифра может попасть в swap)\n");
        tls_rc4 = (RC4State*)tls_rc4_page;
    }
    else
    {
        // Страница существует — открываем для перезаписи (новый KSA)
        if (mprotect_checked(tls_rc4_page, tls_rc4_page_size, PROT_READ | PROT_WRITE,
                             "init_file_crypto reopen sbox") != 0)
            return -1;
    }

    // Чтение g_master под мьютексом: mprotect(PROT_NONE) одним потоком
    // убьёт доступ для другого, который ещё читает → SIGSEGV
    pthread_mutex_lock(&key_mutex);
    if (mprotect_checked(g_master, g_master_page, PROT_READ, "init_file_crypto read key") != 0)
    {
        pthread_mutex_unlock(&key_mutex);
        return -1;
    }

    // Проверка переполнения: g_master->len <= 255 (ограничено в set_key),
    // поэтому g_master->len + 16 <= 271 < 300. Но проверяем явно.
    if (g_master->len + 16 > (int)sizeof(unsigned char[300]))
    {
        fprintf(stderr, "Ошибка: ключ + соль превышают буфер\n");
        mprotect_checked(g_master, g_master_page, PROT_NONE, "init_file_crypto lock key on overflow");
        pthread_mutex_unlock(&key_mutex);
        return -1;
    }

    unsigned char full_key[300];
    int klen = g_master->len + 16;
    memcpy(full_key, g_master->key, g_master->len);
    memcpy(full_key + g_master->len, salt, 16);

    mprotect_checked(g_master, g_master_page, PROT_NONE, "init_file_crypto lock key");
    pthread_mutex_unlock(&key_mutex);

    // KSA: заполняем [0..255], перемешиваем от full_key
    for (int i = 0; i < 256; i++) tls_rc4->sbox[i] = i;
    int j = 0;
    for (int i = 0; i < 256; i++)
    {
        j = (j + tls_rc4->sbox[i] + full_key[i % klen]) % 256;
        unsigned char t = tls_rc4->sbox[i];
        tls_rc4->sbox[i] = tls_rc4->sbox[j];
        tls_rc4->sbox[j] = t;
    }
    tls_rc4->i = 0;
    tls_rc4->j = 0;

    // Затираем full_key на стеке — explicit_bzero НЕ может быть оптимизирован компилятором
    explicit_bzero(full_key, sizeof(full_key));
    klen = 0;

    // Закрываем S-box — недоступен вне процедуры шифрования
    if (mprotect_checked(tls_rc4_page, tls_rc4_page_size, PROT_NONE,
                         "init_file_crypto lock sbox") != 0)
    {
        // Критическая ошибка: S-box остался доступен — затираем и уничтожаем
        explicit_bzero(tls_rc4, sizeof(RC4State));
        tls_rc4 = nullptr;  // чтобы rc4_crypt вернул -1, а не шифровал мусором
        return -1;
    }

    return 0;
}

// Занулить S-box и освободить mmap-страницу
// Вызывается ОДИН РАЗ после обработки всего файла
extern "C" void end_file_crypto()
{
    if (!tls_rc4_page) return;
    mprotect_checked(tls_rc4_page, tls_rc4_page_size, PROT_READ | PROT_WRITE, "end_file_crypto");
    explicit_bzero(tls_rc4_page, tls_rc4_page_size);
    munlock(tls_rc4_page, tls_rc4_page_size);
    munmap(tls_rc4_page, tls_rc4_page_size);
    tls_rc4 = nullptr;
    tls_rc4_page = nullptr;
    tls_rc4_page_size = 0;
}

// Занулить мастер-ключ и освободить mmap-страницу
extern "C" void cleanup_key()
{
    if (!g_master) return;
    mprotect_checked(g_master, g_master_page, PROT_READ | PROT_WRITE, "cleanup_key");
    explicit_bzero(g_master, g_master_page);
    munlock(g_master, g_master_page);
    munmap(g_master, g_master_page);
    g_master = nullptr;
}

// PRGA: шифрование/расшифрование — XOR с гаммой
// Открывает S-box (mprotect PROT_READ|PROT_WRITE), выполняет PRGA,
// закрывает S-box (mprotect PROT_NONE) — состояние недоступно вне процедуры
// Возвращает 0 при успехе, -1 при ошибке (S-box не инициализирован / mprotect упал)
extern "C" int rc4_crypt(void* src, void* dst, int len)
{
    if (!src || !dst || len <= 0) return -1;
    if (!tls_rc4 || !tls_rc4_page) return -1;   // S-box не инициализирован или уже уничтожен

    // Открываем страницу S-box
    if (mprotect_checked(tls_rc4_page, tls_rc4_page_size, PROT_READ | PROT_WRITE,
                         "rc4_crypt open") != 0)
        return -1;

    unsigned char* s = (unsigned char*)src;
    unsigned char* d = (unsigned char*)dst;
    int i = tls_rc4->i;
    int j = tls_rc4->j;

    for (int k = 0; k < len; k++)
    {
        i = (i + 1) % 256;
        j = (j + tls_rc4->sbox[i]) % 256;
        unsigned char t = tls_rc4->sbox[i];
        tls_rc4->sbox[i] = tls_rc4->sbox[j];
        tls_rc4->sbox[j] = t;
        d[k] = s[k] ^ tls_rc4->sbox[(tls_rc4->sbox[i] + tls_rc4->sbox[j]) % 256];
    }

    tls_rc4->i = i;
    tls_rc4->j = j;

    // Затираем локальные копии индексов — explicit_bzero надёжнее volatile
    explicit_bzero(&i, sizeof(i));
    explicit_bzero(&j, sizeof(j));

    // Закрываем страницу S-box — недоступна вне процедуры шифрования
    if (mprotect_checked(tls_rc4_page, tls_rc4_page_size, PROT_NONE,
                         "rc4_crypt lock") != 0)
    {
        // mprotect не смог закрыть страницу — критическая ошибка безопасности
        // Затираем S-box, помечаем как уничтоженный (tls_rc4=nullptr),
        // чтобы последующие вызовы rc4_crypt вернули -1,
        // а не пытались шифровать занулённым S-box
        explicit_bzero(tls_rc4, sizeof(RC4State));
        tls_rc4 = nullptr;
        return -1;
    }

    return 0;
}
