#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "caesar.h"

#define BUFFER_SIZE 4096
#define WORKERS_COUNT 4
#define MAX_PATH_LEN 1024

volatile sig_atomic_t keep_running = 1;

typedef enum
{
    MODE_SEQUENTIAL,
    MODE_PARALLEL,
    MODE_AUTO

} run_mode_t;

typedef struct
{
    const char* input_path;
    char output_path[MAX_PATH_LEN];
    double duration_ms;
    int status;

} file_task_t;

typedef struct
{
    file_task_t* tasks;
    int count;
    int next_index;
    int started;

    pthread_mutex_t mutex;
    pthread_cond_t cond;

} task_queue_t;

typedef struct
{
    task_queue_t* queue;

} worker_args_t;

typedef struct
{
    run_mode_t mode;
    double total_ms;
    double avg_ms;
    int processed_files;
    int success_files;

} run_stats_t;

void sigint_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

double diff_ms(struct timespec start, struct timespec end)
{
    double sec = (double)(end.tv_sec - start.tv_sec) * 1000.0;
    double nsec = (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
    return sec + nsec;
}

const char* mode_to_string(run_mode_t mode)
{
    switch (mode)
    {
        case MODE_SEQUENTIAL: return "sequential";
        case MODE_PARALLEL:   return "parallel";
        case MODE_AUTO:       return "auto";
        default:              return "unknown";
    }
}

run_mode_t choose_auto_mode(int files_count)
{
    return (files_count < 5) ? MODE_SEQUENTIAL : MODE_PARALLEL;
}

int process_one_file(const char* input_file, const char* output_file)
{
    FILE* in = fopen(input_file, "rb");
    if (!in)
    {
        fprintf(stderr, "Input file error [%s]: %s\n", input_file, strerror(errno));
        return 1;
    }

    FILE* out = fopen(output_file, "wb");
    if (!out)
    {
        fprintf(stderr, "Output file error [%s]: %s\n", output_file, strerror(errno));
        fclose(in);
        return 1;
    }

    unsigned char read_buf[BUFFER_SIZE];
    unsigned char enc_buf[BUFFER_SIZE];

    while (keep_running)
    {
        size_t bytes = fread(read_buf, 1, BUFFER_SIZE, in);

        if (bytes > 0)
        {
            caesar(read_buf, enc_buf, (int)bytes);

            if (fwrite(enc_buf, 1, bytes, out) != bytes)
            {
                fprintf(stderr, "Write error [%s]\n", output_file);
                fclose(in);
                fclose(out);
                return 1;
            }
        }

        if (bytes < BUFFER_SIZE)
        {
            if (feof(in))
                break;

            if (ferror(in))
            {
                fprintf(stderr, "Read error [%s]\n", input_file);
                fclose(in);
                fclose(out);
                return 1;
            }
        }
    }

    fclose(in);
    fclose(out);

    if (!keep_running)
        return 2;

    return 0;
}

void process_task(file_task_t* task)
{
    struct timespec start_time;
    struct timespec end_time;

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    task->status = process_one_file(task->input_path, task->output_path);
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    task->duration_ms = diff_ms(start_time, end_time);
}

void* worker_thread(void* arg)
{
    worker_args_t* args = (worker_args_t*)arg;
    task_queue_t* queue = args->queue;

    while (keep_running)
    {
        pthread_mutex_lock(&queue->mutex);

        while (!queue->started && keep_running)
            pthread_cond_wait(&queue->cond, &queue->mutex);

        if (!keep_running)
        {
            pthread_mutex_unlock(&queue->mutex);
            break;
        }

        if (queue->next_index >= queue->count)
        {
            pthread_mutex_unlock(&queue->mutex);
            break;
        }

        int index = queue->next_index;
        queue->next_index++;

        pthread_mutex_unlock(&queue->mutex);

        process_task(&queue->tasks[index]);
    }

    return NULL;
}

void collect_stats(run_mode_t mode, file_task_t* tasks, int count, double total_ms, run_stats_t* stats)
{
    double sum_file_ms = 0.0;
    int success = 0;

    for (int i = 0; i < count; i++)
    {
        sum_file_ms += tasks[i].duration_ms;
        if (tasks[i].status == 0)
            success++;
    }

    stats->mode = mode;
    stats->total_ms = total_ms;
    stats->avg_ms = (count > 0) ? (sum_file_ms / count) : 0.0;
    stats->processed_files = count;
    stats->success_files = success;
}

void print_stats(const run_stats_t* stats, file_task_t* tasks, int count)
{
    printf("\n============================================================\n");
    printf("РЕЖИМ: %s\n", mode_to_string(stats->mode));
    printf("Общее время: %.3f ms\n", stats->total_ms);
    printf("Среднее время на файл: %.3f ms\n", stats->avg_ms);
    printf("Обработано файлов: %d\n", stats->processed_files);
    printf("Успешно обработано: %d\n", stats->success_files);
    printf("============================================================\n");

    for (int i = 0; i < count; i++)
    {
        printf("[%2d] %s -> %s | %.3f ms | %s\n",
               i + 1,
               tasks[i].input_path,
               tasks[i].output_path,
               tasks[i].duration_ms,
               (tasks[i].status == 0) ? "OK" : "ERROR");
    }
}

int run_sequential(file_task_t* tasks, int count, run_stats_t* stats)
{
    struct timespec start_time;
    struct timespec end_time;

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < count && keep_running; i++)
        process_task(&tasks[i]);

    clock_gettime(CLOCK_MONOTONIC, &end_time);

    collect_stats(MODE_SEQUENTIAL, tasks, count, diff_ms(start_time, end_time), stats);
    return 0;
}

int run_parallel(file_task_t* tasks, int count, run_stats_t* stats)
{
    struct timespec start_time;
    struct timespec end_time;

    pthread_t workers[WORKERS_COUNT];
    worker_args_t worker_args;
    task_queue_t queue;

    queue.tasks = tasks;
    queue.count = count;
    queue.next_index = 0;
    queue.started = 0;

    pthread_mutex_init(&queue.mutex, NULL);
    pthread_cond_init(&queue.cond, NULL);

    worker_args.queue = &queue;

    int workers_to_create = (count < WORKERS_COUNT) ? count : WORKERS_COUNT;

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < workers_to_create; i++)
        pthread_create(&workers[i], NULL, worker_thread, &worker_args);

    pthread_mutex_lock(&queue.mutex);
    queue.started = 1;
    pthread_cond_broadcast(&queue.cond);
    pthread_mutex_unlock(&queue.mutex);

    for (int i = 0; i < workers_to_create; i++)
        pthread_join(workers[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &end_time);

    pthread_mutex_destroy(&queue.mutex);
    pthread_cond_destroy(&queue.cond);

    collect_stats(MODE_PARALLEL, tasks, count, diff_ms(start_time, end_time), stats);
    return 0;
}

void build_output_name(const char* input, const char* suffix, char* output, size_t output_size)
{
    snprintf(output, output_size, "%s%s", input, suffix);
}

file_task_t* create_tasks(char* input_files[], int count, const char* suffix)
{
    file_task_t* tasks = (file_task_t*)calloc((size_t)count, sizeof(file_task_t));
    if (!tasks)
        return NULL;

    for (int i = 0; i < count; i++)
    {
        tasks[i].input_path = input_files[i];
        build_output_name(input_files[i], suffix, tasks[i].output_path, sizeof(tasks[i].output_path));
        tasks[i].duration_ms = 0.0;
        tasks[i].status = -1;
    }

    return tasks;
}

void remove_generated_files(file_task_t* tasks, int count)
{
    for (int i = 0; i < count; i++)
        remove(tasks[i].output_path);
}

int parse_mode(const char* arg, run_mode_t* mode)
{
    if (strcmp(arg, "--mode=sequential") == 0)
    {
        *mode = MODE_SEQUENTIAL;
        return 0;
    }

    if (strcmp(arg, "--mode=parallel") == 0)
    {
        *mode = MODE_PARALLEL;
        return 0;
    }

    if (strcmp(arg, "--mode=auto") == 0)
    {
        *mode = MODE_AUTO;
        return 0;
    }

    return 1;
}

void print_usage(const char* prog_name)
{
    printf("Старый режим совместимости:\n");
    printf("  %s input output key\n\n", prog_name);

    printf("Новый режим:\n");
    printf("  %s --mode=sequential --key=42 file1 file2 file3\n", prog_name);
    printf("  %s --mode=parallel   --key=42 file1 file2 file3 file4 file5\n", prog_name);
    printf("  %s --mode=auto       --key=42 file1 file2 file3 ...\n", prog_name);
    printf("\n");
    printf("В новом режиме выходные файлы создаются автоматически:\n");
    printf("  file1 -> file1.enc\n");
    printf("  file2 -> file2.enc\n");
}

int run_old_compatibility_mode(char* input_file, char* output_file, int key)
{
    struct timespec start_time;
    struct timespec end_time;

    set_key((char)key);

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    int status = process_one_file(input_file, output_file);
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    printf("\n============================================================\n");
    printf("РЕЖИМ СОВМЕСТИМОСТИ (старый интерфейс)\n");
    printf("Файл: %s -> %s\n", input_file, output_file);
    printf("Время: %.3f ms\n", diff_ms(start_time, end_time));
    printf("Статус: %s\n", (status == 0) ? "OK" : "ERROR");
    printf("============================================================\n");

    if (!keep_running)
        printf("Операция прервана пользователем\n");

    return status;
}

int main(int argc, char* argv[])
{
    signal(SIGINT, sigint_handler);

    /*
        Старый режим:
        ./secure_copy input output key
    */
    if (argc == 4 &&
        strncmp(argv[1], "--mode=", 7) != 0 &&
        strncmp(argv[1], "--key=", 6) != 0)
    {
        int key = atoi(argv[3]);
        return run_old_compatibility_mode(argv[1], argv[2], key);
    }

    /*
        Новый режим:
        ./secure_copy --mode=sequential --key=42 file1 file2 ...
    */
    if (argc < 4)
    {
        print_usage(argv[0]);
        return 1;
    }

    run_mode_t mode = MODE_AUTO;
    int key = 0;
    int key_set = 0;
    int first_input_index = -1;

    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--mode=", 7) == 0)
        {
            if (parse_mode(argv[i], &mode) != 0)
            {
                fprintf(stderr, "Unknown mode: %s\n", argv[i]);
                return 1;
            }
        }
        else if (strncmp(argv[i], "--key=", 6) == 0)
        {
            key = atoi(argv[i] + 6);
            key_set = 1;
        }
        else
        {
            first_input_index = i;
            break;
        }
    }

    if (!key_set || first_input_index == -1)
    {
        print_usage(argv[0]);
        return 1;
    }

    int files_count = argc - first_input_index;
    char** input_files = &argv[first_input_index];

    if (files_count <= 0)
    {
        fprintf(stderr, "No input files provided\n");
        return 1;
    }

    set_key((char)key);

    if (mode == MODE_SEQUENTIAL || mode == MODE_PARALLEL)
    {
        file_task_t* tasks = create_tasks(input_files, files_count, ".enc");
        if (!tasks)
        {
            fprintf(stderr, "Memory allocation error\n");
            return 1;
        }

        run_stats_t stats;

        if (mode == MODE_SEQUENTIAL)
            run_sequential(tasks, files_count, &stats);
        else
            run_parallel(tasks, files_count, &stats);

        print_stats(&stats, tasks, files_count);
        free(tasks);
    }
    else
    {
        run_mode_t chosen_mode = choose_auto_mode(files_count);
        run_mode_t alt_mode =
            (chosen_mode == MODE_SEQUENTIAL) ? MODE_PARALLEL : MODE_SEQUENTIAL;

        file_task_t* chosen_tasks = create_tasks(input_files, files_count, ".enc");
        file_task_t* alt_tasks = create_tasks(input_files, files_count, ".bench.enc");

        if (!chosen_tasks || !alt_tasks)
        {
            fprintf(stderr, "Memory allocation error\n");
            free(chosen_tasks);
            free(alt_tasks);
            return 1;
        }

        run_stats_t chosen_stats;
        run_stats_t alt_stats;

        printf("AUTO MODE: файлов = %d, выбран режим = %s\n",
               files_count, mode_to_string(chosen_mode));

        if (chosen_mode == MODE_SEQUENTIAL)
            run_sequential(chosen_tasks, files_count, &chosen_stats);
        else
            run_parallel(chosen_tasks, files_count, &chosen_stats);

        if (alt_mode == MODE_SEQUENTIAL)
            run_sequential(alt_tasks, files_count, &alt_stats);
        else
            run_parallel(alt_tasks, files_count, &alt_stats);

        print_stats(&chosen_stats, chosen_tasks, files_count);

        printf("\n================ СРАВНЕНИЕ РЕЖИМОВ ================\n");
        printf("%-12s | %-12s | %-18s | %-10s\n",
               "Режим", "Общее (ms)", "Среднее/файл (ms)", "Файлов");
        printf("---------------------------------------------------\n");
        printf("%-12s | %-12.3f | %-18.3f | %-10d\n",
               mode_to_string(chosen_stats.mode),
               chosen_stats.total_ms,
               chosen_stats.avg_ms,
               chosen_stats.processed_files);
        printf("%-12s | %-12.3f | %-18.3f | %-10d\n",
               mode_to_string(alt_stats.mode),
               alt_stats.total_ms,
               alt_stats.avg_ms,
               alt_stats.processed_files);
        printf("===================================================\n");

        remove_generated_files(alt_tasks, files_count);

        free(chosen_tasks);
        free(alt_tasks);
    }

    if (!keep_running)
        printf("Операция прервана пользователем\n");

    return 0;
}