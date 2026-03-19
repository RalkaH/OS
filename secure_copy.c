#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "caesar.h"

#define BUFFER_SIZE 4096

volatile sig_atomic_t keep_running = 1;

typedef struct
{
    unsigned char data[BUFFER_SIZE];
    size_t size;
    int full;
    int done;

    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;

} buffer_t;

typedef struct
{
    FILE* input;
    buffer_t* buffer;

} producer_args;

typedef struct
{
    FILE* output;
    buffer_t* buffer;

} consumer_args;

void sigint_handler(int sig)
{
    keep_running = 0;
}

void* producer_thread(void* arg)
{
    producer_args* args = (producer_args*)arg;
    buffer_t* buffer = args->buffer;

    unsigned char read_buf[BUFFER_SIZE];
    unsigned char enc_buf[BUFFER_SIZE];

    while (keep_running)
    {
        size_t bytes = fread(read_buf, 1, BUFFER_SIZE, args->input);

        pthread_mutex_lock(&buffer->mutex);

        while (buffer->full && keep_running)
            pthread_cond_wait(&buffer->not_full, &buffer->mutex);

        if (bytes == 0)
        {
            buffer->done = 1;
            pthread_cond_signal(&buffer->not_empty);
            pthread_mutex_unlock(&buffer->mutex);
            break;
        }

        caesar(read_buf, enc_buf, bytes);

        memcpy(buffer->data, enc_buf, bytes);
        buffer->size = bytes;
        buffer->full = 1;

        pthread_cond_signal(&buffer->not_empty);
        pthread_mutex_unlock(&buffer->mutex);
    }

    return NULL;
}

void* consumer_thread(void* arg)
{
    consumer_args* args = (consumer_args*)arg;
    buffer_t* buffer = args->buffer;

    while (1)
    {
        pthread_mutex_lock(&buffer->mutex);

        while (!buffer->full && !buffer->done)
            pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

        if (!buffer->full && buffer->done)
        {
            pthread_mutex_unlock(&buffer->mutex);
            break;
        }

        fwrite(buffer->data, 1, buffer->size, args->output);

        buffer->full = 0;

        pthread_cond_signal(&buffer->not_full);
        pthread_mutex_unlock(&buffer->mutex);
    }

    return NULL;
}

int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        printf("Usage: %s input output key\n", argv[0]);
        return 1;
    }

    char* input_file = argv[1];
    char* output_file = argv[2];
    int key = atoi(argv[3]);

    FILE* in = fopen(input_file, "rb");
    if (!in)
    {
        perror("Input file error");
        return 1;
    }

    FILE* out = fopen(output_file, "wb");
    if (!out)
    {
        perror("Output file error");
        fclose(in);
        return 1;
    }

    set_key((char)key);

    signal(SIGINT, sigint_handler);

    buffer_t buffer;

    buffer.size = 0;
    buffer.full = 0;
    buffer.done = 0;

    pthread_mutex_init(&buffer.mutex, NULL);
    pthread_cond_init(&buffer.not_full, NULL);
    pthread_cond_init(&buffer.not_empty, NULL);

    pthread_t producer;
    pthread_t consumer;

    producer_args p_args = {in, &buffer};
    consumer_args c_args = {out, &buffer};

    pthread_create(&producer, NULL, producer_thread, &p_args);
    pthread_create(&consumer, NULL, consumer_thread, &c_args);

    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    fclose(in);
    fclose(out);

    pthread_mutex_destroy(&buffer.mutex);
    pthread_cond_destroy(&buffer.not_full);
    pthread_cond_destroy(&buffer.not_empty);

    if (!keep_running)
        printf("Операция прервана пользователем\n");

    return 0;
}