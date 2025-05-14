// jpeg_viewer_multithreaded.c
// Compile with: gcc jpeg_viewer_multithreaded.c -o jpeg_viewer -lturbojpeg -lSDL2 -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <signal.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include <turbojpeg.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <assert.h>

#define MAX_PATH 4096
#define DEFAULT_CACHE_MB 128
#define DEFAULT_READAHEAD_MB 64
#define DEFAULT_MAX_THREADS 8
#define MAX_IMAGES 10000

volatile bool running = true;

void handle_sigint(int sig) {
    (void)sig;
    running = false;
}

void debug_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[DEBUG] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

typedef struct {
    char path[MAX_PATH];
    uint8_t *pixels;
    int width;
    int height;
    bool ready;
    bool displayed;
    pthread_mutex_t lock;
} image_t;

typedef struct {
    image_t *images;
    int count;
    size_t max_memory;
    pthread_mutex_t lock;
} image_cache_t;

typedef struct {
    char **files;
    int file_count;
    int next_index;
    pthread_mutex_t lock;
} file_queue_t;

file_queue_t file_queue;
image_t image_store[MAX_IMAGES];
int image_count = 0;

int cpu_count = 4;
int max_threads = DEFAULT_MAX_THREADS;

void *readahead_thread_func(void *arg) {
    for (int i = 0; i < file_queue.file_count; ++i) {
        int fd = open(file_queue.files[i], O_RDONLY);
        if (fd < 0) continue;

        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            readahead(fd, 0, st.st_size);
            debug_log("Readahead done for: %s (%ld bytes)", file_queue.files[i], st.st_size);
        }
        close(fd);
    }
    return NULL;
}

bool is_valid_jpeg(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) return false;
    unsigned char buf[2];
    fread(buf, 1, 2, file);
    fclose(file);
    return buf[0] == 0xFF && buf[1] == 0xD8;
}

void *decode_thread_func(void *arg) {
    while (running) {
        int index = -1;
        pthread_mutex_lock(&file_queue.lock);
        if (file_queue.next_index < file_queue.file_count) {
            index = file_queue.next_index++;
        }
        pthread_mutex_unlock(&file_queue.lock);

        if (index == -1) break;

        char *filename = file_queue.files[index];
        FILE *infile = fopen(filename, "rb");
        if (!infile) continue;

        fseek(infile, 0, SEEK_END);
        long size = ftell(infile);
        rewind(infile);

        unsigned char *jpeg_buf = malloc(size);
        fread(jpeg_buf, 1, size, infile);
        fclose(infile);

        tjhandle tjd = tjInitDecompress();

        int width, height, jpeg_subsamp, jpeg_colorspace;
        if (tjDecompressHeader3(tjd, jpeg_buf, size, &width, &height, &jpeg_subsamp, &jpeg_colorspace) != 0) {
            debug_log("Invalid JPEG: %s", filename);
            tjDestroy(tjd);
            free(jpeg_buf);
            continue;
        }

        int pitch = width * 3;
        uint8_t *buffer = malloc(height * pitch);

        tjDecompress2(tjd, jpeg_buf, size, buffer, width, pitch, height, TJPF_RGB, TJFLAG_FASTDCT);
        tjDestroy(tjd);
        free(jpeg_buf);

        pthread_mutex_lock(&image_store[index].lock);
        image_store[index].pixels = buffer;
        image_store[index].width = width;
        image_store[index].height = height;
        image_store[index].ready = true;
        pthread_mutex_unlock(&image_store[index].lock);

        debug_log("Decoded image: %s", filename);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [options] image1.jpg image2.jpg ...\n", argv[0]);
        return 1;
    }

    signal(SIGINT, handle_sigint);

    SDL_Init(SDL_INIT_VIDEO);

    // Validate JPEG files
    char **valid_files = malloc(sizeof(char *) * (argc - 1));
    int valid_count = 0;
    for (int i = 1; i < argc; ++i) {
        if (is_valid_jpeg(argv[i])) {
            valid_files[valid_count++] = argv[i];
        } else {
            debug_log("Skipping invalid JPEG: %s", argv[i]);
        }
    }

    file_queue.files = valid_files;
    file_queue.file_count = valid_count;
    file_queue.next_index = 0;
    pthread_mutex_init(&file_queue.lock, NULL);

    image_count = file_queue.file_count;
    for (int i = 0; i < image_count; ++i) {
        strncpy(image_store[i].path, file_queue.files[i], MAX_PATH);
        image_store[i].pixels = NULL;
        image_store[i].ready = false;
        image_store[i].displayed = false;
        pthread_mutex_init(&image_store[i].lock, NULL);
    }

    pthread_t readahead_thread;
    pthread_create(&readahead_thread, NULL, readahead_thread_func, NULL);
    pthread_join(readahead_thread, NULL);

    pthread_t decode_threads[DEFAULT_MAX_THREADS];
    for (int i = 0; i < max_threads; ++i) {
        pthread_create(&decode_threads[i], NULL, decode_thread_func, NULL);
    }

    SDL_Window *window = SDL_CreateWindow("JPEG Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_SHOWN);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Event e;

    int current_image = 0;

    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN) {
                debug_log("Key pressed: %s", SDL_GetKeyName(e.key.keysym.sym));
                if (e.key.keysym.sym == SDLK_q) {
                    running = false;
                } else if (e.key.keysym.sym == SDLK_PAGEUP && current_image >= 10) {
                    current_image -= 10;
                } else if (e.key.keysym.sym == SDLK_PAGEDOWN && current_image + 10 < image_count) {
                    current_image += 10;
                } else if (e.key.keysym.sym == SDLK_LEFT && current_image > 0) {
                    current_image--;
                } else if (e.key.keysym.sym == SDLK_RIGHT && current_image + 1 < image_count) {
                    current_image++;
                }
            } else if (e.type == SDL_QUIT) {
                running = false;
            }
        }

        // Wait until the current image is decoded
        while (running) {
            pthread_mutex_lock(&image_store[current_image].lock);
            bool ready = image_store[current_image].ready;
            pthread_mutex_unlock(&image_store[current_image].lock);
            if (ready) break;
            SDL_Delay(10);
        }

        pthread_mutex_lock(&image_store[current_image].lock);
        SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(
            image_store[current_image].pixels,
            image_store[current_image].width,
            image_store[current_image].height,
            24,
            image_store[current_image].width * 3,
            0x0000FF, 0x00FF00, 0xFF0000, 0);
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_FreeSurface(surface);

        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_DestroyTexture(texture);

        SDL_RenderPresent(renderer);
        pthread_mutex_unlock(&image_store[current_image].lock);

        SDL_Delay(30);
    }

    for (int i = 0; i < max_threads; ++i) {
        pthread_join(decode_threads[i], NULL);
    }

    for (int i = 0; i < image_count; ++i) {
        free(image_store[i].pixels);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    free(valid_files);
    return 0;
}
