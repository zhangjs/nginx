
/*
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_UPSTREAM_KEEPALIVE_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_KEEPALIVE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


/*
 * A connection shared by several requests, such as an HTTP/2 session.
 * The protocol module lists it while it can take more requests, which
 * then get it from the keepalive peer instead of a new or an idle
 * connection.  When no request uses it anymore, the protocol module
 * either hands it over to the cache of idle connections, or removes it.
 */

typedef struct {
    ngx_queue_t                        queue;
    void                              *conf;
    ngx_connection_t                  *connection;
    ngx_uint_t                         spare;
    void                              *tag;
    socklen_t                          socklen;
    ngx_sockaddr_t                     sockaddr;
    unsigned                           listed:1;
} ngx_http_upstream_keepalive_shared_t;


ngx_int_t ngx_http_upstream_keepalive_shared_add(ngx_peer_connection_t *pc,
    ngx_http_upstream_keepalive_shared_t *m, ngx_uint_t spare);
void ngx_http_upstream_keepalive_shared_update(
    ngx_http_upstream_keepalive_shared_t *m, ngx_uint_t spare);
ngx_int_t ngx_http_upstream_keepalive_shared_idle(
    ngx_http_upstream_keepalive_shared_t *m);
void ngx_http_upstream_keepalive_shared_remove(
    ngx_http_upstream_keepalive_shared_t *m);


#endif /* _NGX_HTTP_UPSTREAM_KEEPALIVE_H_INCLUDED_ */
