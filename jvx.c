// SPDX-License-Identifier: GPL-2.0-only

/*
 *
 * Debian/Ubuntu Dependencies:
 *
 *    apt-get install libsdl2-dev libsdl2-ttf-dev libturbojpeg0-dev libc6-dev
 *
 * Compile with:
 *
 *    gcc  -DDEBUG_LEVEL=1 -Wall -Werror -g -o jvx jvx.c -lturbojpeg -lSDL2 -lSDL2_ttf -lm
 *
 * Run like this:
 *
 *    jpeg_viewer_batch --draw-filename /media/dave/NIKON\ D7100/DCIM/108NCZ_F/DSC_*
 *
 * Under gdb:
 *
 *    gdb --eval-command='break jvxbreak' --eval-command=run --args jvx
 *
 *
 * TODO:
 *  * Automatically tune memory footprint to the size of the system
 *  * Add an option to further scale images down to less than screen resolution
 *    to take less memory and draw faster.
 *  * Make surfaces smaller for small images
 *
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */

#include <sys/types.h>
#include <sys/wait.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <turbojpeg.h>
#include <pthread.h>
#include <assert.h>
#include <spawn.h>
#include <sys/sysinfo.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <malloc.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <ftw.h>

#define assert_mutex_locked(_mutex)	do {		\
	int err = pthread_mutex_trylock(_mutex);	\
	assert(err);					\
	if (!err)					\
		pthread_mutex_unlock(_mutex);		\
} while (0)

void jvxbreak(void)
{
	printf("jvxbreak()\n");
	return;
}

#define dassert(cond) do {	\
	if (!(cond))		\
		jvxbreak();	\
} while(0)

#define MB (1UL<<20)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define READ_ONCE(x) (*(volatile typeof(x) *) &(x))


#define MAX_PATH_LEN 4096
#define DEFAULT_MEMORY_LIMIT_BYTES    (5000 * MB)
#define DEFAULT_BIG_SKIP	      10

#ifndef DISABLE_THREADING
#define DISABLE_THREADING		false
#endif

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
#define RGB_MASKS 0xFF0000, 0x00FF00, 0x0000FF, 0
#else
#define RGB_MASKS 0x0000FF, 0x00FF00, 0xFF0000, 0
#endif

#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL 1
#endif

static uint64_t now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

#define log_debug_level(level, x...) if (DEBUG_LEVEL >= level) {log_debug(x);}
#define log_debug2(x...) log_debug_level(2, x);
#define log_debug3(x...) log_debug_level(3, x);
#define log_debug4(x...) log_debug_level(4, x);
#define log_debug5(x...) log_debug_level(5, x);
#define log_debug6(x...) log_debug_level(6, x);

static void log_debug(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	uint64_t t = now_ms();
	char buf[64];
	snprintf(buf, sizeof(buf), "[%lu][TID %ld] ", t, syscall(__NR_gettid));
	fputs(buf, stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	va_end(args);
}

static volatile int quit_flag = 0;

uint64_t reclaimed;
uint64_t reclaim_tries;
uint64_t surfaces;



///////////////////////////////////////////////////////
//
//
//
// Variable length array
//
//
//
///////////////////////////////////////////////////////

typedef struct {
	void** data;
	size_t size;
	size_t capacity;
	pthread_mutex_t lock;
} ptr_array;

// Initialize the array
void init_array(ptr_array* arr) {
	arr->data = NULL;
	arr->size = 0;
	arr->capacity = 0;
	pthread_mutex_init(&arr->lock, NULL);
}

// Push a value to the end (thread-safe)
bool push(ptr_array* arr, void *value) {
	bool success = true;
	pthread_mutex_lock(&arr->lock);

	if (arr->size == arr->capacity) {
		size_t new_capacity = arr->capacity ? arr->capacity * 2 : 4;
		void **new_data = realloc(arr->data, new_capacity * sizeof(void *));
		if (!new_data) {
			success = false;
			goto unlock;
		}
		arr->data = new_data;
		arr->capacity = new_capacity;
	}

	arr->data[arr->size++] = value;

unlock:
	pthread_mutex_unlock(&arr->lock);
	return success;
}

// Pop a value from the end (thread-safe)
bool pop(ptr_array* arr, void **out_value) {
	bool success = true;
	pthread_mutex_lock(&arr->lock);

	if (arr->size == 0) {
		success = false;
	} else {
		arr->size--;
		if (out_value) {
			*out_value = arr->data[arr->size];
		}
	}

	pthread_mutex_unlock(&arr->lock);
	return success;
}

// Free the array
void free_array(ptr_array* arr) {
	pthread_mutex_lock(&arr->lock);
	free(arr->data);
	arr->data = NULL;
	arr->size = 0;
	arr->capacity = 0;
	pthread_mutex_unlock(&arr->lock);
	pthread_mutex_destroy(&arr->lock);
}

///////////////////////////////////////////////////////
//
//
//
// END Variable length array
//
//
//
///////////////////////////////////////////////////////

enum image_state
{
	BRAND_NEW = 0,
	MAPPED  = 22,
	DECODED = 33,
	RECLAIMED = 44,
	INVALID = 55,
};

static bool image_ready_to_render(enum image_state state)
{
	if (state == DECODED)
		return true;

	return false;
}

typedef struct image
{
	enum image_state state;

	char filename[MAX_PATH_LEN];
	int width, height;

	uint64_t readahead_performed;
	uint64_t i_mem_footprint;

	int decoded;
	int decoding_in_progress;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int verbose_unlock;

	SDL_Surface *surface;
	int rendering;

	uint64_t timestamp;
	uint64_t readahead_ts;
	uint64_t decode_start_ts;
	uint64_t decode_done_ts;

	unsigned char *jpeg_buf;
	unsigned long jpeg_size;

	struct image *prev;
	struct image *next;
} image_info_t;

typedef struct {
	image_info_t *images;
	int image_count;
	int current_index;
	int last_delta;
	int direction;
	int big_skip;
	int nr_rendered;

	pthread_t *decode_threads;
	int num_decode_threads;

	pthread_t readahead_thread;
	pthread_mutex_t readahead_queue_mutex;
	pthread_cond_t readahead_queue_cond;
	int need_readahead;

	bool gui;
	bool runtime_debugging;
	bool slideshow_loop;
	bool draw_filename;
	bool force_render;

	SDL_Window *window;
	SDL_Renderer *renderer;
	pthread_mutex_t renderer_mutex;
	SDL_Texture *textures[2];
	int current_texture;
	int screen_width, screen_height;

	TTF_Font *font;

	image_info_t **decode_queue;
	int decode_queue_capacity;
	int decode_queue_size;
	int decode_queue_head;
	int decode_queue_tail;
	pthread_mutex_t decode_queue_mutex;
	pthread_cond_t decode_queue_cond;

	int stop_decode_threads;
	int stop_readahead_threads;

	uint64_t memory_footprint;
	uint64_t memory_limit;
} app_state_t;

static app_state_t app_state = {
	.memory_limit = DEFAULT_MEMORY_LIMIT_BYTES,
};


static SDL_Surface *__alloc_screen_surface(void)
{
	int scale_factor = 1.0;
	int new_width;//  = img->width  / scale_factor;
	int new_height;// = img->height / scale_factor;

	new_width  = app_state.screen_width  / scale_factor;
	new_height = app_state.screen_height / scale_factor;

	SDL_Surface *surface2 = SDL_CreateRGBSurface(
		0,
		new_width,
		new_height,
		24,				// bits per pixel
		RGB_MASKS
	);
	return surface2;
}

ptr_array surface_cache;

SDL_Surface *get_surface_for_screen(void)
{
	SDL_Surface *surface;
	void *ptr;
	bool ok = pop(&surface_cache, &ptr);
	surface = ptr;

	if (ok) {
		log_debug4("popped surface: %p", surface);
		return surface;
	}
	log_debug4("failed to pop, allocating");

	return __alloc_screen_surface();
}

void put_surface_for_screen(SDL_Surface *surface)
{
	bool ok = false;

	if (surface_cache.size < 100)
		ok = push(&surface_cache, surface);

	// If the push failed, just free it normally:
	if (!ok)
		SDL_FreeSurface(surface);

	log_debug4("pushed surface: %p size: %d", surface, surface_cache.size);
}

size_t get_rss_mb()
{
	FILE *f = fopen("/proc/self/statm", "r");
	if (!f)
		return 0;

	long pages = 0;
	if (fscanf(f, "%*s %ld", &pages) != 1) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return (size_t)pages * (size_t)sysconf(_SC_PAGESIZE) >> 20;
}

static void signal_handler(int sig) {
	(void)sig;
	quit_flag = 1;
}

static void memory_footprint_inc(image_info_t *img, int64_t by)
{
	char *p = "+";
	if (by < 0) {
		dassert(-by <= app_state.memory_footprint);
		p = "";
	}

	assert_mutex_locked(&img->mutex);

	img->i_mem_footprint += by;

	atomic_fetch_add(&app_state.memory_footprint, by);
	log_debug4("footprint %s%ld to %ld MB", p, by/MB, app_state.memory_footprint/MB);
}

static int64_t memory_footprint(void)
{
	return atomic_load(&app_state.memory_footprint);
}

pthread_mutex_t lru_mutex;
image_info_t *lru_head = NULL;  // MRU at head
image_info_t *lru_tail = NULL;  // LRU at tail
			    //
void lru_touch(image_info_t *img)
{
	pthread_mutex_lock(&lru_mutex);

	// If already at head, do nothing
	if (img == lru_head)
		goto out;

	// Remove from list
	if (img->prev) img->prev->next = img->next;
	if (img->next) img->next->prev = img->prev;
	if (img == lru_tail) lru_tail = img->prev;

	// Insert at head
	img->prev = NULL;
	img->next = lru_head;
	if (lru_head) lru_head->prev = img;
	lru_head = img;

	if (!lru_tail) lru_tail = img;
out:
	pthread_mutex_unlock(&lru_mutex);
}

void lru_demote_to_tail(image_info_t *entry) {
	pthread_mutex_lock(&lru_mutex);

       	// already at tail ?
	if (lru_tail == entry)
		goto out;

	// Unlink from current position
	if (entry->prev) entry->prev->next = entry->next;
	if (entry->next) entry->next->prev = entry->prev;

	// Update head if needed
	if (lru_head == entry)
		lru_head = entry->next;

	// Append at tail
	entry->prev = lru_tail;
	entry->next = NULL;
	if (lru_tail)
		lru_tail->next = entry;
	lru_tail = entry;

	// If list was empty, set head too
	if (!lru_head)
		lru_head = entry;
out:	
	pthread_mutex_unlock(&lru_mutex);
}

void lru_init(void)
{
	for (int i = 0; i < app_state.image_count; i++) {
		app_state.images[i].prev = (i > 0) ? &app_state.images[i - 1] : NULL;
		app_state.images[i].next = (i < app_state.image_count - 1) ? &app_state.images[i + 1] : NULL;
	}
	lru_head = &app_state.images[0];
	lru_tail = &app_state.images[app_state.image_count - 1];
	pthread_mutex_init(&lru_mutex, NULL);
}


// Thread-local storage key
static pthread_key_t pixels_key;
static pthread_once_t pixels_key_once = PTHREAD_ONCE_INIT;

// Free function for thread-local surface
static void free_pixels(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}

struct decoder_thread_bufs
{
	unsigned char *pixels;
	SDL_Surface *surface;
};

// Called once to create the TLS key
static void make_pixels_key() {
    pthread_key_create(&pixels_key, free_pixels);
}

// Get per-thread cached SDL_Surface
struct decoder_thread_bufs* get_thread_bufs(image_info_t *img)
{
	pthread_once(&pixels_key_once, make_pixels_key);

	struct decoder_thread_bufs *b = pthread_getspecific(pixels_key);
	if (b)
       		return b;

	b = malloc(sizeof(*b));

	// b was NULL: unset and unallocated
	b->pixels = malloc(img->width * img->height * 3);

	b->surface = SDL_CreateRGBSurfaceFrom(
		b->pixels,
		img->width,
		img->height,
		24,				// bits per pixel
		img->width * 3,			// pitch (bytes per row)
		RGB_MASKS
	);

	pthread_setspecific(pixels_key, b);

	return b;
}

SDL_Surface *get_thread_surface(image_info_t *img)
{
	return get_thread_bufs(img)->surface;
}
unsigned char *get_thread_pixels(image_info_t *img)
{
	return get_thread_bufs(img)->pixels;
}

static SDL_Surface *__create_image_surface(image_info_t *img, unsigned char *pixels)
{
	SDL_Surface *surface1 = get_thread_surface(img);
	/*SDL_CreateRGBSurfaceFrom(
		pixels,
		img->width,
		img->height,
		24,				// bits per pixel
		img->width * 3,			// pitch (bytes per row)
		RGB_MASKS
	);*/

	/*
	 * The surface _could_ be any size. But images may be
	 * higher resolution than the screen. Storing the whole
	 * image would be wasteful to memory and be more expensive
	 * to draw.
	 *
	 * Create a surface the size of the display. Scale the image
	 * down to fit:
	 */

	SDL_Surface *surface2 = get_surface_for_screen();
		//__alloc_screen_surface();
	size_t rss_after = get_rss_mb();
	if (rss_after > 20000) {
		log_debug("Rss too large: %ld", rss_after);
		exit(23);
	}

	SDL_BlitScaled(surface1, NULL, surface2, NULL);
	//SDL_FreeSurface(surface1);

	return surface2;
}

static void lock_image(image_info_t *img)
{
	pthread_mutex_lock(&img->mutex);
}
static void unlock_image(image_info_t *img)
{
	if (img->verbose_unlock) {
		log_debug("verbose unlock");
		//__builtin_trap();
	}
	pthread_mutex_unlock(&img->mutex);
}

static void check_mincore(image_info_t *img, unsigned char *jpeg_buf, unsigned long jpeg_size)
{
	unsigned long vec_len = (jpeg_size+getpagesize()-1) / getpagesize();
	unsigned char *vec = malloc(vec_len);
	uint64_t incore1 = 0;
	uint64_t incore2 = 0;
	int ret1;
	int ret2;

	if (!vec)
		return;

	ret1 = mincore(jpeg_buf, jpeg_size, vec);
	if (ret1)
		perror("mincore1:");
	for (int i = 0; i < vec_len; i++) {
		if (vec[i])
			incore1++;
	}
	memset(vec, 0, vec_len);

	//FIXME: Do we need this, readahed() and WILLNEED??
	//madvise(jpeg_buf, jpeg_size, MADV_WILLNEED);

	uint64_t sum = 0;
	for (int i = 0; i < jpeg_size; i += sizeof(uint64_t))
		sum += *(uint64_t *)&jpeg_buf[i];

	ret2 = mincore(jpeg_buf, jpeg_size, vec);
	if (ret2)
		perror("mincore2:");
	for (int i = 0; i < vec_len; i++) {
		if (vec[i])
			incore2++;
	}

	if (ret1 || ret2) {
		log_debug("jpeg_buf: %p +size: %p", jpeg_buf, jpeg_buf+jpeg_size);
		__builtin_trap();
	}

	if (incore2 != vec_len)
		log_debug("incore ret:%d/%d %ld->%ld/%ld %s", ret1, ret2, incore1, incore2, vec_len, img->filename);

	free(vec);
}

size_t fd_get_file_size(int fd)
{
	struct stat st;
	if (fstat(fd, &st) != 0) {
		log_debug("fstat failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return st.st_size;
}

int open_and_map_img(image_info_t *img)
{
	unsigned char jpeg_buf[2];
	int ret;

	assert_mutex_locked(&img->mutex);

	if (img->jpeg_buf) {
		if (img->state == RECLAIMED) {
			img->state = MAPPED;
			madvise(img->jpeg_buf, img->jpeg_size, MADV_WILLNEED);
			memory_footprint_inc(img, img->jpeg_size);
		}
		return 0;
	}

	int fd = open(img->filename, O_RDONLY);
	if (fd < 0) {
		log_debug("Failed to open %s: %s", img->filename, strerror(errno));
		return -1;
	}

	/* This avoids mmaping gigantic non-JPEG files: */
	ret = read(fd, &jpeg_buf[0], sizeof(jpeg_buf));
	if (ret < 0) {
		log_debug("error reading jpeg magic: %s", strerror(errno));
		close(fd);
		return -1;
	}
	if  (jpeg_buf[0] != 0xFF || jpeg_buf[1] != 0xD8) {
		log_debug("jpeg is INVALID: %s, magic bytes not found", img->filename);
		img->state = INVALID;
		pthread_cond_broadcast(&img->cond);
		close(fd);
		return -1;
	}

	assert(!img->jpeg_buf);
	img->jpeg_size = fd_get_file_size(fd);
	if (img->jpeg_size == -1) {
		log_debug("error getting file size: %s", img->filename);
		img->state = INVALID;
		pthread_cond_broadcast(&img->cond);
		close(fd);
	}

	assert(img->jpeg_size < 100 * MB);
	img->jpeg_buf = mmap(NULL, img->jpeg_size, PROT_READ, MAP_PRIVATE
			//| MAP_POPULATE
			, fd, 0);

	memory_footprint_inc(img, img->jpeg_size);

	if (img->jpeg_buf == MAP_FAILED) {
		log_debug("mmap failed: %s", strerror(errno));
		close(fd);
		// TODO: FIXME: put this state set and broadcast in a helper together
		img->state = INVALID;
		pthread_cond_broadcast(&img->cond);
		log_debug2("%s() SET INVALID: %s", __func__, img->filename);
		return -1;
	}
	madvise(img->jpeg_buf, img->jpeg_size, MADV_WILLNEED);
	img->state = MAPPED;
	if (0)
	check_mincore(img, img->jpeg_buf, img->jpeg_size);
	close(fd);
	return 0;
}

uint64_t img_surface_size(image_info_t *img)
{
	return img->surface->h * img->surface->pitch;
}

uint64_t image_memory_footprint(image_info_t *img)
{
	uint64_t ret = 0;

	// This is approximate of course. The Surface object is 
	// bigger than the pixels, but they form the majority.
	if (img->surface)
		ret += img_surface_size(img);

	// The mmap() of the image file
	//
	// It is MADV_DONTNEED'd when in RECLAIMED so do not count it then
	if (img->state != RECLAIMED && img->jpeg_buf)
		ret += img->jpeg_size;

	return ret;
}

#define check_img_footprint(x)	\
	assert(image_memory_footprint(x) == (x)->i_mem_footprint)

static int decode_jpeg(image_info_t *img)
{
	unsigned char *jpeg_buf = NULL;
	unsigned long jpeg_size = 0;
	unsigned char *dst_buf = NULL;
	img->decode_start_ts = now_ms();
	tjhandle tj = NULL;
	int rv = -1;

	assert_mutex_locked(&img->mutex);

	log_debug2("Start decoding %s", img->filename);
	if (img->state == INVALID) {
		log_debug2("%s() INVALID: %s", __func__, img->filename);
		goto cleanup;
	}

	jpeg_buf = img->jpeg_buf;
	jpeg_size = img->jpeg_size;
	if (!jpeg_buf) {
		goto cleanup;
	}

	tj = tjInitDecompress();
	if (!tj) {
		log_debug("tjInitDecompress failed");
		goto cleanup;
	}

	int w, h, subsamp, colorspace;
	if (tjDecompressHeader3(tj, jpeg_buf, jpeg_size, &w, &h, &subsamp, &colorspace) != 0) {
		log_debug("tjDecompressHeader3 failed: %s on %s", tjGetErrorStr(), img->filename);
		goto cleanup;
	}
	img->width = w;
	img->height = h;

	// get_thread_pixels() depends on width/height being set:
	dst_buf = get_thread_pixels(img);
	if (!dst_buf) {
		log_debug("malloc failed for %dx%d", w, h);
		exit(22);
		goto cleanup;
	}

	if (tjDecompress2(tj, jpeg_buf, jpeg_size, dst_buf, w, 0, h, TJPF_RGB, TJFLAG_FASTDCT) != 0) {
		log_debug("tjDecompress2 failed: %s", tjGetErrorStr());
		goto cleanup;
	}

	check_img_footprint(img);
	log_debug2("pixels/surface at decode: %p/%p", dst_buf, img->surface);
	img->timestamp = now_ms();
	check_img_footprint(img);
	// FIXME: this seems a little screwy
	//
	// Should we centralize all the state transisions?
	//
	// Should something have MADV_WILLNEED'd before this point?
	if (img->state == RECLAIMED) {
		// RECLAIMED ->jpeg_buf isn't counted, but a DECODED
		// one is. Account for the state change:
		memory_footprint_inc(img, img->jpeg_size);
	}
	img->state = DECODED;
	check_img_footprint(img);

	img->surface = NULL;
	check_img_footprint(img);
	img->surface = __create_image_surface(img, dst_buf);
	memory_footprint_inc(img, img_surface_size(img));
	check_img_footprint(img);
	surfaces++;

	pthread_cond_broadcast(&img->cond);

	log_debug2("Finished decoding %s (%dx%d)", img->filename, w, h);
	rv = 0;
	img->decode_done_ts = now_ms();

	uint64_t decode_len_ms = img->decode_done_ts - img->decode_start_ts;
	if (decode_len_ms > 500)
		log_debug("long decode for %s: %ld ms", img->filename, decode_len_ms);
cleanup:
	//if (dst_buf)
	//	free(dst_buf);
	// Note: this leaves the ->jpeg_buf mmap() in place. It will not
	// get reclaimed until the image itself is reclaimed
	if (0 && img->jpeg_buf) {
		int ret = munmap(img->jpeg_buf, img->jpeg_size);
		assert(!ret);
		img->jpeg_buf = NULL;
		img->jpeg_size = 0;
	}
	if (tj)
		tjDestroy(tj);

	return rv;
}

void img_zap_file_mapping(image_info_t *img)
{
	munmap(img->jpeg_buf, img->jpeg_size);
	img->jpeg_buf = NULL;
	img->jpeg_size = 0;
}

bool try_free_image_resources(image_info_t *img)
{
	bool ret = false;

	assert_mutex_locked(&img->mutex);

	check_img_footprint(img);
	if (!image_memory_footprint(img)) {
		log_debug4("img %s has no memory footprint", img->filename);
		assert(!img->surface);
		return false;
	}
	check_img_footprint(img);

	if (img->surface) {
		long surf_size = img->surface->h * img->surface->pitch;
		log_debug4("img %s reclaiming surface %d MB", img->filename, surf_size/MB );
		memory_footprint_inc(img, -surf_size);
		//SDL_FreeSurface(img->surface);
		put_surface_for_screen(img->surface);
		img->surface = NULL;
		surfaces--;
		ret = true;
	}
	check_img_footprint(img);

	if (img->jpeg_buf) {
		memory_footprint_inc(img, -img->jpeg_size);
		if (quit_flag) {
			img_zap_file_mapping(img);
		} else {
			//madvise(img->jpeg_buf, img->jpeg_size, MADV_DONTNEED);
		}
		log_debug3("state: %d decoded ago: %ld", img->state, now_ms() - img->timestamp);
		ret = true;
	}

	img->state = RECLAIMED;
	pthread_cond_broadcast(&img->cond);
	img->readahead_performed = 0;

	check_img_footprint(img);

	return ret;
}

static void __check_memory_footprint(const char *func, int line)
{
	uint64_t total_calc = 0;
	uint64_t total_acct = 0;

	if (!app_state.runtime_debugging)
		return;

	for (int j = 0; j < app_state.image_count; j++)
		lock_image(&app_state.images[j]);

	for (int j = 0; j < app_state.image_count; j++) {
		uint64_t cmf = image_memory_footprint(&app_state.images[j]);
		total_calc += cmf;
		total_acct += app_state.images[j].i_mem_footprint;
		log_debug5("amf[%d]: %ld", j, app_state.images[j].i_mem_footprint);
		log_debug5("cmf[%d]: %ld", j, cmf);
		dassert(cmf == app_state.images[j].i_mem_footprint);
	}

	for (int j = 0; j < app_state.image_count; j++)
		unlock_image(&app_state.images[j]);

	uint64_t mf = memory_footprint();

	bool ok = (mf == total_calc) && (mf == total_acct);
	if (ok)
		return;
	log_debug("memory_footprint(): %ld calculated: %ld per-image: %ld ok: %d",
			mf,
			total_calc,
			total_acct,
			ok);
	dassert(0);
	dassert(mf == total_calc);
	dassert(mf == total_acct);
}
#define check_memory_footprint() __check_memory_footprint(__func__,__LINE__)

image_info_t *find_oldest_image(uint64_t *total_bytes)
{
	return lru_tail;

	// FIXME remove old slow loop from before LRU:
	image_info_t *ret = NULL;
	uint64_t byte_count = 0;

	for (int i = 0; i < app_state.image_count; i++) {
		image_info_t *img = &app_state.images[i];

		if (image_memory_footprint(img) == 0)
			continue;

		byte_count += image_memory_footprint(img);

		if (img->rendering)
			continue;

		if (ret == NULL ||
		    ret->timestamp > img->timestamp)
			ret = img;
	}
	*total_bytes = byte_count;
	return ret;
}

static void maybe_reclaim_images(void)
{
	int retries = 0;
	uint64_t memory_footprint_slow = 0;
	int freed;
retry:
       	freed = 0;

	for (int i = 0; i < app_state.image_count; i++) {
		if (memory_footprint() < app_state.memory_limit) {
			log_debug2("memory footprint OK: %ld MB", memory_footprint()>>20);
			break;
		}

		image_info_t *img = find_oldest_image(&memory_footprint_slow);
		uint64_t f1 = image_memory_footprint(img);
		check_memory_footprint();
		int failed = pthread_mutex_trylock(&img->mutex);

		// Sent it to the end of the LRU
		lru_touch(img);
		if (failed) {
			log_debug2("reclaim failed to acquire lock for %s", img->filename);
			continue;
		}
		bool freed_one = try_free_image_resources(img);	
		img->timestamp = 0;
		unlock_image(img);
		check_memory_footprint();

		uint64_t f2 = image_memory_footprint(img);
		log_debug3("oldest image footprint: %ld=>%ld %s %p", f1, f2, img->filename, img->surface);

		freed += freed_one;
		reclaimed += freed_one;
		reclaim_tries++;

		if (!i)
			log_debug2("memory footprint: %ld MB, need to reclaim, so far: %d", memory_footprint()>>20, freed);

		if (freed > 5)
			break;
	}
	if (freed)
		return;
	if (memory_footprint() > app_state.memory_limit*2) {
		log_debug("ERROR: too far over footprint %lld==?%lld > %lld freed: %d retries: %d",
				memory_footprint_slow>>20,
				memory_footprint()>>20,
				app_state.memory_limit*2>>20, freed, retries);
		if (retries > 5) {
			exit(1);
		} else {
			retries++;
			goto retry;
		}
	}
}

static void *decode_thread_func(void *arg) {
	(void)arg;

	while (!app_state.stop_decode_threads) {
		//log_debug("taking &app_state.decode_queue_mutex");

		pthread_mutex_lock(&app_state.decode_queue_mutex);
		log_debug4("took &app_state.decode_queue_mutex");
		while (app_state.decode_queue_size == 0 && !app_state.stop_decode_threads) {
			log_debug5("cond1 &app_state.decode_queue_mutex");
			pthread_cond_wait(&app_state.decode_queue_cond, &app_state.decode_queue_mutex);
			log_debug5("cond2 &app_state.decode_queue_mutex");
		}
		image_info_t *img = NULL;
		if (app_state.decode_queue_size > 0) {
			img = app_state.decode_queue[app_state.decode_queue_head];
			//log_debug("dequeued %s", img->filename);
			app_state.decode_queue_head = (app_state.decode_queue_head + 1) % app_state.decode_queue_capacity;
			app_state.decode_queue_size--;
		}
		pthread_mutex_unlock(&app_state.decode_queue_mutex);
		log_debug4("released &app_state.decode_queue_mutex");

		if (img) {
			log_debug3("considering decode of %s", img->filename);

			lru_touch(img);
			lock_image(img);
			if (image_ready_to_render(img->state)) {
				unlock_image(img);
				log_debug3("decode thread skipping over ready image: %s state: %d", img->filename, img->state);
				log_debug5("surface: %p\n", img->surface);
				continue;
			}
			decode_jpeg(img);
			unlock_image(img);
		}
	}
	return NULL;
}

static void enqueue_decode(image_info_t *img) {
	//log_debug("queueing %p %s", img, img->filename);

	//log_debug("taking &app_state.decode_queue_mutex");
	pthread_mutex_lock(&app_state.decode_queue_mutex);
	//log_debug("took &app_state.decode_queue_mutex");
	if (app_state.decode_queue_size < app_state.decode_queue_capacity) {
		app_state.decode_queue[app_state.decode_queue_tail] = img;
		app_state.decode_queue_tail = (app_state.decode_queue_tail + 1) % app_state.decode_queue_capacity;
		app_state.decode_queue_size++;
		pthread_cond_signal(&app_state.decode_queue_cond);
	} else {
		// the queue reached capacity
		assert(0);
	}
	pthread_mutex_unlock(&app_state.decode_queue_mutex);
	//log_debug("released &app_state.decode_queue_mutex");
}

static int get_future_image_index(int nr_to_advance)
{
	int image_nr;

	// Bound it in case the request is huge:
	nr_to_advance %= app_state.image_count;

	int direction = 1;
	if (app_state.last_delta < 0)
		direction = -1;

	image_nr = app_state.current_index;
	image_nr += nr_to_advance * direction;
	// If it ends up negative, bring it back positive:
	if (image_nr < 0)
		image_nr += app_state.image_count;
	image_nr %= app_state.image_count;
	
	return image_nr;
}


static image_info_t *get_future_image(int nr_to_advance)
{
	int image_nr = get_future_image_index(nr_to_advance);
	image_info_t *img = &app_state.images[image_nr];
	return img;
}

static void img_try_readahead(image_info_t *img)
{
	// Don't do readahead if it's already decoded in memory
	if ((img->state == DECODED) ||
	    (img->state == INVALID)) {
		img->readahead_ts = -__LINE__;
		return;
	}
	if (img->readahead_performed) {
		img->readahead_ts = -__LINE__;
		return;
	}

	int fd = open(img->filename, O_RDONLY);
	if (fd < 0) {
		log_debug("error opening %s to read ahead", img->filename);
		img->readahead_ts = -__LINE__;
		return;
	}
	// The thread limit is probably better than a bytes limit:
	//int is_curr_image = (get_future_image(0) == img);

	img->readahead_ts = now_ms();
	size_t file_size = fd_get_file_size(fd);
	if (file_size < 0) {
		log_debug("error getting file size: %s", img->filename);
		img->readahead_ts = -__LINE__;
		goto out;
	}

	readahead(fd, 0, file_size);
	img->readahead_performed = now_ms();
	log_debug2("Readahead %s (%ld bytes) last delta: %d", img->filename, file_size, app_state.last_delta);

	lock_image(img);
	open_and_map_img(img);
	unlock_image(img);

	enqueue_decode(img);
out:
	close(fd);
}

static void *readahead_once(void *arg) {
	(void)arg;
	image_info_t *first_img = get_future_image(0);
	
	check_memory_footprint();

	// First, assume that the user will skip through the images sequentially,
	// one at a time. Read ahead enough to keep all the decode threads busy.
	for (int i = 1; i <= app_state.num_decode_threads * 4; i++) {
		image_info_t *img = get_future_image(i * app_state.last_delta);
		// Did it wrap around?
		if (i && img == first_img) {
			log_debug("wrap @ %d", i);
			break;
		}
		img_try_readahead(img);
		if (memory_footprint() > app_state.memory_limit)
		       maybe_reclaim_images();
		if (first_img->state == RECLAIMED) {
			log_debug("thrashing @ %d", i);
			break;
		}
	}

	// Then, do a (normally) smaller readahead assuming that
	// the user will skip (with page-up/down) at least once.
	int extra_readaheads[] = {
			 app_state.big_skip,
			-app_state.big_skip,
	};
	for (int j = 0; j < ARRAY_SIZE(extra_readaheads); j++ ) {
		image_info_t *img = get_future_image(j);
		if (memory_footprint() > app_state.memory_limit)
		       maybe_reclaim_images();
		img_try_readahead(img);
	}
	maybe_reclaim_images();
	return NULL;
}

static void *readahead_thread_func(void *arg)
{
	while (!app_state.stop_readahead_threads) {
		pthread_mutex_lock(&app_state.readahead_queue_mutex);
		while (app_state.need_readahead == 0 && !app_state.stop_readahead_threads) {
			uint64_t start_wait_ms = now_ms();
			pthread_cond_wait(&app_state.readahead_queue_cond, &app_state.readahead_queue_mutex);
			log_debug3("%s() slept: %ld", __func__, now_ms() - start_wait_ms);
		}
		pthread_mutex_unlock(&app_state.readahead_queue_mutex);

		readahead_once(arg);
		app_state.need_readahead = 0;
	}
	return NULL;
}

static void start_readahead(void)
{
	pthread_mutex_lock(&app_state.readahead_queue_mutex);
	app_state.need_readahead = 1;
	pthread_cond_signal(&app_state.readahead_queue_cond);
	pthread_mutex_unlock(&app_state.readahead_queue_mutex);
}

SDL_Texture *get_next_texture(SDL_Surface *surface)
{
	int next_texture;

	app_state.current_texture = (app_state.current_texture + 1) % 2;
	next_texture = app_state.current_texture;

	if (app_state.textures[next_texture] != NULL)
		return app_state.textures[next_texture];

	app_state.textures[next_texture] = SDL_CreateTexture(
	    app_state.renderer,
	    SDL_PIXELFORMAT_RGB24,       // or match surface->format->format
	    SDL_TEXTUREACCESS_STREAMING,
	    surface->w,
	    surface->h
	);
	SDL_UpdateTexture(app_state.textures[next_texture], NULL, surface->pixels, surface->pitch);

	return app_state.textures[next_texture];
}

static void draw_filename(SDL_Renderer *renderer, image_info_t *img)
{
	char *to_draw;
	char *free_me = NULL;
	int len = asprintf(&to_draw, "%s (%d/%d)", img->filename, 
			app_state.current_index + 1,
			app_state.image_count);

	if (len == -1)
		to_draw = img->filename;
	else
		free_me = to_draw;

	SDL_Color white = {255, 255, 255};
	SDL_Surface *text_surface = TTF_RenderText_Blended(app_state.font, to_draw, white);
	if (!text_surface)
		goto out;

	SDL_Texture *text_texture = SDL_CreateTextureFromSurface(renderer, text_surface);
	SDL_Rect text_rect = {10, 10, text_surface->w, text_surface->h};
	SDL_RenderCopy(renderer, text_texture, NULL, &text_rect);
	SDL_FreeSurface(text_surface);
	SDL_DestroyTexture(text_texture);
out:
	free(free_me);
}

static void __render_image(image_info_t *img) {
	//uint64_t render_start = now_ms();

	if (!image_ready_to_render(img->state)) {
		log_debug("ERROR: Can not render image %s (%dx%d) decoded: %d", img->filename, img->width, img->height, img->state);
		return;
	}

	int tex_w = img->width;
	int tex_h = img->height;
	float scale = fminf((float)app_state.screen_width / tex_w, (float)app_state.screen_height / tex_h);
	tex_w *= scale;
	tex_h *= scale;
	SDL_Rect dst_rect = {
		(app_state.screen_width - tex_w) / 2,
		(app_state.screen_height - tex_h) / 2,
		tex_w, tex_h
	};
	log_debug2("Rendering image %s (%dx%d)", img->filename, img->width, img->height);

	//if (app_state.texture)
	//	SDL_DestroyTexture(app_state.texture);

	SDL_Surface *surface = img->surface;
	log_debug2("about to render surface: %p", surface);

	pthread_mutex_lock(&app_state.renderer_mutex);
	//SDL_Texture *texture = SDL_CreateTexture(app_state.renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, img->width, img->height);
	SDL_Texture *texture = get_next_texture(surface);
	//SDL_CreateTexture(app_state.renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, img->width, img->height);
	//SDL_UpdateTexture(texture, NULL, img->pixels, img->width * 3);
	SDL_UpdateTexture(texture, NULL, surface->pixels, surface->pitch);
	SDL_RenderClear(app_state.renderer);
	SDL_RenderCopy(app_state.renderer, texture, NULL, &dst_rect);

	if (app_state.draw_filename)
		draw_filename(app_state.renderer, img);

	SDL_RenderPresent(app_state.renderer);
	pthread_mutex_unlock(&app_state.renderer_mutex);
	
	//uint64_t render_took = now_ms() - render_start;
	//log_debug2("Done rendering image %s (%dx%d) took:%ld ms", img->filename, img->width, img->height, render_took);
}

static void wait_for_image(image_info_t *img) {
	int first = 1;
	log_debug2("about to wait for image %s (%dx%d)", img->filename, img->width, img->height);
	img_try_readahead(img);
	pthread_mutex_lock(&img->mutex);
	while (!image_ready_to_render(img->state)) {
	       	if (quit_flag)
			break;
		if (img->state == INVALID)
			break;
		if (first)
			log_debug("Waiting for image %s (%dx%d)", img->filename, img->width, img->height);
		pthread_cond_wait(&img->cond, &img->mutex);
		first = 0;
	}
	unlock_image(img);
	log_debug2("Done waiting for image %s, state: %d", img->filename, img->state);
}

static void render_image(image_info_t *img)
{
	uint64_t render_start = now_ms();

	// FIXME: do something when images are invalid, like draw an error message
	if (img->state == INVALID) {
		log_debug("image %s is invalid", img->filename);
		return;
	}

	//if (!DISABLE_THREADING)
	if (0)
		wait_for_image(img);

	log_debug2("locking image %s (%dx%d)", img->filename, img->width, img->height);
	// Lock the image during rendering. Prevents reclaim among other things
	img->verbose_unlock = 1;

	lru_touch(img);
	lock_image(img);
	img->verbose_unlock = 0;
	if (!image_ready_to_render(img->state))
		decode_jpeg(img);
	img->timestamp = now_ms();
	img->rendering = 1;
	uint64_t lock_took = now_ms() - render_start;
	__render_image(img);
	img->rendering = 0;
	unlock_image(img);

	uint64_t render_took = now_ms() - render_start;

	int64_t readahead_ts	     = READ_ONCE(img->readahead_ts);
	int64_t readahead_ago	     = now_ms() - readahead_ts;
	int64_t decode_start_ts_ago  = now_ms() - img->decode_start_ts;
	int64_t decode_done_ts_ago   = now_ms() - img->decode_done_ts;
	if (readahead_ts < 0)
		readahead_ago = readahead_ts;
	if (readahead_ts == 0)
		readahead_ago = -__LINE__;
	assert(readahead_ago < 100000);

	if (lock_took > 2)
	log_debug("Done wait/render image %s (%dx%d) lock:%3ld render:%3ld ra:%3ld decstart:%3ld decdone:%3ld", img->filename, img->width, img->height,
			lock_took, render_took,
			readahead_ago,
			decode_start_ts_ago,
			decode_done_ts_ago);
}

static void __invalidate_image(image_info_t *img)
{
	try_free_image_resources(img);
	// ^ this only frees the mmap() at quit
	// FIXME: refactoring opportunity??
	img_zap_file_mapping(img);
	img->state = INVALID;
}

static void invalidate_image(image_info_t *img)
{
	lock_image(img);
	__invalidate_image(img);
	unlock_image(img);
}

/* must hold image lock */
static bool __image_file_ok(image_info_t *img)
{
	struct stat st;

	assert_mutex_locked(&img->mutex);

	if (stat(img->filename, &st) != 0) {
		__invalidate_image(img);
		log_debug("Error stat()'ing file %s, ignoring...", img->filename);
		check_img_footprint(img);
		return false;
	}

	return true;
}

static bool decode_and_render_image(image_info_t *img)
{
	start_readahead();
	log_debug2("Changing image %s (%dx%d)", img->filename, img->width, img->height);

	lock_image(img);

	bool ok = true;
	if (img->state == INVALID)
		ok = false;
	if (!__image_file_ok(img))
		ok = false;

	if (!ok) {
		unlock_image(img);
		return false;
	}
	open_and_map_img(img);
	unlock_image(img);

	if (app_state.gui) {
		enqueue_decode(img);
		render_image(img);
	}

	return true;
}

#define MAX_FD 20

static char **file_list = NULL;
static size_t file_count = 0;
static size_t file_capacity = 0;

static void add_file(const char *path) {
	if (file_count == file_capacity) {
		file_capacity = file_capacity ? file_capacity * 2 : 64;
		file_list = realloc(file_list, file_capacity * sizeof(char *));
		if (!file_list) {
			perror("realloc");
			exit(1);
		}
	}
	file_list[file_count++] = strdup(path);
}

static int visit(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
	if (typeflag == FTW_F) {
		add_file(fpath);
	}
	return 0;
}

static void collect_files(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) {
		perror(path);
		return;
	}

	if (S_ISDIR(st.st_mode)) {
		if (nftw(path, visit, MAX_FD, FTW_PHYS) == -1) {
			perror("nftw");
		}
	} else if (S_ISREG(st.st_mode)) {
		add_file(path);
	}
}

char **get_all_files_from_args(int argc, char **argv, size_t *out_count)
{
	for (int i = 0; i < argc; ++i) {
		collect_files(argv[i]);
	}

	*out_count = file_count;
	return file_list;
}

// FIXME: change ordering??
void run_action_with_replace(int action_nr, char *filename);

bool process_keypress(void)
{
	SDL_Event e;
	int delta = 0;

	if (!app_state.gui)
		return false;

	image_info_t *img = get_future_image(0);

	SDL_PollEvent(&e);
	if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q)) {
		quit_flag = 1;
	} else if (e.type == SDL_KEYDOWN) {
		int delta = 0;
		if (e.key.keysym.sym == SDLK_RIGHT) {
			delta = 1;
		} else if (e.key.keysym.sym == SDLK_LEFT) {
			delta = -1;
		} else if (e.key.keysym.sym == SDLK_HOME) {
			delta = -app_state.current_index;
		} else if (e.key.keysym.sym == SDLK_END) {
			delta = app_state.image_count - app_state.current_index - 1;
		} else if (e.key.keysym.sym == SDLK_PAGEDOWN) {
			delta = app_state.big_skip;
		} else if (e.key.keysym.sym == SDLK_PAGEUP) {
			delta = -app_state.big_skip;
		} else if (e.key.keysym.sym == SDLK_DELETE) {
			invalidate_image(img);
			app_state.force_render = true;
		} else if (e.key.keysym.sym >= SDLK_0 &&
			 e.key.keysym.sym <= SDLK_9) {
			int num = e.key.keysym.sym - SDLK_0;
			run_action_with_replace(num, img->filename);
			// The action may have messed with the file:
			app_state.force_render = true;
		}
		SDL_Keymod mod = e.key.keysym.mod;
		if (mod & KMOD_SHIFT)
			delta *= 2;
		if (mod & KMOD_CTRL)
			delta *= 4;
		if (mod & KMOD_ALT)
			delta *= 8;

		app_state.last_delta = delta;
		app_state.current_index = (app_state.current_index + delta + app_state.image_count) % app_state.image_count;
		log_debug2("keypress: delta: %d app_state.current_index: %d", delta, app_state.current_index);
	}

	// Report if a keypress caused the app_state.current_index to move:
	return !!delta;
}

extern char **environ;
int run_command(char *const argv[])
{
	pid_t pid;
	int status;

	if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0) {
		perror("posix_spawnp");
		return -1;
	}

	if (waitpid(pid, &status, 0) == -1) {
		perror("waitpid");
		return -1;
	}

	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#define NR_ACTION_KEYS 9
struct action_key
{
	char **arguments;
	int nr_arguments;
};

struct action_key action_keys[NR_ACTION_KEYS];

void add_action_arg(int nr, const char *new_arg)
{
	struct action_key *a;
	if (nr >= NR_ACTION_KEYS)
		return;

	a = &action_keys[nr];

	char **new_arguments = calloc(a->nr_arguments + 1,
				      sizeof(new_arguments[0]));
	for (int i = 0; i < a->nr_arguments; i++) {
		new_arguments[i] = a->arguments[i];
		log_debug4("copied arg[%d]: '%s'\n", i, new_arguments[i]);
	}
	if (new_arg == NULL)
		new_arguments[a->nr_arguments] = NULL;
	else
		new_arguments[a->nr_arguments] = strdup(new_arg);
	// FIXME does that leak memory? ^^
	log_debug4("added new arg[%d]: '%s'\n", a->nr_arguments, new_arg);
	a->nr_arguments++;

	if (a->arguments)
		free(a->arguments);
	a->arguments = new_arguments;
}

void finalize_action_keys(void)
{
	for (int i = 0; i < NR_ACTION_KEYS; i++) {
		struct action_key *a = &action_keys[i];
		if (!a->nr_arguments)
			continue;
		// Add a NULL argument to make posix_spawnp() happy: 
		add_action_arg(i, NULL);
	}
}

/*
 * Run an action key command. If one of the arguments is "{}"
 * then replace it with the current filename. No shell expansion
 * is performed anywhere. If you want it, run a script and do it
 * in there.
 */
void run_action_with_replace(int action_nr, char *filename)
{
	if (action_nr >= NR_ACTION_KEYS)
		return;

	struct action_key *a = &action_keys[action_nr];
	char **arg_copy = calloc(a->nr_arguments,
                                 sizeof(a->arguments[0]));

	if (a->nr_arguments == 0) {
		log_debug("no action set for key %d", action_nr);
		return;
	}

	for (int i = 0; i < a->nr_arguments; i++) {
		char *arg = a->arguments[i];
		arg_copy[i] = arg;
		// Just copy NULL. Don't treat as a string
		if (!arg)
			continue;
		if (!strcmp(arg, "{}")) {
			arg_copy[i] = filename;
		}
	}
	run_command(arg_copy);
	free(arg_copy);
}

void *dave_malloc(size_t size)
{
	log_debug("%s() size: %ld", __func__, size);
	return malloc(size);
}
void dave_free(void *ptr)
{
	log_debug("%s() ptr: %p", __func__, ptr);
	free(ptr);
}
void *dave_calloc(size_t nmemb, size_t size)
{
	log_debug("%s() nmemb: %ld size: %ld", __func__, nmemb, size);
	return calloc(nmemb, size);
}
void *dave_realloc(void *ptr, size_t size)
{
	log_debug("%s() ptr: %p size: %ld", __func__, ptr, size);
	return realloc(ptr, size);
}

void signal_handler_init(void)
{
	signal(SIGINT, signal_handler);
	signal(SIGUSR1, signal_handler);
	signal(SIGUSR2, signal_handler);
	signal(SIGTERM, signal_handler);
}


void process_command_line_args(int argc, char **argv)
{
	static struct option long_opts[] = {
		{"action0", required_argument, 0, '0'},
		{"action1", required_argument, 0, '1'},
		{"action2", required_argument, 0, '2'},
		{"action3", required_argument, 0, '3'},
		{"action4", required_argument, 0, '4'},
		{"action5", required_argument, 0, '5'},
		{"action6", required_argument, 0, '6'},
		{"action7", required_argument, 0, '7'},
		{"action8", required_argument, 0, '8'},
		{"action9", required_argument, 0, '9'},
		{"draw-filename",   no_argument,	0, 'd'},
		{"memory-limit",    required_argument,	0, 'm'},
		{"readahead-bytes", required_argument,	0, 'r'},
		{"slideshow-loop",  no_argument,	0, 's'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "1:2:3:4:5:6:7:8:9:0:dm:r:s", long_opts, NULL)) != -1) {
		switch (opt) {
		case '0' ... '9':
			add_action_arg(opt - '0', optarg);
			break;
		case 'd':
			app_state.draw_filename = true;
			break;
		case 'r':
			break;
		case 'm':
			app_state.memory_limit = strtoull(optarg, NULL, 10);
			break;
		case 's':
			app_state.slideshow_loop = true;
			break;
		default:
			fprintf(stderr, "Usage: %s [--readahead-bytes=INT] [--actionN=arg] image1.jpg image2.jpg ...\n", argv[0]);
			exit(1);
		}
	}
}

size_t init_image_list(int argc, char **argv)
{
	size_t file_count;
	int arg_count = argc - optind;
	char **files = get_all_files_from_args(arg_count, &argv[optind], &file_count);
	for (int i = 0; i < argc; i++) {
		log_debug2("argv[%d]: '%s'", i, argv[i]);
		if (i == optind)
			log_debug2("^^ optind");

	}
	if (file_count == 0) {
		printf("ERROR: no files specified\n");
		exit(1);
	}
	app_state.image_count = file_count;

	app_state.images = calloc(app_state.image_count, sizeof(image_info_t));
	for (int i = 0; i < app_state.image_count; ++i) {
		app_state.images[i].surface = NULL;
		app_state.images[i].readahead_performed = 0;
		strncpy(app_state.images[i].filename, files[i], MAX_PATH_LEN - 1);
		pthread_mutex_init(&app_state.images[i].mutex, NULL);
		pthread_cond_init(&app_state.images[i].cond, NULL);
	}

	return file_count;
}

void gui_init(void)
{
	app_state.gui = 1;
	if (!app_state.gui)
		return;

	//SDL_SetMemoryFunctions(dave_malloc, dave_calloc,dave_realloc, dave_free);
	SDL_Init(SDL_INIT_VIDEO);
	TTF_Init();
	SDL_GetCurrentDisplayMode(0, &(SDL_DisplayMode){0});
	SDL_DisplayMode mode;
	SDL_GetDesktopDisplayMode(0, &mode);
	app_state.screen_width = mode.w;
	app_state.screen_height = mode.h;

	SDL_Rect usable_bounds;
	if (SDL_GetDisplayUsableBounds(0, &usable_bounds) != 0) {
		fprintf(stderr, "SDL_GetDisplayUsableBounds failed: %s\n", SDL_GetError());
		return;
	}
	app_state.screen_width  = usable_bounds.w;
	app_state.screen_height = usable_bounds.h;

	SDL_WindowFlags flags;
	flags = SDL_WINDOW_SHOWN;
	// FULLSCREEN is wonky. It minimizes when you tab away from it
	//flags |= SDL_WINDOW_FULLSCREEN;
	flags |= SDL_WINDOW_BORDERLESS;
	app_state.window = SDL_CreateWindow("JPEG Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, app_state.screen_width, app_state.screen_height, flags);
	app_state.renderer = SDL_CreateRenderer(app_state.window, -1, SDL_RENDERER_ACCELERATED);
	app_state.font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 24);
	app_state.current_texture = 0;
}

void init_decode_threads(void)
{
	app_state.decode_queue_capacity = app_state.image_count;
	app_state.decode_queue = malloc(sizeof(app_state.decode_queue[0]) * app_state.image_count);
	pthread_mutex_init(&app_state.decode_queue_mutex, NULL);
	pthread_cond_init(&app_state.decode_queue_cond, NULL);

	app_state.num_decode_threads = get_nprocs();
	app_state.decode_threads = calloc(app_state.num_decode_threads, sizeof(pthread_t));
	if (!DISABLE_THREADING)
		for (int i = 0; i < app_state.num_decode_threads; ++i)
			pthread_create(&app_state.decode_threads[i], NULL, decode_thread_func, NULL);
}

void init_readahead_thread(void)
{
	app_state.need_readahead = 1;
	pthread_mutex_init(&app_state.readahead_queue_mutex, NULL);
	pthread_cond_init(&app_state.readahead_queue_cond, NULL);
	if (!DISABLE_THREADING)
		pthread_create(&app_state.readahead_thread, NULL, readahead_thread_func, NULL);
}

image_info_t *try_render_one_image(void)//image_info_t *last_img)
{
	static image_info_t *last_img = NULL;
	image_info_t *img = NULL;

	for (int i = 0; i < app_state.image_count; i++) {
		img = get_future_image(0);
		// Only render if the image changed, or if it
		// has been forced:
		if (!app_state.force_render && (img == last_img))
			break;

		bool render_ok = decode_and_render_image(img);
		log_debug2("trying to render %s, ok: %d", img->filename, render_ok);
		if (render_ok) {
			app_state.nr_rendered++;
			app_state.force_render = 0;
			break;
		}

		log_debug("Could not display %s, trying next image...", img->filename);
		img = NULL;
		// get_future_image_index() handles direction internally:
		app_state.current_index = get_future_image_index(1);
		log_debug2("now at: %d last delta: %d...", app_state.current_index, app_state.last_delta);
	}

	last_img = img;

	return img;
}

void sdl_drain_events(void)
{
	// Drain all pending keyboard events to avoid rapid repeated input
	SDL_Event evt_drain;
	while (SDL_PollEvent(&evt_drain)) {
		// Only quit if quit event received during draining
		if (evt_drain.type == SDL_QUIT) {
			quit_flag = 1;
			return;
		}
	}
}

// Makes sure the main loop doesn't run more than once every 30ms
void loop_slowdown(uint64_t loop_start_ts)
{
	return;
	uint64_t loop_duration_ts = now_ms() - loop_start_ts;
	uint64_t min_loop_len_ms = 30;
	if (loop_duration_ts < min_loop_len_ms)
		SDL_Delay(min_loop_len_ms - loop_duration_ts);
}

int main(int argc, char **argv)
{
	init_array(&surface_cache);

	process_command_line_args(argc, argv);
	finalize_action_keys();
	signal_handler_init();

	init_image_list(argc, argv);

	app_state.runtime_debugging = 0;
	app_state.big_skip = DEFAULT_BIG_SKIP;
	lru_init();
	check_memory_footprint();

	gui_init();

	init_decode_threads();
	init_readahead_thread();

	uint64_t start_ts = now_ms();
	while (!quit_flag) {
		uint64_t loop_start_ts = now_ms();
		check_memory_footprint();
		if (DISABLE_THREADING)
			maybe_reclaim_images();

		// Also increments app_state.current_index
		int action_happened = process_keypress();
		if (!action_happened && app_state.slideshow_loop) {
			app_state.last_delta = -1;
			app_state.current_index = get_future_image_index(1);
		}

		image_info_t *img = try_render_one_image();
		if (img == NULL) {
			fprintf(stderr, "ERROR: unable to render any image\n");
			break;
		}

		log_debug2("Done changing image %s (%dx%d)", img->filename, img->width, img->height);
		if (app_state.nr_rendered % 100 == 0) {
			log_debug("frame rate: %.1f/s reclaimed: %ld/%ld rss: %ld surfaces: %ld mem: %ld MB",
					1.0 * app_state.nr_rendered / ((now_ms() - start_ts) / 1000.0),
					reclaimed, reclaim_tries,
					get_rss_mb(),
					surfaces,
					memory_footprint() >> 20
					);
			app_state.nr_rendered = 0;
			start_ts = now_ms();
		}

		if (!app_state.gui)
			continue;
		sdl_drain_events();
		if (app_state.slideshow_loop)
			continue;

		loop_slowdown(loop_start_ts);
	}

	app_state.stop_decode_threads = 1;
	app_state.stop_readahead_threads = 1;
	pthread_cond_broadcast(&app_state.decode_queue_cond);
	pthread_cond_broadcast(&app_state.readahead_queue_cond);
	if (!DISABLE_THREADING) {
		for (int i = 0; i < app_state.num_decode_threads; ++i)
			pthread_join(app_state.decode_threads[i], NULL);
		pthread_join(app_state.readahead_thread, NULL);
	}
	log_debug("all threads joined??");
	for (int i = 0; i < app_state.image_count; ++i) {
		pthread_mutex_destroy(&app_state.images[i].mutex);
		pthread_cond_destroy(&app_state.images[i].cond);
		check_memory_footprint();
		lock_image(&app_state.images[i]);
		try_free_image_resources(&app_state.images[i]);
		unlock_image(&app_state.images[i]);
		check_memory_footprint();
	}

	if (app_state.gui) {
		TTF_CloseFont(app_state.font);
		SDL_DestroyRenderer(app_state.renderer);
		SDL_DestroyWindow(app_state.window);
		TTF_Quit();
		SDL_Quit();
	}
	log_debug("reclaimed: %ld/%ld rss: %ld surfaces: %ld mem: %ld MB",
			reclaimed, reclaim_tries,
			get_rss_mb(),
			surfaces,
			memory_footprint() >> 20
	);

	free_array(&surface_cache);
	return 0;
}

