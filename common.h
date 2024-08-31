//
// Created by 13918962841 on 2024/6/4.
//

#ifndef PPROXY_COMMON_H
#define PPROXY_COMMON_H

#include "http_parser.h"

#include <uv.h>
#include <uv/queue.h>

#include <stdlib.h>
#include <memory.h>

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif // #ifndef min

#define LOG_BUFFER (1 * 1024 * 1024)
#define KEEP_ALIVE (5000) // 5000 msecs.

typedef enum log_level {
  ERRO = 0, WARN, INFO, DEBG, TRAC
} log_level_t;

typedef struct logger {
  uv_thread_t tid;
  uv_loop_t loop;
  uv_timer_t timer;
  int running;
//  FILE *out;
  log_level_t limit;
  uv_mutex_t mtx;
  int  buf_idx;
  char buffer[2][LOG_BUFFER];
  int  wpos;
} logger_t;

extern logger_t logger_;

#define LOG(level, fmt, ...)                                            \
  if (level <= logger_.limit) {                                         \
    char tmp[1024]; int len;                                            \
    time_t ts; struct tm *t;                                            \
    time(&ts);                                                          \
    t = localtime(&ts);                                                 \
    len = sprintf(tmp, "%02d/%02d %02d:%02d:%02d "#level"] " fmt "\n",  \
                  t->tm_mon+1, t->tm_mday, t->tm_hour,                  \
                  t->tm_min, t->tm_sec, __VA_ARGS__);                   \
    if (len > 0) {                                                      \
      uv_mutex_lock(&logger_.mtx);                                      \
      memcpy(logger_.buffer[logger_.buf_idx] + logger_.wpos, tmp, len); \
      logger_.wpos += len;                                              \
      uv_mutex_unlock(&logger_.mtx);                                    \
    }                                                                   \
  }

// TRACE DEBUG INFO WARNING ERROR
// Log write interface.
#define LOGT(fmt, ...) LOG(TRAC, fmt, __VA_ARGS__)
#define LOGD(fmt, ...) LOG(DEBU, fmt, __VA_ARGS__)
#define LOGI(fmt, ...) LOG(INFO, fmt, __VA_ARGS__)
#define LOGW(fmt, ...) LOG(WARN, fmt, __VA_ARGS__)
#define LOGE(fmt, ...) LOG(ERRO, fmt, __VA_ARGS__)

void on_logger_run(void *arg) {
  logger_t *logger;

  logger = (logger_t *) arg;
  while (logger->running) {
    int curr_idx, wpos;

    uv_mutex_lock(&logger->mtx);
    wpos = logger->wpos;
    curr_idx = logger->buf_idx;
    logger->wpos = 0;
    logger->buf_idx = (logger->buf_idx + 1) % 2;
    uv_mutex_unlock(&logger->mtx);

    if (wpos != 0) {
      logger->buffer[curr_idx][wpos] = '\0';
      printf("%s", logger->buffer[curr_idx]);
      fflush(stdout);
    }

    uv_sleep(1000);
  }
}

void logger_init(logger_t *logger, log_level_t level) {
  memset(&logger_, 0, sizeof(logger_t));
  logger_.limit = level;

//  uv_loop_init(&logger->loop);
//  uv_timer_init(&logger_.loop, &logger_.timer);
  uv_mutex_init(&logger_.mtx);

//  uv_timer_start(&logger_.timer, on_logger_timer, 1000, 1000);

  logger_.running = 1;
  uv_thread_create(&logger->tid, on_logger_run, logger);
}

// Alloc memory with struct.
#define ALLOC_ST(t) \
  (t *) malloc(sizeof(t))


//////////////////////////////////////////////////////////////

//#ifdef UNIX

typedef struct thread_context {
  uv_thread_t tid;
  uv_loop_t loop;
  uv_async_t async;
  uv_timer_t timer;

  char txtip[128];

  QUEUE conns_handle;

  QUEUE connfree_handle;
  uv_mutex_t connfree_mtx;
} thread_context_t;

enum conn_state {
  kcs_init,
  kcs_handshake,
  kcs_established,
  kcs_closing,
};

typedef struct endpoint_data {
  char *name;
  uv_stream_t *self;
  uv_stream_t *peer;
  uv_buf_t buf;
} endpoint_data_t;

typedef struct connect_context {
  uv_connect_t conn_req;
  uint64_t last_active_time;

  uv_tcp_t client;
  endpoint_data_t client_data;

  uv_tcp_t target;
  endpoint_data_t target_data;

#define ENDPDATA(handle) ((endpoint_data_t *) (((uv_tcp_t *) (handle)) + 1))

  enum conn_state cs;
  char *serverhost;

  char noconnect;
  char *url;
  char tmp_field[128];
  char host[128];
  http_parser parser;

  QUEUE qhandle;
} connect_context_t;


static uv_key_t tctx_key_;

extern thread_context_t *tthis_context() {
  return (thread_context_t *) uv_key_get(&tctx_key_);
}

void close_connect(connect_context_t *ctx) {
  thread_context_t *tctx;

  if (ctx->cs == kcs_closing)
    return;

  ctx->cs = kcs_closing;
  uv_tcp_close_reset(&(ctx->target), NULL);
  uv_tcp_close_reset(&(ctx->client), NULL);

  QUEUE_REMOVE(&ctx->qhandle);

  tctx = tthis_context();
  uv_mutex_lock(&(tctx->connfree_mtx));
  QUEUE_ADD(&(tctx->connfree_handle), &ctx->qhandle);
  uv_mutex_unlock(&(tctx->connfree_mtx));

  uv_async_send(&(tctx->async));
}

static void on_twork(uv_async_t *async) {
  connect_context_t *conn_ctx;
  thread_context_t *ctx;
  QUEUE tmp;
  QUEUE *n, *h;

  ctx = tthis_context();

  uv_mutex_lock(&ctx->connfree_mtx);
  QUEUE_MOVE(&ctx->connfree_handle, &tmp);
  uv_mutex_unlock(&ctx->connfree_mtx);

  h = &ctx->connfree_handle;
  for ((n) = QUEUE_NEXT(h); (n) != (h); ) {
    conn_ctx = QUEUE_DATA(n, connect_context_t , qhandle);
    (n) = QUEUE_NEXT(n);
    QUEUE_REMOVE(&conn_ctx->qhandle);

    free(conn_ctx->target_data.buf.base);
    free(conn_ctx->client_data.buf.base);
    free(conn_ctx);
  }
}

static void on_check_alive(uv_timer_t *timer) {
  connect_context_t *conn_ctx;
  thread_context_t *ctx;
  QUEUE tmp;
  QUEUE *n, *h;

  ctx = tthis_context();

//  h = &ctx->conns_handle;
//  for ((n) = QUEUE_NEXT(h); (n) != (h); ) {
//    uint64_t diff;
//
//    conn_ctx = QUEUE_DATA(n, connect_context_t , qhandle);
//    (n) = QUEUE_NEXT(n);
//
//    diff = ctx->loop.time - conn_ctx->last_active_time;
//    if (diff < KEEP_ALIVE)
//      continue;
//
//    close_connect(conn_ctx);
//  }
}

extern thread_context_t *thread_ctx_init() {
  thread_context_t *ctx;

  ctx = ALLOC_ST(thread_context_t);
  uv_key_set(&tctx_key_, ctx);

  uv_loop_init(&ctx->loop);
  uv_async_init(&ctx->loop, &ctx->async, on_twork);
  uv_mutex_init(&(ctx->connfree_mtx));
  uv_timer_init(&ctx->loop, &ctx->timer);

  uv_timer_start(&ctx->timer, on_check_alive, 5000, 5000);

  QUEUE_INIT(&(ctx->conns_handle));
  QUEUE_INIT(&(ctx->connfree_handle));

  return ctx;
}

extern void thread_ctx_destroy() {
  thread_context_t *ctx = tthis_context();

  uv_mutex_destroy(&(ctx->connfree_mtx));

  free(ctx);
}

extern char *strip(uv_tcp_t *tcp) {
  char *ip = tthis_context()->txtip;

  struct sockaddr_in addr;
  int addrlen;
  uv_tcp_getsockname(tcp, (struct sockaddr *) &addr, &addrlen);
  uv_ip4_name(&addr, ip, 63);
  return ip;
}

//#endif

extern void encode(char *dst, const char *src, size_t len) {
  for (int i = 0; i < len; i++)
    dst[i] = (char) (src[i] ^ 0x82);
}

extern void make_addrinfo(char *host, int port, struct sockaddr_in *addr) {
  int ret;
  struct addrinfo hints, *res;

  memset(&hints, 0, sizeof(hints));

  hints.ai_family = AF_INET; // Use IPv4
  hints.ai_socktype = SOCK_STREAM;
  ret = getaddrinfo(host, NULL, &hints, &res);
  if (ret != 0) {
    uv_ip4_addr(host, port, addr);
    return;
  }

  memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
  addr->sin_port = htons(port);
  freeaddrinfo(res);
  uv_ip4_name(addr, host, strlen(host));
}

extern int extra_addr(connect_context_t *ctx, char **host, int *port) {
  char *pos, *endptr;
  if (ctx->noconnect) {
    *host = ctx->host;
    *port = (memcmp(ctx->url, "https", 5) == 0) ? 443 : 80;
    return 0;
  }

  pos = strchr(ctx->host, ':');
  if (pos != NULL && *pos == ':') {
    *pos = '\0';
    *host = ctx->host;
    *port = (int) strtol(pos + 1, &endptr, 10);
    return 0;
  }

  pos = strchr(ctx->url, ':');
  if (pos != NULL && *pos == ':') {
    *pos = '\0';
    *host = ctx->url;
    *port = (int) strtol(pos + 1, &endptr, 10);
    return 0;
  }

  return 1;
}

#endif //PPROXY_COMMON_H
