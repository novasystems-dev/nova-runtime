#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "nova_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#define F_OK 0
#define access _access
#define unlink _unlink
#define NOVA_CLOSE_SOCKET(s) closesocket((SOCKET)(s))
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define NOVA_CLOSE_SOCKET(s) close((int)(s))
#endif

#define DEFAULT_ARENA_SIZE (1024 * 1024) // 1MB default region chunk

static int g_nova_argc = 0;
static char **g_nova_argv = NULL;

void nova_set_args(int argc, char **argv) {
    g_nova_argc = argc;
    g_nova_argv = argv;
}

void nova_rt_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

void nova_rt_shutdown(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

NovaArena *nova_arena_create(size_t capacity) {
    if (capacity == 0) {
        capacity = DEFAULT_ARENA_SIZE;
    }
    NovaArena *arena = (NovaArena *)malloc(sizeof(NovaArena));
    if (!arena) {
        nova_panic("Out of memory allocating NovaArena header", __FILE__, __LINE__);
    }
    arena->buffer = (uint8_t *)malloc(capacity);
    if (!arena->buffer) {
        free(arena);
        nova_panic("Out of memory allocating NovaArena buffer", __FILE__, __LINE__);
    }
    arena->capacity = capacity;
    arena->offset = 0;
    arena->next = NULL;
    return arena;
}

void *nova_arena_alloc(NovaArena *arena, size_t size, size_t align) {
    if (align == 0) align = sizeof(void *);
    
    size_t current = (size_t)(arena->buffer + arena->offset);
    size_t aligned = (current + (align - 1)) & ~(align - 1);
    size_t padding = aligned - current;

    if (arena->offset + padding + size > arena->capacity) {
        size_t next_cap = arena->capacity * 2;
        if (next_cap < size + padding) next_cap = size + padding + DEFAULT_ARENA_SIZE;
        NovaArena *next_chunk = nova_arena_create(next_cap);
        arena->next = next_chunk;
        return nova_arena_alloc(next_chunk, size, align);
    }

    arena->offset += padding;
    void *ptr = arena->buffer + arena->offset;
    arena->offset += size;
    return ptr;
}

void nova_arena_reset(NovaArena *arena) {
    while (arena) {
        arena->offset = 0;
        arena = arena->next;
    }
}

void nova_arena_destroy(NovaArena *arena) {
    while (arena) {
        NovaArena *next = arena->next;
        if (arena->buffer) free(arena->buffer);
        free(arena);
        arena = next;
    }
}

// ============================================================================
// CONCURRENCY CHANNELS & THREADS
// ============================================================================

NovaChannel *nova_channel_create(size_t capacity) {
    if (capacity == 0) capacity = 64;
    NovaChannel *chan = (NovaChannel *)malloc(sizeof(NovaChannel));
    if (!chan) nova_panic("Failed to allocate NovaChannel", __FILE__, __LINE__);
    
    chan->buffer = (void **)malloc(sizeof(void *) * capacity);
    if (!chan->buffer) nova_panic("Failed to allocate channel buffer", __FILE__, __LINE__);
    
    chan->capacity = capacity;
    chan->count = 0;
    chan->head = 0;
    chan->tail = 0;
    chan->closed = false;

    pthread_mutex_init(&chan->mutex, NULL);
    pthread_cond_init(&chan->not_empty, NULL);
    pthread_cond_init(&chan->not_full, NULL);
    return chan;
}

void nova_channel_send(NovaChannel *chan, void *item) {
    pthread_mutex_lock(&chan->mutex);
    if (chan->closed) {
        pthread_mutex_unlock(&chan->mutex);
        nova_panic("Attempted to send on closed channel", __FILE__, __LINE__);
    }

    while (chan->count == chan->capacity && !chan->closed) {
        pthread_cond_wait(&chan->not_full, &chan->mutex);
    }

    if (chan->closed) {
        pthread_mutex_unlock(&chan->mutex);
        nova_panic("Channel closed during send", __FILE__, __LINE__);
    }

    chan->buffer[chan->tail] = item;
    chan->tail = (chan->tail + 1) % chan->capacity;
    chan->count++;

    pthread_cond_signal(&chan->not_empty);
    pthread_mutex_unlock(&chan->mutex);
}

void *nova_channel_recv(NovaChannel *chan) {
    pthread_mutex_lock(&chan->mutex);

    while (chan->count == 0 && !chan->closed) {
        pthread_cond_wait(&chan->not_empty, &chan->mutex);
    }

    if (chan->count == 0 && chan->closed) {
        pthread_mutex_unlock(&chan->mutex);
        return NULL; // Closed and empty (Option::None)
    }

    void *item = chan->buffer[chan->head];
    chan->head = (chan->head + 1) % chan->capacity;
    chan->count--;

    pthread_cond_signal(&chan->not_full);
    pthread_mutex_unlock(&chan->mutex);
    return item;
}

void nova_channel_close(NovaChannel *chan) {
    pthread_mutex_lock(&chan->mutex);
    chan->closed = true;
    pthread_cond_broadcast(&chan->not_empty);
    pthread_cond_broadcast(&chan->not_full);
    pthread_mutex_unlock(&chan->mutex);
}

void nova_channel_destroy(NovaChannel *chan) {
    if (!chan) return;
    free(chan->buffer);
    pthread_mutex_destroy(&chan->mutex);
    pthread_cond_destroy(&chan->not_empty);
    pthread_cond_destroy(&chan->not_full);
    free(chan);
}

pthread_t nova_spawn(void *(*func)(void *), void *arg) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, func, arg) != 0) {
        nova_panic("Failed to spawn OS thread", __FILE__, __LINE__);
    }
    return thread;
}

void nova_join(pthread_t thread) {
    pthread_join(thread, NULL);
}

// ============================================================================
// CONSOLE I/O & PANIC
// ============================================================================

void nova_print_str(const char *str) {
    fputs(str, stdout);
    fflush(stdout);
}

void nova_print_int(int64_t val) {
    printf("%ld", (long)val);
    fflush(stdout);
}

void nova_print_float(double val) {
    printf("%f", val);
    fflush(stdout);
}

void nova_print_bool(bool val) {
    fputs(val ? "true" : "false", stdout);
    fflush(stdout);
}

void nova_print_newline(void) {
    putchar('\n');
    fflush(stdout);
}

void nova_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    nanosleep(&ts, NULL);
#endif
}

void nova_clear_screen(void) {
    fputs("\033[2J\033[H", stdout);
    fflush(stdout);
}

void nova_hide_cursor(void) {
    fputs("\033[?25l", stdout);
    fflush(stdout);
}

void nova_show_cursor(void) {
    fputs("\033[?25h", stdout);
    fflush(stdout);
}

int64_t nova_random_int(int64_t min, int64_t max) {
    if (min >= max) return min;
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
    return min + (rand() % (max - min + 1));
}

// Low-level POSIX / Winsock Socket Networking
int64_t nova_tcp_listen(int64_t port) {
#ifdef _WIN32
    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) return -1;
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        NOVA_CLOSE_SOCKET(server_fd);
        return -2;
    }
    
    if (listen(server_fd, 128) < 0) {
        NOVA_CLOSE_SOCKET(server_fd);
        return -3;
    }
    
    return (int64_t)server_fd;
#else
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        NOVA_CLOSE_SOCKET(server_fd);
        return -2;
    }
    
    if (listen(server_fd, 128) < 0) {
        NOVA_CLOSE_SOCKET(server_fd);
        return -3;
    }
    
    return (int64_t)server_fd;
#endif
}

int64_t nova_tcp_accept(int64_t server_fd) {
    struct sockaddr_in client_addr;
#ifdef _WIN32
    int client_len = sizeof(client_addr);
    SOCKET client_fd = accept((SOCKET)server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd == INVALID_SOCKET) return -1;
    return (int64_t)client_fd;
#else
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept((int)server_fd, (struct sockaddr *)&client_addr, &client_len);
    return (int64_t)client_fd;
#endif
}

int64_t nova_tcp_send_response(int64_t client_fd, const char *status_code, const char *body) {
    if (client_fd < 0) return -1;
    char header[512];
    size_t body_len = body ? strlen(body) : 0;
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Server: Nova-Native/0.1.0\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status_code ? status_code : "200 OK",
        body_len);
    
#ifdef _WIN32
    int w1 = send((SOCKET)client_fd, header, header_len, 0);
    int w2 = 0;
    if (body_len > 0) {
        w2 = send((SOCKET)client_fd, body, (int)body_len, 0);
    }
#else
    ssize_t w1 = write((int)client_fd, header, (size_t)header_len);
    ssize_t w2 = 0;
    if (body_len > 0) {
        w2 = write((int)client_fd, body, body_len);
    }
#endif
    NOVA_CLOSE_SOCKET(client_fd);
    return (int64_t)(w1 + w2);
}

void nova_tcp_close(int64_t fd) {
    if (fd >= 0) {
        NOVA_CLOSE_SOCKET(fd);
    }
}

void nova_panic(const char *msg, const char *file, int line) {
    fprintf(stderr, "\n[NOVA RUNTIME PANIC] %s at %s:%d\n", msg, file, line);
    exit(1);
}

// ============================================================================
// ATOMIC OPERATIONS (std/sync)
// ============================================================================

int64_t nova_atomic_load(int64_t *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}

void nova_atomic_store(int64_t *ptr, int64_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST);
}

int64_t nova_atomic_add(int64_t *ptr, int64_t delta) {
    return __atomic_fetch_add(ptr, delta, __ATOMIC_SEQ_CST);
}

int64_t nova_atomic_sub(int64_t *ptr, int64_t delta) {
    return __atomic_fetch_sub(ptr, delta, __ATOMIC_SEQ_CST);
}

bool nova_atomic_cas(int64_t *ptr, int64_t expected, int64_t desired) {
    return __atomic_compare_exchange_n(ptr, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// ============================================================================
// MUTEX SYNCHRONIZATION
// ============================================================================

pthread_mutex_t *nova_mutex_create(void) {
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (m) pthread_mutex_init(m, NULL);
    return m;
}

void nova_mutex_lock(pthread_mutex_t *mutex) {
    if (mutex) pthread_mutex_lock(mutex);
}

void nova_mutex_unlock(pthread_mutex_t *mutex) {
    if (mutex) pthread_mutex_unlock(mutex);
}

void nova_mutex_destroy(pthread_mutex_t *mutex) {
    if (mutex) {
        pthread_mutex_destroy(mutex);
        free(mutex);
    }
}

// ============================================================================
// FILESYSTEM OPERATIONS (std/fs)
// ============================================================================

const char *nova_fs_read_to_string(const char *path) {
    if (!path) return "";
    FILE *f = fopen(path, "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return ""; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ""; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

bool nova_fs_write_string(const char *path, const char *content) {
    if (!path) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    if (content) {
        fputs(content, f);
    }
    fclose(f);
    return true;
}

bool nova_fs_exists(const char *path) {
    if (!path) return false;
    return access(path, F_OK) == 0;
}

int64_t nova_fs_file_size(const char *path) {
    if (!path) return -1;
    struct stat st;
    if (stat(path, &st) == 0) {
        return (int64_t)st.st_size;
    }
    return -1;
}

bool nova_fs_remove(const char *path) {
    if (!path) return false;
    return unlink(path) == 0;
}

bool nova_fs_mkdir_p(const char *path) {
    if (!path || strlen(path) == 0) return false;
    char tmp[1024];
#ifdef _WIN32
    char winPath[1024];
    strncpy(winPath, path, sizeof(winPath) - 1);
    winPath[sizeof(winPath) - 1] = '\0';
    for (char *p = winPath; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    snprintf(tmp, sizeof(tmp), "mkdir \"%s\" 2>nul", winPath);
#else
    snprintf(tmp, sizeof(tmp), "mkdir -p \"%s\" 2>/dev/null", path);
#endif
    return system(tmp) == 0;
}

const char *nova_fs_sha256(const char *path) {
    return nova_crypto_sha256_file(path);
}

// ============================================================================
// OS & PROCESS OPERATIONS (std/os)
// ============================================================================

int64_t nova_os_args_count(void) {
    return (int64_t)g_nova_argc;
}

const char *nova_os_args_get(int64_t idx) {
    if (idx >= 0 && idx < g_nova_argc && g_nova_argv) {
        return g_nova_argv[idx];
    }
    return "";
}

int64_t nova_process_run(const char *cmd) {
    if (!cmd) return -1;
    int res = system(cmd);
#ifdef _WIN32
    return (int64_t)res;
#else
    return (int64_t)WEXITSTATUS(res);
#endif
}

const char *nova_os_getenv(const char *name) {
    if (!name) return "";
    const char *val = getenv(name);
    return val ? val : "";
}

// ============================================================================
// STRING UTILITIES (std/string)
// ============================================================================

bool nova_str_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

const char *nova_str_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *res = (char *)malloc(la + lb + 1);
    if (!res) return "";
    memcpy(res, a, la);
    memcpy(res + la, b, lb);
    res[la + lb] = '\0';
    return res;
}

const char *nova_str_concat3(const char *a, const char *b, const char *c) {
    if (!a) a = "";
    if (!b) b = "";
    if (!c) c = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t lc = strlen(c);
    char *res = (char *)malloc(la + lb + lc + 1);
    if (!res) return "";
    memcpy(res, a, la);
    memcpy(res + la, b, lb);
    memcpy(res + la + lb, c, lc);
    res[la + lb + lc] = '\0';
    return res;
}

// ============================================================================
// HIGH-PRECISION TIME OPERATIONS (std/time)
// ============================================================================

int64_t nova_time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)(ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL);
}

int64_t nova_time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)(ts.tv_sec * 1000000000LL + ts.tv_nsec);
}

void nova_time_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
#endif
}

// ============================================================================
// MATHEMATICAL OPERATIONS (std/math)
// ============================================================================

int64_t nova_math_abs(int64_t x) {
    return x < 0 ? -x : x;
}

int64_t nova_math_min(int64_t a, int64_t b) {
    return a < b ? a : b;
}

int64_t nova_math_max(int64_t a, int64_t b) {
    return a > b ? a : b;
}

int64_t nova_math_clamp(int64_t v, int64_t low, int64_t high) {
    if (v < low) return low;
    if (v > high) return high;
    return v;
}

int64_t nova_math_sqrt(int64_t x) {
    if (x <= 0) return 0;
    return (int64_t)sqrt((double)x);
}

int64_t nova_math_pow(int64_t base, int64_t exp) {
    if (exp < 0) return 0;
    int64_t res = 1;
    while (exp > 0) {
        if (exp & 1) res *= base;
        base *= base;
        exp >>= 1;
    }
    return res;
}

int64_t nova_math_gcd(int64_t a, int64_t b) {
    while (b != 0) {
        int64_t t = b;
        b = a % b;
        a = t;
    }
    return a >= 0 ? a : -a;
}

int64_t nova_math_lcm(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    int64_t g = nova_math_gcd(a, b);
    return (nova_math_abs(a * b)) / g;
}

// ============================================================================
// CRYPTOGRAPHIC OPERATIONS (std/crypto)
// ============================================================================

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    unsigned long long bitlen;
    uint32_t state[8];
} nova_sha256_ctx;

#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void nova_sha256_transform(nova_sha256_ctx *ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];
    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for ( ; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e,f,g) + sha256_k[i] + m[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void nova_sha256_init(nova_sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void nova_sha256_update(nova_sha256_ctx *ctx, const uint8_t data[], size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            nova_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void nova_sha256_final(nova_sha256_ctx *ctx, uint8_t hash[]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        nova_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    nova_sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        hash[i + 4]  = (uint8_t)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        hash[i + 8]  = (uint8_t)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        hash[i + 12] = (uint8_t)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        hash[i + 16] = (uint8_t)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        hash[i + 20] = (uint8_t)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        hash[i + 24] = (uint8_t)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        hash[i + 28] = (uint8_t)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

const char *nova_crypto_sha256(const char *input) {
    if (!input) input = "";
    nova_sha256_ctx ctx;
    nova_sha256_init(&ctx);
    nova_sha256_update(&ctx, (const uint8_t *)input, strlen(input));
    uint8_t hash[32];
    nova_sha256_final(&ctx, hash);
    char *hex = (char *)malloc(65);
    for (int i = 0; i < 32; i++) {
        sprintf(hex + (i * 2), "%02x", hash[i]);
    }
    hex[64] = '\0';
    return hex;
}

const char *nova_crypto_sha256_file(const char *path) {
    if (!path) return "0000000000000000000000000000000000000000000000000000000000000000";
    FILE *f = fopen(path, "rb");
    if (!f) return "0000000000000000000000000000000000000000000000000000000000000000";
    nova_sha256_ctx ctx;
    nova_sha256_init(&ctx);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        nova_sha256_update(&ctx, buf, n);
    }
    fclose(f);
    uint8_t hash[32];
    nova_sha256_final(&ctx, hash);
    char *hex = (char *)malloc(65);
    for (int i = 0; i < 32; i++) {
        sprintf(hex + (i * 2), "%02x", hash[i]);
    }
    hex[64] = '\0';
    return hex;
}

bool nova_crypto_verify_hash(const char *data, const char *expected_hash) {
    if (!data || !expected_hash) return false;
    const char *computed = nova_crypto_sha256(data);
    return strcmp(computed, expected_hash) == 0;
}
