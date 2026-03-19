#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#include "caesar.h"

#define BUFFER_SIZE 4096
#define THREAD_COUNT 3

char** files;
int file_count;
int current_index = 0;

int copied_count = 0;

char* output_dir;

pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;


void log_operation(const char* filename, const char* status, double time_spent)
{
    FILE* log = fopen("log.txt", "a");
    if (!log) return;

    time_t now = time(NULL);
    char time_str[64];

    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(log, "[%s] Thread %llu: %s - %s (%.2f sec)\n",
            time_str,
            (unsigned long long)pthread_self(),
            filename,
            status,
            time_spent);


    fclose(log);
}


void process_file(const char* input_name)
{
    FILE* in = fopen(input_name, "rb");
    if (!in)
    {
        pthread_mutex_lock(&global_mutex);
        log_operation(input_name, "ERROR_OPEN_INPUT", 0);
        pthread_mutex_unlock(&global_mutex);
        return;
    }

    char output_path[512];
    snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, input_name);

    FILE* out = fopen(output_path, "wb");
    if (!out)
    {
        fclose(in);

        pthread_mutex_lock(&global_mutex);
        log_operation(input_name, "ERROR_OPEN_OUTPUT", 0);
        pthread_mutex_unlock(&global_mutex);
        return;
    }

    unsigned char buf[BUFFER_SIZE];
    unsigned char enc[BUFFER_SIZE];

    size_t bytes;
    clock_t start = clock();

    while ((bytes = fread(buf, 1, BUFFER_SIZE, in)) > 0)
    {
        caesar(buf, enc, bytes);
        fwrite(enc, 1, bytes, out);
    }

    fclose(in);
    fclose(out);

    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;

    pthread_mutex_lock(&global_mutex);
    log_operation(input_name, "SUCCESS", time_spent);
    copied_count++;
    pthread_mutex_unlock(&global_mutex);

    printf("Файл обработан: %s (%.2f сек)\n", input_name, time_spent);
}


void* worker_thread(void* arg)
{
    while (1)
    {
        int index;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;

        int res = pthread_mutex_timedlock(&global_mutex, &ts);

        if (res == ETIMEDOUT)
        {
            printf("⚠ Возможная взаимоблокировка: поток ждет >5 сек\n");
            continue;
        }

        if (current_index >= file_count)
        {
            pthread_mutex_unlock(&global_mutex);
            break;
        }

        index = current_index++;
        pthread_mutex_unlock(&global_mutex);

        process_file(files[index]);
    }

    return NULL;
}


int main(int argc, char* argv[])
{

    if (argc < 4)
    {
        printf("Usage: %s file1 file2 ... output_dir key\n", argv[0]);
        return 1;
    }

    file_count = argc - 3;
    files = &argv[1];

    output_dir = argv[argc - 2];
    int key = atoi(argv[argc - 1]);

    set_key((char)key);

    #ifdef _WIN32
        mkdir(output_dir);
    #else
        mkdir(output_dir, 0777);
    #endif

    pthread_t threads[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_create(&threads[i], NULL, worker_thread, NULL);

    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_join(threads[i], NULL);

    printf("Всего обработано файлов: %d\n", copied_count);

    return 0;
}