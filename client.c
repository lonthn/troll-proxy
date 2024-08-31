//
// Created by zeqi.luo on 2024/5/31.
//

#include "common.h"

#include <uv.h>

#include <stdlib.h>
#include <memory.h>
#include <time.h>

// Config macros.
#define SERVER_ADDR "0.0.0.0"
#define SERVER_PORT 8080

#define RELAY_ADDR "172.192.128.1"
#define RELAY_PORT 8080

#define THREAD_NUM 4
#define BUFFER_SIZE 65536

#define HANDSHAKE_REQ \
  "CONNECT 106.14.25.179:13500 HTTP/1.1\r\n" \
  "Host: 106.14.25.179:13500\r\n"            \
  "Proxy-Connection: keep-alive\r\n"         \
  "User-Agent: libuv-1.44.2\r\n"             \
  "\r\n"

#define HANDSHAKE_RES \
  "HTTP/1.1 200 Connection established\r\n\r\n"

void on_trun(void *arg);

void on_connection(uv_stream_t *server, int status);
void on_connect(uv_connect_t *req, int status);
void on_timer(uv_timer_t *timer);

void on_handshake_write(uv_write_t *req, int status);
void handshake_alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf);
void handshake_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

void poll_cb(uv_poll_t *poll, int status, int events);

void alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf);
void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
void write_cb(uv_write_t *req, int status);

void close_connect(connect_context_t *ctx);

static int threads_idx_;
static thread_context_t threads_[THREAD_NUM];
static connect_context_t conns_root_;
static struct sockaddr relay_addr_;
static char *identity_str_[2] = {
    "client", "relay"
};

logger_t logger_;

int main() {
  int err;
  uv_tcp_t server;
  struct sockaddr_in addr;
  thread_context_t *ctx;

  /* 这是一些初始化的操作 */
  uv_key_create(&tctx_key_);
  ctx = thread_ctx_init();
  logger_init(&logger_, INFO);
  uv_tcp_init(&(ctx->loop), &server);

  threads_idx_ = 0;
  for (int i = 0; i < THREAD_NUM; i++) {
    uv_thread_create(&(threads_[i].tid), on_trun, NULL);
  }

  /// 启动一个 tcp 的服务，接收需要被代理的客户端连接
  uv_ip4_addr(RELAY_ADDR, RELAY_PORT, (struct sockaddr_in *) &relay_addr_);
  uv_ip4_addr(SERVER_ADDR, SERVER_PORT, &addr);
  err = uv_tcp_bind(&server, (struct sockaddr *) &addr, 0);
  if (err != 0) {
    LOGE("Address bind error: %d", err);
    return err;
  }
  /* 开始监听连接 */
  uv_listen((uv_stream_t *) &server, 1024, on_connection);

  LOGI("Listening on port %d...", SERVER_PORT);

  uv_run(&ctx->loop, UV_RUN_DEFAULT);

  thread_ctx_destroy();

  uv_key_delete(&tctx_key_);
  return 0;
}

void on_trun(void *arg) {
  thread_context_t *ctx;

  ctx = thread_ctx_init();

  uv_run(&(ctx->loop), UV_RUN_DEFAULT);

  thread_ctx_destroy();
}

void on_connection(uv_stream_t *server, int status) {
  int err;

  thread_context_t *ctx;
  connect_context_t *conn_ctx;

  if (status < 0) {
    LOGE("New connection error %s", uv_strerror(status));
    return;
  }

  ctx = tthis_context();//&(threads_[threads_idx_]);
  threads_idx_ = (threads_idx_+1) % THREAD_NUM;

  conn_ctx = ALLOC_ST(connect_context_t);
  QUEUE_INIT(&(conn_ctx->qhandle));
  QUEUE_ADD(&ctx->conns_handle, &conn_ctx->qhandle);
  conn_ctx->cs = kcs_init;
  conn_ctx->client.data = conn_ctx;
  conn_ctx->target.data = conn_ctx;
  conn_ctx->client_data.name = identity_str_[0];
  conn_ctx->target_data.name = identity_str_[1];
  conn_ctx->client_data.buf = uv_buf_init(malloc(BUFFER_SIZE), BUFFER_SIZE);
  conn_ctx->target_data.buf  = uv_buf_init(malloc(BUFFER_SIZE), BUFFER_SIZE);
  conn_ctx->client_data.self = (uv_stream_t *) &(conn_ctx->client);
  conn_ctx->target_data.self  = (uv_stream_t *) &(conn_ctx->target);
  conn_ctx->client_data.peer = conn_ctx->target_data.self;
  conn_ctx->target_data.peer = conn_ctx->client_data.self;
  uv_tcp_init(server->loop, &(conn_ctx->client));
  uv_tcp_init(server->loop, &(conn_ctx->target));

  uv_accept(server, (uv_stream_t *) &(conn_ctx->client));
  LOGI("Accepted a new connection [%s].", strip(&(conn_ctx->client)));

  conn_ctx->conn_req.data = conn_ctx;
  err = uv_tcp_connect(&(conn_ctx->conn_req),
                       &(conn_ctx->target),
                       &relay_addr_,
                       on_connect);
  if (err != 0) {
    LOGE("Client[%s] post relay's connect error %d",
         strip(&(conn_ctx->client)), err);
    close_connect(conn_ctx);
  }

}

void on_connect(uv_connect_t *req, int status) {
  int err;
  connect_context_t *ctx;
  uv_write_t *write_req;
  uv_buf_t sendbuf;

  ctx = (connect_context_t *) req->data;
  if (status < 0) {
    LOGE("Client[%s] connect to relay error: %s",
         strip(&(ctx->client)), uv_strerror(status));
    close_connect(ctx);
    return;
  }

  LOGI("Client[%s] connected relay!", strip(&(ctx->client)));

  // 与中继服务器成功建立连接

  ctx->cs = kcs_handshake;
  write_req = malloc(sizeof(uv_write_t));
  write_req->data = ctx;

  sendbuf = uv_buf_init(HANDSHAKE_REQ, (uint32_t) strlen(HANDSHAKE_REQ));
  err = uv_write(write_req, ctx->target_data.self,
                 &sendbuf, 1, on_handshake_write);
  if (err != 0) {
    LOGE("Client[%s] post write-handshake-req error %d",
         strip(&(ctx->client)), err);
    close_connect(ctx);
  }
}

void on_handshake_write(uv_write_t *req, int status) {
  int err;
  connect_context_t *ctx;

  ctx = (connect_context_t *) req->data;

  if (status < 0) {
    LOGE("Client[%s] handshake write error: %s",
         strip(&ctx->client), uv_strerror(status));
    close_connect(ctx);
    goto label_return;
  }

  err = uv_read_start(ctx->target_data.self,
                      handshake_alloc_cb,
                      handshake_read_cb);
  if (err != 0) {
    LOGE("Client[%s] post read-handshake-req error %d",
         strip(&(ctx->client)), err);
    close_connect(ctx);
  }

  label_return:
  free(req);
}

void handshake_alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
  connect_context_t *conn_ctx = handle->data;
  *buf = conn_ctx->target_data.buf;
}

void handshake_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  int err;
  connect_context_t *ctx;
  endpoint_data_t *ed;

  ctx = (connect_context_t *) stream->data;
  ed = ENDPDATA(stream);
  if (nread <= 0) {
    LOGE("Client[%s] handshake read error: %s",
         strip(&(ctx->client)), uv_strerror((int) nread));

    if (nread == UV_EOF)
      close_connect(ctx);
    return;
  }

  int min_size = (int) strlen(HANDSHAKE_RES);
  if (memcmp(buf->base, HANDSHAKE_RES, min_size) != 0) {
    LOGE("Client[%s] handshake error: %s",
         strip(&(ctx->client)), buf->base);
    close_connect(ctx);
    return;
  }

  ctx->cs = kcs_established;
  LOGI("Client[%s] handshake successfully!", strip(&(ctx->client)));

  ctx->target.alloc_cb = alloc_cb;
  ctx->target.read_cb = read_cb;

  err = uv_read_start(ed->peer, alloc_cb, read_cb);
  if (err != 0) {
    LOGE("Client[%s] post read-client-req error: %s",
         strip(&(ctx->client)), uv_strerror(err));
    close_connect(ctx);
    return;
  }
}

void alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
  endpoint_data_t *ed;

  ed = ENDPDATA(handle);
  if (ed->buf.len < suggested) {
    ed->buf.base = realloc(ed->buf.base, suggested);
    ed->buf.len = (ULONG) suggested;
  }
  *buf = ed->buf;
}

void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  int err, idx;
  connect_context_t *ctx;
  endpoint_data_t *ed;
  uv_write_t *write_req;
  uv_buf_t sendbuf;

  ed = ENDPDATA(stream);
  ctx = (connect_context_t *) stream->data;
  if (nread <= 0) {
    LOGE("Client[%s] read from %s error: %s",
         strip(&(ctx->client)), ed->name,
         uv_strerror((int) nread));

    if (nread == UV_EOF)
      close_connect(ctx);
    return;
  }

  ctx->last_active_time = stream->loop->time;

  write_req = malloc(sizeof(uv_write_t) + nread);
  sendbuf = uv_buf_init((char *) (write_req + 1), (int) nread);
//  memcpy(sendbuf.base, buf->base, nread);
  encode(sendbuf.base, buf->base, nread);

  write_req->data = ctx;

  err = uv_write(write_req, ed->peer,
                 &sendbuf, 1, write_cb);

  if (err != 0) {
    LOGE("Client[%s] post write-req error: %s",
         strip(&(ctx->client)), uv_strerror(err));
    close_connect(ctx);
  }
}

void write_cb(uv_write_t *req, int status) {
  connect_context_t *ctx;

  ctx = (connect_context_t *) req->data;

  if (ctx == NULL
//   || status == -125
//   || status == UV__ECONNRESET
//   || status == UV__ECANCELED
   )
    goto label_return;

  if (status < 0) {
    LOGE("Client[%s] write error: %s",
         strip(&(ctx->client)), uv_strerror(status));
    close_connect(ctx);
    goto label_return;
  }

//  ctx->last_active_time = ctx->client.loop->time;

  label_return:
  free(req);
}

