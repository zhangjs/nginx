
/*
 * Copyright (C) Maxim Dounin
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_proxy_module.h>


#define NGX_HTTP_PROXY_V2_MAX_CONTROL_BUFS  1000
#define NGX_HTTP_PROXY_V2_READ_BUFFER_SIZE  65536
#define NGX_HTTP_PROXY_V2_READ_BUDGET       262144
#define NGX_HTTP_PROXY_V2_READ_STEPS        64
#define NGX_HTTP_PROXY_V2_DATA_QUEUE_SIZE   NGX_HTTP_V2_DEFAULT_WINDOW
#define NGX_HTTP_PROXY_V2_WRITE_BATCH       262144
#define NGX_HTTP_PROXY_V2_CONTROL_BUDGET    16384
#define NGX_HTTP_PROXY_V2_MAX_STREAM_ID     0x7fffffff

#define NGX_HTTP_PROXY_V2_NO_ERROR          0x0
#define NGX_HTTP_PROXY_V2_REFUSED_STREAM    0x7
#define NGX_HTTP_PROXY_V2_CANCEL            0x8


typedef enum {
    ngx_http_proxy_v2_st_start = 0,
    ngx_http_proxy_v2_st_length_2,
    ngx_http_proxy_v2_st_length_3,
    ngx_http_proxy_v2_st_type,
    ngx_http_proxy_v2_st_flags,
    ngx_http_proxy_v2_st_stream_id,
    ngx_http_proxy_v2_st_stream_id_2,
    ngx_http_proxy_v2_st_stream_id_3,
    ngx_http_proxy_v2_st_stream_id_4,
    ngx_http_proxy_v2_st_payload,
    ngx_http_proxy_v2_st_padding
} ngx_http_proxy_v2_state_e;


typedef struct ngx_http_proxy_v2_stream_s  ngx_http_proxy_v2_stream_t;


/*
 * The session reads into chunks; DATA payload is queued to streams by
 * reference, so it is copied once, to the request buffers.  A chunk is
 * reused when no stream references it anymore.
 */

typedef struct ngx_http_proxy_v2_chunk_s  ngx_http_proxy_v2_chunk_t;

struct ngx_http_proxy_v2_chunk_s {
    ngx_http_proxy_v2_chunk_t     *next;
    ngx_uint_t                     refs;
    u_char                        *start;
    u_char                        *end;
};


typedef struct ngx_http_proxy_v2_segment_s  ngx_http_proxy_v2_segment_t;

struct ngx_http_proxy_v2_segment_s {
    ngx_http_proxy_v2_segment_t   *next;
    ngx_http_proxy_v2_chunk_t     *chunk;
    u_char                        *pos;
    u_char                        *last;
};


/*
 * Only the session writes to the transport.  The wire is the queue of
 * frames composed for writing, in order; an entry refers to a stream
 * buffer, recycled to the stream once written, or to a session buffer.
 */

typedef struct ngx_http_proxy_v2_out_s  ngx_http_proxy_v2_out_t;

struct ngx_http_proxy_v2_out_s {
    ngx_chain_t                    chain;
    ngx_http_proxy_v2_out_t       *next;
    ngx_http_proxy_v2_stream_t    *stream;
    ngx_chain_t                   *link;
    off_t                          size;
};


typedef struct ngx_http_proxy_v2_header_block_s
    ngx_http_proxy_v2_header_block_t;

struct ngx_http_proxy_v2_header_block_s {
    ngx_http_proxy_v2_header_block_t  *next;
    ngx_list_t                         fields;
    ngx_uint_t                         status;
    size_t                             length;

    /* raw header frames, see ngx_http_proxy_v2_record_header() */
    size_t                             raw_offset;
    size_t                             raw_length;

    unsigned                           end_stream:1;
};


typedef struct {
    ngx_connection_t              *connection;

    /* attached streams, in order, and the index of committed ones */
    ngx_queue_t                    attached;
    ngx_uint_t                     seq;
    ngx_rbtree_t                   streams;
    ngx_rbtree_node_t              sentinel;

    ngx_buf_t                      buffer;
    ngx_http_proxy_v2_chunk_t     *chunk;
    ngx_http_proxy_v2_chunk_t     *free_chunks;
    off_t                          received;

    /* NGX_DONE is EOF; NGX_HTTP_UPSTREAM_INVALID_HEADER is a parse error */
    ngx_int_t                      read_status;

    ngx_chain_t                   *wire;
    ngx_chain_t                  **wire_last;
    ngx_http_proxy_v2_out_t       *wire_free;
    off_t                          wire_size;
    off_t                          send_limit;
    ngx_uint_t                     cursor;
    ngx_buf_t                      preface;
    size_t                         send_lowat;

#if (NGX_HTTP_SSL)
    ngx_connection_handler_pt      original_save_session;
#endif

    ngx_chain_t                   *out;
    ngx_chain_t                  **last_out;
    ngx_chain_t                   *free;
    ngx_uint_t                     allocated;

    size_t                         init_window;
    size_t                         send_window;
    size_t                         recv_window;
    ngx_uint_t                     last_stream_id;

    ngx_http_proxy_v2_state_e      state;
    ngx_uint_t                     frame_state;

    size_t                         rest;
    ngx_uint_t                     stream_id;
    ngx_uint_t                     continuation_sid;
    u_char                         type;
    u_char                         flags;
    u_char                         padding;

    ngx_uint_t                     window_update;
    ngx_uint_t                     setting_id;
    ngx_uint_t                     setting_value;
    ngx_uint_t                     goaway_last_stream_id;
    ngx_uint_t                     goaway_id;
    ngx_uint_t                     goaway_error;
    u_char                         ping_data[8];

    unsigned                       goaway:1;
    unsigned                       read_gated:1;

    /* replays header frames stored in a cache file, without a connection */
    unsigned                       cached:1;
} ngx_http_proxy_v2_session_t;


struct ngx_http_proxy_v2_stream_s {
    ngx_connection_t                    connection;
    ngx_event_t                         read;
    ngx_event_t                         write;

    ngx_queue_t                         queue;
    ngx_uint_t                          seq;
    ngx_rbtree_node_t                   node;

    ngx_uint_t                          fragment_state;

    ngx_chain_t                        *in;
    ngx_chain_t                        *free;
    ngx_uint_t                          queued;

    ngx_http_proxy_v2_session_t        *session;

    ngx_http_proxy_v2_header_block_t   *header_block;
    ngx_http_proxy_v2_header_block_t   *headers;
    ngx_http_proxy_v2_header_block_t  **last_headers;
    ngx_http_proxy_v2_header_block_t   *trailers;

    ngx_http_proxy_v2_segment_t        *data;
    ngx_http_proxy_v2_segment_t       **data_last;
    ngx_http_proxy_v2_segment_t        *data_free;
    size_t                              data_size;
    off_t                               data_parsed;
    off_t                               received_start;
    off_t                               received;

    ngx_uint_t                          id;
    ngx_uint_t                          rst_received_error;

    ssize_t                             send_window;
    size_t                              recv_window;

    ngx_uint_t                          index;
    ngx_str_t                           name;
    ngx_str_t                           value;

    u_char                             *field_end;
    size_t                              header_buffer_size;

    /* raw response header frames, copied to u->buffer for caching */
    u_char                             *raw;
    size_t                              raw_len;
    size_t                              raw_block;
    unsigned                            raw_overflow:1;

    size_t                              header_limit;
    size_t                              field_length;
    size_t                              field_rest;
    u_char                              field_state;

    unsigned                            literal:1;
    unsigned                            field_huffman:1;

    unsigned                            header_sent:1;
    unsigned                            output_closed:1;
    unsigned                            output_blocked:1;
    unsigned                            output_discarded:1;
    unsigned                            parsing_headers:1;
    unsigned                            end_stream:1;
    unsigned                            done:1;
    unsigned                            headers_parsed:1;
    unsigned                            rst_parsed:1;
    unsigned                            rst_received:1;
    unsigned                            unprocessed:1;

    /* not processed, without a failure of the upstream */
    unsigned                            graceful:1;
};


typedef struct {
    ngx_http_proxy_ctx_t           ctx;
    off_t                          length;
    off_t                          data_used;

    ngx_http_proxy_v2_stream_t     stream;

    unsigned                       body_started:1;
} ngx_http_proxy_v2_ctx_t;


typedef struct {
    u_char                        length_0;
    u_char                        length_1;
    u_char                        length_2;
    u_char                        type;
    u_char                        flags;
    u_char                        stream_id_0;
    u_char                        stream_id_1;
    u_char                        stream_id_2;
    u_char                        stream_id_3;
} ngx_http_proxy_v2_frame_t;


static ngx_int_t ngx_http_proxy_v2_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_proxy_v2_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_proxy_v2_init_connection(ngx_http_request_t *r,
    ngx_http_upstream_t *u);
static void ngx_http_proxy_v2_free_connection(ngx_http_request_t *r,
    ngx_http_upstream_t *u, ngx_int_t rc);
static ngx_int_t ngx_http_proxy_v2_init_session(
    ngx_http_proxy_v2_session_t *sess, off_t limit, size_t lowat);
static void ngx_http_proxy_v2_release_session(
    ngx_http_proxy_v2_session_t *sess, void *data);
static void ngx_http_proxy_v2_init_stream(ngx_http_proxy_v2_stream_t *stream,
    ngx_http_request_t *r);
static void ngx_http_proxy_v2_session_read_handler(ngx_event_t *ev);
static void ngx_http_proxy_v2_session_write_handler(ngx_event_t *ev);
static void ngx_http_proxy_v2_session_notify_read(
    ngx_http_proxy_v2_session_t *sess);
static ngx_int_t ngx_http_proxy_v2_session_handle_events(
    ngx_http_proxy_v2_session_t *sess);
static void ngx_http_proxy_v2_update_read_event(
    ngx_http_proxy_v2_stream_t *stream);
static void ngx_http_proxy_v2_notify_read(ngx_http_proxy_v2_stream_t *stream);
static void ngx_http_proxy_v2_update_write_event(
    ngx_http_proxy_v2_stream_t *stream);
static ssize_t ngx_http_proxy_v2_recv(ngx_connection_t *c, u_char *buf,
    size_t size);
static ssize_t ngx_http_proxy_v2_recv_chain(ngx_connection_t *c,
    ngx_chain_t *in, off_t limit);
static size_t ngx_http_proxy_v2_read_data(ngx_http_proxy_v2_stream_t *stream,
    u_char *buf, size_t size);
static ssize_t ngx_http_proxy_v2_recv_status(
    ngx_http_proxy_v2_stream_t *stream);
static void ngx_http_proxy_v2_clear_event(ngx_event_t *ev);
static ngx_int_t ngx_http_proxy_v2_reset_buffer(
    ngx_http_proxy_v2_session_t *sess);
static void ngx_http_proxy_v2_release_chunk(ngx_http_proxy_v2_session_t *sess,
    ngx_http_proxy_v2_chunk_t *chunk);
static void ngx_http_proxy_v2_release_data(ngx_http_proxy_v2_stream_t *stream);
#if (NGX_HTTP_SSL)
static void ngx_http_proxy_v2_ssl_save_session(ngx_connection_t *c);
#endif
static ngx_int_t ngx_http_proxy_v2_body_output_filter(void *data,
    ngx_chain_t *in);
static ngx_int_t ngx_http_proxy_v2_session_write(
    ngx_http_proxy_v2_session_t *sess);
static ngx_int_t ngx_http_proxy_v2_compose(ngx_http_proxy_v2_session_t *sess);
static ngx_int_t ngx_http_proxy_v2_compose_stream(
    ngx_http_proxy_v2_session_t *sess, ngx_http_proxy_v2_stream_t *stream);
static ngx_int_t ngx_http_proxy_v2_compose_data(
    ngx_http_proxy_v2_session_t *sess, ngx_http_proxy_v2_stream_t *stream);
static ngx_chain_t *ngx_http_proxy_v2_get_slice(
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *buf);
static ngx_int_t ngx_http_proxy_v2_wire_add(ngx_http_proxy_v2_session_t *sess,
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b, ngx_chain_t *link);
static void ngx_http_proxy_v2_wire_complete(ngx_http_proxy_v2_session_t *sess,
    ngx_chain_t *chain);
static void ngx_http_proxy_v2_wire_drop(ngx_http_proxy_v2_session_t *sess);
static ngx_uint_t ngx_http_proxy_v2_output_pending(
    ngx_http_proxy_v2_session_t *sess);
static ngx_uint_t ngx_http_proxy_v2_input_pending(
    ngx_http_proxy_v2_session_t *sess);
static ngx_uint_t ngx_http_proxy_v2_need_read(
    ngx_http_proxy_v2_session_t *sess);
static ngx_uint_t ngx_http_proxy_v2_stream_input_consumed(
    ngx_http_proxy_v2_ctx_t *ctx);
static ngx_uint_t ngx_http_proxy_v2_stream_input_done(
    ngx_http_proxy_v2_ctx_t *ctx);
static ngx_uint_t ngx_http_proxy_v2_stream_output_done(
    ngx_http_proxy_v2_stream_t *stream);
static ngx_uint_t ngx_http_proxy_v2_session_can_keepalive(
    ngx_http_proxy_v2_session_t *sess);
static ngx_uint_t ngx_http_proxy_v2_can_keepalive(ngx_http_proxy_v2_ctx_t *ctx);
static ngx_uint_t ngx_http_proxy_v2_close_stream(ngx_http_request_t *r,
    ngx_http_proxy_v2_stream_t *stream);
static ngx_int_t ngx_http_proxy_v2_finish_body(ngx_http_request_t *r,
    ngx_http_proxy_v2_ctx_t *ctx);
static ngx_int_t ngx_http_proxy_v2_process_header(ngx_http_request_t *r);
#if (NGX_HTTP_CACHE)
static ngx_int_t ngx_http_proxy_v2_process_cached_header(ngx_http_request_t *r);
#endif
static ngx_int_t ngx_http_proxy_v2_record_header(
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b, ngx_uint_t new_frame);
static ngx_int_t ngx_http_proxy_v2_parse_headers(
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b, ngx_uint_t new_frame);
static ngx_int_t ngx_http_proxy_v2_process_headers(ngx_http_request_t *r,
    ngx_http_proxy_v2_stream_t *stream);
static ngx_int_t ngx_http_proxy_v2_process_trailers(ngx_http_request_t *r,
    ngx_http_proxy_v2_stream_t *stream);
static ngx_int_t ngx_http_proxy_v2_copy_headers(
    ngx_http_proxy_v2_header_block_t *block, ngx_list_t *fields);
static ngx_int_t ngx_http_proxy_v2_filter_init(void *data);
static ngx_int_t ngx_http_proxy_v2_non_buffered_filter(void *data,
    ssize_t bytes);
static ngx_int_t ngx_http_proxy_v2_process_body(ngx_http_request_t *r,
    ngx_http_proxy_v2_ctx_t *ctx);
static ngx_int_t ngx_http_proxy_v2_body_filter(ngx_event_pipe_t *p,
    ngx_buf_t *buf);
static ngx_int_t ngx_http_proxy_v2_parse_data(
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_process_data(ngx_http_request_t *r,
    ngx_http_proxy_v2_ctx_t *ctx, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_replenish_window(
    ngx_http_proxy_v2_stream_t *stream);
static ngx_int_t ngx_http_proxy_v2_session_process_frame(
    ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_process_connection_frame(
    ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_stream_process_frame(
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b, ngx_uint_t new_frame);
static ngx_int_t ngx_http_proxy_v2_stream_process_window_update(
    ngx_http_proxy_v2_session_t *sess, ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_skip_closed_frame(
    ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b, ngx_uint_t new_frame);
static ngx_int_t ngx_http_proxy_v2_charge_window(
    ngx_http_proxy_v2_session_t *sess);
static ngx_int_t ngx_http_proxy_v2_skip_frame(ngx_http_proxy_v2_session_t *sess,
    ngx_buf_t *b);

static ngx_int_t ngx_http_proxy_v2_parse_frame(
    ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_validate_frame(
    ngx_http_proxy_v2_session_t *sess);
static ngx_int_t ngx_http_proxy_v2_parse_header(
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_parse_fragment(
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_validate_header_name(ngx_str_t *s);
static ngx_int_t ngx_http_proxy_v2_validate_header_value(ngx_str_t *s);
static ngx_int_t ngx_http_proxy_v2_parse_rst_stream(
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_parse_goaway(
    ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_parse_window_update(
    ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_parse_settings(
    ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b);
static ngx_int_t ngx_http_proxy_v2_parse_ping(
    ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b);

static ngx_int_t ngx_http_proxy_v2_send_settings_ack(
    ngx_http_proxy_v2_session_t *sess);
static ngx_int_t ngx_http_proxy_v2_send_ping_ack(
    ngx_http_proxy_v2_session_t *sess);
static ngx_int_t ngx_http_proxy_v2_send_window_update(
    ngx_http_proxy_v2_session_t *sess, ngx_uint_t id, size_t n);
static ngx_int_t ngx_http_proxy_v2_send_rst_stream(
    ngx_http_proxy_v2_session_t *sess, ngx_uint_t id, ngx_uint_t error);

static ngx_chain_t *ngx_http_proxy_v2_get_buf(
    ngx_http_proxy_v2_stream_t *stream);
static ngx_chain_t *ngx_http_proxy_v2_get_control_buf(
    ngx_http_proxy_v2_session_t *sess);
static ngx_http_proxy_v2_stream_t *ngx_http_proxy_v2_get_stream(
    ngx_connection_t *c);
static off_t ngx_http_proxy_v2_stream_received(
    ngx_http_proxy_v2_stream_t *stream);
static ngx_http_proxy_v2_ctx_t *
    ngx_http_proxy_v2_get_ctx(ngx_http_request_t *r);
static ngx_http_proxy_v2_session_t *ngx_http_proxy_v2_get_session(
    ngx_peer_connection_t *pc);
static ngx_http_proxy_v2_session_t *ngx_http_proxy_v2_find_session(
    ngx_connection_t *c);
static ngx_http_proxy_v2_stream_t *ngx_http_proxy_v2_find_stream(
    ngx_http_proxy_v2_session_t *sess, ngx_uint_t id);
static ngx_http_proxy_v2_stream_t *ngx_http_proxy_v2_first_stream(
    ngx_http_proxy_v2_session_t *sess);
static ngx_http_proxy_v2_stream_t *ngx_http_proxy_v2_next_stream(
    ngx_http_proxy_v2_stream_t *stream);
static ngx_int_t ngx_http_proxy_v2_attach_stream(
    ngx_http_proxy_v2_session_t *sess, ngx_http_proxy_v2_stream_t *stream);
static void ngx_http_proxy_v2_detach_stream(ngx_http_proxy_v2_stream_t *stream);
static void ngx_http_proxy_v2_cleanup(void *data);

static void ngx_http_proxy_v2_abort_request(ngx_http_request_t *r);
static void ngx_http_proxy_v2_finalize_request(ngx_http_request_t *r,
    ngx_int_t rc);


static ngx_http_module_t  ngx_http_proxy_v2_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_proxy_v2_module = {
    NGX_MODULE_V1,
    &ngx_http_proxy_v2_module_ctx,         /* module context */
    NULL,                                  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static u_char  ngx_http_proxy_v2_connection_start[] =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"         /* connection preface */

    "\x00\x00\x12\x04\x00\x00\x00\x00\x00"     /* settings frame */
    "\x00\x01\x00\x00\x00\x00"                 /* header table size */
    "\x00\x02\x00\x00\x00\x00"                 /* disable push */
    "\x00\x04\x7f\xff\xff\xff"                 /* initial window */

    "\x00\x00\x04\x08\x00\x00\x00\x00\x00"     /* window update frame */
    "\x7f\xff\x00\x00";


ngx_int_t
ngx_http_proxy_v2_handler(ngx_http_request_t *r)
{
    ngx_int_t                    rc;
    ngx_http_upstream_t         *u;
    ngx_http_proxy_v2_ctx_t     *ctx;
    ngx_http_proxy_loc_conf_t   *plcf;
#if (NGX_HTTP_CACHE)
    ngx_http_proxy_main_conf_t  *pmcf;
#endif

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_proxy_v2_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_proxy_v2_module);

    ngx_http_set_ctx(r, &ctx->ctx, ngx_http_proxy_module);

    ctx->stream.last_headers = &ctx->stream.headers;
    ctx->stream.data_last = &ctx->stream.data;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);

    plcf->upstream.preserve_output = 1;
    plcf->upstream.ignore_input = 1;

    u = r->upstream;

    if (plcf->proxy_lengths == NULL) {
        ctx->ctx.vars = plcf->vars;
        u->schema = plcf->vars.schema;
#if (NGX_HTTP_SSL)
        u->ssl = plcf->ssl;
#endif

    } else {
        if (ngx_http_proxy_eval(r, &ctx->ctx, plcf) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

#if (NGX_HTTP_SSL)
    ngx_str_set(&u->ssl_alpn_protocol, NGX_HTTP_V2_ALPN_PROTO);
#endif

    u->output.tag = (ngx_buf_tag_t) &ngx_http_proxy_v2_module;

    u->conf = &plcf->upstream;

#if (NGX_HTTP_CACHE)
    pmcf = ngx_http_get_module_main_conf(r, ngx_http_proxy_module);

    u->caches = &pmcf->caches;
    u->create_key = ngx_http_proxy_create_key;
#endif

    u->create_request = ngx_http_proxy_v2_create_request;
    u->reinit_request = ngx_http_proxy_v2_reinit_request;
    u->init_connection = ngx_http_proxy_v2_init_connection;
    u->free_connection = ngx_http_proxy_v2_free_connection;
    u->process_header = ngx_http_proxy_v2_process_header;
    u->abort_request = ngx_http_proxy_v2_abort_request;
    u->finalize_request = ngx_http_proxy_v2_finalize_request;

    if (plcf->redirects) {
        u->rewrite_redirect = ngx_http_proxy_rewrite_redirect;
    }

    if (plcf->cookie_domains || plcf->cookie_paths || plcf->cookie_flags) {
        u->rewrite_cookie = ngx_http_proxy_rewrite_cookie;
    }

    u->buffering = plcf->upstream.buffering;

    u->pipe = ngx_pcalloc(r->pool, sizeof(ngx_event_pipe_t));
    if (u->pipe == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u->pipe->input_filter = ngx_http_proxy_v2_body_filter;
    u->pipe->input_ctx = r;

    u->input_filter_init = ngx_http_proxy_v2_filter_init;
    u->input_filter = ngx_http_proxy_v2_non_buffered_filter;
    u->input_filter_ctx = r;

    u->accel = 1;

    if (!plcf->upstream.request_buffering
        && plcf->body_values == NULL && plcf->upstream.pass_request_body)
    {
        r->request_body_no_buffering = 1;
    }

    rc = ngx_http_read_client_request_body(r, ngx_http_upstream_init);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}


static ngx_int_t
ngx_http_proxy_v2_create_request(ngx_http_request_t *r)
{
    u_char                       *p, *tmp, *key_tmp, *val_tmp, *headers_frame,
                                 *headers_end;
    size_t                        len, headers_len, tmp_len,
                                  key_len, val_len, uri_len,
                                  loc_len, body_len;
    uintptr_t                     escape;
    ngx_buf_t                    *b;
    ngx_str_t                     method, host;
    ngx_uint_t                    i, next, unparsed_uri;
    ngx_chain_t                  *cl, *body;
    ngx_list_part_t              *part;
    ngx_table_elt_t              *header;
    ngx_http_upstream_t          *u;
    ngx_http_proxy_v2_ctx_t      *ctx;
    ngx_http_script_code_pt       code;
    ngx_http_script_engine_t      e, le;
    ngx_http_proxy_headers_t     *headers;
    ngx_http_proxy_v2_frame_t    *f;
    ngx_http_proxy_loc_conf_t    *plcf;
    ngx_http_script_len_code_pt   lcode;

    u = r->upstream;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);

#if (NGX_HTTP_CACHE)
    headers = u->cacheable ? &plcf->headers_cache : &plcf->headers;
#else
    headers = &plcf->headers;
#endif

    if (u->method.len) {
        /* HEAD was changed to GET to cache response */
        method = u->method;

    } else if (plcf->method) {
        if (ngx_http_complex_value(r, plcf->method, &method) != NGX_OK) {
            return NGX_ERROR;
        }

    } else {
        method = r->method_name;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_proxy_v2_module);

    if (method.len == 4
        && ngx_strncasecmp(method.data, (u_char *) "HEAD", 4) == 0)
    {
        ctx->ctx.head = 1;
    }

    len = sizeof(ngx_http_proxy_v2_frame_t);               /* headers frame */

    headers_len = 0;

    /* :method header */

    if ((method.len == 3 && ngx_strncmp(method.data, "GET", 3) == 0)
        || (method.len == 4 && ngx_strncmp(method.data, "POST", 4) == 0))
    {
        len += 1;
        tmp_len = 0;

    } else {
        if (method.len > NGX_HTTP_V2_MAX_FIELD) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "too long http2 method: \"%V\"", &method);
            return NGX_ERROR;
        }

        len += 1 + NGX_HTTP_V2_INT_OCTETS + method.len;
        tmp_len = method.len;
    }

    /* :scheme header */

    len += 1;

    /* :path header */

    escape = 0;
    loc_len = 0;
    unparsed_uri = 0;

    if (plcf->proxy_lengths && ctx->ctx.vars.uri.len) {
        uri_len = ctx->ctx.vars.uri.len;

    } else if (ctx->ctx.vars.uri.len == 0 && r->valid_unparsed_uri) {
        unparsed_uri = 1;
        uri_len = r->unparsed_uri.len;

    } else {
        loc_len = (r->valid_location && ctx->ctx.vars.uri.len)
                  ? ngx_min(plcf->location.len, r->uri.len) : 0;

        if (r->quoted_uri || r->internal) {
            escape = 2 * ngx_escape_uri(NULL, r->uri.data + loc_len,
                                        r->uri.len - loc_len, NGX_ESCAPE_URI);
        }

        uri_len = ctx->ctx.vars.uri.len + r->uri.len - loc_len + escape
                  + sizeof("?") - 1 + r->args.len;
    }

    if (uri_len == 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "zero length URI to proxy");
        return NGX_ERROR;
    }

    if (uri_len > NGX_HTTP_V2_MAX_FIELD) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "too long http2 URI");
        return NGX_ERROR;
    }

    len += 1 + NGX_HTTP_V2_INT_OCTETS + uri_len;

    if (tmp_len < uri_len) {
        tmp_len = uri_len;
    }

    /* :authority header */

    host.len = 0;
#if (NGX_SUPPRESS_WARN)
    host.data = NULL;
#endif

    if (plcf->host_value
        && ngx_http_complex_value(r, plcf->host_value, &host) != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (host.len == 0) {
        host = ctx->ctx.vars.host_header;
    }

    if (host.len > NGX_HTTP_V2_MAX_FIELD) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "too long http2 host: \"%V\"", &host);
        return NGX_ERROR;
    }

    len += 1 + NGX_HTTP_V2_INT_OCTETS + host.len;

    if (tmp_len < host.len) {
        tmp_len = host.len;
    }

    /* other headers */

    ngx_memzero(&le, sizeof(ngx_http_script_engine_t));

    ngx_http_script_flush_no_cacheable_variables(r, plcf->body_flushes);
    ngx_http_script_flush_no_cacheable_variables(r, headers->flushes);

    body_len = 0;

    if (plcf->body_lengths) {
        le.ip = plcf->body_lengths->elts;
        le.request = r;
        le.flushed = 1;

        while (*(uintptr_t *) le.ip) {
            lcode = *(ngx_http_script_len_code_pt *) le.ip;
            body_len += lcode(&le);
        }

        ctx->ctx.internal_body_length = body_len;

    } else if (r->headers_in.chunked && r->reading_body) {
        ctx->ctx.internal_body_length = -1;

    } else {
        ctx->ctx.internal_body_length = r->headers_in.content_length_n;
    }

    le.ip = headers->lengths->elts;
    le.request = r;
    le.flushed = 1;

    while (*(uintptr_t *) le.ip) {

        lcode = *(ngx_http_script_len_code_pt *) le.ip;
        key_len = lcode(&le);

        for (val_len = 0; *(uintptr_t *) le.ip; val_len += lcode(&le)) {
            lcode = *(ngx_http_script_len_code_pt *) le.ip;
        }
        le.ip += sizeof(uintptr_t);

        if (val_len == 0) {
            continue;
        }

        if (key_len > NGX_HTTP_V2_MAX_FIELD) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "too long http2 header name");
            return NGX_ERROR;
        }

        if (val_len > NGX_HTTP_V2_MAX_FIELD) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "too long http2 header value");
            return NGX_ERROR;
        }

        headers_len += 1 + NGX_HTTP_V2_INT_OCTETS + key_len
                         + NGX_HTTP_V2_INT_OCTETS + val_len;

        if (tmp_len < key_len) {
            tmp_len = key_len;
        }

        if (tmp_len < val_len) {
            tmp_len = val_len;
        }
    }

    len += headers_len;

    if (plcf->upstream.pass_request_headers) {
        part = &r->headers_in.headers.part;
        header = part->elts;

        for (i = 0; /* void */; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }

                part = part->next;
                header = part->elts;
                i = 0;
            }

            if (ngx_hash_find(&headers->hash, header[i].hash,
                              header[i].lowcase_key, header[i].key.len))
            {
                continue;
            }

            if (header[i].key.len > NGX_HTTP_V2_MAX_FIELD) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "too long http2 header name: \"%V\"",
                              &header[i].key);
                return NGX_ERROR;
            }

            if (header[i].value.len > NGX_HTTP_V2_MAX_FIELD) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "too long http2 header value: \"%V: %V\"",
                              &header[i].key, &header[i].value);
                return NGX_ERROR;
            }

            len += 1 + NGX_HTTP_V2_INT_OCTETS + header[i].key.len
                     + NGX_HTTP_V2_INT_OCTETS + header[i].value.len;

            if (tmp_len < header[i].key.len) {
                tmp_len = header[i].key.len;
            }

            if (tmp_len < header[i].value.len) {
                tmp_len = header[i].value.len;
            }
        }
    }

    /* continuation frames */

    len += sizeof(ngx_http_proxy_v2_frame_t)
           * (len / NGX_HTTP_V2_DEFAULT_FRAME_SIZE);


    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    tmp = ngx_palloc(r->pool, tmp_len * 3);
    if (tmp == NULL) {
        return NGX_ERROR;
    }

    key_tmp = tmp + tmp_len;
    val_tmp = tmp + 2 * tmp_len;

    /* headers frame */

    headers_frame = b->last;

    f = (ngx_http_proxy_v2_frame_t *) b->last;
    b->last += sizeof(ngx_http_proxy_v2_frame_t);

    f->length_0 = 0;
    f->length_1 = 0;
    f->length_2 = 0;
    f->type = NGX_HTTP_V2_HEADERS_FRAME;
    f->flags = 0;
    f->stream_id_0 = 0;
    f->stream_id_1 = 0;
    f->stream_id_2 = 0;
    f->stream_id_3 = 1;

    if (method.len == 3 && ngx_strncmp(method.data, "GET", 3) == 0) {
        *b->last++ = ngx_http_v2_indexed(NGX_HTTP_V2_METHOD_GET_INDEX);

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http proxy header: \":method: GET\"");

    } else if (method.len == 4 && ngx_strncmp(method.data, "POST", 4) == 0) {
        *b->last++ = ngx_http_v2_indexed(NGX_HTTP_V2_METHOD_POST_INDEX);

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http proxy header: \":method: POST\"");

    } else {
        *b->last++ = ngx_http_v2_inc_indexed(NGX_HTTP_V2_METHOD_INDEX);
        b->last = ngx_http_v2_write_value(b->last, method.data,
                                          method.len, tmp);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http proxy header: \":method: %V\"", &method);
    }

#if (NGX_HTTP_SSL)
    if (u->ssl) {
        *b->last++ = ngx_http_v2_indexed(NGX_HTTP_V2_SCHEME_HTTPS_INDEX);

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http proxy header: \":scheme: https\"");
    } else
#endif
    {
        *b->last++ = ngx_http_v2_indexed(NGX_HTTP_V2_SCHEME_HTTP_INDEX);

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http proxy header: \":scheme: http\"");
    }

    if (plcf->proxy_lengths && ctx->ctx.vars.uri.len) {

        *b->last++ = ngx_http_v2_inc_indexed(NGX_HTTP_V2_PATH_INDEX);
        b->last = ngx_http_v2_write_value(b->last, ctx->ctx.vars.uri.data,
                                          ctx->ctx.vars.uri.len, tmp);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http proxy header: \":path: %V\"", &ctx->ctx.vars.uri);

    } else if (unparsed_uri) {

        if (r->unparsed_uri.len == 1 && r->unparsed_uri.data[0] == '/') {
            *b->last++ = ngx_http_v2_indexed(NGX_HTTP_V2_PATH_ROOT_INDEX);

        } else {
            *b->last++ = ngx_http_v2_inc_indexed(NGX_HTTP_V2_PATH_INDEX);
            b->last = ngx_http_v2_write_value(b->last, r->unparsed_uri.data,
                                              r->unparsed_uri.len, tmp);
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http proxy header: \":path: %V\"", &r->unparsed_uri);

    } else {
        p = val_tmp;

        if (r->valid_location) {
            p = ngx_copy(p, ctx->ctx.vars.uri.data, ctx->ctx.vars.uri.len);
        }

        if (escape) {
            ngx_escape_uri(p, r->uri.data + loc_len,
                           r->uri.len - loc_len, NGX_ESCAPE_URI);
            p += r->uri.len - loc_len + escape;

        } else {
            p = ngx_copy(p, r->uri.data + loc_len, r->uri.len - loc_len);
        }

        if (r->args.len > 0) {
            *p++ = '?';
            p = ngx_copy(p, r->args.data, r->args.len);
        }

        *b->last++ = ngx_http_v2_inc_indexed(NGX_HTTP_V2_PATH_INDEX);
        b->last = ngx_http_v2_write_value(b->last, val_tmp, p - val_tmp, tmp);

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http proxy header: \":path: %*s\"", p - val_tmp,
                       val_tmp);
    }

    *b->last++ = ngx_http_v2_inc_indexed(NGX_HTTP_V2_AUTHORITY_INDEX);
    b->last = ngx_http_v2_write_value(b->last, host.data, host.len, tmp);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http proxy header: \":authority: %V\"", &host);

    ngx_memzero(&e, sizeof(ngx_http_script_engine_t));

    e.ip = headers->values->elts;
    e.request = r;
    e.flushed = 1;

    le.ip = headers->lengths->elts;

    headers_end = b->last + headers_len;

    while (*(uintptr_t *) le.ip) {

        lcode = *(ngx_http_script_len_code_pt *) le.ip;
        key_len = lcode(&le);

        for (val_len = 0; *(uintptr_t *) le.ip; val_len += lcode(&le)) {
            lcode = *(ngx_http_script_len_code_pt *) le.ip;
        }
        le.ip += sizeof(uintptr_t);

        if (val_len == 0) {
            e.skip = 1;

            while (*(uintptr_t *) e.ip) {
                code = *(ngx_http_script_code_pt *) e.ip;
                code((ngx_http_script_engine_t *) &e);
            }
            e.ip += sizeof(uintptr_t);

            e.skip = 0;

            continue;
        }

        if (headers_end - b->last < 1) {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "no buffer space in HTTP/2 create request");
            return NGX_ERROR;
        }

        *b->last++ = 0;

        e.pos = key_tmp;
        e.end = key_tmp + tmp_len;

        code = *(ngx_http_script_code_pt *) e.ip;
        code((ngx_http_script_engine_t *) &e);

        if (e.status) {
            return NGX_ERROR;
        }

        key_len = e.pos - key_tmp;

        if (headers_end - b->last
            < (ssize_t) (NGX_HTTP_V2_INT_OCTETS + key_len))
        {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "no buffer space in HTTP/2 create request");
            return NGX_ERROR;
        }

        b->last = ngx_http_v2_write_name(b->last, key_tmp, key_len, tmp);

        e.pos = val_tmp;
        e.end = val_tmp + tmp_len;

        while (*(uintptr_t *) e.ip) {
            code = *(ngx_http_script_code_pt *) e.ip;
            code((ngx_http_script_engine_t *) &e);
        }
        e.ip += sizeof(uintptr_t);

        if (e.status) {
            return NGX_ERROR;
        }

        val_len = e.pos - val_tmp;

        if (headers_end - b->last
            < (ssize_t) (NGX_HTTP_V2_INT_OCTETS + val_len))
        {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "no buffer space in HTTP/2 create request");
            return NGX_ERROR;
        }

        b->last = ngx_http_v2_write_value(b->last, val_tmp, val_len, tmp);

#if (NGX_DEBUG)
        if (r->connection->log->log_level & NGX_LOG_DEBUG_HTTP) {
            ngx_strlow(key_tmp, key_tmp, key_len);

            ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "http proxy header: \"%*s: %*s\"",
                           key_len, key_tmp, val_len, val_tmp);
        }
#endif
    }

    if (plcf->upstream.pass_request_headers) {
        part = &r->headers_in.headers.part;
        header = part->elts;

        for (i = 0; /* void */; i++) {

            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }

                part = part->next;
                header = part->elts;
                i = 0;
            }

            if (ngx_hash_find(&headers->hash, header[i].hash,
                              header[i].lowcase_key, header[i].key.len))
            {
                continue;
            }

            *b->last++ = 0;

            b->last = ngx_http_v2_write_name(b->last, header[i].key.data,
                                             header[i].key.len, tmp);

            b->last = ngx_http_v2_write_value(b->last, header[i].value.data,
                                              header[i].value.len, tmp);

#if (NGX_DEBUG)
            if (r->connection->log->log_level & NGX_LOG_DEBUG_HTTP) {
                ngx_strlow(tmp, header[i].key.data, header[i].key.len);

                ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                               "http proxy header: \"%*s: %V\"",
                               header[i].key.len, tmp, &header[i].value);
            }
#endif
        }
    }

    /* update headers frame length */

    len = b->last - headers_frame - sizeof(ngx_http_proxy_v2_frame_t);

    if (len > NGX_HTTP_V2_DEFAULT_FRAME_SIZE) {
        len = NGX_HTTP_V2_DEFAULT_FRAME_SIZE;
        next = 1;

    } else {
        next = 0;
    }

    f = (ngx_http_proxy_v2_frame_t *) headers_frame;

    f->length_0 = (u_char) ((len >> 16) & 0xff);
    f->length_1 = (u_char) ((len >> 8) & 0xff);
    f->length_2 = (u_char) (len & 0xff);

    /* create additional continuation frames */

    p = headers_frame;

    while (next) {
        p += sizeof(ngx_http_proxy_v2_frame_t) + NGX_HTTP_V2_DEFAULT_FRAME_SIZE;
        len = b->last - p;

        ngx_memmove(p + sizeof(ngx_http_proxy_v2_frame_t), p, len);
        b->last += sizeof(ngx_http_proxy_v2_frame_t);

        if (len > NGX_HTTP_V2_DEFAULT_FRAME_SIZE) {
            len = NGX_HTTP_V2_DEFAULT_FRAME_SIZE;
            next = 1;

        } else {
            next = 0;
        }

        f = (ngx_http_proxy_v2_frame_t *) p;

        f->length_0 = (u_char) ((len >> 16) & 0xff);
        f->length_1 = (u_char) ((len >> 8) & 0xff);
        f->length_2 = (u_char) (len & 0xff);
        f->type = NGX_HTTP_V2_CONTINUATION_FRAME;
        f->flags = 0;
        f->stream_id_0 = 0;
        f->stream_id_1 = 0;
        f->stream_id_2 = 0;
        f->stream_id_3 = 1;
    }

    f->flags |= NGX_HTTP_V2_END_HEADERS_FLAG;

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http proxy header: %*xs%s, len: %uz",
                   (size_t) ngx_min(b->last - b->pos, 256), b->pos,
                   b->last - b->pos > 256 ? "..." : "",
                   b->last - b->pos);

    if (r->request_body_no_buffering) {

        u->request_bufs = cl;

    } else if (plcf->body_values == NULL && plcf->upstream.pass_request_body) {

        body = u->request_bufs;
        u->request_bufs = cl;

        if (body == NULL) {
            f = (ngx_http_proxy_v2_frame_t *) headers_frame;
            f->flags |= NGX_HTTP_V2_END_STREAM_FLAG;
        }

        while (body) {
            b = ngx_alloc_buf(r->pool);
            if (b == NULL) {
                return NGX_ERROR;
            }

            ngx_memcpy(b, body->buf, sizeof(ngx_buf_t));

            cl->next = ngx_alloc_chain_link(r->pool);
            if (cl->next == NULL) {
                return NGX_ERROR;
            }

            cl = cl->next;
            cl->buf = b;

            body = body->next;
        }

        b->last_buf = 1;

    } else if (body_len) {

        u->request_bufs = cl;

        b = ngx_create_temp_buf(r->pool, body_len);
        if (b == NULL) {
            return NGX_ERROR;
        }

        cl->next = ngx_alloc_chain_link(r->pool);
        if (cl->next == NULL) {
            return NGX_ERROR;
        }

        cl = cl->next;
        cl->buf = b;

        e.ip = plcf->body_values->elts;
        e.pos = b->last;
        e.end = b->last + body_len;
        e.request = r;
        e.flushed = 1;
        e.skip = 0;

        while (*(uintptr_t *) e.ip) {
            code = *(ngx_http_script_code_pt *) e.ip;
            code((ngx_http_script_engine_t *) &e);
        }

        if (e.status) {
            return NGX_ERROR;
        }

        b->last = e.pos;
        b->last_buf = 1;

    } else {
        u->request_bufs = cl;

        f = (ngx_http_proxy_v2_frame_t *) headers_frame;
        f->flags |= NGX_HTTP_V2_END_STREAM_FLAG;

        b->last_buf = 1;
    }

    u->output.output_filter = ngx_http_proxy_v2_body_output_filter;
    u->output.filter_ctx = r;

    b->flush = 1;
    cl->next = NULL;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_reinit_request(ngx_http_request_t *r)
{
    ngx_http_proxy_v2_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_proxy_v2_module);

    if (ctx == NULL) {
        return NGX_OK;
    }

    ctx->stream.header_block = NULL;
    ctx->stream.headers = NULL;
    ctx->stream.last_headers = &ctx->stream.headers;
    ctx->stream.trailers = NULL;
    ctx->stream.raw_len = 0;
    ctx->stream.raw_block = 0;
    ctx->stream.raw_overflow = 0;

    ctx->stream.header_sent = 0;
    ctx->stream.output_closed = 0;
    ctx->stream.output_blocked = 0;
    ctx->stream.output_discarded = 0;
    ctx->stream.parsing_headers = 0;
    ctx->stream.end_stream = 0;
    ctx->stream.done = 0;
    ctx->stream.headers_parsed = 0;
    ctx->stream.rst_parsed = 0;
    ctx->stream.rst_received = 0;
    ctx->stream.unprocessed = 0;
    ctx->stream.graceful = 0;
    ctx->stream.rst_received_error = 0;
    ctx->stream.data = NULL;
    ctx->stream.data_last = &ctx->stream.data;
    ctx->stream.data_size = 0;
    ctx->stream.data_parsed = 0;
    ctx->stream.received_start = 0;
    ctx->stream.received = 0;
    ctx->data_used = 0;
    ctx->stream.session = NULL;
    ctx->stream.in = NULL;
    ctx->stream.queued = 0;
    ctx->body_started = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_body_output_filter(void *data, ngx_chain_t *in)
{
    ngx_http_request_t  *r = data;

    ngx_int_t                     rc, ret;
    ngx_http_upstream_t          *u;
    ngx_http_proxy_v2_ctx_t      *ctx;
    ngx_http_proxy_v2_stream_t   *stream;
    ngx_http_proxy_v2_session_t  *sess;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http proxy output filter");

    ctx = ngx_http_proxy_v2_get_ctx(r);

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    stream = &ctx->stream;
    sess = stream->session;

    /* input after the output was discarded (see finish_body) is dropped */

    if (in && !stream->output_discarded) {
        if (ngx_chain_add_copy(r->pool, &stream->in, in) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (sess->read_status == NGX_ERROR
        || sess->read_status == NGX_HTTP_UPSTREAM_INVALID_HEADER
        || stream->unprocessed
        || (stream->rst_received
            && (stream->rst_received_error || !stream->done)))
    {
        /* let the read path consume preceding input and report the error */

        stream->write.ready = 0;
        stream->output_blocked = 1;
        ngx_http_proxy_v2_notify_read(stream);
        return NGX_AGAIN;
    }

    if (sess->connection->error) {
        return NGX_ERROR;
    }

    if (stream->rst_received && stream->done) {

        /*
         * The upstream sent a complete response and then reset the stream
         * with NO_ERROR: the rest of the request body is not needed and
         * must not be sent on the closed stream (RFC 9113, 8.1).
         */

        stream->in = NULL;
        stream->output_discarded = 1;
    }

    /* the session writes the stream output along with other frames */

    if (ngx_http_proxy_v2_session_write(sess) == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (ngx_http_proxy_v2_session_handle_events(sess) != NGX_OK) {
        sess->connection->error = 1;
        return NGX_ERROR;
    }

    /* the output is consumed once written to the transport */

    rc = (stream->in || stream->queued) ? NGX_AGAIN : NGX_OK;

    stream->output_blocked = (rc == NGX_AGAIN);
    ngx_http_proxy_v2_update_write_event(stream);

    if (stream->done) {

        u = r->upstream;

        ret = ngx_http_proxy_v2_finish_body(r, ctx);

        if (ret == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (ret == NGX_DONE) {
            ngx_post_event(u->connection->read, &ngx_posted_events);
        }
    }

    return rc;
}


static ngx_int_t
ngx_http_proxy_v2_session_write(ngx_http_proxy_v2_session_t *sess)
{
    off_t              sent;
    ngx_chain_t       *chain;
    ngx_connection_t  *c;

    c = sess->connection;

    if (c->error) {
        return NGX_ERROR;
    }

#if (NGX_HTTP_SSL)
    if (c->ssl && c->ssl->saved_write_handler) {
        /* SSL_read() owns write readiness until its WANT_WRITE is resolved */
        return NGX_AGAIN;
    }
#endif

    sent = c->sent;

    for ( ;; ) {

        if (ngx_http_proxy_v2_compose(sess) != NGX_OK) {
            return NGX_ERROR;
        }

        if (sess->wire == NULL && !c->buffered) {
            return NGX_OK;
        }

        chain = c->send_chain(c, sess->wire, sess->send_limit);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http proxy session write: %p", chain);

        if (chain == NGX_CHAIN_ERROR) {
            c->error = 1;
            return NGX_ERROR;
        }

        ngx_http_proxy_v2_wire_complete(sess, chain);

        if (chain || c->buffered) {

            if (chain && c->write->ready) {
                /* the send limit was reached */
                ngx_post_event(c->write, &ngx_posted_next_events);
            }

            return NGX_AGAIN;
        }

        if (sess->send_limit && c->sent - sent >= sess->send_limit) {
            ngx_post_event(c->write, &ngx_posted_next_events);
            return NGX_AGAIN;
        }
    }
}


static ngx_int_t
ngx_http_proxy_v2_compose(ngx_http_proxy_v2_session_t *sess)
{
    size_t                       budget;
    ngx_int_t                    rc;
    ngx_uint_t                   n;
    ngx_chain_t                 *cl;
    ngx_http_proxy_v2_stream_t  *stream, *next;

    /*
     * Frames are composed in rounds: queued control frames first, within
     * a budget, then one unit from each stream with output: its header
     * block, or a single DATA frame.  A round interrupted by a full batch
     * is continued from the same stream.
     */

    while (sess->wire_size < NGX_HTTP_PROXY_V2_WRITE_BATCH) {

        n = 0;
        budget = NGX_HTTP_PROXY_V2_CONTROL_BUDGET;

        while (sess->out && budget) {
            cl = sess->out;
            sess->out = cl->next;

            budget -= ngx_min(budget, (size_t) ngx_buf_size(cl->buf));

            if (ngx_http_proxy_v2_wire_add(sess, NULL, cl->buf, cl) != NGX_OK)
            {
                return NGX_ERROR;
            }

            n++;
        }

        if (sess->out == NULL) {
            sess->last_out = &sess->out;
        }

        stream = ngx_http_proxy_v2_first_stream(sess);

        while (stream && stream->seq < sess->cursor) {
            stream = ngx_http_proxy_v2_next_stream(stream);
        }

        for ( /* void */ ; stream; stream = next) {
            next = ngx_http_proxy_v2_next_stream(stream);

            rc = ngx_http_proxy_v2_compose_stream(sess, stream);

            if (rc == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (rc == NGX_OK) {
                n++;
            }

            if (sess->wire_size >= NGX_HTTP_PROXY_V2_WRITE_BATCH) {
                sess->cursor = next ? next->seq : 0;
                return NGX_OK;
            }
        }

        sess->cursor = 0;

        if (n == 0) {
            return NGX_OK;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_compose_stream(ngx_http_proxy_v2_session_t *sess,
    ngx_http_proxy_v2_stream_t *stream)
{
    u_char                     *p;
    ngx_buf_t                  *b;
    ngx_chain_t                *cl, *ln;
    ngx_http_proxy_v2_frame_t  *f;

    if (stream->in == NULL || stream->unprocessed || stream->rst_received) {
        return NGX_DECLINED;
    }

    if (stream->header_sent) {
        return ngx_http_proxy_v2_compose_data(sess, stream);
    }

    /*
     * The stream identifier is allocated when the header block is
     * composed, so that identifiers increase in the order streams are
     * opened on the wire (RFC 9113, Section 5.1.1).
     */

    if (sess->last_stream_id > NGX_HTTP_PROXY_V2_MAX_STREAM_ID - 2) {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "no more http2 stream identifiers");

        /* the request is retried with another connection */

        stream->unprocessed = 1;
        stream->graceful = 1;
        ngx_http_proxy_v2_notify_read(stream);
        return NGX_DECLINED;
    }

    stream->id = sess->last_stream_id ? sess->last_stream_id + 2 : 1;
    sess->last_stream_id = stream->id;

    stream->node.key = stream->id;
    ngx_rbtree_insert(&sess->streams, &stream->node);

    /* the first buffer contains the header block, an atomic unit */

    ln = stream->in;
    b = ln->buf;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                   "http proxy output header, stream %ui", stream->id);

    /* bind request frames to the stream identifier on every attempt */

    for (p = b->pos; p < b->last; /* void */ ) {
        f = (ngx_http_proxy_v2_frame_t *) p;
        p += sizeof(ngx_http_proxy_v2_frame_t);

        f->stream_id_0 = (u_char) ((stream->id >> 24) & 0xff);
        f->stream_id_1 = (u_char) ((stream->id >> 16) & 0xff);
        f->stream_id_2 = (u_char) ((stream->id >> 8) & 0xff);
        f->stream_id_3 = (u_char) (stream->id & 0xff);

        p += (f->length_0 << 16) + (f->length_1 << 8) + f->length_2;
    }

    if (b->last_buf) {
        stream->output_closed = 1;
    }

    cl = ngx_http_proxy_v2_get_slice(stream, b);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf->last_shadow = 1;

    stream->header_sent = 1;
    stream->in = ln->next;
    ngx_free_chain(stream->connection.pool, ln);

    return ngx_http_proxy_v2_wire_add(sess, stream, cl->buf, cl);
}


static ngx_int_t
ngx_http_proxy_v2_compose_data(ngx_http_proxy_v2_session_t *sess,
    ngx_http_proxy_v2_stream_t *stream)
{
    off_t                       size;
    size_t                      limit;
    ngx_buf_t                  *b, *buf;
    ngx_uint_t                  last;
    ngx_chain_t                *cl, *hl, *ln;
    ngx_http_proxy_v2_frame_t  *f;

    /* empty buffers, such as the last one of an empty body */

    last = 0;

    while (stream->in && ngx_buf_size(stream->in->buf) == 0) {
        ln = stream->in;
        last = ln->buf->last_buf;
        stream->in = ln->next;
        ngx_free_chain(stream->connection.pool, ln);

        if (last) {
            break;
        }
    }

    cl = NULL;
    size = 0;

    if (stream->in && !last) {

        limit = ngx_min(ngx_max(stream->send_window, 0), sess->send_window);
        limit = ngx_min(limit, NGX_HTTP_V2_DEFAULT_FRAME_SIZE);

        if (limit == 0) {
            return NGX_DECLINED;
        }

        buf = stream->in->buf;

        cl = ngx_http_proxy_v2_get_slice(stream, buf);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        b = cl->buf;
        size = ngx_min(ngx_buf_size(buf), (off_t) limit);

        if (size == ngx_buf_size(buf)) {

            /* the original buffer is marked as sent with the slice */

            b->last_shadow = 1;
            last = buf->last_buf;

            ln = stream->in;
            stream->in = ln->next;
            ngx_free_chain(stream->connection.pool, ln);

            while (!last && stream->in
                   && ngx_buf_size(stream->in->buf) == 0)
            {
                ln = stream->in;
                last = ln->buf->last_buf;
                stream->in = ln->next;
                ngx_free_chain(stream->connection.pool, ln);
            }

        } else if (buf->in_file) {
            b->file_last = b->file_pos + size;
            buf->file_pos = b->file_last;

        } else {
            b->last = b->pos + (size_t) size;
            buf->pos = b->last;
        }

        stream->send_window -= size;
        sess->send_window -= size;

    } else if (!last) {
        return NGX_DECLINED;
    }

    hl = ngx_http_proxy_v2_get_buf(stream);
    if (hl == NULL) {
        return NGX_ERROR;
    }

    f = (ngx_http_proxy_v2_frame_t *) hl->buf->last;
    hl->buf->last += sizeof(ngx_http_proxy_v2_frame_t);

    f->length_0 = (u_char) ((size >> 16) & 0xff);
    f->length_1 = (u_char) ((size >> 8) & 0xff);
    f->length_2 = (u_char) (size & 0xff);
    f->type = NGX_HTTP_V2_DATA_FRAME;
    f->flags = last ? NGX_HTTP_V2_END_STREAM_FLAG : 0;
    f->stream_id_0 = (u_char) ((stream->id >> 24) & 0xff);
    f->stream_id_1 = (u_char) ((stream->id >> 16) & 0xff);
    f->stream_id_2 = (u_char) ((stream->id >> 8) & 0xff);
    f->stream_id_3 = (u_char) (stream->id & 0xff);

    if (last) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                       "http proxy output last, stream %ui", stream->id);

        stream->output_closed = 1;
        stream->in = NULL;
    }

    if (ngx_http_proxy_v2_wire_add(sess, stream, hl->buf, hl) != NGX_OK) {
        return NGX_ERROR;
    }

    if (cl && ngx_http_proxy_v2_wire_add(sess, stream, cl->buf, cl) != NGX_OK)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_chain_t *
ngx_http_proxy_v2_get_slice(ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *buf)
{
    u_char       *start;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    cl = ngx_chain_get_free_buf(stream->connection.pool, &stream->free);
    if (cl == NULL) {
        return NULL;
    }

    b = cl->buf;
    start = b->start;

    ngx_memcpy(b, buf, sizeof(ngx_buf_t));

    /*
     * restore b->start to preserve memory allocated in the buffer,
     * to reuse it later for DATA frame headers
     */

    b->start = start;

    b->tag = (ngx_buf_tag_t) &ngx_http_proxy_v2_body_output_filter;
    b->shadow = buf;
    b->last_shadow = 0;
    b->last_buf = 0;
    b->last_in_chain = 0;
    b->flush = 1;

    return cl;
}


static ngx_int_t
ngx_http_proxy_v2_wire_add(ngx_http_proxy_v2_session_t *sess,
    ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b, ngx_chain_t *link)
{
    ngx_http_proxy_v2_out_t  *out;

    out = sess->wire_free;

    if (out) {
        sess->wire_free = out->next;

    } else {
        out = ngx_palloc(sess->connection->pool,
                         sizeof(ngx_http_proxy_v2_out_t));
        if (out == NULL) {
            return NGX_ERROR;
        }
    }

    out->chain.buf = b;
    out->chain.next = NULL;
    out->stream = stream;
    out->link = link;
    out->size = ngx_buf_size(b);

    *sess->wire_last = &out->chain;
    sess->wire_last = &out->chain.next;
    sess->wire_size += out->size;

    if (stream) {
        stream->queued++;
    }

    return NGX_OK;
}


static void
ngx_http_proxy_v2_wire_complete(ngx_http_proxy_v2_session_t *sess,
    ngx_chain_t *chain)
{
    ngx_buf_t                   *b;
    ngx_chain_t                 *cl;
    ngx_http_proxy_v2_out_t     *out;
    ngx_http_proxy_v2_stream_t  *stream;

    /* the entries before the returned chain are written */

    while (sess->wire && sess->wire != chain) {
        out = (ngx_http_proxy_v2_out_t *) sess->wire;
        sess->wire = out->chain.next;
        sess->wire_size -= out->size;

        cl = out->link;
        stream = out->stream;

        if (stream) {
            b = cl->buf;

            if (b->shadow && b->last_shadow) {

                /* mark the original buffer as sent */

                b->shadow->pos = b->shadow->last;
                b->shadow->file_pos = b->shadow->file_last;
            }

            b->shadow = NULL;

            cl->next = stream->free;
            stream->free = cl;

            if (--stream->queued == 0 && stream->output_blocked) {
                stream->write.ready = 1;
                ngx_post_event(&stream->write, &ngx_posted_events);
            }

        } else if (cl) {
            cl->next = sess->free;
            sess->free = cl;
        }

        out->next = sess->wire_free;
        sess->wire_free = out;
    }

    if (sess->wire == NULL) {
        sess->wire_last = &sess->wire;
    }
}


static void
ngx_http_proxy_v2_wire_drop(ngx_http_proxy_v2_session_t *sess)
{
    ngx_chain_t              *cl;
    ngx_http_proxy_v2_out_t  *out;

    /* unwritten entries are dropped, request buffers are not touched */

    while (sess->wire) {
        out = (ngx_http_proxy_v2_out_t *) sess->wire;
        sess->wire = out->chain.next;

        cl = out->link;

        if (out->stream == NULL && cl) {
            cl->next = sess->free;
            sess->free = cl;
        }

        out->next = sess->wire_free;
        sess->wire_free = out;
    }

    sess->wire_last = &sess->wire;
    sess->wire_size = 0;
    sess->cursor = 0;
}


static ngx_uint_t
ngx_http_proxy_v2_output_pending(ngx_http_proxy_v2_session_t *sess)
{
    return sess->out || sess->wire || sess->connection->buffered;
}


static ngx_uint_t
ngx_http_proxy_v2_input_pending(ngx_http_proxy_v2_session_t *sess)
{
    return sess->buffer.pos != sess->buffer.last
           || sess->state != ngx_http_proxy_v2_st_start
           || sess->continuation_sid;
}


static ngx_uint_t
ngx_http_proxy_v2_need_read(ngx_http_proxy_v2_session_t *sess)
{
    ngx_http_proxy_v2_stream_t  *stream;

    if (ngx_http_proxy_v2_input_pending(sess)) {
        return 1;
    }

    for (stream = ngx_http_proxy_v2_first_stream(sess);
         stream;
         stream = ngx_http_proxy_v2_next_stream(stream))
    {
        if (!stream->done && !stream->rst_received) {
            return 1;
        }
    }

    return 0;
}


static ngx_uint_t
ngx_http_proxy_v2_stream_input_consumed(ngx_http_proxy_v2_ctx_t *ctx)
{
    ngx_http_proxy_v2_stream_t  *stream;

    stream = &ctx->stream;

    return stream->data_size == 0
           && ctx->data_used == stream->data_parsed
           && stream->header_block == NULL && stream->headers == NULL
           && stream->trailers == NULL && !stream->rst_parsed;
}


static ngx_uint_t
ngx_http_proxy_v2_stream_input_done(ngx_http_proxy_v2_ctx_t *ctx)
{
    return ctx->stream.done && !ctx->stream.parsing_headers
           && ngx_http_proxy_v2_stream_input_consumed(ctx);
}


static ngx_uint_t
ngx_http_proxy_v2_stream_output_done(ngx_http_proxy_v2_stream_t *stream)
{
    return (stream->output_closed || stream->output_discarded)
           && !stream->output_blocked
           && stream->in == NULL && stream->queued == 0;
}


static ngx_uint_t
ngx_http_proxy_v2_session_can_keepalive(ngx_http_proxy_v2_session_t *sess)
{
    ngx_connection_t  *c;

    c = sess->connection;

    if (c->error || sess->goaway || sess->read_status != NGX_OK
        || ngx_http_proxy_v2_input_pending(sess)
        || ngx_http_proxy_v2_output_pending(sess))
    {
        return 0;
    }

    /* stream identifiers cannot be reused (RFC 9113, Section 5.1.1) */

    if (sess->last_stream_id > NGX_HTTP_PROXY_V2_MAX_STREAM_ID - 2) {
        return 0;
    }

#if (NGX_HTTP_SSL)
    if (c->ssl
        && (c->ssl->saved_read_handler || c->ssl->saved_write_handler))
    {
        return 0;
    }
#endif

    return 1;
}


static ngx_uint_t
ngx_http_proxy_v2_can_keepalive(ngx_http_proxy_v2_ctx_t *ctx)
{
    ngx_http_proxy_v2_stream_t  *stream;

    stream = &ctx->stream;

    /* with discarded output, the stream is only closed after a reset */

    return ngx_http_proxy_v2_stream_input_done(ctx)
           && ngx_http_proxy_v2_stream_output_done(stream)
           && (!stream->output_discarded || stream->rst_received)
           && ngx_http_proxy_v2_session_can_keepalive(stream->session);
}


static ngx_uint_t
ngx_http_proxy_v2_close_stream(ngx_http_request_t *r,
    ngx_http_proxy_v2_stream_t *stream)
{
    ngx_connection_t             *c;
    ngx_http_proxy_v2_session_t  *sess;

    sess = stream->session;
    c = sess->connection;

    /*
     * A stream released before it is closed, e.g., on a client abort or
     * a header-only response, is reset with CANCEL.  The connection is
     * only kept if this completes synchronously: with no frame partially
     * written and the reset written at once.
     */

    if (!ngx_http_proxy_v2_session_can_keepalive(sess)
        || c->read->timedout || c->write->timedout)
    {
        return 0;
    }

    if (!stream->header_sent
        || stream->rst_received
        || (stream->done && stream->output_closed))
    {
        /* the stream was never opened, or is already closed */
        return 1;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http proxy cancel stream %ui", stream->id);

    stream->in = NULL;

    if (ngx_http_proxy_v2_send_rst_stream(sess, stream->id,
                                          NGX_HTTP_PROXY_V2_CANCEL)
        != NGX_OK)
    {
        return 0;
    }

    return ngx_http_proxy_v2_session_write(sess) == NGX_OK
           && !ngx_http_proxy_v2_output_pending(sess);
}


static ngx_int_t
ngx_http_proxy_v2_finish_body(ngx_http_request_t *r,
    ngx_http_proxy_v2_ctx_t *ctx)
{
    ngx_int_t                     rc;
    ngx_http_upstream_t          *u;
    ngx_http_proxy_v2_stream_t   *stream;
    ngx_http_proxy_v2_session_t  *sess;

    /* body length and retry decisions must be settled before completion */

    if (!ctx->body_started) {
        return NGX_AGAIN;
    }

    rc = ngx_http_proxy_v2_process_body(r, ctx);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (rc != NGX_DONE) {
        return NGX_AGAIN;
    }

    u = r->upstream;
    stream = &ctx->stream;
    sess = stream->session;

    if (ctx->length > 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream prematurely closed stream");
        return NGX_ERROR;
    }

    if (!ngx_http_proxy_v2_stream_input_consumed(ctx)) {
        return NGX_AGAIN;
    }

    /* drain supplied input, without waiting for a speculative socket read */

    if (ngx_http_proxy_v2_input_pending(sess)
        || !ngx_http_proxy_v2_stream_input_done(ctx))
    {
        if (sess->read_status == NGX_DONE) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "upstream prematurely closed connection");
            return NGX_ERROR;
        }

        return NGX_AGAIN;
    }

    /* the session writer still borrows request-pool buffers */

    if (ngx_http_proxy_v2_output_pending(sess) || stream->queued) {
        return NGX_AGAIN;
    }

    if (!r->reading_body
        && !ngx_http_proxy_v2_stream_output_done(stream))
    {
        /*
         * The response is complete while the request body is still being
         * sent: as with other upstream protocols, stop sending it instead
         * of waiting for the upstream to accept it, which it may never do.
         * Unless the upstream reset the stream with NO_ERROR, the stream
         * remains open on our side, and is cancelled when released, see
         * ngx_http_proxy_v2_close_stream().
         */

        stream->in = NULL;
        stream->output_discarded = 1;
        stream->output_blocked = 0;
    }

    u->keepalive = ngx_http_proxy_v2_can_keepalive(ctx);
    u->pipe->length = 0;
    u->length = 0;

    return NGX_DONE;
}


static ngx_int_t
ngx_http_proxy_v2_process_header(ngx_http_request_t *r)
{
    ngx_int_t                    rc;
    ngx_http_proxy_v2_ctx_t     *ctx;
    ngx_http_proxy_v2_stream_t  *stream;

#if (NGX_HTTP_CACHE)
    if (r->cached) {
        return ngx_http_proxy_v2_process_cached_header(r);
    }
#endif

    ctx = ngx_http_proxy_v2_get_ctx(r);

    if (ctx == NULL) {

        if (!r->upstream->request_sent) {

            /*
             * The upstream can send its connection preface as soon as
             * the connection is established, before the request is sent
             * and the stream is set up: the input is processed later.
             */

            return NGX_AGAIN;
        }

        return NGX_ERROR;
    }

    stream = &ctx->stream;

    /* queued headers precede a subsequently received reset or error */

    if (stream->headers) {
        rc = ngx_http_proxy_v2_process_headers(r, stream);
        ngx_http_proxy_v2_update_read_event(stream);
        return rc;
    }

    if (stream->rst_parsed) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream rejected request with error %ui",
                      stream->rst_received_error);
        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }

    if (stream->session->read_status == NGX_HTTP_UPSTREAM_INVALID_HEADER) {
        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }

    /* transport errors and EOF are published on the logical read event */

    ngx_http_proxy_v2_update_read_event(stream);

    return NGX_AGAIN;
}


#if (NGX_HTTP_CACHE)

static ngx_int_t
ngx_http_proxy_v2_process_cached_header(ngx_http_request_t *r)
{
    u_char                       *p;
    ngx_int_t                     rc;
    ngx_buf_t                    *b;
    ngx_http_upstream_t          *u;
    ngx_http_proxy_v2_ctx_t      *ctx;
    ngx_http_proxy_v2_stream_t   *stream;
    ngx_http_proxy_v2_session_t  *sess;

    ctx = ngx_http_get_module_ctx(r, ngx_http_proxy_v2_module);

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    u = r->upstream;
    b = &u->buffer;
    stream = &ctx->stream;

    if (stream->session == NULL) {

        /*
         * The cache file stores the raw response header frames; they are
         * replayed through a session without a connection.  The header
         * table size is advertised as zero, so header blocks are decoded
         * without the original connection context.
         */

        if (b->last - b->pos < (ssize_t) sizeof(ngx_http_proxy_v2_frame_t)) {
            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }

        sess = ngx_pcalloc(r->pool, sizeof(ngx_http_proxy_v2_session_t));
        if (sess == NULL) {
            return NGX_ERROR;
        }

        sess->connection = r->connection;
        sess->cached = 1;

        ngx_queue_init(&sess->attached);
        ngx_rbtree_init(&sess->streams, &sess->sentinel,
                        ngx_rbtree_insert_value);

        p = b->pos;

        stream->id = ((ngx_uint_t) (p[5] & 0x7f) << 24)
                     | (p[6] << 16) | (p[7] << 8) | p[8];

        ngx_queue_insert_tail(&sess->attached, &stream->queue);
        stream->node.key = stream->id;
        ngx_rbtree_insert(&sess->streams, &stream->node);
        stream->session = sess;

        stream->connection.data = r;
        stream->connection.pool = r->pool;
        stream->connection.log = r->connection->log;
        stream->header_buffer_size = u->conf->buffer_size;

    } else {
        sess = stream->session;
    }

    for ( ;; ) {

        if (stream->headers) {

            if (stream->headers->status == NGX_HTTP_EARLY_HINTS) {

                /* early hints are not replayed from cache */

                stream->headers = stream->headers->next;

                if (stream->headers == NULL) {
                    stream->last_headers = &stream->headers;
                }

                continue;
            }

            return ngx_http_proxy_v2_process_headers(r, stream);
        }

        if (b->pos == b->last) {
            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }

        rc = ngx_http_proxy_v2_session_process_frame(sess, b);

        if (rc == NGX_ERROR) {
            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }
    }
}

#endif


static ngx_int_t
ngx_http_proxy_v2_record_header(ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *b, ngx_uint_t new_frame)
{
    u_char                       *p;
    size_t                        size;
    ngx_http_proxy_v2_session_t  *sess;

    /*
     * Raw response header frames are kept so that the request copies them
     * to u->buffer, as they were received there before: this keeps the
     * stored cache header and the early hints size accounting unchanged.
     * The frame header itself was already consumed by the session parser
     * and is rebuilt; the payload is recorded as the parser consumes it.
     */

    sess = stream->session;

    if (stream->raw_overflow) {
        return NGX_OK;
    }

    if (stream->raw == NULL) {
        stream->raw = ngx_pnalloc(stream->connection.pool,
                                  stream->header_buffer_size);
        if (stream->raw == NULL) {
            return NGX_ERROR;
        }
    }

    size = ngx_min((size_t) (b->last - b->pos), sess->rest);

    if (new_frame) {
        size += sizeof(ngx_http_proxy_v2_frame_t);
    }

    if (size > stream->header_buffer_size - stream->raw_len) {
        stream->raw_overflow = 1;
        return NGX_OK;
    }

    p = stream->raw + stream->raw_len;

    if (new_frame) {
        *p++ = (u_char) ((sess->rest >> 16) & 0xff);
        *p++ = (u_char) ((sess->rest >> 8) & 0xff);
        *p++ = (u_char) (sess->rest & 0xff);
        *p++ = sess->type;
        *p++ = sess->flags;
        *p++ = (u_char) ((sess->stream_id >> 24) & 0x7f);
        *p++ = (u_char) ((sess->stream_id >> 16) & 0xff);
        *p++ = (u_char) ((sess->stream_id >> 8) & 0xff);
        *p++ = (u_char) (sess->stream_id & 0xff);

        size -= sizeof(ngx_http_proxy_v2_frame_t);
    }

    ngx_memcpy(p, b->pos, size);

    stream->raw_len = p + size - stream->raw;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_parse_headers(ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *b, ngx_uint_t new_frame)
{
    size_t                             size;
    ngx_int_t                          rc, status;
    ngx_table_elt_t                   *h;
    ngx_connection_t                  *c;
    ngx_http_proxy_v2_session_t       *sess;
    ngx_http_proxy_v2_header_block_t  *block;

    c = &stream->connection;
    sess = stream->session;
    block = stream->header_block;

    if (block == NULL) {
        block = ngx_pcalloc(c->pool, sizeof(*block));
        if (block == NULL) {
            return NGX_ERROR;
        }

        if (ngx_list_init(&block->fields, c->pool, 4,
                          sizeof(ngx_table_elt_t)) != NGX_OK)
        {
            return NGX_ERROR;
        }

        stream->header_block = block;
    }

    if (!sess->cached && !stream->headers_parsed) {
        if (ngx_http_proxy_v2_record_header(stream, b, new_frame) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (new_frame && !stream->headers_parsed) {
        /* bound response header input independently of receive buffers */

        size = sizeof(ngx_http_proxy_v2_frame_t) + sess->rest;

        if (block->length > stream->header_buffer_size
            || size > stream->header_buffer_size - block->length)
        {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "upstream sent too large http2 header block");
            return NGX_ERROR;
        }

        block->length += size;
    }

    for ( ;; ) {
        rc = ngx_http_proxy_v2_parse_header(stream, b);

        if (rc == NGX_AGAIN) {
            /* a complete frame can precede the rest of its header block */

            return sess->state == ngx_http_proxy_v2_st_start
                   ? NGX_OK : NGX_AGAIN;
        }

        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (rc == NGX_HTTP_PARSE_HEADER_DONE) {
            block->end_stream = stream->end_stream;

            if (stream->headers_parsed) {
                if (!block->end_stream) {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream sent trailer without "
                                  "end stream flag");
                    return NGX_ERROR;
                }

                stream->trailers = block;

            } else {
                if (block->status == 0) {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream sent no :status header");
                    return NGX_ERROR;
                }

                if (block->status == NGX_HTTP_EARLY_HINTS
                    && block->end_stream)
                {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream prematurely closed stream");
                    return NGX_ERROR;
                }

                *stream->last_headers = block;
                stream->last_headers = &block->next;

                block->raw_offset = stream->raw_block;
                block->raw_length = stream->raw_len - stream->raw_block;
                stream->raw_block = stream->raw_len;

                if (block->status != NGX_HTTP_EARLY_HINTS) {
                    stream->headers_parsed = 1;
                }
            }

            if (block->end_stream) {
                stream->done = 1;
            }

            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http proxy header block done");

            stream->header_block = NULL;

            return NGX_OK;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http proxy header: \"%V: %V\"",
                       &stream->name, &stream->value);

        if (stream->name.len && stream->name.data[0] == ':') {
            if (stream->headers_parsed
                || stream->name.len != sizeof(":status") - 1
                || ngx_strncmp(stream->name.data, ":status",
                               sizeof(":status") - 1) != 0)
            {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent invalid header \"%V: %V\"",
                              &stream->name, &stream->value);
                return NGX_ERROR;
            }

            if (block->status) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent duplicate :status header");
                return NGX_ERROR;
            }

            status = stream->value.len == 3
                     ? ngx_atoi(stream->value.data, 3) : NGX_ERROR;

            if (status == NGX_ERROR) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent invalid :status \"%V\"",
                              &stream->value);
                return NGX_ERROR;
            }

            if (status < NGX_HTTP_OK && status != NGX_HTTP_EARLY_HINTS) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent unexpected :status \"%V\"",
                              &stream->value);
                return NGX_ERROR;
            }

            block->status = status;
            continue;
        }

        if (!stream->headers_parsed && block->status == 0) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "upstream sent no :status header");
            return NGX_ERROR;
        }

        h = ngx_list_push(&block->fields);
        if (h == NULL) {
            return NGX_ERROR;
        }

        ngx_memzero(h, sizeof(ngx_table_elt_t));
        h->key = stream->name;
        h->value = stream->value;
        h->lowcase_key = h->key.data;
        h->hash = ngx_hash_key(h->key.data, h->key.len);
    }
}


static ngx_int_t
ngx_http_proxy_v2_process_headers(ngx_http_request_t *r,
    ngx_http_proxy_v2_stream_t *stream)
{
    ngx_int_t                          rc;
    ngx_uint_t                         i;
    ngx_list_part_t                   *part;
    ngx_table_elt_t                   *h;
    ngx_http_upstream_t               *u;
    ngx_http_upstream_header_t        *hh;
    ngx_http_upstream_main_conf_t     *umcf;
    ngx_http_proxy_v2_header_block_t  *block;

    u = r->upstream;
    block = stream->headers;

    if (stream->raw_overflow
        || (size_t) (u->buffer.end - u->buffer.last) < block->raw_length)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream sent too big header");
        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }

    if (block->raw_length) {
        u->buffer.last = ngx_cpymem(u->buffer.last,
                                    stream->raw + block->raw_offset,
                                    block->raw_length);
        u->buffer.pos = u->buffer.last;
    }

    /* only complete header blocks reach the request layer */

    u->headers_in.status_n = block->status;

    if (u->state && u->state->status == 0) {
        u->state->status = block->status;
    }

    if (ngx_http_proxy_v2_copy_headers(block, &u->headers_in.headers)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (block->status == NGX_HTTP_EARLY_HINTS) {
        rc = NGX_HTTP_UPSTREAM_EARLY_HINTS;

    } else {
        umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);
        part = &u->headers_in.headers.part;
        h = part->elts;

        for (i = 0; /* void */; i++) {
            if (i >= part->nelts) {
                if (part->next == NULL) {
                    break;
                }

                part = part->next;
                h = part->elts;
                i = 0;
            }

            hh = ngx_hash_find(&umcf->headers_in_hash, h[i].hash,
                               h[i].lowcase_key, h[i].key.len);

            if (hh) {
                rc = hh->handler(r, &h[i], hh->offset);
                if (rc != NGX_OK) {
                    return rc;
                }
            }
        }

        rc = NGX_OK;
    }

    stream->headers = block->next;
    if (stream->headers == NULL) {
        stream->last_headers = &stream->headers;
    }

    return rc;
}


static ngx_int_t
ngx_http_proxy_v2_process_trailers(ngx_http_request_t *r,
    ngx_http_proxy_v2_stream_t *stream)
{
    ngx_http_proxy_v2_header_block_t  *block;

    block = stream->trailers;

    if (ngx_http_proxy_v2_copy_headers(block,
                                       &r->upstream->headers_in.trailers)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    stream->trailers = NULL;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_copy_headers(ngx_http_proxy_v2_header_block_t *block,
    ngx_list_t *fields)
{
    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_table_elt_t  *src, *dst;

    part = &block->fields.part;
    src = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            src = part->elts;
            i = 0;
        }

        dst = ngx_list_push(fields);
        if (dst == NULL) {
            return NGX_ERROR;
        }

        /* decoded strings already belong to the request pool */

        *dst = src[i];
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_filter_init(void *data)
{
    ngx_http_request_t  *r = data;

    ngx_int_t                 rc;
    ngx_http_upstream_t      *u;
    ngx_http_proxy_v2_ctx_t  *ctx;

    u = r->upstream;
    ctx = ngx_http_get_module_ctx(r, ngx_http_proxy_v2_module);

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    if (u->headers_in.status_n == NGX_HTTP_NO_CONTENT
        || u->headers_in.status_n == NGX_HTTP_NOT_MODIFIED
        || ctx->ctx.head)
    {
        ctx->length = 0;

    } else {
        ctx->length = u->headers_in.content_length_n;
    }

    /* a completed response can still have unconsumed DATA */

    if (ctx->stream.done && ctx->length > ctx->stream.data_parsed) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream prematurely closed stream");
        return NGX_ERROR;
    }

    u->pipe->length = 1;
    u->length = 1;
    ctx->body_started = 1;

    rc = ngx_http_proxy_v2_finish_body(r, ctx);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (rc == NGX_AGAIN && ctx->stream.done
        && (ngx_http_proxy_v2_output_pending(ctx->stream.session)
            || ctx->stream.queued))
    {
        ngx_post_event(u->connection->write, &ngx_posted_events);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_non_buffered_filter(void *data, ssize_t bytes)
{
    ngx_http_request_t  *r = data;

    ngx_buf_t                *b, *buf;
    ngx_chain_t              *cl, **ll;
    ngx_http_upstream_t      *u;
    ngx_http_proxy_v2_ctx_t  *ctx;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http proxy non buffered filter bytes:%z", bytes);

    ctx = ngx_http_get_module_ctx(r, ngx_http_proxy_v2_module);

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    u = r->upstream;
    b = &u->buffer;

    /* the stream connection returns DATA payload only */

    if (bytes) {
        for (cl = u->out_bufs, ll = &u->out_bufs; cl; cl = cl->next) {
            ll = &cl->next;
        }

        cl = ngx_chain_get_free_buf(r->pool, &u->free_bufs);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        *ll = cl;

        buf = cl->buf;

        buf->flush = 1;
        buf->memory = 1;

        buf->pos = b->last;
        b->last += bytes;
        buf->last = b->last;
        buf->tag = u->output.tag;

        if (ngx_http_proxy_v2_process_data(r, ctx, buf) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (ngx_http_proxy_v2_finish_body(r, ctx) == NGX_ERROR) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_process_body(ngx_http_request_t *r,
    ngx_http_proxy_v2_ctx_t *ctx)
{
    ngx_http_proxy_v2_stream_t  *stream;

    stream = &ctx->stream;

    /* deliver queued DATA before trailers or a reset */

    if (stream->data_size) {
        return NGX_OK;
    }

    /* DATA handed to the pipe must pass its input filter before metadata */

    if (ctx->data_used != stream->data_parsed) {
        return NGX_AGAIN;
    }

    if (stream->trailers) {
        if (ngx_http_proxy_v2_process_trailers(r, stream) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (stream->rst_parsed) {
        if (stream->rst_received_error || !stream->done) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "upstream rejected request with error %ui",
                          stream->rst_received_error);
            return NGX_ERROR;
        }

        stream->rst_parsed = 0;
    }

    if (stream->session->read_status == NGX_ERROR
        || stream->session->read_status == NGX_HTTP_UPSTREAM_INVALID_HEADER)
    {
        return NGX_ERROR;
    }

    if (stream->session->read_status == NGX_DONE && !stream->done) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream prematurely closed connection");
        return NGX_ERROR;
    }

    /* receive completion still requires input and output to be drained */

    return stream->done ? NGX_DONE : NGX_AGAIN;
}


static ngx_int_t
ngx_http_proxy_v2_body_filter(ngx_event_pipe_t *p, ngx_buf_t *b)
{
    ngx_int_t                 rc;
    ngx_buf_t                *buf;
    ngx_chain_t              *cl;
    ngx_http_request_t       *r;
    ngx_http_proxy_v2_ctx_t  *ctx;

    r = p->input_ctx;
    ctx = ngx_http_get_module_ctx(r, ngx_http_proxy_v2_module);

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    if (b->pos == b->last) {
        /* pending stream state does not require new input bytes */

        rc = ngx_http_proxy_v2_finish_body(r, ctx);
        return rc == NGX_ERROR ? NGX_ERROR : NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http proxy filter bytes:%z", b->last - b->pos);

    if (ngx_http_proxy_v2_process_data(r, ctx, b) != NGX_OK) {
        return NGX_ERROR;
    }

    cl = ngx_chain_get_free_buf(p->pool, &p->free);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    buf = cl->buf;
    ngx_memzero(buf, sizeof(ngx_buf_t));
    buf->pos = b->pos;
    buf->last = b->last;
    buf->start = b->start;
    buf->end = b->end;
    buf->tag = p->tag;
    buf->temporary = 1;
    buf->recycled = 1;
    buf->shadow = b;
    buf->last_shadow = 1;
    b->shadow = buf;
    buf->num = b->num;

    if (p->in) {
        *p->last_in = cl;

    } else {
        p->in = cl;
    }

    p->last_in = &cl->next;

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, p->log, 0,
                   "input buf %p %z", buf->pos, buf->last - buf->pos);

    rc = ngx_http_proxy_v2_finish_body(r, ctx);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_parse_data(ngx_http_proxy_v2_stream_t *stream, ngx_buf_t *b)
{
    size_t                        size;
    ngx_http_proxy_v2_segment_t  *seg;
    ngx_http_proxy_v2_session_t  *sess;

    sess = stream->session;
    size = ngx_min(b->last - b->pos, (ssize_t) sess->rest - sess->padding);

    if (size == 0) {
        return NGX_AGAIN;
    }

    /*
     * The DATA queue is bounded: when it is full, the rest of the input
     * is kept in the session buffer, and reading from the transport stops
     * until the request consumes the queue, as the event pipe does when
     * its buffers are exhausted.
     */

    if (stream->data_size == NGX_HTTP_PROXY_V2_DATA_QUEUE_SIZE) {
        sess->read_gated = 1;
        return NGX_AGAIN;
    }

    size = ngx_min(size,
                   NGX_HTTP_PROXY_V2_DATA_QUEUE_SIZE - stream->data_size);

    seg = (ngx_http_proxy_v2_segment_t *)
          ((u_char *) stream->data_last
           - offsetof(ngx_http_proxy_v2_segment_t, next));

    if (stream->data && seg->chunk == sess->chunk && seg->last == b->pos) {

        /* the rest of a payload continues the last segment */

        seg->last += size;

    } else {
        seg = stream->data_free;

        if (seg) {
            stream->data_free = seg->next;

        } else {
            seg = ngx_palloc(stream->connection.pool,
                             sizeof(ngx_http_proxy_v2_segment_t));
            if (seg == NULL) {
                return NGX_ERROR;
            }
        }

        seg->next = NULL;
        seg->chunk = sess->chunk;
        seg->pos = b->pos;
        seg->last = b->pos + size;

        sess->chunk->refs++;

        *stream->data_last = seg;
        stream->data_last = &seg->next;
    }

    stream->data_size += size;
    stream->data_parsed += size;
    sess->rest -= size;
    b->pos += size;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_process_data(ngx_http_request_t *r,
    ngx_http_proxy_v2_ctx_t *ctx, ngx_buf_t *b)
{
    size_t  size;

    size = b->last - b->pos;

    if (size == 0) {
        return NGX_OK;
    }

    if (ctx->length != -1 && (off_t) size > ctx->length) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream sent response body larger "
                      "than indicated content length");
        return NGX_ERROR;
    }

    ctx->data_used += size;

    if (ctx->length != -1) {
        ctx->length -= size;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_replenish_window(ngx_http_proxy_v2_stream_t *stream)
{
    size_t  n;

    /*
     * The stream window is large, and the DATA queue bounds memory instead
     * (see ngx_http_proxy_v2_parse_data()); refill it as the connection
     * window is refilled.
     */

    if (stream->done || stream->rst_received
        || stream->session->read_status != NGX_OK
        || stream->recv_window >= NGX_HTTP_V2_MAX_WINDOW / 4)
    {
        return NGX_OK;
    }

    n = NGX_HTTP_V2_MAX_WINDOW - stream->recv_window;

    if (ngx_http_proxy_v2_send_window_update(stream->session, stream->id, n)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    stream->recv_window += n;

    ngx_post_event(stream->session->connection->write, &ngx_posted_events);

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_session_process_frame(ngx_http_proxy_v2_session_t *sess,
    ngx_buf_t *b)
{
    ngx_int_t                    rc;
    ngx_uint_t                   new_frame;
    ngx_http_proxy_v2_stream_t  *stream;

    new_frame = sess->state < ngx_http_proxy_v2_st_payload;

    if (new_frame) {
        rc = ngx_http_proxy_v2_parse_frame(sess, b);
        if (rc != NGX_OK) {
            return rc;
        }

        if (ngx_http_proxy_v2_validate_frame(sess) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    rc = ngx_http_proxy_v2_process_connection_frame(sess, b);
    if (rc != NGX_DECLINED) {
        return rc;
    }

    switch (sess->type) {

    case NGX_HTTP_V2_HEADERS_FRAME:
    case NGX_HTTP_V2_CONTINUATION_FRAME:
    case NGX_HTTP_V2_DATA_FRAME:
    case NGX_HTTP_V2_RST_STREAM_FRAME:
    case NGX_HTTP_V2_WINDOW_UPDATE_FRAME:

        stream = ngx_http_proxy_v2_find_stream(sess, sess->stream_id);

        if (stream == NULL) {
            return ngx_http_proxy_v2_skip_closed_frame(sess, b, new_frame);
        }

        rc = ngx_http_proxy_v2_stream_process_frame(stream, b, new_frame);

        if (rc == NGX_ERROR
            && (sess->type == NGX_HTTP_V2_HEADERS_FRAME
                || sess->type == NGX_HTTP_V2_CONTINUATION_FRAME))
        {
            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                          "upstream sent invalid %s",
                          stream->headers_parsed ? "trailer" : "header");
        }

        return rc;

    default:
        /* PRIORITY and unknown frames do not require an attached stream */

        return ngx_http_proxy_v2_skip_frame(sess, b);
    }
}


static ngx_int_t
ngx_http_proxy_v2_process_connection_frame(ngx_http_proxy_v2_session_t *sess,
    ngx_buf_t *b)
{
    ssize_t                      window_update;
    ngx_int_t                    rc;
    ngx_uint_t                   graceful;
    ngx_http_proxy_v2_stream_t  *stream;

    if (sess->type == NGX_HTTP_V2_GOAWAY_FRAME) {

        rc = ngx_http_proxy_v2_parse_goaway(sess, b);

        if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        /*
         * Streams above last_stream_id were not processed and can be
         * retried (RFC 9113, Section 6.8), while the others can complete.
         * The session is not reused.  The last stream identifier
         * can only decrease with subsequent GOAWAY frames.
         */

        if (!sess->goaway || sess->goaway_id < sess->goaway_last_stream_id) {
            sess->goaway_last_stream_id = sess->goaway_id;
        }

        sess->goaway = 1;

        /* the first GOAWAY not processing a stream decides its outcome */

        graceful = (sess->goaway_error == NGX_HTTP_PROXY_V2_NO_ERROR);

        for (stream = ngx_http_proxy_v2_first_stream(sess);
             stream;
             stream = ngx_http_proxy_v2_next_stream(stream))
        {
            if (stream->id == 0) {

                /* the stream will not be opened */

                if (!stream->unprocessed) {
                    stream->unprocessed = 1;
                    stream->graceful = graceful;
                }

                continue;
            }

            if (sess->goaway_last_stream_id >= stream->id) {
                continue;
            }

            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                          "upstream sent goaway with error %ui, "
                          "stream %ui not processed",
                          sess->goaway_error, stream->id);

            if (stream->headers_parsed || stream->headers) {
                /* a response was sent on the stream */
                return NGX_ERROR;
            }

            if (!stream->unprocessed) {
                stream->unprocessed = 1;
                stream->graceful = graceful;
            }
        }

        return NGX_OK;
    }

    if (sess->type == NGX_HTTP_V2_WINDOW_UPDATE_FRAME && sess->stream_id == 0) {

        rc = ngx_http_proxy_v2_parse_window_update(sess, b);

        if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (sess->window_update == 0) {
            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                          "upstream sent zero window update");
            return NGX_ERROR;
        }

        if (sess->window_update > NGX_HTTP_V2_MAX_WINDOW - sess->send_window) {
            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                          "upstream sent too large window update");
            return NGX_ERROR;
        }

        sess->send_window += sess->window_update;

        for (stream = ngx_http_proxy_v2_first_stream(sess);
             stream;
             stream = ngx_http_proxy_v2_next_stream(stream))
        {
            if (stream->in) {
                ngx_post_event(sess->connection->write, &ngx_posted_events);
                break;
            }
        }

        return NGX_OK;
    }

    if (sess->type == NGX_HTTP_V2_SETTINGS_FRAME) {

        for ( ;; ) {
            rc = ngx_http_proxy_v2_parse_settings(sess, b);

            if (rc == NGX_AGAIN || rc == NGX_ERROR) {
                return rc;
            }

            if (rc == NGX_DONE) {
                break;
            }

            /* apply each complete setting in wire order */

            if (sess->setting_id == 0x04) {
                /* SETTINGS_INITIAL_WINDOW_SIZE */

                if (sess->setting_value > NGX_HTTP_V2_MAX_WINDOW) {
                    ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                                  "upstream sent settings frame "
                                  "with too large initial window size: %ui",
                                  sess->setting_value);
                    return NGX_ERROR;
                }

                window_update = (ssize_t) sess->setting_value
                                - (ssize_t) sess->init_window;

                /* validate the adjustment before changing any stream */

                if (window_update > 0) {
                    for (stream = ngx_http_proxy_v2_first_stream(sess);
                         stream;
                         stream = ngx_http_proxy_v2_next_stream(stream))
                    {
                        if (stream->send_window
                            > (ssize_t) NGX_HTTP_V2_MAX_WINDOW - window_update)
                        {
                            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                                          "upstream sent settings frame "
                                          "with too large initial window "
                                          "size: %ui", sess->setting_value);
                            return NGX_ERROR;
                        }
                    }
                }

                for (stream = ngx_http_proxy_v2_first_stream(sess);
                     stream;
                     stream = ngx_http_proxy_v2_next_stream(stream))
                {
                    stream->send_window += window_update;
                }

                sess->init_window = sess->setting_value;
            }
        }

        if (!(sess->flags & NGX_HTTP_V2_ACK_FLAG)) {
            if (ngx_http_proxy_v2_send_settings_ack(sess) != NGX_OK) {
                return NGX_ERROR;
            }

            ngx_post_event(sess->connection->write, &ngx_posted_events);

        } else {
            for (stream = ngx_http_proxy_v2_first_stream(sess);
                 stream;
                 stream = ngx_http_proxy_v2_next_stream(stream))
            {
                if (stream->in) {
                    ngx_post_event(sess->connection->write, &ngx_posted_events);
                    break;
                }
            }
        }

        return NGX_OK;
    }

    if (sess->type == NGX_HTTP_V2_PING_FRAME) {

        rc = ngx_http_proxy_v2_parse_ping(sess, b);

        if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (ngx_http_proxy_v2_send_ping_ack(sess) != NGX_OK) {
            return NGX_ERROR;
        }

        ngx_post_event(sess->connection->write, &ngx_posted_events);

        return NGX_OK;
    }

    if (sess->type == NGX_HTTP_V2_PUSH_PROMISE_FRAME) {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "upstream sent unexpected push promise frame");
        return NGX_ERROR;
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_proxy_v2_stream_process_frame(ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *b, ngx_uint_t new_frame)
{
    size_t                        n;
    ngx_int_t                     rc;
    ngx_connection_t             *c;
    ngx_http_proxy_v2_session_t  *sess;

    c = &stream->connection;
    sess = stream->session;

    if (new_frame) {
        if ((stream->rst_received
             || (stream->done && sess->type != NGX_HTTP_V2_RST_STREAM_FRAME))
            && sess->type != NGX_HTTP_V2_WINDOW_UPDATE_FRAME)
        {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "upstream sent frame for closed stream %ui",
                          sess->stream_id);
            return NGX_ERROR;
        }

        if (sess->type == NGX_HTTP_V2_DATA_FRAME) {
            if (!stream->headers_parsed) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent unexpected http2 frame: %d",
                              sess->type);
                return NGX_ERROR;
            }

            if (sess->rest > stream->recv_window) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream violated stream flow control, "
                              "received %uz data frame with window %uz",
                              sess->rest, stream->recv_window);
                return NGX_ERROR;
            }

            /* charge the full payload once, including padding */

            if (ngx_http_proxy_v2_charge_window(sess) != NGX_OK) {
                return NGX_ERROR;
            }

            stream->recv_window -= sess->rest;
        }
    }

    switch (sess->type) {

    case NGX_HTTP_V2_HEADERS_FRAME:
    case NGX_HTTP_V2_CONTINUATION_FRAME:

        return ngx_http_proxy_v2_parse_headers(stream, b, new_frame);

    case NGX_HTTP_V2_WINDOW_UPDATE_FRAME:

        return ngx_http_proxy_v2_stream_process_window_update(sess, stream, b);

    case NGX_HTTP_V2_RST_STREAM_FRAME:

        rc = ngx_http_proxy_v2_parse_rst_stream(stream, b);
        if (rc != NGX_OK) {
            return rc;
        }

        /* close the stream before publishing the reset to the request */

        stream->rst_received = 1;

        if (stream->rst_received_error == NGX_HTTP_PROXY_V2_REFUSED_STREAM
            && !stream->headers_parsed && stream->headers == NULL)
        {
            /* the request was not processed (RFC 9113, Section 8.7) */

            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "upstream refused stream %ui", stream->id);

            stream->unprocessed = 1;
            return NGX_OK;
        }

        stream->rst_parsed = 1;
        return NGX_OK;

    case NGX_HTTP_V2_DATA_FRAME:

        for ( ;; ) {
            if (sess->state == ngx_http_proxy_v2_st_padding) {
                n = ngx_min((size_t) (b->last - b->pos), sess->rest);
                sess->rest -= n;
                b->pos += n;

                if (sess->rest == 0) {
                    sess->state = ngx_http_proxy_v2_st_start;

                    if (sess->flags & NGX_HTTP_V2_END_STREAM_FLAG) {
                        stream->done = 1;
                    }
                }

                if (ngx_http_proxy_v2_replenish_window(stream) != NGX_OK) {
                    return NGX_ERROR;
                }

                return sess->rest ? NGX_AGAIN : NGX_OK;
            }

            if (sess->flags & NGX_HTTP_V2_PADDED_FLAG) {

                if (b->pos == b->last) {
                    return NGX_AGAIN;
                }

                sess->flags &= ~NGX_HTTP_V2_PADDED_FLAG;
                sess->padding = *b->pos++;
                sess->rest -= 1;

                if (sess->padding > sess->rest) {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream sent http2 frame with too long "
                                  "padding: %d in frame %uz",
                                  sess->padding, sess->rest);
                    return NGX_ERROR;
                }

                if (ngx_http_proxy_v2_replenish_window(stream) != NGX_OK) {
                    return NGX_ERROR;
                }
            }

            if (sess->rest == sess->padding) {
                sess->state = ngx_http_proxy_v2_st_padding;
                continue;
            }

            return ngx_http_proxy_v2_parse_data(stream, b);
        }
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_proxy_v2_stream_process_window_update(
    ngx_http_proxy_v2_session_t *sess, ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *b)
{
    ngx_int_t  rc;

    rc = ngx_http_proxy_v2_parse_window_update(sess, b);
    if (rc != NGX_OK) {
        return rc;
    }

    if (sess->window_update == 0) {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "upstream sent zero window update");
        return NGX_ERROR;
    }

    if (stream->send_window > (ssize_t) NGX_HTTP_V2_MAX_WINDOW
                              - (ssize_t) sess->window_update)
    {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "upstream sent too large window update");
        return NGX_ERROR;
    }

    stream->send_window += (ssize_t) sess->window_update;

    if (stream->in) {
        ngx_post_event(sess->connection->write, &ngx_posted_events);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_skip_closed_frame(ngx_http_proxy_v2_session_t *sess,
    ngx_buf_t *b, ngx_uint_t new_frame)
{
    if (sess->stream_id % 2 == 0 || sess->stream_id > sess->last_stream_id) {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "upstream sent frame for unknown stream %ui",
                      sess->stream_id);
        return NGX_ERROR;
    }

    /*
     * Frames for a stream already closed on our side, such as a cancelled
     * one, are ignored (RFC 9113, Section 5.1).  Header blocks need not be
     * decoded, as the header table size is zero, while DATA payload still
     * counts against the connection window.
     */

    if (new_frame) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                       "http proxy frame %d for closed stream %ui",
                       sess->type, sess->stream_id);

        if (sess->type == NGX_HTTP_V2_DATA_FRAME
            && ngx_http_proxy_v2_charge_window(sess) != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    /*
     * The skipped header block ends with the frame, which can be partially
     * received already, e.g., for a stream released while receiving it.
     */

    if ((sess->type == NGX_HTTP_V2_HEADERS_FRAME
         || sess->type == NGX_HTTP_V2_CONTINUATION_FRAME)
        && (sess->flags & NGX_HTTP_V2_END_HEADERS_FLAG))
    {
        sess->continuation_sid = 0;
    }

    return ngx_http_proxy_v2_skip_frame(sess, b);
}


static ngx_int_t
ngx_http_proxy_v2_charge_window(ngx_http_proxy_v2_session_t *sess)
{
    size_t  n;

    if (sess->rest > sess->recv_window) {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "upstream violated connection flow control, "
                      "received %uz data frame with window %uz",
                      sess->rest, sess->recv_window);
        return NGX_ERROR;
    }

    sess->recv_window -= sess->rest;

    if (sess->recv_window < NGX_HTTP_V2_MAX_WINDOW / 4) {
        n = NGX_HTTP_V2_MAX_WINDOW - sess->recv_window;

        if (ngx_http_proxy_v2_send_window_update(sess, 0, n) != NGX_OK) {
            return NGX_ERROR;
        }

        sess->recv_window += n;

        ngx_post_event(sess->connection->write, &ngx_posted_events);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_skip_frame(ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b)
{
    if (b->last - b->pos < (ssize_t) sess->rest) {
        sess->rest -= b->last - b->pos;
        b->pos = b->last;
        return NGX_AGAIN;
    }

    b->pos += sess->rest;
    sess->rest = 0;
    sess->state = ngx_http_proxy_v2_st_start;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_parse_frame(ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b)
{
    u_char                     ch, *p;
    ngx_http_proxy_v2_state_e  state;

    state = sess->state;

    for (p = b->pos; p < b->last; p++) {
        ch = *p;

#if 0
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                       "http proxy frame byte: %02Xd, s:%d", ch, state);
#endif

        switch (state) {

        case ngx_http_proxy_v2_st_start:
            sess->rest = ch << 16;
            state = ngx_http_proxy_v2_st_length_2;
            break;

        case ngx_http_proxy_v2_st_length_2:
            sess->rest |= ch << 8;
            state = ngx_http_proxy_v2_st_length_3;
            break;

        case ngx_http_proxy_v2_st_length_3:
            sess->rest |= ch;

            state = ngx_http_proxy_v2_st_type;
            break;

        case ngx_http_proxy_v2_st_type:
            sess->type = ch;
            state = ngx_http_proxy_v2_st_flags;
            break;

        case ngx_http_proxy_v2_st_flags:
            sess->flags = ch;
            state = ngx_http_proxy_v2_st_stream_id;
            break;

        case ngx_http_proxy_v2_st_stream_id:
            sess->stream_id = (ch & 0x7f) << 24;
            state = ngx_http_proxy_v2_st_stream_id_2;
            break;

        case ngx_http_proxy_v2_st_stream_id_2:
            sess->stream_id |= ch << 16;
            state = ngx_http_proxy_v2_st_stream_id_3;
            break;

        case ngx_http_proxy_v2_st_stream_id_3:
            sess->stream_id |= ch << 8;
            state = ngx_http_proxy_v2_st_stream_id_4;
            break;

        case ngx_http_proxy_v2_st_stream_id_4:
            sess->stream_id |= ch;

            ngx_log_debug4(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                           "http proxy frame: %d, len: %uz, f:%d, i:%ui",
                           sess->type, sess->rest, sess->flags,
                           sess->stream_id);

            b->pos = p + 1;

            sess->state = ngx_http_proxy_v2_st_payload;
            sess->frame_state = 0;
            sess->padding = 0;

            return NGX_OK;

        /* suppress warning */
        case ngx_http_proxy_v2_st_payload:
        case ngx_http_proxy_v2_st_padding:
            break;
        }
    }

    b->pos = p;
    sess->state = state;

    return NGX_AGAIN;
}


static ngx_int_t
ngx_http_proxy_v2_validate_frame(ngx_http_proxy_v2_session_t *sess)
{
    size_t  min;

    /* validate the complete frame header before consuming its payload */

    if (sess->rest > NGX_HTTP_V2_DEFAULT_FRAME_SIZE) {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "upstream sent too large http2 frame: %uz", sess->rest);
        return NGX_ERROR;
    }

    /* a header block cannot be interleaved with any other frames */

    if (sess->continuation_sid) {
        if (sess->type != NGX_HTTP_V2_CONTINUATION_FRAME
            || sess->stream_id != sess->continuation_sid)
        {
            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                          "upstream sent unexpected http2 frame: %d "
                          "for stream %ui while expecting continuation "
                          "for stream %ui", sess->type, sess->stream_id,
                          sess->continuation_sid);
            return NGX_ERROR;
        }

    } else if (sess->type == NGX_HTTP_V2_CONTINUATION_FRAME) {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "upstream sent unexpected http2 frame: %d", sess->type);
        return NGX_ERROR;
    }

    switch (sess->type) {

    case NGX_HTTP_V2_DATA_FRAME:
    case NGX_HTTP_V2_HEADERS_FRAME:
    case NGX_HTTP_V2_PRIORITY_FRAME:
    case NGX_HTTP_V2_RST_STREAM_FRAME:
    case NGX_HTTP_V2_PUSH_PROMISE_FRAME:
    case NGX_HTTP_V2_CONTINUATION_FRAME:

        if (sess->stream_id == 0) {
            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                          "upstream sent http2 frame: %d with zero stream id",
                          sess->type);
            return NGX_ERROR;
        }

        break;

    case NGX_HTTP_V2_SETTINGS_FRAME:
    case NGX_HTTP_V2_PING_FRAME:
    case NGX_HTTP_V2_GOAWAY_FRAME:

        if (sess->stream_id) {
            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                          "upstream sent http2 frame: %d "
                          "with non-zero stream id: %ui",
                          sess->type, sess->stream_id);
            return NGX_ERROR;
        }

        break;
    }

    switch (sess->type) {

    case NGX_HTTP_V2_DATA_FRAME:

        if ((sess->flags & NGX_HTTP_V2_PADDED_FLAG) && sess->rest == 0) {
            goto invalid_length;
        }

        break;

    case NGX_HTTP_V2_HEADERS_FRAME:

        min = (sess->flags & NGX_HTTP_V2_PADDED_FLAG ? 1 : 0)
              + (sess->flags & NGX_HTTP_V2_PRIORITY_FLAG ? 5 : 0);

        if (sess->rest < min) {
            goto invalid_length;
        }

        break;

    case NGX_HTTP_V2_RST_STREAM_FRAME:
    case NGX_HTTP_V2_WINDOW_UPDATE_FRAME:

        if (sess->rest != 4) {
            goto invalid_length;
        }

        break;

    case NGX_HTTP_V2_SETTINGS_FRAME:

        if (sess->rest % 6 != 0
            || ((sess->flags & NGX_HTTP_V2_ACK_FLAG) && sess->rest != 0))
        {
            goto invalid_length;
        }

        break;

    case NGX_HTTP_V2_PING_FRAME:

        if (sess->rest != 8) {
            goto invalid_length;
        }

        break;

    case NGX_HTTP_V2_GOAWAY_FRAME:

        if (sess->rest < 8) {
            goto invalid_length;
        }

        break;
    }

    if (sess->type == NGX_HTTP_V2_HEADERS_FRAME) {
        sess->continuation_sid = sess->stream_id;
    }

    return NGX_OK;

invalid_length:

    ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                  "upstream sent http2 frame: %d with invalid length: %uz",
                  sess->type, sess->rest);

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_proxy_v2_parse_header(ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *b)
{
    u_char                        ch, *p, *last;
    ngx_int_t                     rc;
    ngx_connection_t             *c;
    ngx_http_proxy_v2_session_t  *sess;
    enum {
        sw_start = 0,
        sw_padding_length,
        sw_dependency,
        sw_dependency_2,
        sw_dependency_3,
        sw_dependency_4,
        sw_weight,
        sw_fragment,
        sw_padding
    } state;

    c = &stream->connection;
    sess = stream->session;
    state = sess->frame_state;

    if (state == sw_start) {

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http proxy parse header: start");

        if (sess->type == NGX_HTTP_V2_HEADERS_FRAME) {
            stream->parsing_headers = 1;
            stream->fragment_state = 0;
            stream->end_stream = 0;
            stream->header_limit = stream->header_buffer_size;

            if (sess->flags & NGX_HTTP_V2_END_STREAM_FLAG) {
                stream->end_stream = 1;
            }

            if (sess->flags & NGX_HTTP_V2_PADDED_FLAG) {
                state = sw_padding_length;

            } else if (sess->flags & NGX_HTTP_V2_PRIORITY_FLAG) {
                state = sw_dependency;

            } else {
                state = sw_fragment;
            }

        } else if (sess->type == NGX_HTTP_V2_CONTINUATION_FRAME) {
            state = sw_fragment;
        }

        sess->padding = 0;
        sess->frame_state = state;
    }

    if (state < sw_fragment) {

        if (b->last - b->pos < (ssize_t) sess->rest) {
            last = b->last;

        } else {
            last = b->pos + sess->rest;
        }

        for (p = b->pos; p < last; p++) {
            ch = *p;

#if 0
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http proxy header byte: %02Xd s:%d", ch, state);
#endif

            /*
             * headers frame:
             *
             * +---------------+
             * |Pad Length? (8)|
             * +-+-------------+----------------------------------------------+
             * |E|                 Stream Dependency? (31)                    |
             * +-+-------------+----------------------------------------------+
             * |  Weight? (8)  |
             * +-+-------------+----------------------------------------------+
             * |                   Header Block Fragment (*)                ...
             * +--------------------------------------------------------------+
             * |                           Padding (*)                      ...
             * +--------------------------------------------------------------+
             */

            switch (state) {

            case sw_padding_length:

                sess->padding = ch;

                if (sess->flags & NGX_HTTP_V2_PRIORITY_FLAG) {
                    state = sw_dependency;
                    break;
                }

                goto fragment;

            case sw_dependency:
                state = sw_dependency_2;
                break;

            case sw_dependency_2:
                state = sw_dependency_3;
                break;

            case sw_dependency_3:
                state = sw_dependency_4;
                break;

            case sw_dependency_4:
                state = sw_weight;
                break;

            case sw_weight:
                goto fragment;

            /* suppress warning */
            case sw_start:
            case sw_fragment:
            case sw_padding:
                break;
            }
        }

        sess->rest -= p - b->pos;
        b->pos = p;

        sess->frame_state = state;
        return NGX_AGAIN;

    fragment:

        p++;
        sess->rest -= p - b->pos;
        b->pos = p;

        if (sess->padding > sess->rest) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "upstream sent http2 frame with too long "
                          "padding: %d in frame %uz",
                          sess->padding, sess->rest);
            return NGX_ERROR;
        }

        state = sw_fragment;
        sess->frame_state = state;
    }

    if (state == sw_fragment) {

        rc = ngx_http_proxy_v2_parse_fragment(stream, b);

        if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (rc == NGX_OK) {
            return NGX_OK;
        }

        /* rc == NGX_DONE */

        state = sw_padding;
        sess->frame_state = state;
    }

    if (state == sw_padding) {

        if (b->last - b->pos < (ssize_t) sess->rest) {

            sess->rest -= b->last - b->pos;
            b->pos = b->last;

            return NGX_AGAIN;
        }

        b->pos += sess->rest;
        sess->rest = 0;

        sess->state = ngx_http_proxy_v2_st_start;

        if (sess->flags & NGX_HTTP_V2_END_HEADERS_FLAG) {

            if (stream->fragment_state) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent truncated http2 header");
                return NGX_ERROR;
            }

            stream->parsing_headers = 0;
            sess->continuation_sid = 0;

            return NGX_HTTP_PARSE_HEADER_DONE;
        }

        return NGX_AGAIN;
    }

    /* unreachable */

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_proxy_v2_parse_fragment(ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *b)
{
    u_char                        ch, *p, *last;
    size_t                        len, size;
    ngx_uint_t                    index, size_update;
    ngx_connection_t             *c;
    ngx_http_proxy_v2_session_t  *sess;
    enum {
        sw_start = 0,
        sw_index,
        sw_name_length,
        sw_name_length_2,
        sw_name_length_3,
        sw_name_length_4,
        sw_name,
        sw_name_bytes,
        sw_value_length,
        sw_value_length_2,
        sw_value_length_3,
        sw_value_length_4,
        sw_value,
        sw_value_bytes
    } state;

    c = &stream->connection;
    sess = stream->session;

    /* header block fragment */

#if 0
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http proxy header fragment %p:%p rest:%uz",
                   b->pos, b->last, sess->rest);
#endif

    if (b->last - b->pos < (ssize_t) sess->rest - sess->padding) {
        last = b->last;

    } else {
        last = b->pos + sess->rest - sess->padding;
    }

    state = stream->fragment_state;

    for (p = b->pos; p < last; p++) {
        ch = *p;

#if 0
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http proxy header byte: %02Xd s:%d", ch, state);
#endif

        switch (state) {

        case sw_start:
            stream->index = 0;

            if ((ch & 0x80) == 0x80) {
                /*
                 * indexed header:
                 *
                 *   0   1   2   3   4   5   6   7
                 * +---+---+---+---+---+---+---+---+
                 * | 1 |        Index (7+)         |
                 * +---+---------------------------+
                 */

                index = ch & ~0x80;

                if (index == 0 || index > 61) {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream sent invalid http2 "
                                  "table index: %ui", index);
                    return NGX_ERROR;
                }

                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                               "http proxy indexed header: %ui", index);

                stream->index = index;
                stream->literal = 0;

                goto done;

            } else if ((ch & 0xc0) == 0x40) {
                /*
                 * literal header with incremental indexing:
                 *
                 *   0   1   2   3   4   5   6   7
                 * +---+---+---+---+---+---+---+---+
                 * | 0 | 1 |      Index (6+)       |
                 * +---+---+-----------------------+
                 * | H |     Value Length (7+)     |
                 * +---+---------------------------+
                 * | Value String (Length octets)  |
                 * +-------------------------------+
                 *
                 *   0   1   2   3   4   5   6   7
                 * +---+---+---+---+---+---+---+---+
                 * | 0 | 1 |           0           |
                 * +---+---+-----------------------+
                 * | H |     Name Length (7+)      |
                 * +---+---------------------------+
                 * |  Name String (Length octets)  |
                 * +---+---------------------------+
                 * | H |     Value Length (7+)     |
                 * +---+---------------------------+
                 * | Value String (Length octets)  |
                 * +-------------------------------+
                 */

                index = ch & ~0xc0;

                if (index > 61) {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream sent invalid http2 "
                                  "table index: %ui", index);
                    return NGX_ERROR;
                }

                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                               "http proxy literal header: %ui", index);

                if (index == 0) {
                    state = sw_name_length;
                    break;
                }

                stream->index = index;
                stream->literal = 1;

                state = sw_value_length;
                break;

            } else if ((ch & 0xe0) == 0x20) {
                /*
                 * dynamic table size update:
                 *
                 *   0   1   2   3   4   5   6   7
                 * +---+---+---+---+---+---+---+---+
                 * | 0 | 0 | 1 |   Max size (5+)   |
                 * +---+---------------------------+
                 */

                size_update = ch & ~0xe0;

                if (size_update > 0) {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream sent invalid http2 "
                                  "dynamic table size update: %ui",
                                  size_update);
                    return NGX_ERROR;
                }

                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                               "http proxy table size update: %ui",
                               size_update);

                break;

            } else if ((ch & 0xf0) == 0x10) {
                /*
                 *  literal header field never indexed:
                 *
                 *   0   1   2   3   4   5   6   7
                 * +---+---+---+---+---+---+---+---+
                 * | 0 | 0 | 0 | 1 |  Index (4+)   |
                 * +---+---+-----------------------+
                 * | H |     Value Length (7+)     |
                 * +---+---------------------------+
                 * | Value String (Length octets)  |
                 * +-------------------------------+
                 *
                 *   0   1   2   3   4   5   6   7
                 * +---+---+---+---+---+---+---+---+
                 * | 0 | 0 | 0 | 1 |       0       |
                 * +---+---+-----------------------+
                 * | H |     Name Length (7+)      |
                 * +---+---------------------------+
                 * |  Name String (Length octets)  |
                 * +---+---------------------------+
                 * | H |     Value Length (7+)     |
                 * +---+---------------------------+
                 * | Value String (Length octets)  |
                 * +-------------------------------+
                 */

                index = ch & ~0xf0;

                if (index == 0x0f) {
                    stream->index = index;
                    stream->literal = 1;
                    state = sw_index;
                    break;
                }

                if (index == 0) {
                    state = sw_name_length;
                    break;
                }

                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                               "http proxy literal header never indexed: %ui",
                               index);

                stream->index = index;
                stream->literal = 1;

                state = sw_value_length;
                break;

            } else if ((ch & 0xf0) == 0x00) {
                /*
                 * literal header field without indexing:
                 *
                 *   0   1   2   3   4   5   6   7
                 * +---+---+---+---+---+---+---+---+
                 * | 0 | 0 | 0 | 0 |  Index (4+)   |
                 * +---+---+-----------------------+
                 * | H |     Value Length (7+)     |
                 * +---+---------------------------+
                 * | Value String (Length octets)  |
                 * +-------------------------------+
                 *
                 *   0   1   2   3   4   5   6   7
                 * +---+---+---+---+---+---+---+---+
                 * | 0 | 0 | 0 | 0 |       0       |
                 * +---+---+-----------------------+
                 * | H |     Name Length (7+)      |
                 * +---+---------------------------+
                 * |  Name String (Length octets)  |
                 * +---+---------------------------+
                 * | H |     Value Length (7+)     |
                 * +---+---------------------------+
                 * | Value String (Length octets)  |
                 * +-------------------------------+
                 */

                index = ch & ~0xf0;

                if (index == 0x0f) {
                    stream->index = index;
                    stream->literal = 1;
                    state = sw_index;
                    break;
                }

                if (index == 0) {
                    state = sw_name_length;
                    break;
                }

                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                             "http proxy literal header without indexing: %ui",
                               index);

                stream->index = index;
                stream->literal = 1;

                state = sw_value_length;
                break;
            }

            /* not reached */

            return NGX_ERROR;

        case sw_index:
            stream->index = stream->index + (ch & ~0x80);

            if (ch & 0x80) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent http2 table index "
                              "with continuation flag");
                return NGX_ERROR;
            }

            if (stream->index > 61) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent invalid http2 "
                              "table index: %ui", stream->index);
                return NGX_ERROR;
            }

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http proxy header index: %ui", stream->index);

            state = sw_value_length;
            break;

        case sw_name_length:
            stream->field_huffman = ch & 0x80 ? 1 : 0;
            stream->field_length = ch & ~0x80;

            if (stream->field_length == 0x7f) {
                state = sw_name_length_2;
                break;
            }

            if (stream->field_length == 0) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent zero http2 "
                              "header name length");
                return NGX_ERROR;
            }

            state = sw_name;
            break;

        case sw_name_length_2:
            stream->field_length += ch & ~0x80;

            if (ch & 0x80) {
                state = sw_name_length_3;
                break;
            }

            state = sw_name;
            break;

        case sw_name_length_3:
            stream->field_length += (ch & ~0x80) << 7;

            if (ch & 0x80) {
                state = sw_name_length_4;
                break;
            }

            state = sw_name;
            break;

        case sw_name_length_4:
            stream->field_length += (ch & ~0x80) << 14;

            if (ch & 0x80) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent too large http2 "
                              "header name length");
                return NGX_ERROR;
            }

            state = sw_name;
            break;

        case sw_name:
            stream->name.len = stream->field_huffman
                               ? stream->field_length * 8 / 5
                               : stream->field_length;

            if (stream->name.len > stream->header_limit) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent too large http2 "
                              "header name length: %uz",
                              stream->name.len);
                return NGX_ERROR;
            }

            stream->name.data = ngx_pnalloc(c->pool, stream->name.len + 1);
            if (stream->name.data == NULL) {
                return NGX_ERROR;
            }

            stream->field_end = stream->name.data;
            stream->field_rest = stream->field_length;
            stream->field_state = 0;

            state = sw_name_bytes;

            /* fall through */

        case sw_name_bytes:

            ngx_log_debug4(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http proxy name: len:%uz h:%d last:%uz, rest:%uz",
                           stream->field_length,
                           stream->field_huffman,
                           last - p,
                           sess->rest - (p - b->pos));

            size = ngx_min(last - p, (ssize_t) stream->field_rest);
            stream->field_rest -= size;

            if (stream->field_huffman) {
                if (ngx_http_huff_decode(&stream->field_state, p, size,
                                         &stream->field_end,
                                         stream->field_rest == 0,
                                         c->log)
                    != NGX_OK)
                {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream sent invalid encoded header");
                    return NGX_ERROR;
                }

                stream->name.len = stream->field_end - stream->name.data;
                stream->name.data[stream->name.len] = '\0';

            } else {
                stream->field_end = ngx_cpymem(stream->field_end, p, size);
                stream->name.data[stream->name.len] = '\0';
            }

            p += size - 1;

            if (stream->field_rest == 0) {
                state = sw_value_length;
            }

            break;

        case sw_value_length:
            stream->field_huffman = ch & 0x80 ? 1 : 0;
            stream->field_length = ch & ~0x80;

            if (stream->field_length == 0x7f) {
                state = sw_value_length_2;
                break;
            }

            if (stream->field_length == 0) {
                ngx_str_set(&stream->value, "");
                goto done;
            }

            state = sw_value;
            break;

        case sw_value_length_2:
            stream->field_length += ch & ~0x80;

            if (ch & 0x80) {
                state = sw_value_length_3;
                break;
            }

            state = sw_value;
            break;

        case sw_value_length_3:
            stream->field_length += (ch & ~0x80) << 7;

            if (ch & 0x80) {
                state = sw_value_length_4;
                break;
            }

            state = sw_value;
            break;

        case sw_value_length_4:
            stream->field_length += (ch & ~0x80) << 14;

            if (ch & 0x80) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent too large http2 "
                              "header value length");
                return NGX_ERROR;
            }

            state = sw_value;
            break;

        case sw_value:
            stream->value.len = stream->field_huffman
                                ? stream->field_length * 8 / 5
                                : stream->field_length;

            if (stream->value.len > stream->header_limit) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent too large http2 "
                              "header value length: %uz",
                              stream->value.len);
                return NGX_ERROR;
            }

            stream->value.data = ngx_pnalloc(c->pool, stream->value.len + 1);
            if (stream->value.data == NULL) {
                return NGX_ERROR;
            }

            stream->field_end = stream->value.data;
            stream->field_rest = stream->field_length;
            stream->field_state = 0;

            state = sw_value_bytes;

            /* fall through */

        case sw_value_bytes:

            ngx_log_debug4(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http proxy value: len:%uz h:%d last:%uz, rest:%uz",
                           stream->field_length,
                           stream->field_huffman,
                           last - p,
                           sess->rest - (p - b->pos));

            size = ngx_min(last - p, (ssize_t) stream->field_rest);
            stream->field_rest -= size;

            if (stream->field_huffman) {
                if (ngx_http_huff_decode(&stream->field_state, p, size,
                                         &stream->field_end,
                                         stream->field_rest == 0,
                                         c->log)
                    != NGX_OK)
                {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                                  "upstream sent invalid encoded header");
                    return NGX_ERROR;
                }

                stream->value.len = stream->field_end - stream->value.data;
                stream->value.data[stream->value.len] = '\0';

            } else {
                stream->field_end = ngx_cpymem(stream->field_end, p, size);
                stream->value.data[stream->value.len] = '\0';
            }

            p += size - 1;

            if (stream->field_rest == 0) {
                goto done;
            }

            break;
        }

        continue;

    done:

        p++;
        sess->rest -= p - b->pos;
        stream->fragment_state = sw_start;
        b->pos = p;

        if (stream->index) {
            stream->name = *ngx_http_v2_get_static_name(stream->index);
        }

        if (stream->index && !stream->literal) {
            stream->value = *ngx_http_v2_get_static_value(stream->index);
        }

        if (!stream->index) {
            if (ngx_http_proxy_v2_validate_header_name(&stream->name)
                != NGX_OK)
            {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent invalid header: \"%V: %V\"",
                              &stream->name, &stream->value);
                return NGX_ERROR;
            }
        }

        if (!stream->index || stream->literal) {
            if (ngx_http_proxy_v2_validate_header_value(&stream->value)
                != NGX_OK)
            {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "upstream sent invalid header: \"%V: %V\"",
                              &stream->name, &stream->value);
                return NGX_ERROR;
            }
        }

        len = stream->name.len + stream->value.len;

        if (len > stream->header_limit) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "upstream sent too large http2 header");
            return NGX_ERROR;
        }

        stream->header_limit -= len;

        return NGX_OK;
    }

    sess->rest -= p - b->pos;
    stream->fragment_state = state;
    b->pos = p;

    if (sess->rest > sess->padding) {
        return NGX_AGAIN;
    }

    return NGX_DONE;
}


static ngx_int_t
ngx_http_proxy_v2_validate_header_name(ngx_str_t *s)
{
    u_char      ch;
    ngx_uint_t  i;

    for (i = 0; i < s->len; i++) {
        ch = s->data[i];

        if (ch == ':' && i > 0) {
            return NGX_ERROR;
        }

        if (ch >= 'A' && ch <= 'Z') {
            return NGX_ERROR;
        }

        if (ch <= 0x20 || ch == 0x7f) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_validate_header_value(ngx_str_t *s)
{
    u_char      ch;
    ngx_uint_t  i;

    for (i = 0; i < s->len; i++) {
        ch = s->data[i];

        if (ch == '\0' || ch == CR || ch == LF) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_parse_rst_stream(ngx_http_proxy_v2_stream_t *stream,
    ngx_buf_t *b)
{
    u_char                        ch, *p, *last;
    ngx_http_proxy_v2_session_t  *sess;
    enum {
        sw_start = 0,
        sw_error_2,
        sw_error_3,
        sw_error_4
    } state;

    sess = stream->session;

    if (b->last - b->pos < (ssize_t) sess->rest) {
        last = b->last;

    } else {
        last = b->pos + sess->rest;
    }

    state = sess->frame_state;

    for (p = b->pos; p < last; p++) {
        ch = *p;

#if 0
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                       "http proxy rst byte: %02Xd s:%d", ch, state);
#endif

        switch (state) {

        case sw_start:
            stream->rst_received_error = (ngx_uint_t) ch << 24;
            state = sw_error_2;
            break;

        case sw_error_2:
            stream->rst_received_error |= ch << 16;
            state = sw_error_3;
            break;

        case sw_error_3:
            stream->rst_received_error |= ch << 8;
            state = sw_error_4;
            break;

        case sw_error_4:
            stream->rst_received_error |= ch;
            state = sw_start;

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                           "http proxy error: %ui", stream->rst_received_error);

            break;
        }
    }

    sess->rest -= p - b->pos;
    sess->frame_state = state;
    b->pos = p;

    if (sess->rest > 0) {
        return NGX_AGAIN;
    }

    sess->state = ngx_http_proxy_v2_st_start;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_parse_goaway(ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b)
{
    u_char  ch, *p, *last;
    enum {
        sw_start = 0,
        sw_last_stream_id_2,
        sw_last_stream_id_3,
        sw_last_stream_id_4,
        sw_error,
        sw_error_2,
        sw_error_3,
        sw_error_4,
        sw_debug
    } state;

    if (b->last - b->pos < (ssize_t) sess->rest) {
        last = b->last;

    } else {
        last = b->pos + sess->rest;
    }

    state = sess->frame_state;

    for (p = b->pos; p < last; p++) {
        ch = *p;

#if 0
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                       "http proxy goaway byte: %02Xd s:%d", ch, state);
#endif

        switch (state) {

        case sw_start:
            sess->goaway_id = (ch & 0x7f) << 24;
            state = sw_last_stream_id_2;
            break;

        case sw_last_stream_id_2:
            sess->goaway_id |= ch << 16;
            state = sw_last_stream_id_3;
            break;

        case sw_last_stream_id_3:
            sess->goaway_id |= ch << 8;
            state = sw_last_stream_id_4;
            break;

        case sw_last_stream_id_4:
            sess->goaway_id |= ch;
            state = sw_error;
            break;

        case sw_error:
            sess->goaway_error = (ngx_uint_t) ch << 24;
            state = sw_error_2;
            break;

        case sw_error_2:
            sess->goaway_error |= ch << 16;
            state = sw_error_3;
            break;

        case sw_error_3:
            sess->goaway_error |= ch << 8;
            state = sw_error_4;
            break;

        case sw_error_4:
            sess->goaway_error |= ch;
            state = sw_debug;
            break;

        case sw_debug:
            break;
        }
    }

    sess->rest -= p - b->pos;
    sess->frame_state = state;
    b->pos = p;

    if (sess->rest > 0) {
        return NGX_AGAIN;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                   "http proxy goaway: %ui, stream %ui",
                   sess->goaway_error, sess->goaway_id);

    sess->state = ngx_http_proxy_v2_st_start;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_parse_window_update(ngx_http_proxy_v2_session_t *sess,
    ngx_buf_t *b)
{
    u_char  ch, *p, *last;
    enum {
        sw_start = 0,
        sw_size_2,
        sw_size_3,
        sw_size_4
    } state;

    if (b->last - b->pos < (ssize_t) sess->rest) {
        last = b->last;

    } else {
        last = b->pos + sess->rest;
    }

    state = sess->frame_state;

    for (p = b->pos; p < last; p++) {
        ch = *p;

#if 0
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                       "http proxy window update byte: %02Xd s:%d", ch, state);
#endif

        switch (state) {

        case sw_start:
            sess->window_update = (ch & 0x7f) << 24;
            state = sw_size_2;
            break;

        case sw_size_2:
            sess->window_update |= ch << 16;
            state = sw_size_3;
            break;

        case sw_size_3:
            sess->window_update |= ch << 8;
            state = sw_size_4;
            break;

        case sw_size_4:
            sess->window_update |= ch;
            state = sw_start;
            break;
        }
    }

    sess->rest -= p - b->pos;
    sess->frame_state = state;
    b->pos = p;

    if (sess->rest > 0) {
        return NGX_AGAIN;
    }

    sess->state = ngx_http_proxy_v2_st_start;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                   "http proxy window update: %ui", sess->window_update);

    return NGX_OK;
}


/* NGX_OK returns one setting; NGX_DONE completes the frame, including ACKs */

static ngx_int_t
ngx_http_proxy_v2_parse_settings(ngx_http_proxy_v2_session_t *sess,
    ngx_buf_t *b)
{
    u_char  ch, *p, *last;
    enum {
        sw_start = 0,
        sw_id,
        sw_id_2,
        sw_value,
        sw_value_2,
        sw_value_3,
        sw_value_4
    } state;

    if (b->last - b->pos < (ssize_t) sess->rest) {
        last = b->last;

    } else {
        last = b->pos + sess->rest;
    }

    state = sess->frame_state;

    if (state == sw_start) {

        if (sess->flags & NGX_HTTP_V2_ACK_FLAG) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                           "http proxy settings ack");

            sess->state = ngx_http_proxy_v2_st_start;

            return NGX_DONE;
        }

        state = sw_id;
        sess->frame_state = state;
    }

    if (sess->rest == 0) {
        sess->state = ngx_http_proxy_v2_st_start;
        return NGX_DONE;
    }

    for (p = b->pos; p < last; p++) {
        ch = *p;

#if 0
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                       "http proxy settings byte: %02Xd s:%d", ch, state);
#endif

        switch (state) {

        case sw_start:
        case sw_id:
            sess->setting_id = ch << 8;
            state = sw_id_2;
            break;

        case sw_id_2:
            sess->setting_id |= ch;
            state = sw_value;
            break;

        case sw_value:
            sess->setting_value = (ngx_uint_t) ch << 24;
            state = sw_value_2;
            break;

        case sw_value_2:
            sess->setting_value |= ch << 16;
            state = sw_value_3;
            break;

        case sw_value_3:
            sess->setting_value |= ch << 8;
            state = sw_value_4;
            break;

        case sw_value_4:
            sess->setting_value |= ch;
            state = sw_id;

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                           "http proxy setting: %ui %ui",
                           sess->setting_id, sess->setting_value);

            /* return the last tuple before reporting frame completion */

            p++;
            sess->rest -= p - b->pos;
            sess->frame_state = state;
            b->pos = p;

            return NGX_OK;
        }
    }

    sess->rest -= p - b->pos;
    sess->frame_state = state;
    b->pos = p;

    return NGX_AGAIN;
}


static ngx_int_t
ngx_http_proxy_v2_parse_ping(ngx_http_proxy_v2_session_t *sess, ngx_buf_t *b)
{
    u_char  ch, *p, *last;
    enum {
        sw_start = 0,
        sw_data_2,
        sw_data_3,
        sw_data_4,
        sw_data_5,
        sw_data_6,
        sw_data_7,
        sw_data_8
    } state;

    if (b->last - b->pos < (ssize_t) sess->rest) {
        last = b->last;

    } else {
        last = b->pos + sess->rest;
    }

    state = sess->frame_state;

    if (state == sw_start) {

        if (sess->flags & NGX_HTTP_V2_ACK_FLAG) {
            ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                          "upstream sent ping frame with ack flag");
            return NGX_ERROR;
        }
    }

    for (p = b->pos; p < last; p++) {
        ch = *p;

#if 0
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                       "http proxy ping byte: %02Xd s:%d", ch, state);
#endif

        if (state < sw_data_8) {
            sess->ping_data[state] = ch;
            state++;

        } else {
            sess->ping_data[7] = ch;
            state = sw_start;

            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                           "http proxy ping");
        }
    }

    sess->rest -= p - b->pos;
    sess->frame_state = state;
    b->pos = p;

    if (sess->rest > 0) {
        return NGX_AGAIN;
    }

    sess->state = ngx_http_proxy_v2_st_start;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_send_settings_ack(ngx_http_proxy_v2_session_t *sess)
{
    ngx_chain_t                *cl;
    ngx_http_proxy_v2_frame_t  *f;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                   "http proxy send settings ack");

    cl = ngx_http_proxy_v2_get_control_buf(sess);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    f = (ngx_http_proxy_v2_frame_t *) cl->buf->last;
    cl->buf->last += sizeof(ngx_http_proxy_v2_frame_t);

    f->length_0 = 0;
    f->length_1 = 0;
    f->length_2 = 0;
    f->type = NGX_HTTP_V2_SETTINGS_FRAME;
    f->flags = NGX_HTTP_V2_ACK_FLAG;
    f->stream_id_0 = 0;
    f->stream_id_1 = 0;
    f->stream_id_2 = 0;
    f->stream_id_3 = 0;

    *sess->last_out = cl;
    sess->last_out = &cl->next;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_send_ping_ack(ngx_http_proxy_v2_session_t *sess)
{
    ngx_chain_t                *cl;
    ngx_http_proxy_v2_frame_t  *f;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                   "http proxy send ping ack");

    cl = ngx_http_proxy_v2_get_control_buf(sess);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    f = (ngx_http_proxy_v2_frame_t *) cl->buf->last;
    cl->buf->last += sizeof(ngx_http_proxy_v2_frame_t);

    f->length_0 = 0;
    f->length_1 = 0;
    f->length_2 = 8;
    f->type = NGX_HTTP_V2_PING_FRAME;
    f->flags = NGX_HTTP_V2_ACK_FLAG;
    f->stream_id_0 = 0;
    f->stream_id_1 = 0;
    f->stream_id_2 = 0;
    f->stream_id_3 = 0;

    cl->buf->last = ngx_copy(cl->buf->last, sess->ping_data, 8);

    *sess->last_out = cl;
    sess->last_out = &cl->next;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_send_window_update(ngx_http_proxy_v2_session_t *sess,
    ngx_uint_t id, size_t n)
{
    ngx_chain_t                *cl;
    ngx_http_proxy_v2_frame_t  *f;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                   "http proxy send window update: stream %ui, %uz", id, n);

    cl = ngx_http_proxy_v2_get_control_buf(sess);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    f = (ngx_http_proxy_v2_frame_t *) cl->buf->last;
    cl->buf->last += sizeof(ngx_http_proxy_v2_frame_t);

    f->length_0 = 0;
    f->length_1 = 0;
    f->length_2 = 4;
    f->type = NGX_HTTP_V2_WINDOW_UPDATE_FRAME;
    f->flags = 0;
    f->stream_id_0 = (u_char) ((id >> 24) & 0xff);
    f->stream_id_1 = (u_char) ((id >> 16) & 0xff);
    f->stream_id_2 = (u_char) ((id >> 8) & 0xff);
    f->stream_id_3 = (u_char) (id & 0xff);

    *cl->buf->last++ = (u_char) ((n >> 24) & 0xff);
    *cl->buf->last++ = (u_char) ((n >> 16) & 0xff);
    *cl->buf->last++ = (u_char) ((n >> 8) & 0xff);
    *cl->buf->last++ = (u_char) (n & 0xff);

    *sess->last_out = cl;
    sess->last_out = &cl->next;

    return NGX_OK;
}


static ngx_int_t
ngx_http_proxy_v2_send_rst_stream(ngx_http_proxy_v2_session_t *sess,
    ngx_uint_t id, ngx_uint_t error)
{
    ngx_chain_t                *cl;
    ngx_http_proxy_v2_frame_t  *f;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                   "http proxy send rst stream: stream %ui, error %ui",
                   id, error);

    cl = ngx_http_proxy_v2_get_control_buf(sess);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    f = (ngx_http_proxy_v2_frame_t *) cl->buf->last;
    cl->buf->last += sizeof(ngx_http_proxy_v2_frame_t);

    f->length_0 = 0;
    f->length_1 = 0;
    f->length_2 = 4;
    f->type = NGX_HTTP_V2_RST_STREAM_FRAME;
    f->flags = 0;
    f->stream_id_0 = (u_char) ((id >> 24) & 0xff);
    f->stream_id_1 = (u_char) ((id >> 16) & 0xff);
    f->stream_id_2 = (u_char) ((id >> 8) & 0xff);
    f->stream_id_3 = (u_char) (id & 0xff);

    *cl->buf->last++ = (u_char) ((error >> 24) & 0xff);
    *cl->buf->last++ = (u_char) ((error >> 16) & 0xff);
    *cl->buf->last++ = (u_char) ((error >> 8) & 0xff);
    *cl->buf->last++ = (u_char) (error & 0xff);

    *sess->last_out = cl;
    sess->last_out = &cl->next;

    return NGX_OK;
}


static ngx_chain_t *
ngx_http_proxy_v2_get_buf(ngx_http_proxy_v2_stream_t *stream)
{
    u_char       *start;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    cl = ngx_chain_get_free_buf(stream->connection.pool, &stream->free);
    if (cl == NULL) {
        return NULL;
    }

    b = cl->buf;
    start = b->start;

    if (start == NULL) {
        start = ngx_palloc(stream->connection.pool,
                           sizeof(ngx_http_proxy_v2_frame_t));
        if (start == NULL) {
            return NULL;
        }

    }

    ngx_memzero(b, sizeof(ngx_buf_t));

    b->start = start;
    b->pos = start;
    b->last = start;
    b->end = start + sizeof(ngx_http_proxy_v2_frame_t);

    b->tag = (ngx_buf_tag_t) &ngx_http_proxy_v2_body_output_filter;
    b->temporary = 1;
    b->flush = 1;

    return cl;
}


static ngx_chain_t *
ngx_http_proxy_v2_get_control_buf(ngx_http_proxy_v2_session_t *sess)
{
    u_char       *start;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    if (sess->free == NULL
        && sess->allocated >= NGX_HTTP_PROXY_V2_MAX_CONTROL_BUFS)
    {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "upstream sent too many queued http2 control frames");
        return NULL;
    }

    cl = ngx_chain_get_free_buf(sess->connection->pool, &sess->free);
    if (cl == NULL) {
        return NULL;
    }

    b = cl->buf;
    start = b->start;

    if (start == NULL) {

        /* the largest control payload is the eight-byte PING data */

        start = ngx_palloc(sess->connection->pool,
                           sizeof(ngx_http_proxy_v2_frame_t) + 8);
        if (start == NULL) {
            return NULL;
        }

        sess->allocated++;
    }

    ngx_memzero(b, sizeof(ngx_buf_t));

    b->start = start;
    b->pos = start;
    b->last = start;
    b->end = start + sizeof(ngx_http_proxy_v2_frame_t) + 8;

    b->tag = (ngx_buf_tag_t) &ngx_http_proxy_v2_module;
    b->temporary = 1;
    b->flush = 1;

    return cl;
}


static ngx_http_proxy_v2_stream_t *
ngx_http_proxy_v2_get_stream(ngx_connection_t *c)
{
    return (ngx_http_proxy_v2_stream_t *)
           ((u_char *) c - offsetof(ngx_http_proxy_v2_stream_t, connection));
}


static off_t
ngx_http_proxy_v2_stream_received(ngx_http_proxy_v2_stream_t *stream)
{
    /* keep the final count available after the session has been released */

    if (stream->session == NULL) {
        return stream->received;
    }

    /* attachments do not overlap while a session has one active stream */

    return stream->session->received - stream->received_start;
}


static ngx_http_proxy_v2_ctx_t *
ngx_http_proxy_v2_get_ctx(ngx_http_request_t *r)
{
    ngx_http_proxy_v2_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_proxy_v2_module);

    if (ctx == NULL || ctx->stream.session == NULL) {
        return NULL;
    }

    return ctx;
}


static ngx_int_t
ngx_http_proxy_v2_init_connection(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ngx_http_proxy_v2_ctx_t      *ctx;
    ngx_http_core_loc_conf_t     *clcf;
    ngx_http_proxy_v2_stream_t   *stream;
    ngx_http_proxy_v2_session_t  *sess;

    ctx = ngx_http_get_module_ctx(r, ngx_http_proxy_v2_module);

    if (ctx == NULL) {
        return NGX_ERROR;
    }

    sess = ngx_http_proxy_v2_get_session(&u->peer);
    if (sess == NULL) {
        return NGX_ERROR;
    }

    stream = &ctx->stream;

    if (ngx_http_proxy_v2_attach_stream(sess, stream) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_http_proxy_v2_init_stream(stream, r);

    /*
     * The upstream module set its event handler on the peer connection when
     * connecting; request I/O now goes through the stream connection.
     * The connect timer, if any, is not needed anymore.
     */

    stream->read.handler = u->peer.connection->read->handler;
    stream->write.handler = u->peer.connection->write->handler;

    if (u->peer.connection->write->timer_set) {
        ngx_del_timer(u->peer.connection->write);
    }

    u->connection = &stream->connection;

    ngx_http_proxy_v2_update_read_event(stream);
    ngx_http_proxy_v2_update_write_event(stream);

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    if (ngx_http_proxy_v2_init_session(sess, clcf->sendfile_max_chunk,
                                       u->conf->send_lowat)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, sess->connection->log, 0,
                   "http proxy stream connection %p, peer %p",
                   &stream->connection, sess->connection);

    return NGX_OK;
}


static void
ngx_http_proxy_v2_free_connection(ngx_http_request_t *r, ngx_http_upstream_t *u,
    ngx_int_t rc)
{
    ngx_connection_t             *c, *sc;
    ngx_http_proxy_v2_ctx_t      *ctx;
    ngx_http_proxy_v2_stream_t   *stream;
    ngx_http_proxy_v2_session_t  *sess;

    sc = u->connection;

    /* setup can fail before the request gets a stream I/O connection */

    if (sc == NULL || sc == u->peer.connection) {
        u->keepalive = 0;
        return;
    }

    /* response filters can reset module contexts before finalization */

    stream = ngx_http_proxy_v2_get_stream(sc);

    /* physical reads include framing; the pipe counts DATA payload only */

    if (u->state) {
        u->state->bytes_received = ngx_http_proxy_v2_stream_received(stream);
    }

    /*
     * The upstream connection, if any, is decided on when the stream is
     * released, which can happen before a cached response is sent, e.g.,
     * on a revalidation: a stream released already, or replaying headers
     * from a cache file, does not change the decision.
     */

    sess = stream->session;

    if (sess == NULL) {
        return;
    }

    if (sess->cached) {
        stream->session = NULL;
        return;
    }

    ctx = (ngx_http_proxy_v2_ctx_t *)
          ((u_char *) stream - offsetof(ngx_http_proxy_v2_ctx_t, stream));

    c = sess->connection;

    c->error |= sc->error;
    c->write->error |= sc->write->error;

    /* a rate-limit timer expiration is not an upstream timeout */

    c->read->timedout |= sc->read->timedout && !sc->read->delayed;
    c->write->timedout |= sc->write->timedout && !sc->write->delayed;

    if (stream->unprocessed) {
        u->unprocessed = 1;

        /* a refused stream or a graceful shutdown is not a peer failure */

        u->skip_peer_failure = stream->rst_received || stream->graceful;
    }

    /*
     * A header-only response is finalized before its body processing is
     * started, so the decision is made here from the stream state rather
     * than left to body completion.
     */

    u->keepalive = (rc == 0 || rc == NGX_AGAIN)
                   && ngx_http_proxy_v2_can_keepalive(ctx);

    if (!u->keepalive) {
        u->keepalive = ngx_http_proxy_v2_close_stream(r, stream);
    }

    /*
     * The stream is closed: the connection can be kept alive, though the
     * request body might not have been sent in full, which the keepalive
     * module checks.
     */

    if (u->keepalive) {
        u->request_body_sent = 1;
    }

    ngx_http_proxy_v2_release_session(sess, r);
    ngx_http_proxy_v2_detach_stream(stream);
}


static ngx_int_t
ngx_http_proxy_v2_init_session(ngx_http_proxy_v2_session_t *sess,
    off_t limit, size_t lowat)
{
    ngx_connection_t  *c;

    sess->send_limit = limit;
    sess->send_lowat = lowat;

    c = sess->connection;
    c->data = sess;
    c->read->handler = ngx_http_proxy_v2_session_read_handler;
    c->write->handler = ngx_http_proxy_v2_session_write_handler;

#if (NGX_HTTP_SSL)
    if (c->ssl) {
        sess->original_save_session = c->ssl->save_session;

        if (sess->original_save_session) {
            c->ssl->save_session = ngx_http_proxy_v2_ssl_save_session;
        }
    }
#endif

    if (sess->preface.pos != sess->preface.last) {
        /* queue the connection preface before the first request frames */

        if (ngx_http_proxy_v2_wire_add(sess, NULL, &sess->preface, NULL)
            != NGX_OK)
        {
            return NGX_ERROR;
        }
    }

    if (ngx_http_proxy_v2_session_handle_events(sess) != NGX_OK) {
        c->error = 1;
        return NGX_ERROR;
    }

    if (c->read->ready) {
        ngx_post_event(c->read, &ngx_posted_events);
    }

    return NGX_OK;
}


static void
ngx_http_proxy_v2_release_session(ngx_http_proxy_v2_session_t *sess,
    void *data)
{
    ngx_connection_t  *c;

    c = sess->connection;

    ngx_http_proxy_v2_clear_event(c->read);
    ngx_http_proxy_v2_clear_event(c->write);

    /*
     * The upstream module may close the connection after this, and SSL
     * callbacks invoked on close, such as session saving, expect c->data
     * to point to the request, as it does without a session.
     */

    c->data = data;

#if (NGX_HTTP_SSL)
    if (c->ssl) {
        c->ssl->save_session = sess->original_save_session;

        /* an outstanding SSL retry makes this connection non-reusable */

        c->ssl->saved_read_handler = NULL;
        c->ssl->saved_write_handler = NULL;
    }
#endif

    /* drop request buffers from the wire before detaching the streams */

    ngx_http_proxy_v2_wire_drop(sess);
}


static void
ngx_http_proxy_v2_init_stream(ngx_http_proxy_v2_stream_t *stream,
    ngx_http_request_t *r)
{
    ngx_event_t       *rev, *wev;
    ngx_connection_t  *c, *sc;

    c = stream->session->connection;
    sc = &stream->connection;
    rev = &stream->read;
    wev = &stream->write;

    /* each attempt gets its own request I/O state, independent of the peer */

    ngx_memzero(sc, sizeof(ngx_connection_t));
    ngx_memzero(rev, sizeof(ngx_event_t));
    ngx_memzero(wev, sizeof(ngx_event_t));

    sc->fd = (ngx_socket_t) -1;
    sc->data = r;
    sc->read = rev;
    sc->write = wev;
    sc->recv = ngx_http_proxy_v2_recv;
    sc->recv_chain = ngx_http_proxy_v2_recv_chain;
    sc->pool = r->pool;
    sc->log = c->log;
    sc->type = c->type;
    sc->number = c->number;
    sc->start_time = c->start_time;
    sc->sendfile = c->sendfile;
    sc->tcp_nodelay = NGX_TCP_NODELAY_DISABLED;
    sc->tcp_nopush = NGX_TCP_NOPUSH_DISABLED;

    stream->header_buffer_size = r->upstream->conf->buffer_size;

    rev->data = sc;
    rev->log = c->log;
    rev->handler = ngx_http_empty_handler;
    rev->virtual = 1;
    rev->available = -1;
    rev->index = NGX_INVALID_INDEX;

    /*
     * A virtual event is never registered in the kernel; it is marked
     * active so that request I/O code, such as the non-buffered upstream
     * path, arms read timeouts on it as on a registered event.
     */

    rev->active = 1;

    *wev = *rev;
    wev->write = 1;
}


static void
ngx_http_proxy_v2_session_read_handler(ngx_event_t *ev)
{
    size_t                        read;
    ssize_t                       n;
    ngx_int_t                     rc;
    ngx_buf_t                    *b;
    ngx_uint_t                    steps;
    ngx_connection_t             *c;
    ngx_http_proxy_v2_stream_t   *stream;
    ngx_http_proxy_v2_session_t  *sess;

    c = ev->data;
    sess = c->data;

    /* this request supplies only the single-active log context */

    stream = ngx_http_proxy_v2_first_stream(sess);

    if (stream) {
        ngx_http_set_log_request(c->log, stream->connection.data);
    }

    c->log->action = "reading upstream";

    if (sess->read_status != NGX_OK) {
        goto done;
    }

    if (c->error || ev->error) {
        sess->read_status = NGX_ERROR;
        goto done;
    }

    if (sess->read_gated) {
        goto done;
    }

    b = &sess->buffer;
    read = 0;
    steps = NGX_HTTP_PROXY_V2_READ_STEPS;

    for ( ;; ) {
        /* empty input can still finish a zero-length payload or padding */

        rc = ngx_http_proxy_v2_session_process_frame(sess, b);

        if (rc == NGX_OK) {
            if (--steps == 0) {
                ngx_post_event(ev, &ngx_posted_next_events);
                break;
            }

            continue;
        }

        if (rc != NGX_AGAIN) {
            sess->read_status = NGX_HTTP_UPSTREAM_INVALID_HEADER;
            break;
        }

        if (sess->read_gated) {
            /* the rest of the input is kept until the queue is consumed */
            break;
        }

        if (ngx_http_proxy_v2_reset_buffer(sess) != NGX_OK) {
            sess->read_status = NGX_ERROR;
            break;
        }

        /* drain supplied frames, but do not read past a complete response */

        if (!ngx_http_proxy_v2_need_read(sess)) {
#if (NGX_HTTP_SSL)
            /* finish an SSL_read() already waiting for write readiness */

            if (c->ssl == NULL || c->ssl->saved_write_handler == NULL)
#endif
            {
                break;
            }
        }

        /* yield after a bounded amount of input */

        if (read >= NGX_HTTP_PROXY_V2_READ_BUDGET) {
#if (NGX_HTTP_SSL)
            if (c->ssl
                && (c->ssl->saved_read_handler || c->ssl->saved_write_handler))
            {
                break;
            }
#endif

            if (ev->ready) {
                ngx_post_event(ev, &ngx_posted_next_events);
            }

            break;
        }

#if (NGX_HTTP_SSL)
        if (c->ssl && c->ssl->saved_read_handler) {
            /* SSL_write() owns transport reads until WANT_READ resolves */

            break;
        }
#endif

        n = c->recv(c, b->last, b->end - b->last);

        if (n == NGX_AGAIN) {
            break;
        }

        if (n == NGX_ERROR) {
            sess->read_status = NGX_ERROR;
            break;
        }

        if (n == 0) {
            sess->read_status = NGX_DONE;

#if (NGX_HTTP_SSL)
            if (c->ssl && c->ssl->saved_write_handler) {
                /* a terminal SSL_read() no longer owns write readiness */

                c->write->handler = c->ssl->saved_write_handler;
                c->ssl->saved_write_handler = NULL;
                c->write->ready = 1;
                ngx_post_event(c->write, &ngx_posted_events);
            }
#endif

            break;
        }

        sess->received += n;
        b->last += n;
        read += n;
    }

done:

    if (ngx_http_proxy_v2_session_handle_events(sess) != NGX_OK) {
        c->error = 1;

        if (sess->read_status != NGX_HTTP_UPSTREAM_INVALID_HEADER) {
            sess->read_status = NGX_ERROR;
        }
    }

    ngx_http_proxy_v2_session_notify_read(sess);
}


static void
ngx_http_proxy_v2_session_write_handler(ngx_event_t *ev)
{
    ngx_int_t                     rc;
    ngx_connection_t             *c;
    ngx_http_proxy_v2_stream_t   *stream;
    ngx_http_proxy_v2_session_t  *sess;

    c = ev->data;
    sess = c->data;
    stream = ngx_http_proxy_v2_first_stream(sess);

    if (stream == NULL) {
        return;
    }

    ngx_http_set_log_request(c->log, stream->connection.data);

    rc = NGX_OK;

    if (sess->read_status != NGX_ERROR
        && sess->read_status != NGX_HTTP_UPSTREAM_INVALID_HEADER)
    {
        rc = ngx_http_proxy_v2_session_write(sess);
    }

    if (rc == NGX_ERROR
        || ngx_http_proxy_v2_session_handle_events(sess) != NGX_OK)
    {
        c->error = 1;

        if (sess->read_status != NGX_HTTP_UPSTREAM_INVALID_HEADER) {
            sess->read_status = NGX_ERROR;
        }
    }

    if (sess->read_status == NGX_ERROR
        || sess->read_status == NGX_HTTP_UPSTREAM_INVALID_HEADER)
    {
        /* transport errors are reported on the read side */

        ngx_http_proxy_v2_session_notify_read(sess);
    }
}


static void
ngx_http_proxy_v2_session_notify_read(ngx_http_proxy_v2_session_t *sess)
{
    ngx_http_proxy_v2_stream_t  *stream;

    /* request callbacks run later and may remove streams from the index */

    for (stream = ngx_http_proxy_v2_first_stream(sess);
         stream;
         stream = ngx_http_proxy_v2_next_stream(stream))
    {
        ngx_http_proxy_v2_notify_read(stream);
    }
}


static ngx_int_t
ngx_http_proxy_v2_session_handle_events(ngx_http_proxy_v2_session_t *sess)
{
    ngx_uint_t         flags, read;
    ngx_connection_t  *c;

    c = sess->connection;
    flags = (c->read->eof || c->read->error) ? NGX_CLOSE_EVENT : 0;
    /*
     * With edge-triggered events, the read event stays registered while
     * the read gate is closed: the read handler returns early, and the
     * read event is posted when the gate is reopened.  This avoids
     * removing and adding the event for each gated chunk of input.
     */

    read = sess->read_status == NGX_OK
           && (!sess->read_gated || (ngx_event_flags & NGX_USE_CLEAR_EVENT))
           && ngx_http_proxy_v2_need_read(sess);

#if (NGX_HTTP_SSL)
    if ((sess->read_status == NGX_OK || sess->read_status == NGX_DONE)
        && c->ssl && c->ssl->saved_read_handler)
    {
        read = 1;
    }
#endif

    if (!read) {
        if (c->read->active
            && ngx_del_event(c->read, NGX_READ_EVENT, 0) != NGX_OK)
        {
            return NGX_ERROR;
        }

        if (c->read->posted) {
            ngx_delete_posted_event(c->read);
        }

    } else if (ngx_handle_read_event(c->read, flags) != NGX_OK) {
        return NGX_ERROR;
    }

    if (sess->read_status == NGX_ERROR
        || sess->read_status == NGX_HTTP_UPSTREAM_INVALID_HEADER)
    {
        if (c->write->active
            && ngx_del_event(c->write, NGX_WRITE_EVENT, 0) != NGX_OK)
        {
            return NGX_ERROR;
        }

        return NGX_OK;
    }

    if (ngx_handle_write_event(c->write, sess->send_lowat) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_proxy_v2_update_read_event(ngx_http_proxy_v2_stream_t *stream)
{
    off_t                         received;
    ngx_event_t                  *ev;
    ngx_http_request_t           *r;
    ngx_http_upstream_t          *u;
    ngx_http_proxy_v2_session_t  *sess;

    sess = stream->session;
    ev = &stream->read;
    r = stream->connection.data;
    u = r->upstream;
    received = ngx_http_proxy_v2_stream_received(stream);

    if (u->state) {
        u->state->bytes_received = received;
    }

    if (received) {
        u->response_received = 1;
    }

    ev->ready = stream->headers || stream->data_size || stream->trailers
                || stream->rst_parsed || stream->done || stream->unprocessed
                || sess->read_status != NGX_OK;

    /* let queued headers and resets reach process_header() before errors */

    ev->error = (sess->read_status == NGX_ERROR || stream->unprocessed)
                && stream->headers == NULL && !stream->rst_parsed;

    /* the transport end is seen by the request after queued input */

    ev->eof = sess->read_status == NGX_DONE
              && stream->headers == NULL && stream->data_size == 0
              && stream->trailers == NULL;
}


static void
ngx_http_proxy_v2_notify_read(ngx_http_proxy_v2_stream_t *stream)
{
    ngx_http_proxy_v2_update_read_event(stream);

    if (stream->read.ready) {
        ngx_post_event(&stream->read, &ngx_posted_events);
    }
}


static void
ngx_http_proxy_v2_update_write_event(ngx_http_proxy_v2_stream_t *stream)
{
    ngx_connection_t  *c, *sc;

    c = stream->session->connection;
    sc = &stream->connection;

    stream->write.ready = !stream->output_blocked;
    stream->write.error = c->write->error;
    sc->error |= c->error;
    sc->sent = c->sent;
}


static ssize_t
ngx_http_proxy_v2_recv(ngx_connection_t *c, u_char *buf, size_t size)
{
    size_t                       n;
    ngx_http_proxy_v2_stream_t  *stream;

    stream = ngx_http_proxy_v2_get_stream(c);

    if (stream->data_size == 0) {
        return ngx_http_proxy_v2_recv_status(stream);
    }

    if (size == 0) {
        return NGX_AGAIN;
    }

    n = ngx_http_proxy_v2_read_data(stream, buf, size);
    ngx_http_proxy_v2_update_read_event(stream);

    if (ngx_http_proxy_v2_replenish_window(stream) != NGX_OK) {
        /* deliver copied bytes before reporting the failure */

        stream->session->read_status = NGX_ERROR;
        ngx_http_proxy_v2_session_notify_read(stream->session);
    }

    return n;
}


static ssize_t
ngx_http_proxy_v2_recv_chain(ngx_connection_t *c, ngx_chain_t *in, off_t limit)
{
    size_t                       size;
    ssize_t                      n;
    ngx_chain_t                 *cl;
    ngx_http_proxy_v2_stream_t  *stream;

    stream = ngx_http_proxy_v2_get_stream(c);

    if (stream->data_size == 0) {
        return ngx_http_proxy_v2_recv_status(stream);
    }

    if (limit == 0) {
        limit = NGX_MAX_OFF_T_VALUE;
    }

    n = 0;

    for (cl = in; cl && stream->data_size && n < limit; cl = cl->next) {
        size = cl->buf->end - cl->buf->last;
        size = (size_t) ngx_min((off_t) size, limit - n);

        /* event_pipe advances buffer positions after recv_chain() returns */

        n += ngx_http_proxy_v2_read_data(stream, cl->buf->last, size);
    }

    ngx_http_proxy_v2_update_read_event(stream);

    if (n == 0) {
        return NGX_AGAIN;
    }

    if (ngx_http_proxy_v2_replenish_window(stream) != NGX_OK) {
        stream->session->read_status = NGX_ERROR;
        ngx_http_proxy_v2_session_notify_read(stream->session);
    }

    return n;
}


static size_t
ngx_http_proxy_v2_read_data(ngx_http_proxy_v2_stream_t *stream, u_char *buf,
    size_t size)
{
    size_t                        n, copied;
    ngx_http_proxy_v2_segment_t  *seg;
    ngx_http_proxy_v2_session_t  *sess;

    size = ngx_min(size, stream->data_size);

    if (size == 0) {
        return 0;
    }

    sess = stream->session;

    for (copied = 0; copied < size; copied += n) {
        seg = stream->data;

        n = ngx_min((size_t) (seg->last - seg->pos), size - copied);
        ngx_memcpy(buf + copied, seg->pos, n);
        seg->pos += n;

        if (seg->pos == seg->last) {
            stream->data = seg->next;

            if (stream->data == NULL) {
                stream->data_last = &stream->data;
            }

            ngx_http_proxy_v2_release_chunk(sess, seg->chunk);

            seg->next = stream->data_free;
            stream->data_free = seg;
        }
    }

    stream->data_size -= size;

    if (sess->read_gated
        && NGX_HTTP_PROXY_V2_DATA_QUEUE_SIZE - stream->data_size
           >= NGX_HTTP_V2_DEFAULT_FRAME_SIZE)
    {
        /* resume parsing the kept input and reading the transport */

        sess->read_gated = 0;
        ngx_post_event(sess->connection->read, &ngx_posted_events);
    }

    return size;
}


static ssize_t
ngx_http_proxy_v2_recv_status(ngx_http_proxy_v2_stream_t *stream)
{
    ngx_int_t                 rc;
    ngx_http_request_t       *r;
    ngx_http_proxy_v2_ctx_t  *ctx;

    r = stream->connection.data;
    ctx = ngx_http_proxy_v2_get_ctx(r);

    if (ctx == NULL) {
        stream->read.error = 1;
        return NGX_ERROR;
    }

    rc = ngx_http_proxy_v2_finish_body(r, ctx);

    if (rc == NGX_ERROR) {
        stream->read.error = 1;
        return NGX_ERROR;
    }

    stream->read.ready = 0;

    if (rc == NGX_DONE) {
        /* logical EOF is published only after pipe completion is settled */

        stream->read.eof = 1;
        return 0;
    }

    return NGX_AGAIN;
}


static void
ngx_http_proxy_v2_clear_event(ngx_event_t *ev)
{
    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    if (ev->posted) {
        ngx_delete_posted_event(ev);
    }

    ev->handler = ngx_http_empty_handler;
}


static ngx_int_t
ngx_http_proxy_v2_reset_buffer(ngx_http_proxy_v2_session_t *sess)
{
    ngx_buf_t                  *b;
    ngx_http_proxy_v2_chunk_t  *chunk;

    /* all input is parsed; queued DATA may still reference the chunk */

    chunk = sess->chunk;

    if (chunk == NULL || chunk->refs) {

        chunk = sess->free_chunks;

        if (chunk) {
            sess->free_chunks = chunk->next;

        } else {
            chunk = ngx_palloc(sess->connection->pool,
                               sizeof(ngx_http_proxy_v2_chunk_t)
                               + NGX_HTTP_PROXY_V2_READ_BUFFER_SIZE);
            if (chunk == NULL) {
                return NGX_ERROR;
            }

            chunk->start = (u_char *) (chunk + 1);
            chunk->end = chunk->start + NGX_HTTP_PROXY_V2_READ_BUFFER_SIZE;
        }

        chunk->next = NULL;
        chunk->refs = 0;

        /*
         * The previous chunk, if any, is still referenced; it is added
         * to the free list when the last reference is released.
         */

        sess->chunk = chunk;
    }

    b = &sess->buffer;

    b->start = chunk->start;
    b->end = chunk->end;
    b->pos = b->start;
    b->last = b->start;

    return NGX_OK;
}


static void
ngx_http_proxy_v2_release_chunk(ngx_http_proxy_v2_session_t *sess,
    ngx_http_proxy_v2_chunk_t *chunk)
{
    if (--chunk->refs == 0 && chunk != sess->chunk) {
        chunk->next = sess->free_chunks;
        sess->free_chunks = chunk;
    }
}


static void
ngx_http_proxy_v2_release_data(ngx_http_proxy_v2_stream_t *stream)
{
    ngx_http_proxy_v2_segment_t  *seg;

    while (stream->data) {
        seg = stream->data;
        stream->data = seg->next;

        ngx_http_proxy_v2_release_chunk(stream->session, seg->chunk);

        seg->next = stream->data_free;
        stream->data_free = seg;
    }

    stream->data_last = &stream->data;
    stream->data_size = 0;
}


#if (NGX_HTTP_SSL)

static void
ngx_http_proxy_v2_ssl_save_session(ngx_connection_t *c)
{
    ngx_http_request_t           *r;
    ngx_http_upstream_t          *u;
    ngx_http_proxy_v2_stream_t   *stream;
    ngx_http_proxy_v2_session_t  *sess;

    sess = c->data;

    if (sess == NULL) {
        return;
    }

    stream = ngx_http_proxy_v2_first_stream(sess);

    if (stream == NULL) {
        return;
    }

    r = stream->connection.data;
    u = r->upstream;

    ngx_http_set_log_request(r->connection->log, r);

    u->peer.save_session(&u->peer, u->peer.data);
}

#endif


static ngx_http_proxy_v2_session_t *
ngx_http_proxy_v2_get_session(ngx_peer_connection_t *pc)
{
    ngx_connection_t             *c;
    ngx_pool_cleanup_t           *cln;
    ngx_http_proxy_v2_session_t  *sess;

    c = pc->connection;

    if (pc->cached) {

        sess = ngx_http_proxy_v2_find_session(c);
        if (sess == NULL) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "no session found for "
                          "keepalive http2 connection");
            return NULL;
        }

        return sess;
    }

    cln = ngx_pool_cleanup_add(c->pool, sizeof(ngx_http_proxy_v2_session_t));
    if (cln == NULL) {
        return NULL;
    }

    cln->handler = ngx_http_proxy_v2_cleanup;
    sess = cln->data;
    ngx_memzero(sess, sizeof(ngx_http_proxy_v2_session_t));
    sess->connection = c;

    ngx_queue_init(&sess->attached);
    ngx_rbtree_init(&sess->streams, &sess->sentinel,
                    ngx_rbtree_insert_value);

    sess->buffer.temporary = 1;

    if (ngx_http_proxy_v2_reset_buffer(sess) != NGX_OK) {
        return NULL;
    }

    sess->last_out = &sess->out;
    sess->wire_last = &sess->wire;

    sess->preface.start = ngx_http_proxy_v2_connection_start;
    sess->preface.pos = sess->preface.start;
    sess->preface.last = sess->preface.start
                        + sizeof(ngx_http_proxy_v2_connection_start) - 1;
    sess->preface.end = sess->preface.last;
    sess->preface.memory = 1;
    sess->preface.flush = 1;

    sess->init_window = NGX_HTTP_V2_DEFAULT_WINDOW;
    sess->send_window = NGX_HTTP_V2_DEFAULT_WINDOW;
    sess->recv_window = NGX_HTTP_V2_MAX_WINDOW;

    return sess;
}


static ngx_http_proxy_v2_session_t *
ngx_http_proxy_v2_find_session(ngx_connection_t *c)
{
    ngx_pool_cleanup_t  *cln;

    if (c->pool) {
        for (cln = c->pool->cleanup; cln; cln = cln->next) {
            if (cln->handler == ngx_http_proxy_v2_cleanup) {
                return cln->data;
            }
        }
    }

    return NULL;
}


static ngx_http_proxy_v2_stream_t *
ngx_http_proxy_v2_find_stream(ngx_http_proxy_v2_session_t *sess, ngx_uint_t id)
{
    ngx_rbtree_node_t  *node, *sentinel;

    node = sess->streams.root;
    sentinel = sess->streams.sentinel;

    while (node != sentinel) {
        if (id < node->key) {
            node = node->left;
            continue;
        }

        if (id > node->key) {
            node = node->right;
            continue;
        }

        return ngx_rbtree_data(node, ngx_http_proxy_v2_stream_t, node);
    }

    return NULL;
}


static ngx_http_proxy_v2_stream_t *
ngx_http_proxy_v2_first_stream(ngx_http_proxy_v2_session_t *sess)
{
    if (ngx_queue_empty(&sess->attached)) {
        return NULL;
    }

    return ngx_queue_data(ngx_queue_head(&sess->attached),
                          ngx_http_proxy_v2_stream_t, queue);
}


static ngx_http_proxy_v2_stream_t *
ngx_http_proxy_v2_next_stream(ngx_http_proxy_v2_stream_t *stream)
{
    ngx_queue_t  *q;

    q = ngx_queue_next(&stream->queue);

    if (q == ngx_queue_sentinel(&stream->session->attached)) {
        return NULL;
    }

    return ngx_queue_data(q, ngx_http_proxy_v2_stream_t, queue);
}


static ngx_int_t
ngx_http_proxy_v2_attach_stream(ngx_http_proxy_v2_session_t *sess,
    ngx_http_proxy_v2_stream_t *stream)
{
    /* requests are still attached one at a time */

    if (!ngx_queue_empty(&sess->attached)
        || ngx_http_proxy_v2_output_pending(sess)
        || sess->read_status != NGX_OK
        || sess->buffer.pos != sess->buffer.last
        || sess->state != ngx_http_proxy_v2_st_start || sess->continuation_sid)
    {
        ngx_log_error(NGX_LOG_ERR, sess->connection->log, 0,
                      "http2 session is still in use");
        return NGX_ERROR;
    }

    /* the identifier is allocated when the stream is opened */

    stream->id = 0;
    stream->seq = ++sess->seq;

    stream->send_window = sess->init_window;
    stream->recv_window = NGX_HTTP_V2_MAX_WINDOW;

    stream->received_start = sess->received;
    stream->received = 0;

    ngx_queue_insert_tail(&sess->attached, &stream->queue);

    stream->session = sess;

    return NGX_OK;
}


static void
ngx_http_proxy_v2_detach_stream(ngx_http_proxy_v2_stream_t *stream)
{
    ngx_http_proxy_v2_session_t  *sess;

    sess = stream->session;

    stream->received = ngx_http_proxy_v2_stream_received(stream);

    /* remove request-pool storage from the session before releasing it */

    if (stream->id) {
        ngx_rbtree_delete(&sess->streams, &stream->node);
    }

    ngx_queue_remove(&stream->queue);

    ngx_http_proxy_v2_clear_event(&stream->read);
    ngx_http_proxy_v2_clear_event(&stream->write);
    stream->read.closed = 1;
    stream->write.closed = 1;
    stream->connection.data = NULL;

    stream->header_block = NULL;
    stream->headers = NULL;
    stream->last_headers = &stream->headers;
    stream->trailers = NULL;
    ngx_http_proxy_v2_release_data(stream);
    stream->rst_parsed = 0;
    stream->session = NULL;
}


static void
ngx_http_proxy_v2_cleanup(void *data)
{
    ngx_http_proxy_v2_session_t  *sess = data;

    ngx_http_proxy_v2_stream_t  *stream;

    if (ngx_queue_empty(&sess->attached)) {
        return;
    }

    ngx_http_proxy_v2_release_session(sess, NULL);

    while ((stream = ngx_http_proxy_v2_first_stream(sess)) != NULL) {
        ngx_http_proxy_v2_detach_stream(stream);
    }
}


static void
ngx_http_proxy_v2_abort_request(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "abort proxy http2 request");
    return;
}


static void
ngx_http_proxy_v2_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize proxy http2 request");
    return;
}
