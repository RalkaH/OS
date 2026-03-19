#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
    #include <direct.h>
    #define MKDIR(path) _mkdir(path)
#else
    #include <sys/types.h>
    #define MKDIR(path) mkdir(path, 0755)
#endif

#include "caesar.h"

#define BUFFER_SIZE 4096
#define THREAD_COUNT 3

char** files;
int file_count;
int current_index = 0;
int copied_count = 0;
char* output_dir;

pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct
{
    int thread_num;
} thread_arg_t;

void create_directory(const char* path)
{
    if (!path || path[0] == '\0')
        return;

    char temp[512];
    snprintf(temp, sizeof(temp), "%s", path);

    size_t len = strlen(temp);
    if (len == 0)
        return;

    if (len > 1 && (temp[len - 1] == '/' || temp[len - 1] == '\\'))
        temp[len - 1] = '\0';

    for (char* p = temp + 1; *p; p++)
    {
        if (*p == '/' || *p == '\\')
        {
            char old = *p;
            *p = '\0';

            if (MKDIR(temp) != 0 && errno != EEXIST)
                perror("Failed to create directory");

            *p = old;
        }
    }

    if (MKDIR(temp) != 0 && errno != EEXIST)
        perror("Failed to create directory");
}

int is_regular_file(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;

#ifdef _WIN32
    return (st.st_mode & _S_IFREG) != 0;
#else
    return S_ISREG(st.st_mode);
#endif
}

void log_operation(int thread_num, const char* filename, const char* status, double time_spent)
{
    FILE* log = fopen("log.txt", "a");
    if (!log)
        return;

    time_t now = time(NULL);
    char time_str[64];

    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(log, "[%s] Thread %d: %s - %s (%.2f sec)\n",
            time_str,
            thread_num,
            filename,
            status,
            time_spent);

    fclose(log);
}

void process_file(const char* input_name, int thread_num)
{
    if (!is_regular_file(input_name))
    {
        pthread_mutex_lock(&global_mutex);
        log_operation(thread_num, input_name, "SKIPPED_NOT_REGULAR_FILE", 0);
        pthread_mutex_unlock(&global_mutex);
        return;
    }

    FILE* in = fopen(input_name, "rb");
    if (!in)
    {
        pthread_mutex_lock(&global_mutex);
        log_operation(thread_num, input_name, "ERROR_OPEN_INPUT", 0);
        pthread_mutex_unlock(&global_mutex);
        return;
    }

    const char* filename = input_name;
    for (const char* p = input_name; *p; p++)
    {
        if (*p == '/' || *p == '\\')
            filename = p + 1;
    }

    char output_path[512];
    int needed = snprintf(output_path, sizeof(output_path), "%s/%s", output_dir, filename);
    if (needed < 0 || needed >= (int)sizeof(output_path))
    {
        fclose(in);
        pthread_mutex_lock(&global_mutex);
        log_operation(thread_num, input_name, "ERROR_PATH_TOO_LONG", 0);
        pthread_mutex_unlock(&global_mutex);
        return;
    }

    FILE* out = fopen(output_path, "wb");
    if (!out)
    {
        fclose(in);

        pthread_mutex_lock(&global_mutex);
        log_operation(thread_num, input_name, "ERROR_OPEN_OUTPUT", 0);
        pthread_mutex_unlock(&global_mutex);
        return;
    }

    unsigned char buf[BUFFER_SIZE];
    unsigned char enc[BUFFER_SIZE];

    size_t bytes;
    clock_t start = clock();

    while ((bytes = fread(buf, 1, BUFFER_SIZE, in)) > 0)
    {
        caesar(buf, enc, (int)bytes);
        fwrite(enc, 1, bytes, out);
    }

    fclose(in);
    fclose(out);

    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;

    pthread_mutex_lock(&global_mutex);
    copied_count++;
    log_operation(thread_num, input_name, "SUCCESS", time_spent);
    pthread_mutex_unlock(&global_mutex);

    printf("Thread %d processed: %s (%.2f sec)\n", thread_num, input_name, time_spent);
}

void* worker_thread(void* arg)
{
    thread_arg_t* t = (thread_arg_t*)arg;
    int thread_num = t->thread_num;

    while (1)
    {
        int index;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;

        int res = pthread_mutex_timedlock(&global_mutex, &ts);

        if (res == ETIMEDOUT)
        {
            printf("WARNING: Possible deadlock: thread %d waiting >5 sec\n", thread_num);
            continue;
        }

        if (current_index >= file_count)
        {
            pthread_mutex_unlock(&global_mutex);
            break;
        }

        index = current_index++;
        pthread_mutex_unlock(&global_mutex);

        process_file(files[index], thread_num);
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

    create_directory(output_dir);

    pthread_t threads[THREAD_COUNT];
    thread_arg_t args[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        args[i].thread_num = i + 1;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_join(threads[i], NULL);

    printf("Total files processed: %d\n", copied_count);

    return 0;
}