//
// Created by zeqi.luo on 2024/5/31.
//

#include "common.h"

#include <uv.h>

#include <stdlib.h>
#include <memory.h>
#include <time.h>
//#include <pthread.h>

// Config macros.
#define SERVER_ADDR "0.0.0.0"
#define SERVER_PORT 13500

#define THREAD_NUM 4
#define BUFFER_SIZE 65536

#define MONITOR_CONTENT_FMT \
  "<html>" \
  "<h3>client incoming: %ld  -- outgoing: %ld</h3>" \
  "<h3>target incoming: %ld  -- outgoing: %ld</h3>" \
  "</html>"

#define MONITOR_RESPONSE_FMT \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Type: text/html\r\n" \
  "Content-Length: %ld\r\n" \
  "\r\n" \
  "%s" \

void on_trun(void *arg);

void on_connection(uv_stream_t *server, int status);
void on_connect(uv_connect_t *req, int status);

void on_handshake_write(uv_write_t *req, int status);
void on_handshake_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf);
void on_handshake_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

int on_url(http_parser*, const char *at, size_t length);
int on_header_field(http_parser*, const char *at, size_t length);
int on_header_value(http_parser*, const char *at, size_t length);
int on_message_complete(http_parser* parser);

void on_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf);
void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
void on_write(uv_write_t *req, int status);

void on_timer(uv_timer_t *timer);


static int threads_idx_;
static thread_context_t threads_[THREAD_NUM];
static http_parser_settings parser_setting_;
static char hs_ok_resp_[] = "HTTP/1.1 200 Connection established\r\n\r\n";

//static int64_t last_utps_;
//static int64_t last_dtps_;
//static int64_t utraffic_;
//static int64_t dtraffic_;

struct traffic {
  int64_t incoming;
  int64_t outgoing;
  int64_t itraffic;
  int64_t otraffic;
} traffic_sts[2];

char *identity_str_[2] = {
    "client", "target"
};

logger_t logger_;

int main(int argc, char **argv) {
  int err;
  uv_tcp_t server;
  uv_timer_t timer;
  struct sockaddr_in addr;
  thread_context_t *ctx;

  /* 这是一些初始化的操作 */
  uv_key_create(&tctx_key_);
  ctx = thread_ctx_init();

  logger_init(&logger_, INFO);

  http_parser_settings_init(&parser_setting_);
  parser_setting_.on_url = on_url;
  parser_setting_.on_header_field = on_header_field;
  parser_setting_.on_header_value = on_header_value;
  parser_setting_.on_message_complete = on_message_complete;

  encode(hs_ok_resp_, hs_ok_resp_, strlen(hs_ok_resp_));

  threads_idx_ = 0;
  for (int i = 0; i < THREAD_NUM; i++) {
    uv_thread_create(&(threads_[i].tid), on_trun, NULL);
  }

  // 开启流量统计定时器
  memset(traffic_sts, 0, sizeof(struct traffic) * 2);
  uv_timer_init(&ctx->loop, &timer);
  uv_timer_start(&timer, on_timer, 1000, 1000);

  /* 启动一个 tcp 的服务，接收需要被代理的客户端连接 */
  uv_tcp_init(&ctx->loop, &server);

  uv_ip4_addr(SERVER_ADDR, SERVER_PORT, &addr);
  err = uv_tcp_bind(&server, (struct sockaddr *) &addr, 0);
  if (err != 0) {
    LOGE("Addres bind error: %d", err);
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
  connect_context_t *cpair;
  thread_context_t *ctx;

  if (status < 0) {
    LOGE("New connection error %s", uv_strerror(status));
    return;
  }

  ctx = tthis_context();

  cpair = ALLOC_ST(connect_context_t);
  cpair->cs = kcs_init;
  QUEUE_INIT(&cpair->qhandle);
  QUEUE_ADD(&ctx->conns_handle, &cpair->qhandle);
  cpair->client_data.name = identity_str_[0];
  cpair->target_data.name = identity_str_[1];
  cpair->client_data.buf = uv_buf_init(malloc(BUFFER_SIZE), 0);
  cpair->target_data.buf = uv_buf_init(malloc(BUFFER_SIZE), 0);
  cpair->client_data.self  = (uv_stream_t *) &(cpair->client);
  cpair->target_data.self   = (uv_stream_t *) &(cpair->target);
  cpair->client_data.peer  = cpair->target_data.self;
  cpair->target_data.peer   = cpair->client_data.self;

  uv_tcp_init(server->loop, &(cpair->client));
  uv_tcp_init(server->loop, &(cpair->target));
  http_parser_init(&cpair->parser, HTTP_REQUEST);
  cpair->parser.data = cpair;
  cpair->client.data = cpair;
  cpair->target.data = cpair;

  uv_accept(server, (uv_stream_t *) &(cpair->client));
  LOGI("Accepted a new connection [%s].", strip(&(cpair->client)));

  err = uv_read_start(cpair->client_data.self,
                      on_handshake_alloc,
                      on_handshake_read);
  if (err != 0) {
    LOGE("Client[%s] post read-handshake-req error %d",
         strip(&(cpair->client)), err);
    close_connect(cpair);
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

  // 与目标服务器成功建立连接
  ctx->cs = kcs_established;
  write_req = ALLOC_ST(uv_write_t);
  write_req->data = ctx;

  if (ctx->noconnect) {
    sendbuf = ctx->client_data.buf;
    err = uv_write(write_req, ctx->target_data.self,
                   &sendbuf, 1, on_handshake_write);
  } else {
    sendbuf = uv_buf_init(hs_ok_resp_, strlen(hs_ok_resp_));
    err = uv_write(write_req, ctx->client_data.self,
                   &sendbuf, 1, on_handshake_write);
  }

  if (err != 0) {
    LOGE("Client[%s] post write-handshake-res error %d",
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

  ctx->client_data.buf.len = BUFFER_SIZE;
  ctx->target_data.buf.len = BUFFER_SIZE;

  ctx->cs = kcs_established;
  LOGI("Client[%s] handshake successfully!",
       strip(&(ctx->client)));

  ctx->client.alloc_cb = on_alloc;
  ctx->client.read_cb = on_read;

  err = uv_read_start(ctx->target_data.self,
                      on_alloc,
                      on_read);
  if (err != 0) {
    LOGE("Client[%s] post read-target-req error %d",
         strip(&(ctx->client)), err);
    close_connect(ctx);
  }

  label_return:
  free(req);
}

void on_handshake_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
  connect_context_t *ctx;

  ctx = (connect_context_t *) handle->data;
  *buf = ctx->client_data.buf;
  buf->base += buf->len;
  buf->len = BUFFER_SIZE - buf->len;
}

void on_handshake_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  size_t nparserd;
  connect_context_t *ctx;

  ctx = (connect_context_t *) stream->data;
  if (nread <= 0) {
    LOGE("Client[%s] handshake read error: %s",
         strip(&(ctx->client)), uv_strerror(nread));
    if (nread == UV_EOF) {
      close_connect(ctx);
    }
    return;
  }

  ctx->client_data.buf.len += nread;
  if (memcmp(buf->base, "GET", 3) != 0)
    encode(buf->base, buf->base, nread);

  nparserd = http_parser_execute(&ctx->parser, &parser_setting_,
                                 ctx->client_data.buf.base,
                                 ctx->client_data.buf.len);
  if (nparserd != ctx->client_data.buf.len) {
    LOGE("Client[%s] handshake error: %s",
         strip(&(ctx->client)),
         http_errno_description(ctx->parser.http_errno))
    ctx->client_data.buf.base[ctx->client_data.buf.len] = '\0';
    LOGE("error request: %s", ctx->client_data.buf.base)
    close_connect(ctx);
    return;
  }
}

int on_url(http_parser* parser, const char *at, size_t length) {
  connect_context_t *ctx = parser->data;
  ctx->url = malloc(length + 1);
  ctx->url[length] = '\0';
  memcpy(ctx->url, at, length);
  return 0;
}

int on_header_field(http_parser* parser, const char *at, size_t length) {
  int copylen;
  connect_context_t *ctx = parser->data;
  copylen = min(length, sizeof(ctx->tmp_field) - 1);
  memcpy(ctx->tmp_field, at, copylen);
  ctx->tmp_field[copylen] = '\0';
  return 0;
}

int on_header_value(http_parser* parser, const char *at, size_t length) {
  int copylen;
  connect_context_t *ctx = parser->data;
  if (memcmp(ctx->tmp_field, "Host", 4) != 0)
    return 0;

  copylen = min(length, sizeof(ctx->host));
  memcpy(ctx->host, at, length);
  ctx->host[copylen] = '\0';
  return 0;
}

int on_message_complete(http_parser* parser) {
  int ret;
  char *pos;
  char *host;
  int port;
  char *endptr;
  struct sockaddr_in target_addr;

  connect_context_t *ctx = parser->data;

  ctx->noconnect = (char) (parser->method != HTTP_CONNECT);
  if (extra_addr(ctx, &host, &port) != 0) {
      LOGE("Client[%s] invalid handshake request: %s",
           strip(&(ctx->client)), http_method_str(parser->method));
      return 1;
  }

  if (parser->method == HTTP_GET && memcmp(ctx->url, "/monitor", 8) == 0) {
    char content[256];
    uv_write_t *req;
    uv_buf_t sendbuf;

    sprintf(content,
            "<html>"
            "<h3>client incoming: %ld  -- outgoing: %ld</h3>"
            "<h3>target incoming: %ld  -- outgoing: %ld</h3>"
            "</html>",
            traffic_sts[0].incoming, traffic_sts[0].outgoing,
            traffic_sts[1].incoming, traffic_sts[1].outgoing);

    sprintf(ctx->target_data.buf.base,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %lu\r\n"
            "\r\n"
            "%s", strlen(content), content);

    req = ALLOC_ST(uv_write_t);
    sendbuf = uv_buf_init(
        ctx->target_data.buf.base,
        strlen(ctx->target_data.buf.base)
    );

    req->data = ctx;

    uv_write(req, ctx->client_data.self, &sendbuf, 1, on_write);
    return 0;
  }

  LOGI("Client[%s] post-connect-req %s:%d",
       strip(&(ctx->client)), host, port);

  make_addrinfo(host, port, &target_addr);

  ctx->conn_req.data = ctx;
  ret = uv_tcp_connect(&(ctx->conn_req),
                       &(ctx->target),
                       (struct sockaddr *) &target_addr,
                       on_connect);
  if (ret != 0) {
    LOGE("Client[%s] post relay's connect error %d",
         strip(&(ctx->client)), ret);
    return 1;
  }

  return 0;
}

void on_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
  endpoint_data_t *edata;

  edata = ENDPDATA(handle);
  if (edata->buf.len < suggested) {
    edata->buf.base = realloc(edata->buf.base, suggested);
    edata->buf.len = suggested;
    LOGI("Warning: realloc recv buffer[%s]", edata->name);
  }

  *buf = edata->buf;
}

void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  int err;
  endpoint_data_t *edata;
  connect_context_t *ctx;
  uv_buf_t sendbuf;

  edata = ENDPDATA(stream);
  ctx = (connect_context_t *) stream->data;
  if (nread <= 0) {
    LOGE("Client[%s] read %s error: %s",
         strip(&(ctx->client)),
         edata->name,
         uv_strerror(nread));

    if (nread == UV_EOF)
      close_connect(ctx);

    return;
  }

  uv_write_t *write_req = malloc(sizeof(uv_write_t) + 1 + sizeof(int) + nread);

  if (ctx->client_data.self == stream) {
    traffic_sts[0].itraffic += nread;
    *((char *) (write_req + 1)) = 1;
  } else {
    traffic_sts[1].itraffic += nread;
    *((char *) (write_req + 1)) = 0;
  }

  char *base = (char *) (write_req + 1) + 1;
  *((int *) base) = (int) nread;

  sendbuf = uv_buf_init(base + sizeof(int), nread);
  encode(sendbuf.base, buf->base, nread);

  write_req->data = ctx;
  err = uv_write(write_req, edata->peer,
                 &sendbuf, 1, on_write);
  if (err != 0) {
    LOGE("Client[%s] post write-req error: %s",
         strip(&(ctx->client)), uv_strerror(err))
    close_connect(ctx);
  }
}

void on_write(uv_write_t *req, int status) {
  connect_context_t *ctx;

  ctx = (connect_context_t *) req->data;
  if (status == -125)
    goto label_return;

  char *ptr = (char *) (req + 1);
  traffic_sts[*ptr].otraffic = *((int *) (ptr + 1));

  if (status < 0) {
    LOGE("Client[%s] write error: %s",
         strip(&(ctx->client)), uv_strerror(errno));
    close_connect(ctx);
//    goto label_return;
  }

  label_return:
  free(req);
}

void on_timer(uv_timer_t *timer) {
  traffic_sts[0].incoming = traffic_sts[0].itraffic;
  traffic_sts[0].outgoing = traffic_sts[0].otraffic;
  traffic_sts[1].incoming = traffic_sts[1].itraffic;
  traffic_sts[1].outgoing = traffic_sts[1].otraffic;
  traffic_sts[0].itraffic = 0;
  traffic_sts[0].otraffic = 0;
  traffic_sts[1].itraffic = 0;
  traffic_sts[1].otraffic = 0;
}