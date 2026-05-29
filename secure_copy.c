/* 64-битные файловые операции — ДОЛЖНО быть до любых #include,
   иначе off_t/fseeko останутся 32-битными на 32-битных системах */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <errno.h>

#include "rc4.h"

#define BUFFER_SIZE 4096
#define MAX_WORKERS 5
#define MAX_PATH_LEN 1024
#define MAX_NAME_LEN 1023    /* максимальная длина имени файла в образе */

typedef struct
{
    char real_path[MAX_PATH_LEN];
    char image_path[MAX_PATH_LEN];
    uint32_t file_size;      // размер исходного файла (stat)
    off_t image_offset;      // предрасчитанное смещение в образе
} file_task_t;

typedef struct
{
    file_task_t* tasks;
    int count;
    int capacity;
    int next_index;          // атомарно через __atomic_fetch_add (без мьютекса)
    char* image_name;
    int image_fd;            // fd образа для pwrite
    off_t original_size;     // исходный размер образа (для отката при ошибке)
} task_queue_t;

typedef struct
{
    char name[MAX_PATH_LEN];
    uint32_t size;
} list_item_t;


/* ---------------------------------------------------------------
 * Вспомогательные функции для little-endian формата образа.
 * ТЗ: "Образ, созданный программой одного студента, должен
 * корректно обрабатываться программой другого студента."
 * Фиксированный LE-формат гарантирует совместимость
 * независимо от архитектуры.
 * --------------------------------------------------------------- */
static void write_le32(unsigned char* buf, uint32_t val)
{
    buf[0] = (unsigned char)(val & 0xFF);
    buf[1] = (unsigned char)((val >> 8) & 0xFF);
    buf[2] = (unsigned char)((val >> 16) & 0xFF);
    buf[3] = (unsigned char)((val >> 24) & 0xFF);
}

static uint32_t read_le32(const unsigned char* buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}


void generate_salt(unsigned char* salt)
{
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (urandom)
    {
        if (fread(salt, 1, 16, urandom) != 16)
        {
            fprintf(stderr, "Ошибка: не удалось прочитать /dev/urandom\n");
            fclose(urandom);
            exit(1);
        }
        fclose(urandom);
    }
    else
    {
        fprintf(stderr, "Ошибка: /dev/urandom недоступен, невозможно сгенерировать соль\n");
        exit(1);
    }
}

// Рекурсивный сбор файлов + stat() для предрасчёта размеров
void collect_files(task_queue_t* q, const char* base_real, const char* base_image)
{
    struct stat st;
    if (stat(base_real, &st) != 0)
    {
        fprintf(stderr, "Предупреждение: путь не найден: %s\n", base_real);
        return;
    }

    if (S_ISDIR(st.st_mode))
    {
        DIR* dir = opendir(base_real);
        if (!dir) return;
        struct dirent* entry;

        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            char next_real[MAX_PATH_LEN];
            char next_image[MAX_PATH_LEN];

            snprintf(next_real, MAX_PATH_LEN, "%s/%s", base_real, entry->d_name);

            if (strlen(base_image) == 0)
                snprintf(next_image, MAX_PATH_LEN, "%s", entry->d_name);
            else
                snprintf(next_image, MAX_PATH_LEN, "%s/%s", base_image, entry->d_name);

            collect_files(q, next_real, next_image);
        }
        closedir(dir);
    }
    else if (S_ISREG(st.st_mode))
    {
        if (q->count >= q->capacity)
        {
            q->capacity = (q->capacity == 0) ? 16 : q->capacity * 2;
            file_task_t* tmp = (file_task_t*)realloc(q->tasks, q->capacity * sizeof(file_task_t));
            if (!tmp) { fprintf(stderr, "Ошибка памяти при сборе файлов\n"); return; }
            q->tasks = tmp;
        }
        // Лимит 4GB: file_size хранится в uint32_t (4 байта в заголовке образа)
        if (st.st_size > (off_t)0xFFFFFFFF)
        {
            fprintf(stderr, "Ошибка: файл '%s' превышает 4GB (%ld байт)\n",
                    base_image, (long)st.st_size);
            return;
        }
        strncpy(q->tasks[q->count].real_path, base_real, MAX_PATH_LEN - 1);
        q->tasks[q->count].real_path[MAX_PATH_LEN - 1] = '\0';
        strncpy(q->tasks[q->count].image_path, base_image, MAX_PATH_LEN - 1);
        q->tasks[q->count].image_path[MAX_PATH_LEN - 1] = '\0';
        q->tasks[q->count].file_size = (uint32_t)st.st_size;
        q->tasks[q->count].image_offset = 0;
        q->count++;
    }
}

// Шифрование файла: ОДНА соль, ОДИН непрерывный RC4-поток на весь файл
// Формат записи: [4B file_size][4B name_len][16B salt][name][encrypted_data]
// Все числовые поля записываются в little-endian (write_le32)
int process_file_to_image(const char* input_file, int image_fd,
                           const char* internal_name, off_t offset, uint32_t known_size)
{
    FILE* in = fopen(input_file, "rb");
    if (!in) return 1;

    uint32_t name_len = (uint32_t)strlen(internal_name);

    // Валидация имени
    if (name_len == 0 || name_len > MAX_NAME_LEN)
    {
        fprintf(stderr, "Ошибка: недопустимая длина имени файла (%u): '%s'\n",
                name_len, internal_name);
        fclose(in);
        return 1;
    }

    uint32_t file_size = known_size;

    // Генерируем ОДНУ соль для всего файла
    unsigned char salt[16];
    generate_salt(salt);

    // Записываем заголовок: [4B file_size][4B name_len][16B salt][name] (LE32)
    int header_size = 4 + 4 + 16 + (int)name_len;
    unsigned char* header = (unsigned char*)malloc(header_size);
    if (!header) { fclose(in); return 1; }

    write_le32(header, file_size);
    write_le32(header + 4, name_len);
    memcpy(header + 8, salt, 16);
    memcpy(header + 24, internal_name, name_len);

    ssize_t pw_ret = pwrite(image_fd, header, header_size, offset);
    free(header);

    if (pw_ret != header_size)
    {
        fprintf(stderr, "Ошибка: не удалось записать заголовок для '%s' (pwrite вернул %zd, ожидалось %d)\n",
                internal_name, pw_ret, header_size);
        fclose(in);
        return 1;
    }

    // Для пустого файла — не инициализируем криптографию (нечего шифровать)
    if (file_size == 0)
    {
        fclose(in);
        return 0;
    }

    // Инициализируем RC4 ОДИН РАЗ для всего файла (KSA с key = пароль||соль)
    if (init_file_crypto(salt) != 0)
    {
        fprintf(stderr, "Ошибка: не удалось инициализировать шифрование для '%s'\n", internal_name);
        fclose(in);
        return 1;
    }

    // Шифруем данные чанками — один непрерывный RC4-поток
    // Чанки — только I/O буфер, криптографически это один поток
    off_t pos = offset + header_size;
    unsigned char* read_buf = (unsigned char*)malloc(BUFFER_SIZE);
    unsigned char* enc_buf = (unsigned char*)malloc(BUFFER_SIZE);
    if (!read_buf || !enc_buf)
    {
        fprintf(stderr, "Ошибка выделения памяти при шифровании\n");
        free(read_buf); free(enc_buf);
        fclose(in);
        end_file_crypto();
        return 1;
    }

    uint32_t remaining = file_size;
    int error = 0;

    while (remaining > 0 && !error)
    {
        uint32_t this_chunk = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        size_t bytes = fread(read_buf, 1, this_chunk, in);

        if (bytes != this_chunk) { error = 1; break; }

        // rc4_crypt теперь возвращает int — проверяем ошибку шифрования
        if (rc4_crypt(read_buf, enc_buf, (int)bytes) != 0)
        {
            fprintf(stderr, "Ошибка шифрования чанка для '%s'\n", internal_name);
            error = 1;
            break;
        }

        pw_ret = pwrite(image_fd, enc_buf, bytes, pos);
        if (pw_ret != (ssize_t)bytes)
        {
            fprintf(stderr, "Ошибка: pwrite для '%s' вернул %zd, ожидалось %u\n",
                    internal_name, pw_ret, this_chunk);
            error = 1;
            break;
        }
        pos += bytes;
        remaining -= bytes;
    }

    // Уничтожаем S-box один раз после обработки всего файла
    end_file_crypto();

    // Затираем буферы перед освобождением
    explicit_bzero(read_buf, BUFFER_SIZE);
    explicit_bzero(enc_buf, BUFFER_SIZE);

    free(read_buf);
    free(enc_buf);
    fclose(in);
    return error;
}

// Воркер: берёт задачу через __atomic_fetch_add, пишет через pwrite — без мьютексов
void* worker_thread(void* arg)
{
    task_queue_t* q = (task_queue_t*)arg;

    while (1)
    {
        int idx = __atomic_fetch_add(&q->next_index, 1, __ATOMIC_SEQ_CST);
        if (idx >= q->count) break;

        file_task_t* task = &q->tasks[idx];

        if (process_file_to_image(task->real_path, q->image_fd,
                                   task->image_path, task->image_offset,
                                   task->file_size) == 0)
        {
            printf("Добавлен: %s\n", task->image_path);
        }
        else
        {
            fprintf(stderr, "Ошибка обработки: %s\n", task->real_path);
        }
    }
    return NULL;
}


int compare_items(const void* a, const void* b)
{
    return strcmp(((list_item_t*)a)->name, ((list_item_t*)b)->name);
}

// Список файлов в образе. Формат: [4B size][4B name_len][16B salt][name][encrypted_data]
// Числовые поля читаются как little-endian (read_le32)
void cmd_list(const char* image)
{
    FILE* img = fopen(image, "rb");
    if (!img)
    {
        fprintf(stderr, "Не удалось открыть образ: %s\n", image);
        return;
    }

    list_item_t* items = NULL;
    int count = 0, capacity = 0;
    unsigned char hdr_buf[8];   // file_size + name_len (LE32)
    unsigned char salt[16];

    while (fread(hdr_buf, 1, 8, img) == 8 &&
           fread(salt, 1, 16, img) == 16)
    {
        uint32_t file_size = read_le32(hdr_buf);
        uint32_t name_len = read_le32(hdr_buf + 4);

        // Валидация: name_len должна быть > 0 и <= MAX_NAME_LEN
        if (name_len == 0 || name_len > MAX_NAME_LEN)
        {
            fprintf(stderr, "Предупреждение: некорректная длина имени (%u), пропуск записи\n",
                    name_len);
            // Пропускаем данные: если name_len > MAX_NAME_LEN, считаем её для fseeko
            if (fseeko(img, (off_t)name_len, SEEK_CUR) != 0) break;
            if (file_size > 0 && fseeko(img, (off_t)file_size, SEEK_CUR) != 0) break;
            continue;
        }

        if (count >= capacity)
        {
            capacity = (capacity == 0) ? 16 : capacity * 2;
            items = (list_item_t*)realloc(items, capacity * sizeof(list_item_t));
        }

        if (fread(items[count].name, 1, name_len, img) != name_len) break;
        items[count].name[name_len] = '\0';
        items[count].size = file_size;
        count++;

        // Пропускаем зашифрованные данные (один непрерывный блок)
        if (file_size > 0 && fseeko(img, (off_t)file_size, SEEK_CUR) != 0) break;
    }
    fclose(img);

    if (count > 0)
    {
        // Дедупликация: оставляем последнюю версию при одинаковых именах
        int* keep = (int*)calloc(count, sizeof(int));
        for (int i = 0; i < count; i++) keep[i] = 1;
        for (int i = 0; i < count; i++)
        {
            for (int j = i + 1; j < count; j++)
            {
                if (keep[i] && strcmp(items[i].name, items[j].name) == 0)
                {
                    keep[i] = 0;
                    break;
                }
            }
        }
        int unique = 0;
        for (int i = 0; i < count; i++)
        {
            if (keep[i])
            {
                if (unique != i) items[unique] = items[i];
                unique++;
            }
        }
        free(keep);
        count = unique;

        qsort(items, count, sizeof(list_item_t), compare_items);
    }

    printf("\nСодержимое образа '%s':\n", image);
    printf("--------------------------------------------------\n");
    for (int i = 0; i < count; i++)
        printf("%-40s | %u байт\n", items[i].name, items[i].size);
    printf("--------------------------------------------------\n");

    free(items);
}

// Извлечение файла: ищем последнее вхождение, расшифровываем одним RC4-потоком
// Числовые поля читаются как little-endian (read_le32)
void cmd_get(const char* image, const char* out_file, const char* target)
{
    FILE* img = fopen(image, "rb");
    if (!img)
    {
        fprintf(stderr, "Не удалось открыть образ: %s\n", image);
        return;
    }

    unsigned char hdr_buf[8];
    unsigned char salt[16];
    char current_name[MAX_PATH_LEN];

    off_t last_data_pos = -1;
    uint32_t last_file_size = 0;
    unsigned char last_salt[16];

    while (fread(hdr_buf, 1, 8, img) == 8 &&
           fread(salt, 1, 16, img) == 16)
    {
        uint32_t file_size = read_le32(hdr_buf);
        uint32_t name_len = read_le32(hdr_buf + 4);

        off_t name_pos = ftello(img);

        // Валидация name_len
        if (name_len == 0 || name_len > MAX_NAME_LEN)
        {
            if (fseeko(img, (off_t)name_len, SEEK_CUR) != 0) break;
            if (file_size > 0 && fseeko(img, (off_t)file_size, SEEK_CUR) != 0) break;
            continue;
        }

        if (fread(current_name, 1, name_len, img) != name_len) break;
        current_name[name_len] = '\0';

        if (strcmp(current_name, target) == 0)
        {
            last_data_pos = name_pos + name_len;
            last_file_size = file_size;
            memcpy(last_salt, salt, 16);
        }

        // Пропускаем зашифрованные данные
        if (file_size > 0 && fseeko(img, (off_t)file_size, SEEK_CUR) != 0) break;
    }

    if (last_data_pos >= 0)
    {
        fseeko(img, last_data_pos, SEEK_SET);

        FILE* out = fopen(out_file, "wb");
        if (!out)
        {
            fprintf(stderr, "Ошибка создания файла вывода: %s\n", out_file);
            fclose(img);
            return;
        }

        // Пустой файл — просто создаём пустой выходной файл
        if (last_file_size == 0)
        {
            fclose(out);
            fclose(img);
            printf("Файл '%s' успешно извлечен в '%s'\n", target, out_file);
            return;
        }

        // Инициализируем RC4 ОДИН РАЗ — один непрерывный поток на весь файл
        if (init_file_crypto(last_salt) != 0)
        {
            fprintf(stderr, "Ошибка: не удалось инициализировать расшифровку для '%s'\n", target);
            fclose(out);
            fclose(img);
            return;
        }

        unsigned char* in_buf = (unsigned char*)malloc(BUFFER_SIZE);
        unsigned char* out_buf = (unsigned char*)malloc(BUFFER_SIZE);
        if (!in_buf || !out_buf)
        {
            fprintf(stderr, "Ошибка памяти при расшифровке\n");
            free(in_buf); free(out_buf);
            fclose(out); fclose(img);
            end_file_crypto();
            return;
        }

        uint32_t remaining = last_file_size;
        int error = 0;
        while (remaining > 0 && !error)
        {
            uint32_t this_chunk = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
            size_t bytes_read = fread(in_buf, 1, this_chunk, img);
            if (bytes_read != this_chunk)
            {
                fprintf(stderr, "Предупреждение: неполное чтение данных для '%s' (ожидалось %u, прочитано %zu)\n",
                        target, this_chunk, bytes_read);
                error = 1;
                break;
            }

            // rc4_crypt теперь возвращает int — проверяем ошибку
            if (rc4_crypt(in_buf, out_buf, (int)this_chunk) != 0)
            {
                fprintf(stderr, "Ошибка расшифровки чанка для '%s'\n", target);
                error = 1;
                break;
            }

            size_t written = fwrite(out_buf, 1, this_chunk, out);
            if (written != this_chunk)
            {
                fprintf(stderr, "Ошибка: записано %zu из %u байт в '%s'\n",
                        written, this_chunk, out_file);
                error = 1;
                break;
            }
            remaining -= this_chunk;
        }

        end_file_crypto();  // уничтожаем S-box один раз после всех чанков

        // Затираем буферы перед освобождением
        explicit_bzero(in_buf, BUFFER_SIZE);
        explicit_bzero(out_buf, BUFFER_SIZE);

        free(in_buf);
        free(out_buf);
        fclose(out);
        fclose(img);

        if (!error)
            printf("Файл '%s' успешно извлечен в '%s'\n", target, out_file);
        return;
    }

    fprintf(stderr, "Файл '%s' не найден в образе.\n", target);
    fclose(img);
}

// Проверка, начинается ли строка с указанного префикса флага
// Поддерживает и -key и --key
static int is_flag(const char* arg, const char* name)
{
    if (strcmp(arg, name) == 0) return 1;
    // Проверяем двойное тире: --key == -key
    if (arg[0] == '-' && arg[1] == '-' && strcmp(arg + 2, name + 1) == 0) return 1;
    return 0;
}

int main(int argc, char* argv[])
{
    int is_add = 0, is_list = 0, is_get = 0;
    char *key = NULL, *image = NULL, *out_file = NULL, *target = NULL;
    // Позиционные аргументы для -get (out_file и target могут быть без флага -out)
    char *get_positional[2] = {NULL, NULL};
    int get_pos_count = 0;

    char **inputs = NULL;
    int inputs_count = 0;

    for (int i = 1; i < argc; i++)
    {
        if (is_flag(argv[i], "-add")) is_add = 1;
        else if (is_flag(argv[i], "-list")) is_list = 1;
        else if (is_flag(argv[i], "-get")) is_get = 1;
        else if (is_flag(argv[i], "-key") && i + 1 < argc) key = argv[++i];
        else if (is_flag(argv[i], "-image") && i + 1 < argc) image = argv[++i];
        else if (is_flag(argv[i], "-out") && i + 1 < argc) out_file = argv[++i];
        else
        {
            if (is_add)
            {
                if (!inputs) inputs = (char**)malloc(argc * sizeof(char*));
                inputs[inputs_count++] = argv[i];
            }
            else if (is_get)
            {
                // Собираем позиционные аргументы для -get
                if (get_pos_count < 2)
                    get_positional[get_pos_count++] = argv[i];
            }
        }
    }

    // Для -get: если -out не указан, первый позиционный = out_file, второй = target
    // Если -out указан, первый позиционный = target
    if (is_get)
    {
        if (!out_file && get_pos_count >= 2)
        {
            out_file = get_positional[0];
            target = get_positional[1];
        }
        else if (out_file && !target && get_pos_count >= 1)
        {
            target = get_positional[0];
        }
        else if (!out_file && get_pos_count == 1)
        {
            // Только один позиционный — это target, out_file не указан
            target = get_positional[0];
        }
    }

    if (is_add && key && image && inputs_count > 0)
    {
        set_key(key);

        task_queue_t q;
        memset(&q, 0, sizeof(q));
        q.image_name = image;

        // Собираем файлы + получаем размеры через stat()
        for (int i = 0; i < inputs_count; i++)
        {
            struct stat st;
            if (stat(inputs[i], &st) == 0 && S_ISDIR(st.st_mode))
                collect_files(&q, inputs[i], "");
            else
            {
                // Для одиночного файла — используем basename как путь в образе
                char* tmp = strdup(inputs[i]);
                char* bname = basename(tmp);
                collect_files(&q, inputs[i], bname);
                free(tmp);
            }
        }

        if (q.count > 0)
        {
            // Предрасчёт смещений: [4B file_size][4B name_len][16B salt][name][encrypted_data]
            off_t current_offset = 0;

            struct stat img_st;
            if (stat(image, &img_st) == 0 && img_st.st_size > 0)
                current_offset = img_st.st_size;

            q.original_size = current_offset;

            for (int i = 0; i < q.count; i++)
            {
                q.tasks[i].image_offset = current_offset;
                uint32_t name_len = (uint32_t)strlen(q.tasks[i].image_path);
                off_t record_size = 4 + 4 + 16 + (off_t)name_len + (off_t)q.tasks[i].file_size;
                current_offset += record_size;
            }

            // Создаём/открываем образ для записи
            q.image_fd = open(image, O_RDWR | O_CREAT, 0644);
            if (q.image_fd < 0)
            {
                fprintf(stderr, "Не удалось создать образ: %s\n", image);
                free(q.tasks);
                free(inputs);
                cleanup_key();
                return 1;
            }

            if (ftruncate(q.image_fd, current_offset) != 0)
            {
                fprintf(stderr, "Ошибка: ftruncate не смог выделить место для образа (%s)\n",
                        strerror(errno));
                close(q.image_fd);
                free(q.tasks);
                free(inputs);
                cleanup_key();
                return 1;
            }

            // Запускаем потоки — всегда параллельно, без мьютексов на запись
            int t_count = (q.count < MAX_WORKERS) ? q.count : MAX_WORKERS;
            pthread_t threads[MAX_WORKERS];

            for (int i = 0; i < t_count; i++)
                pthread_create(&threads[i], NULL, worker_thread, &q);

            for (int i = 0; i < t_count; i++)
                pthread_join(threads[i], NULL);

            close(q.image_fd);
            free(q.tasks);
        }

        free(inputs);
        cleanup_key();
    }
    else if (is_list && image)
    {
        cmd_list(image);
        free(inputs);  // на случай если были собраны
    }
    else if (is_get && key && image && out_file && target)
    {
        set_key(key);
        cmd_get(image, out_file, target);
        cleanup_key();
        free(inputs);
    }
    else
    {
        fprintf(stderr, "Ошибка: неверные аргументы\n");
        printf("Использование:\n");
        printf("  ./secure_copy -add -key <key> -image <img.img> <file1|dir1> ...\n");
        printf("  ./secure_copy -list -image <img.img>\n");
        printf("  ./secure_copy -get -image <img.img> -key <key> -out <result> <filename>\n");
        printf("  ./secure_copy -get -image <img.img> -key <key> <result> <filename>\n");
        printf("\nПоддерживаются флаги с одним тире (-key) и двумя (--key).\n");
        free(inputs);
        return 1;
    }

    return 0;
}
