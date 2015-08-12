
/*
 * Copyright (C) Roman Arutyunyan
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_md5.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_netcall_module.h"
#include "ngx_rtmp_record_module.h"
#include "ngx_rtmp_relay_module.h"
#include "ngx_rtmp_live_module.h"


static ngx_rtmp_connect_pt                      next_connect;
static ngx_rtmp_disconnect_pt                   next_disconnect;
static ngx_rtmp_publish_pt                      next_publish;
static ngx_rtmp_play_pt                         next_play;
static ngx_rtmp_close_stream_pt                 next_close_stream;
static ngx_rtmp_record_done_pt                  next_record_done;


static char *ngx_rtmp_notify_sp_ip(ngx_conf_t *cf, ngx_command_t *cmd,
       void *conf);
static char *ngx_rtmp_notify_on_srv_event(ngx_conf_t *cf, ngx_command_t *cmd,
       void *conf);
static char *ngx_rtmp_notify_on_app_event(ngx_conf_t *cf, ngx_command_t *cmd,
       void *conf);
static char *ngx_rtmp_notify_method(ngx_conf_t *cf, ngx_command_t *cmd,
       void *conf);
static void * ngx_rtmp_notify_create_main_conf(ngx_conf_t *cf);
static ngx_int_t ngx_rtmp_notify_postconfiguration(ngx_conf_t *cf);
static void * ngx_rtmp_notify_create_app_conf(ngx_conf_t *cf);
static char * ngx_rtmp_notify_merge_app_conf(ngx_conf_t *cf,
       void *parent, void *child);
static void * ngx_rtmp_notify_create_srv_conf(ngx_conf_t *cf);
static char * ngx_rtmp_notify_merge_srv_conf(ngx_conf_t *cf, void *parent,
       void *child);
static ngx_int_t ngx_rtmp_notify_done(ngx_rtmp_session_t *s, char *cbname,
       ngx_uint_t url_idx);

ngx_str_t   ngx_rtmp_notify_urlencoded =
            ngx_string("application/x-www-form-urlencoded");


#define NGX_RTMP_NOTIFY_PUBLISHING              0x01
#define NGX_RTMP_NOTIFY_PLAYING                 0x02


enum {
    NGX_RTMP_NOTIFY_PLAY,
    NGX_RTMP_NOTIFY_PUBLISH,
    NGX_RTMP_NOTIFY_PLAY_DONE,
    NGX_RTMP_NOTIFY_PUBLISH_DONE,
    NGX_RTMP_NOTIFY_DONE,
    NGX_RTMP_NOTIFY_RECORD_DONE,
    NGX_RTMP_NOTIFY_UPDATE,
    NGX_RTMP_NOTIFY_APP_MAX
};


enum {
    NGX_RTMP_NOTIFY_CONNECT,
    NGX_RTMP_NOTIFY_DISCONNECT,
    NGX_RTMP_NOTIFY_SRV_MAX
};

enum {
	NGX_RTMP_NOTIFY_SP_TELECOM,
	NGX_RTMP_NOTIFY_SP_UNICOM,
	NGX_RTMP_NOTIFY_SP_MAX
};

typedef struct {
    ngx_url_t                                  *url[NGX_RTMP_NOTIFY_APP_MAX];
    ngx_flag_t                                  active;
    ngx_uint_t                                  method;
    ngx_msec_t                                  update_timeout;
    ngx_flag_t                                  update_strict;
    ngx_flag_t                                  relay_redirect;
    ngx_uint_t                                  update_switch;
    ngx_uint_t                                  update_fail_ignore;
	ngx_str_t                                   socket_dir;
} ngx_rtmp_notify_app_conf_t;


typedef struct {
    ngx_url_t                                  *url[NGX_RTMP_NOTIFY_SRV_MAX];
    ngx_uint_t                                  method;
} ngx_rtmp_notify_srv_conf_t;

typedef struct {
    ngx_str_t                                   iplist[NGX_RTMP_NOTIFY_SP_MAX];
} ngx_rtmp_notify_main_conf_t;


typedef struct {
    ngx_uint_t                                  flags;
    u_char                                      name[NGX_RTMP_MAX_NAME];
    u_char                                      args[NGX_RTMP_MAX_ARGS];
    ngx_event_t                                 update_evt;
    time_t                                      start;
} ngx_rtmp_notify_ctx_t;


typedef struct {
    u_char                                     *cbname;
    ngx_uint_t                                  url_idx;
} ngx_rtmp_notify_done_t;


static ngx_command_t  ngx_rtmp_notify_commands[] = {

	{ ngx_string("telecom_ip"),
      NGX_RTMP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_MAIN_CONF_OFFSET,
      offsetof(ngx_rtmp_notify_main_conf_t, iplist) +
      NGX_RTMP_NOTIFY_SP_TELECOM * sizeof(ngx_str_t),
      NULL },

	{ ngx_string("unicom_ip"),
      NGX_RTMP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_MAIN_CONF_OFFSET,
      offsetof(ngx_rtmp_notify_main_conf_t, iplist) +
      NGX_RTMP_NOTIFY_SP_UNICOM * sizeof(ngx_str_t),
      NULL },

    { ngx_string("on_connect"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_srv_event,
      NGX_RTMP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("on_disconnect"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_srv_event,
      NGX_RTMP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("on_publish"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_app_event,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("on_play"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_app_event,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("on_publish_done"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_app_event,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("on_play_done"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_app_event,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("on_done"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_app_event,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("on_record_done"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_RTMP_REC_CONF|
                         NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_app_event,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("on_update"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_on_app_event,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("notify_method"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_rtmp_notify_method,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("notify_update_timeout"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_notify_app_conf_t, update_timeout),
      NULL },

    { ngx_string("notify_update_strict"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_notify_app_conf_t, update_strict),
      NULL },

    { ngx_string("notify_relay_redirect"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_notify_app_conf_t, relay_redirect),
      NULL },
      
    { ngx_string("notify_update_switch"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_notify_app_conf_t, update_switch),
      NULL },

    { ngx_string("notify_update_fail_ignore"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_notify_app_conf_t, update_fail_ignore),
      NULL },

	{ ngx_string("rtmp_socket_dir"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_notify_app_conf_t, socket_dir),
      NULL },
      
      ngx_null_command
};


static ngx_rtmp_module_t  ngx_rtmp_notify_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_rtmp_notify_postconfiguration,      /* postconfiguration */
    ngx_rtmp_notify_create_main_conf,       /* create main configuration */
    NULL,                                   /* init main configuration */
    ngx_rtmp_notify_create_srv_conf,        /* create server configuration */
    ngx_rtmp_notify_merge_srv_conf,         /* merge server configuration */
    ngx_rtmp_notify_create_app_conf,        /* create app configuration */
    ngx_rtmp_notify_merge_app_conf          /* merge app configuration */
};


ngx_module_t  ngx_rtmp_notify_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_notify_module_ctx,            /* module context */
    ngx_rtmp_notify_commands,               /* module directives */
    NGX_RTMP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_rtmp_notify_create_app_conf(ngx_conf_t *cf)
{
    ngx_rtmp_notify_app_conf_t     *nacf;
    ngx_uint_t                      n;

    nacf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_notify_app_conf_t));
    if (nacf == NULL) {
        return NULL;
    }

    for (n = 0; n < NGX_RTMP_NOTIFY_APP_MAX; ++n) {
        nacf->url[n] = NGX_CONF_UNSET_PTR;
    }

    nacf->method = NGX_CONF_UNSET_UINT;
    nacf->update_timeout = NGX_CONF_UNSET_MSEC;
    nacf->update_strict = NGX_CONF_UNSET;
    nacf->relay_redirect = NGX_CONF_UNSET;
    nacf->update_switch = NGX_CONF_UNSET ;
    nacf->update_fail_ignore = NGX_CONF_UNSET ;

    return nacf;
}


static char *
ngx_rtmp_notify_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_notify_app_conf_t *prev = parent;
    ngx_rtmp_notify_app_conf_t *conf = child;
    ngx_uint_t                  n;

    for (n = 0; n < NGX_RTMP_NOTIFY_APP_MAX; ++n) {
        ngx_conf_merge_ptr_value(conf->url[n], prev->url[n], NULL);
        if (conf->url[n]) {
            conf->active = 1;
        }
    }

    if (conf->active) {
        prev->active = 1;
    }

    ngx_conf_merge_uint_value(conf->method, prev->method,
                              NGX_RTMP_NETCALL_HTTP_POST);
    ngx_conf_merge_msec_value(conf->update_timeout, prev->update_timeout,
                              30000);
    ngx_conf_merge_value(conf->update_strict, prev->update_strict, 0);
    ngx_conf_merge_value(conf->relay_redirect, prev->relay_redirect, 0);
    ngx_conf_merge_value(conf->update_switch, prev->update_switch, NGX_RTMP_NOTIFY_PUBLISHING); 
    ngx_conf_merge_value(conf->update_fail_ignore, prev->update_fail_ignore, 0); 

    return NGX_CONF_OK;
}


static void *
ngx_rtmp_notify_create_main_conf(ngx_conf_t *cf)
{
    ngx_rtmp_notify_main_conf_t  *nmcf;

    nmcf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_notify_main_conf_t));
    if (nmcf == NULL) {
        return NULL;
    }

    return nmcf;
}


static void *
ngx_rtmp_notify_create_srv_conf(ngx_conf_t *cf)
{
    ngx_rtmp_notify_srv_conf_t     *nscf;
    ngx_uint_t                      n;

    nscf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_notify_srv_conf_t));
    if (nscf == NULL) {
        return NULL;
    }

    for (n = 0; n < NGX_RTMP_NOTIFY_SRV_MAX; ++n) {
        nscf->url[n] = NGX_CONF_UNSET_PTR;
    }

    nscf->method = NGX_CONF_UNSET_UINT;

    return nscf;
}


static char *
ngx_rtmp_notify_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_notify_srv_conf_t *prev = parent;
    ngx_rtmp_notify_srv_conf_t *conf = child;
    ngx_uint_t                  n;

    for (n = 0; n < NGX_RTMP_NOTIFY_SRV_MAX; ++n) {
        ngx_conf_merge_ptr_value(conf->url[n], prev->url[n], NULL);
    }

    ngx_conf_merge_uint_value(conf->method, prev->method,
                              NGX_RTMP_NETCALL_HTTP_POST);

    return NGX_CONF_OK;
}


static ngx_chain_t *
ngx_rtmp_notify_create_request(ngx_rtmp_session_t *s, ngx_pool_t *pool,
                                   ngx_uint_t url_idx, ngx_chain_t *args)
{
    ngx_rtmp_notify_app_conf_t *nacf;
    ngx_chain_t                *al, *bl, *cl, *ret;
    ngx_url_t                  *url;

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

    url = nacf->url[url_idx];

    al = ngx_rtmp_netcall_http_format_session(s, pool);
    if (al == NULL) {
        return NULL;
    }

    al->next = args;

    bl = NULL;

    if (nacf->method == NGX_RTMP_NETCALL_HTTP_POST) {
        cl = al;
        al = bl;
        bl = cl;
    }

    ret = ngx_rtmp_netcall_http_format_request(nacf->method, &url->host,
                                                &url->uri, al, bl, pool,
                                                &ngx_rtmp_notify_urlencoded);
    return ret;
}


static ngx_chain_t *
ngx_rtmp_notify_connect_create(ngx_rtmp_session_t *s, void *arg,
        ngx_pool_t *pool)
{
    ngx_rtmp_connect_t             *v = arg;

    ngx_rtmp_notify_srv_conf_t     *nscf;
    ngx_url_t                      *url;
    ngx_chain_t                    *al, *bl;
    ngx_buf_t                      *b;
    ngx_str_t                      *addr_text;
    size_t                          app_len, args_len, flashver_len,
                                    swf_url_len, tc_url_len, page_url_len;

    nscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_notify_module);

    al = ngx_alloc_chain_link(pool);
    if (al == NULL) {
        return NULL;
    }

    /* these values are still missing in session
     * so we have to construct the request from
     * connection struct */

    app_len = ngx_strlen(v->app);
    args_len = ngx_strlen(v->args);
    flashver_len = ngx_strlen(v->flashver);
    swf_url_len = ngx_strlen(v->swf_url);
    tc_url_len = ngx_strlen(v->tc_url);
    page_url_len = ngx_strlen(v->page_url);

    addr_text = &s->connection->addr_text;

    b = ngx_create_temp_buf(pool,
            sizeof("call=connect") - 1 +
            sizeof("&app=") - 1 + app_len * 3 +
            sizeof("&flashver=") - 1 + flashver_len * 3 +
            sizeof("&swfurl=") - 1 + swf_url_len * 3 +
            sizeof("&tcurl=") - 1 + tc_url_len * 3 +
            sizeof("&pageurl=") - 1 + page_url_len * 3 +
            sizeof("&addr=") - 1 + addr_text->len * 3 +
            sizeof("&epoch=") - 1 + NGX_INT32_LEN +
            1 + args_len
        );

    if (b == NULL) {
        return NULL;
    }

    al->buf = b;
    al->next = NULL;

    b->last = ngx_cpymem(b->last, (u_char*) "app=", sizeof("app=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->app, app_len,
                                       NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&flashver=",
                         sizeof("&flashver=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->flashver, flashver_len,
                                       NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&swfurl=",
                         sizeof("&swfurl=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->swf_url, swf_url_len,
                                       NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&tcurl=",
                         sizeof("&tcurl=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->tc_url, tc_url_len,
                                       NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&pageurl=",
                         sizeof("&pageurl=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->page_url, page_url_len,
                                       NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&addr=", sizeof("&addr=") -1);
    b->last = (u_char*) ngx_escape_uri(b->last, addr_text->data,
                                       addr_text->len, NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&epoch=", sizeof("&epoch=") -1);
    b->last = ngx_sprintf(b->last, "%uD", (uint32_t) s->epoch);

    b->last = ngx_cpymem(b->last, (u_char*) "&call=connect",
                         sizeof("&call=connect") - 1);

    if (args_len) {
        *b->last++ = '&';
        b->last = (u_char *) ngx_cpymem(b->last, v->args, args_len);
    }

    url = nscf->url[NGX_RTMP_NOTIFY_CONNECT];

    bl = NULL;

    if (nscf->method == NGX_RTMP_NETCALL_HTTP_POST) {
        bl = al;
        al = NULL;
    }

    return ngx_rtmp_netcall_http_format_request(nscf->method, &url->host,
                                                &url->uri, al, bl, pool,
                                                &ngx_rtmp_notify_urlencoded);
}


static ngx_chain_t *
ngx_rtmp_notify_disconnect_create(ngx_rtmp_session_t *s, void *arg,
        ngx_pool_t *pool)
{
    ngx_rtmp_notify_srv_conf_t     *nscf;
    ngx_url_t                      *url;
    ngx_chain_t                    *al, *bl, *pl;
    ngx_buf_t                      *b;

    nscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_notify_module);

    pl = ngx_alloc_chain_link(pool);
    if (pl == NULL) {
        return NULL;
    }

    b = ngx_create_temp_buf(pool,
                            sizeof("&call=disconnect") +
                            sizeof("&app=") + s->app.len * 3 +
                            1 + s->args.len);
    if (b == NULL) {
        return NULL;
    }

    pl->buf = b;
    pl->next = NULL;

    b->last = ngx_cpymem(b->last, (u_char*) "&call=disconnect",
                         sizeof("&call=disconnect") - 1);

    b->last = ngx_cpymem(b->last, (u_char*) "&app=", sizeof("&app=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, s->app.data, s->app.len,
                                       NGX_ESCAPE_ARGS);

    if (s->args.len) {
        *b->last++ = '&';
        b->last = (u_char *) ngx_cpymem(b->last, s->args.data, s->args.len);
    }

    url = nscf->url[NGX_RTMP_NOTIFY_DISCONNECT];

    al = ngx_rtmp_netcall_http_format_session(s, pool);
    if (al == NULL) {
        return NULL;
    }

    al->next = pl;

    bl = NULL;

    if (nscf->method == NGX_RTMP_NETCALL_HTTP_POST) {
        bl = al;
        al = NULL;
    }

    return ngx_rtmp_netcall_http_format_request(nscf->method, &url->host,
                                                &url->uri, al, bl, pool,
                                                &ngx_rtmp_notify_urlencoded);
}


static ngx_chain_t *
ngx_rtmp_notify_publish_create(ngx_rtmp_session_t *s, void *arg,
        ngx_pool_t *pool)
{
    ngx_rtmp_publish_t             *v = arg;

    ngx_chain_t                    *pl;
    ngx_buf_t                      *b;
    size_t                          name_len, type_len, args_len, srv_len;
    ngx_rtmp_notify_ctx_t          *ctx;
    ngx_rtmp_notify_app_conf_t     *nacf;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);
    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

    pl = ngx_alloc_chain_link(pool);
    if (pl == NULL) {
        return NULL;
    }

    srv_len = s->host_in.len;
    name_len = ngx_strlen(v->name);
    type_len = ngx_strlen(v->type);
    args_len = ngx_strlen(v->args);

    b = ngx_create_temp_buf(pool,
                            sizeof("&call=publish") +
                            sizeof("&srv=") + srv_len +
                            sizeof("&name=") + name_len * 3 +
                            sizeof("&type=") + type_len * 3 +
                            sizeof("&flags=") + NGX_INT32_LEN + //added by Edward.Wu
                            sizeof("&updatetimeout=") + NGX_INT32_LEN + //added by Edward.Wu
                            1 + args_len);
    if (b == NULL) {
        return NULL;
    }

    pl->buf = b;
    pl->next = NULL;

    b->last = ngx_cpymem(b->last, (u_char*) "&call=publish",
                         sizeof("&call=publish") - 1);

    if (srv_len > 0) {

        b->last = ngx_cpymem(b->last, (u_char*) "&srv=", sizeof("&srv=") - 1);
	    b->last = ngx_cpymem(b->last, s->host_in.data, s->host_in.len);
    }
	
    b->last = ngx_cpymem(b->last, (u_char*) "&name=", sizeof("&name=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->name, name_len,
                                       NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&type=", sizeof("&type=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->type, type_len,
                                       NGX_ESCAPE_ARGS);
    /* added by Edward.Wu */
    b->last = ngx_cpymem(b->last, (u_char *) "&flags=",
                         sizeof("&flags=") - 1);
    b->last = ngx_sprintf(b->last, "%i", ctx->flags);
    b->last = ngx_cpymem(b->last, (u_char *) "&updatetimeout=",
                         sizeof("&updatetimeout=") - 1);
    b->last = ngx_sprintf(b->last, "%i", nacf->update_timeout);
    /* end */

    if (args_len) {
        *b->last++ = '&';
        // b->last = (u_char *) ngx_cpymem(b->last, v->args, args_len);
        u_char *arg_start = v->args;
        do {
            if( !arg_start )
                    break;

            u_char *equals = (u_char*)ngx_strchr(arg_start, '=');
            if( !equals ) {
                    break; // error occurs.
            }

            u_char finish = 0;
            u_char *arg_end = (u_char*)ngx_strchr(arg_start, '&');
            if( !arg_end ) {
                    finish = 1;
                    arg_end = v->args + args_len;
            }

            if( equals >= arg_end ) {
                    break; // a '=' character must be front of '&'
            }

            b->last = ngx_cpymem(b->last, arg_start, equals - arg_start + 1); // contains '='
            ++ equals;
            b->last = (u_char*) ngx_escape_uri(b->last, equals, arg_end - equals, NGX_ESCAPE_ARGS);
            *b->last++ = '&';

            if( !finish ) {
                    arg_start = arg_end + 1;
            } else {
                    break;
            }

        } while (1);
    }

    return ngx_rtmp_notify_create_request(s, pool, NGX_RTMP_NOTIFY_PUBLISH, pl);
}


static ngx_chain_t *
ngx_rtmp_notify_play_create(ngx_rtmp_session_t *s, void *arg,
        ngx_pool_t *pool)
{
    ngx_rtmp_play_t                *v = arg;

    ngx_chain_t                    *pl;
    ngx_buf_t                      *b;
    size_t                         name_len, args_len, srv_len;
    ngx_rtmp_core_main_conf_t      *cmcf;
    ngx_rtmp_server_name_t         *names;
    ngx_rtmp_core_srv_conf_t       **cscf;
    size_t                         n, j;
	ngx_uint_t                     found = 0;


	cmcf = ngx_rtmp_core_main_conf;
    if (cmcf == NULL) {
		
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0, "ngx_rtmp_notify_play_create: cmcf is null");
        return NULL;
    }

	cscf = cmcf->servers.elts;
    for (n = 0; n < cmcf->servers.nelts; ++n, ++cscf) {

	    for (j = 0; j < (*cscf)->server_names.nelts; j++) {
			
             names = (*cscf)->server_names.elts;
			 if (0 == ngx_strncasecmp(s->host_in.data, names->up_srv_name.data, s->host_in.len) ||
			     0 == ngx_strncasecmp(s->host_in.data, names->name.data, s->host_in.len)) {

                ngx_log_error(NGX_LOG_ERR, s->connection->log, 0, "s->host_in is: %V, up_srv_name is: %V, name is: %V", 
					&s->host_in, &names->up_srv_name, &names->name);
                found =1;
				break;
			 }
		}
		if (found == 1) {

			break;
		}
	}

	if (found == 1) {

    	srv_len = names->up_srv_name.len;
	} else {

        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,"ngx_rtmp_notify_play_create: not found cscf");
		return NULL;
	}

    pl = ngx_alloc_chain_link(pool);
    if (pl == NULL) {
        return NULL;
    }

    name_len = ngx_strlen(v?v->name:s->name);
    if (v)
        args_len = ngx_strlen(v->args);
    else if (s && s->args.len>0)
        args_len = s->args.len;
    else
    	args_len = 0;

    
    b = ngx_create_temp_buf(pool,
                            sizeof("&call=play") +
                            sizeof("&srv=") + srv_len +
                            sizeof("&name=") + name_len * 3 +
                            sizeof("&start=&duration=&reset=") +
                            NGX_INT32_LEN * 3 + 1 + args_len);
    if (b == NULL) {
        return NULL;
    }

    pl->buf = b;
    pl->next = NULL;

    b->last = ngx_cpymem(b->last, (u_char*) "&call=play",
                         sizeof("&call=play") - 1);


    if (srv_len > 0) {
		
        b->last = ngx_cpymem(b->last, (u_char*) "&srv=", sizeof("&srv=") - 1);
	    b->last = ngx_cpymem(b->last, names->up_srv_name.data, names->up_srv_name.len);
    }

    b->last = ngx_cpymem(b->last, (u_char*) "&name=", sizeof("&name=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v?v->name:s->name, name_len,
                                       NGX_ESCAPE_ARGS);

    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "&start=%uD&duration=%uD&reset=%d",
                           (uint32_t) (v?v->start:s->start), (uint32_t) (v?v->duration:s->duration),
                           (v?v->reset:s->reset) & 1);

	// Uri-Encode all extra parameters.
    if (args_len) {
        *b->last++ = '&';
        u_char *arg_start = v?v->args:s->args.data;
        do {
            if( !arg_start )
                    break;

            u_char *equals = (u_char*)ngx_strchr(arg_start, '=');
            if( !equals ) {
                    break; // error occurs.
            }

            u_char finish = 0;
            u_char *arg_end = (u_char*)ngx_strchr(arg_start, '&');
            if( !arg_end ) {
                    finish = 1;
                    arg_end = v->args + args_len;
            }

            if( equals >= arg_end ) {
                    break; // a '=' character must be front of '&'
            }

            b->last = ngx_cpymem(b->last, arg_start, equals - arg_start + 1); // contains '='
            ++ equals;
            b->last = (u_char*) ngx_escape_uri(b->last, equals, arg_end - equals, NGX_ESCAPE_ARGS);
            *b->last++ = '&';

            if( !finish ) {
                    arg_start = arg_end + 1;
            } else {
                    break;
            }

        } while (1);
    }

    return ngx_rtmp_notify_create_request(s, pool, NGX_RTMP_NOTIFY_PLAY, pl);
}


static ngx_chain_t *
ngx_rtmp_notify_done_create(ngx_rtmp_session_t *s, void *arg,
        ngx_pool_t *pool)
{
    ngx_rtmp_notify_done_t         *ds = arg;

    ngx_chain_t                    *pl;
    ngx_buf_t                      *b;
    size_t                          cbname_len, name_len, args_len, srv_len;
    ngx_rtmp_notify_ctx_t          *ctx;

    ngx_rtmp_core_main_conf_t      *cmcf;
    ngx_rtmp_server_name_t         *names;
    ngx_rtmp_core_srv_conf_t       **cscf;
    size_t                         n, j;
    ngx_uint_t                     found = 0;

   
   if (ngx_memcmp((char *)ds->cbname, "play_done", ngx_strlen("play_done")) == 0) {

	    cmcf = ngx_rtmp_core_main_conf;
	    if (cmcf == NULL) {
			
	        return NULL;
	    }

		cscf = cmcf->servers.elts;
	    for (n = 0; n < cmcf->servers.nelts; ++n, ++cscf) {

		    for (j = 0; j < (*cscf)->server_names.nelts; j++) {
				
	             names = (*cscf)->server_names.elts;
				 if (0 == ngx_strncasecmp(s->host_in.data, names->name.data, s->host_in.len)) {

	                found = 1;
					break;
				 }
			}

			if (found == 1) {

				break;
			}
		}

		if (found == 1) {

	    	srv_len = names->up_srv_name.len;
		} else {

			return NULL;
		}
   	}else if (ngx_memcmp((char *)ds->cbname, "publish_done", ngx_strlen("publish_done")) == 0) {

		srv_len = s->host_in.len;
	} else {

		//do nothing
	}

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

    pl = ngx_alloc_chain_link(pool);
    if (pl == NULL) {
        return NULL;
    }

    cbname_len = ngx_strlen(ds->cbname);
    name_len = ctx ? ngx_strlen(ctx->name) : 0;
    args_len = ctx ? ngx_strlen(ctx->args) : 0;

    b = ngx_create_temp_buf(pool,
                            sizeof("&call=") + cbname_len +
                            sizeof("&srv=") + srv_len +
                            sizeof("&name=") + name_len * 3 +
                            1 + args_len);
    if (b == NULL) {
        return NULL;
    }

    pl->buf = b;
    pl->next = NULL;

    b->last = ngx_cpymem(b->last, (u_char*) "&call=", sizeof("&call=") - 1);
    b->last = ngx_cpymem(b->last, ds->cbname, cbname_len);

    if (srv_len > 0) {

        if (ngx_memcmp((char *)ds->cbname, "publish_done", ngx_strlen("publish_done")) == 0) {

		    b->last = ngx_cpymem(b->last, (u_char*) "&srv=", sizeof("&srv=") - 1);
	    	b->last = ngx_cpymem(b->last, s->host_in.data, s->host_in.len);
			
		}else if (ngx_memcmp((char *)ds->cbname, "play_done", ngx_strlen("play_done")) == 0){


			b->last = ngx_cpymem(b->last, (u_char*) "&srv=", sizeof("&srv=") - 1);
			b->last = ngx_cpymem(b->last, names->up_srv_name.data, names->up_srv_name.len);

		}else {

            //do nothing
		}
	}

    if (name_len) {
        b->last = ngx_cpymem(b->last, (u_char*) "&name=", sizeof("&name=") - 1);
        b->last = (u_char*) ngx_escape_uri(b->last, ctx->name, name_len,
                                           NGX_ESCAPE_ARGS);
    }

    if (args_len) {
        *b->last++ = '&';
        // b->last = (u_char *) ngx_cpymem(b->last, v->args, args_len);
        u_char *arg_start = ctx->args;
        do {
            if( !arg_start )
                    break;

            u_char *equals = (u_char*)ngx_strchr(arg_start, '=');
            if( !equals ) {
                    break; // error occurs.
            }

            u_char finish = 0;
            u_char *arg_end = (u_char*)ngx_strchr(arg_start, '&');
            if( !arg_end ) {
                    finish = 1;
                    arg_end = ctx->args + args_len;
            }

            if( equals >= arg_end ) {
                    break; // a '=' character must be front of '&'
            }

            b->last = ngx_cpymem(b->last, arg_start, equals - arg_start + 1); // contains '='
            ++ equals;
            b->last = (u_char*) ngx_escape_uri(b->last, equals, arg_end - equals, NGX_ESCAPE_ARGS);
            *b->last++ = '&';

            if( !finish ) {
                    arg_start = arg_end + 1;
            } else {
                    break;
            }

        } while (1);
    }

    return ngx_rtmp_notify_create_request(s, pool, ds->url_idx, pl);
}


static ngx_chain_t *
ngx_rtmp_notify_update_create(ngx_rtmp_session_t *s, void *arg,
        ngx_pool_t *pool)
{
    ngx_chain_t                    *pl;
    ngx_buf_t                      *b;
    size_t                          name_len, args_len, srv_len;
    ngx_rtmp_notify_ctx_t          *ctx;
    ngx_str_t                       sfx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);
    //nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

    pl = ngx_alloc_chain_link(pool);
    if (pl == NULL) {
        return NULL;
    }

    if (ctx->flags & NGX_RTMP_NOTIFY_PUBLISHING) {
        ngx_str_set(&sfx, "_publish");
    } else if (ctx->flags & NGX_RTMP_NOTIFY_PLAYING) {
        ngx_str_set(&sfx, "_play");
    } else {
        ngx_str_null(&sfx);
    }

    name_len = ctx ? ngx_strlen(ctx->name) : 0;
    args_len = ctx ? ngx_strlen(ctx->args) : 0;
    srv_len = s->host_in.len;
    b = ngx_create_temp_buf(pool,
                            sizeof("&call=update") + sfx.len +
                            sizeof("&time=") + NGX_TIME_T_LEN +
                            sizeof("&timestamp=") + NGX_INT32_LEN +
                            sizeof("&srv=") + srv_len +
                            sizeof("&name=") + name_len * 3 +
                            1 + args_len);
    if (b == NULL) {
        return NULL;
    }

    pl->buf = b;
    pl->next = NULL;

    b->last = ngx_cpymem(b->last, (u_char*) "&call=update",
                         sizeof("&call=update") - 1);
    b->last = ngx_cpymem(b->last, sfx.data, sfx.len);

    b->last = ngx_cpymem(b->last, (u_char *) "&time=",
                         sizeof("&time=") - 1);
    b->last = ngx_sprintf(b->last, "%T", ngx_cached_time->sec - ctx->start);

    b->last = ngx_cpymem(b->last, (u_char *) "&timestamp=",
                         sizeof("&timestamp=") - 1);
    b->last = ngx_sprintf(b->last, "%D", s->current_time);
	
	if (srv_len) {
		
		b->last = ngx_cpymem(b->last, (u_char *) "&srv=",
							 sizeof("&srv=") - 1);
		b->last = ngx_cpymem(b->last, (char *)s->host_in.data, s->host_in.len);
	}
	
    if (name_len) {
        b->last = ngx_cpymem(b->last, (u_char*) "&name=", sizeof("&name=") - 1);
        b->last = (u_char*) ngx_escape_uri(b->last, ctx->name, name_len,
                                           NGX_ESCAPE_ARGS);
    }

    if (args_len) {
        *b->last++ = '&';
        // b->last = (u_char *) ngx_cpymem(b->last, v->args, args_len);
        u_char *arg_start = ctx->args;
        do {
            if( !arg_start )
                    break;

            u_char *equals = (u_char*)ngx_strchr(arg_start, '=');
            if( !equals ) {
                    break; // error occurs.
            }

            u_char finish = 0;
            u_char *arg_end = (u_char*)ngx_strchr(arg_start, '&');
            if( !arg_end ) {
                    finish = 1;
                    arg_end = ctx->args + args_len;
            }

            if( equals >= arg_end ) {
                    break; // a '=' character must be front of '&'
            }

            b->last = ngx_cpymem(b->last, arg_start, equals - arg_start + 1); // contains '='
            ++ equals;
            b->last = (u_char*) ngx_escape_uri(b->last, equals, arg_end - equals, NGX_ESCAPE_ARGS);
            *b->last++ = '&';

            if( !finish ) {
                    arg_start = arg_end + 1;
            } else {
                    break;
            }

        } while (1);
    }
	
    return ngx_rtmp_notify_create_request(s, pool, NGX_RTMP_NOTIFY_UPDATE, pl);
}


static ngx_chain_t *
ngx_rtmp_notify_record_done_create(ngx_rtmp_session_t *s, void *arg,
                                   ngx_pool_t *pool)
{
    ngx_rtmp_record_done_t         *v = arg;

    ngx_rtmp_notify_ctx_t          *ctx;
    ngx_chain_t                    *pl;
    ngx_buf_t                      *b;
    size_t                          name_len, args_len;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

    pl = ngx_alloc_chain_link(pool);
    if (pl == NULL) {
        return NULL;
    }

    name_len  = ngx_strlen(ctx->name);
    args_len  = ngx_strlen(ctx->args);

    b = ngx_create_temp_buf(pool,
                            sizeof("&call=record_done") +
                            sizeof("&recorder=") + v->recorder.len +
                            sizeof("&name=") + name_len * 3 +
                            sizeof("&path=") + v->path.len * 3 +
                            1 + args_len);
    if (b == NULL) {
        return NULL;
    }

    pl->buf = b;
    pl->next = NULL;

    b->last = ngx_cpymem(b->last, (u_char*) "&call=record_done",
                         sizeof("&call=record_done") - 1);

    b->last = ngx_cpymem(b->last, (u_char *) "&recorder=",
                         sizeof("&recorder=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->recorder.data,
                                       v->recorder.len, NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&name=", sizeof("&name=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, ctx->name, name_len,
                                       NGX_ESCAPE_ARGS);

    b->last = ngx_cpymem(b->last, (u_char*) "&path=", sizeof("&path=") - 1);
    b->last = (u_char*) ngx_escape_uri(b->last, v->path.data, v->path.len,
                                       NGX_ESCAPE_ARGS);

    if (args_len) {
        *b->last++ = '&';
        b->last = (u_char *) ngx_cpymem(b->last, ctx->args, args_len);
    }

    return ngx_rtmp_notify_create_request(s, pool, NGX_RTMP_NOTIFY_RECORD_DONE,
                                          pl);
}


static ngx_int_t
ngx_rtmp_notify_parse_http_retcode(ngx_rtmp_session_t *s,
        ngx_chain_t *in)
{
    ngx_buf_t      *b;
    ngx_int_t       n;
    u_char          c;

    /* find 10th character */

    n = 9;
    while (in) {
        b = in->buf;
        if (b->last - b->pos > n) {
            c = b->pos[n];
            if (c >= (u_char)'0' && c <= (u_char)'9') {
                switch (c) {
                    case (u_char) '2':
                        return NGX_OK;
                    case (u_char) '3':
                        return NGX_AGAIN;
                    default:
                        return NGX_ERROR;
                }
            }

            ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                    "notify: invalid HTTP retcode: %d..", (int)c);

            return NGX_ERROR;
        }
        n -= (b->last - b->pos);
        in = in->next;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
            "notify: empty or broken HTTP response");

    /*
     * not enough data;
     * it can happen in case of empty or broken reply
     */

    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_notify_parse_http_header(ngx_rtmp_session_t *s,
        ngx_chain_t *in, ngx_str_t *name, u_char *data, size_t len)
{
    ngx_buf_t      *b;
    ngx_int_t       matched;
    u_char         *p, c;
    ngx_uint_t      n;

    enum {
        parse_name,
        parse_space,
        parse_value,
        parse_value_newline
    } state = parse_name;

    n = 0;
    matched = 0;

    while (in) {
        b = in->buf;

        for (p = b->pos; p != b->last; ++p) {
            c = *p;

            if (c == '\r') {
                continue;
            }

            switch (state) {
                case parse_value_newline:
                    if (c == ' ' || c == '\t') {
                        state = parse_space;
                        break;
                    }

                    if (matched) {
                        return n;
                    }

                    if (c == '\n') {
                        return NGX_OK;
                    }

                    n = 0;
                    state = parse_name;

                case parse_name:
                    switch (c) {
                        case ':':
                            matched = (n == name->len);
                            n = 0;
                            state = parse_space;
                            break;
                        case '\n':
                            n = 0;
                            break;
                        default:
                            if (n < name->len &&
                                ngx_tolower(c) == ngx_tolower(name->data[n]))
                            {
                                ++n;
                                break;
                            }
                            n = name->len + 1;
                    }
                    break;

                case parse_space:
                    if (c == ' ' || c == '\t') {
                        break;
                    }
                    state = parse_value;

                case parse_value:
                    if (c == '\n') {
                        state = parse_value_newline;
                        break;
                    }

                    if (matched && n + 1 < len) {
                        data[n++] = c;
                    }

                    break;
            }
        }

        in = in->next;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_rtmp_notify_parse_http_body(ngx_rtmp_session_t *s,
        ngx_chain_t *in, ngx_str_t *body)
{
    ngx_buf_t      *b;
    ngx_int_t       matched;
    u_char         *p, c, pre;
    ngx_uint_t      n;
    


    n = 0;
    matched = 0;
    
    pre = '\0';

    while (in) {
        b = in->buf;

        for (p = b->pos; p != b->last; ++p) {
            if (4 == matched )
                break;
            c = *p;

            if ( c == '\r' || c == '\n' ) {
                if (pre != c){
                    matched ++;
                    pre = c; 
                    continue;
                }
            }
            matched = 0;
        }
        if (4 == matched )
            break;
        in = in->next;
    }

    if (4 == matched )
    {
        /* FIXME: not confider mutliple in buffers */
        //p = (u_char *)ngx_strchr(p, '\n');
        body->data = p;
        body->len = in->buf->last - p;
    }
    

    return NGX_OK;
}


static void
ngx_rtmp_notify_clear_flag(ngx_rtmp_session_t *s, ngx_uint_t flag)
{
    ngx_rtmp_notify_ctx_t  *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

    ctx->flags &= ~flag;
}


static ngx_int_t
ngx_rtmp_notify_connect_handle(ngx_rtmp_session_t *s,
        void *arg, ngx_chain_t *in)
{
    ngx_rtmp_connect_t *v = arg;
    ngx_int_t           rc;
    u_char              app[NGX_RTMP_MAX_NAME];

    static ngx_str_t    location = ngx_string("location");

    rc = ngx_rtmp_notify_parse_http_retcode(s, in);
    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (rc == NGX_AGAIN) {
        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "notify: connect redirect received");

        rc = ngx_rtmp_notify_parse_http_header(s, in, &location, app,
                                               sizeof(app) - 1);
        if (rc > 0) {
            *ngx_cpymem(v->app, app, rc) = 0;
            ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                          "notify: connect redirect to '%s'", v->app);
        }
    }

    return next_connect(s, v);
}


static void
ngx_rtmp_notify_set_name(u_char *dst, size_t dst_len, u_char *src,
    size_t src_len)
{
    u_char     result[16], *p;
    ngx_md5_t  md5;

    ngx_md5_init(&md5);
    ngx_md5_update(&md5, src, src_len);
    ngx_md5_final(result, &md5);

    p = ngx_hex_dump(dst, result, ngx_min((dst_len - 1) / 2, 16));
    *p = '\0';
}


static ngx_int_t
ngx_rtmp_notify_publish_handle(ngx_rtmp_session_t *s,
        void *arg, ngx_chain_t *in)
{
    ngx_rtmp_publish_t         *v = arg;
    ngx_int_t                   rc;
    ngx_str_t                   local_name;
    ngx_rtmp_relay_target_t     target;
    ngx_url_t                  *u;
    ngx_rtmp_notify_app_conf_t *nacf;
    u_char                      name[NGX_RTMP_MAX_NAME];
    ngx_str_t                   http_ret;

    static ngx_str_t    location = ngx_string("location");

	if ( !in ) {
		ngx_log_error(NGX_LOG_WARN, s->connection->log, 0,
			"notify: publish received none!");
		return NGX_ERROR;
	}
	
    http_ret.data = in->buf->start;
    http_ret.len = in->buf->last - in->buf->start;
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                   "notify: publish received: %V", &http_ret);

    rc = ngx_rtmp_notify_parse_http_retcode(s, in);
    if (rc == NGX_ERROR) {
        ngx_rtmp_notify_clear_flag(s, NGX_RTMP_NOTIFY_PUBLISHING);
        return NGX_ERROR;
    }

    if (rc != NGX_AGAIN) {
        goto next;
    }

    /* HTTP 3xx */


    rc = ngx_rtmp_notify_parse_http_header(s, in, &location, name,
                                           sizeof(name) - 1);
    if (rc <= 0) {
        goto next;
    }

    if (ngx_strncasecmp(name, (u_char *) "rtmp://", 7)) {
        *ngx_cpymem(v->name, name, rc) = 0;
        ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                      "notify: publish redirect to '%s'", v->name);
        goto next;
    }

    /* push */

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
    if (nacf->relay_redirect) {
        ngx_rtmp_notify_set_name(v->name, NGX_RTMP_MAX_NAME, name, (size_t) rc);
    }

    ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                  "notify: push '%s' to '%*s'", v->name, rc, name);

    local_name.data = v->name;
    local_name.len = ngx_strlen(v->name);

    ngx_memzero(&target, sizeof(target));

    u = &target.url;
    u->url = local_name;
    u->url.data = name + 7;
    u->url.len = rc - 7;
    u->default_port = 1935;
    u->uri_part = 1;
    u->no_resolve = 1; /* want ip here */

    if (ngx_parse_url(s->connection->pool, u) != NGX_OK) {
        ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                      "notify: push failed '%V'", &local_name);
        return NGX_ERROR;
    }

    ngx_rtmp_relay_push(s, &local_name, &target);

next:

    return next_publish(s, v);
}


static ngx_int_t
ngx_rtmp_notify_json_decode(ngx_rtmp_session_t *s,
		const char *jsonstr, ngx_addr_t *local, ngx_str_t *url, const ngx_str_t *app, const ngx_str_t *name)
{
	ngx_rtmp_notify_main_conf_t *nmcf;
	struct json_object          *obj;
	struct json_object          *root_obj;
	struct sockaddr_in          *local_addr_in;
	ngx_int_t                    ret = NGX_OK;

	nmcf = ngx_rtmp_get_module_main_conf(s, ngx_rtmp_notify_module);

	root_obj = json_tokener_parse(jsonstr);

	int32_t port;
	if (json_object_object_get_ex(root_obj, "port", &obj)) {
		port = json_object_get_int(obj);
		obj = NULL;
	} else {
		json_object_put(root_obj);
		ret = NGX_ERROR;
		goto finally;
	}

	const char *ip_list[NGX_RTMP_NOTIFY_SP_MAX] = {NULL, NULL};
	int         ip_len_list[NGX_RTMP_NOTIFY_SP_MAX] = {0, 0};
	if (json_object_object_get_ex(root_obj, "telecom_ip", &obj)) {
		ip_list[NGX_RTMP_NOTIFY_SP_TELECOM] = json_object_get_string(obj);
		ip_len_list[NGX_RTMP_NOTIFY_SP_TELECOM] = json_object_get_string_len(obj);
		obj = NULL;
	}

	if (json_object_object_get_ex(root_obj, "unicom_ip", &obj)) {
		ip_list[NGX_RTMP_NOTIFY_SP_UNICOM] = json_object_get_string(obj);
		ip_len_list[NGX_RTMP_NOTIFY_SP_UNICOM] = json_object_get_string_len(obj);
		obj = NULL;
	}

	ngx_int_t xcom_sp_idx = ngx_random() % 2;
  	if (ip_list[xcom_sp_idx] && nmcf->iplist[xcom_sp_idx].len > 0) {
		
	} else {
		xcom_sp_idx = (xcom_sp_idx + 1) % 2;
		if (ip_list[xcom_sp_idx] && nmcf->iplist[xcom_sp_idx].len > 0) {
			
		} else {
			json_object_put(root_obj);
			ret = NGX_ERROR;
			goto finally;
		}
	}

	// set local
	ngx_memzero(local->sockaddr, sizeof(*local->sockaddr));
	local_addr_in = (struct sockaddr_in *)local->sockaddr;
	local_addr_in->sin_family      = AF_INET;
	local_addr_in->sin_addr.s_addr = inet_addr(nmcf->iplist[xcom_sp_idx].data);

	ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
            "slot=%i, json_decode: name='%V' app='%V' local_ip='%s' local_type='%d' remote_ip='%s' remote_type='%d'",
            ngx_process_slot, name, app, nmcf->iplist[xcom_sp_idx].data, xcom_sp_idx, ip_list[xcom_sp_idx], xcom_sp_idx);

	// set remote
	url->len = ngx_snprintf(url->data, url->len, "rtmp://%s:%d/%V/%V",
		ip_list[xcom_sp_idx], port, app, name) - url->data;

finally:
	json_object_put(root_obj);
	return ret;
}


static ngx_int_t
ngx_rtmp_notify_play_handle(ngx_rtmp_session_t *s,
        void *arg, ngx_chain_t *in)
{
	ngx_rtmp_notify_app_conf_t *nacf;
    ngx_rtmp_play_t            *v = arg;
	ngx_int_t                   rc;
	ngx_str_t                   tmp_name;
	ngx_rtmp_relay_target_t     target;
	ngx_url_t                  *u;
	struct sockaddr             sockaddr;
	ngx_addr_t                  local_addr;
	u_char                      cluster_url[NGX_RTMP_MAX_NAME];
	ngx_int_t                   cluster_url_len = NGX_RTMP_MAX_NAME;
	u_char                      str_result[NGX_RTMP_MAX_NAME];
	u_char                      str_action[NGX_RTMP_MAX_NAME];
	u_char                      str_tcurl[NGX_RTMP_MAX_NAME];
	ngx_str_t                   http_ret;
	static ngx_str_t            result = ngx_string("result");
	static ngx_str_t            action = ngx_string("action");
	static ngx_str_t            tcurl  = ngx_string("tcurl");
	ngx_chain_t                 evt_in;
	ngx_buf_t                   evt_buf;

	if ( !in ) {
		ngx_log_error(NGX_LOG_WARN, s->connection->log, 0,
			"notify: play received none!");
		return NGX_ERROR;
	}

    http_ret.data = in->buf->start;
    http_ret.len = in->buf->last - in->buf->start;
    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                   "notify: play received: \n%V", &http_ret);

	nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

	rc = ngx_rtmp_notify_parse_http_retcode(s, in);
	ngx_log_error(NGX_LOG_INFO, s->connection->log, 0, "Notify response status %d", rc);
	switch (rc) {
		case NGX_OK:    // 200 继续走下面的modules
			goto next;
		case NGX_AGAIN: // 302 调用relay
			break;
		case NGX_ERROR: // 403 关闭连接
		default:        // fatal error
			goto error;
	}

	ngx_memzero(&str_action, sizeof(str_action));
	rc = ngx_rtmp_notify_parse_http_header(s, in, &action, str_action,
		sizeof(str_action) - 1);
	if (rc <= 0)
		goto error;

	ngx_memzero(&str_tcurl, sizeof(str_tcurl));
	rc = ngx_rtmp_notify_parse_http_header(s, in, &tcurl, str_tcurl,
		sizeof(str_tcurl) - 1);
	if (rc <= 0)
		goto error;

	ngx_memzero(&str_result, sizeof(str_result));
	rc = ngx_rtmp_notify_parse_http_header(s, in, &result, str_result,
		sizeof(str_result) - 1);
	if (rc <= 0)
		goto error;

	if (ngx_strcasecmp(str_action, (u_char *) "local") == 0) {    // Local relay
		s->relay_type = NGX_LOCAL_RELAY;
	} else if (ngx_strcasecmp(str_action, (u_char *) "remote") == 0) {    // Remote relay
		s->relay_type = NGX_REMOTE_RELAY;
	} else if (ngx_strcasecmp(str_action, (u_char *) "cluster") == 0) {
		s->relay_type = NGX_CLUSTER_RELAY;
	} else {
		goto error;
	}

	ngx_memzero(&target, sizeof(target));
	target.tc_url.data = str_tcurl;
	target.tc_url.len  = ngx_strlen(str_tcurl);
	target.relay_type  = s->relay_type;
	target.app         = s->app;
	tmp_name.data      = v?v->name:s->name;
	tmp_name.len       = ngx_strlen(v?v->name:s->name);
	if (s->relay_type == NGX_LOCAL_RELAY) {

		ngx_int_t n = ngx_atoi(str_result, ngx_strlen(str_result));
		if (n < 0 || n >= NGX_MAX_PROCESSES || n == ngx_process_slot) {
			goto error;
		}
		ngx_str_set(&target.page_url, "nginx-local-pull");
		target.tag  = &ngx_rtmp_notify_module;
		target.data = &ngx_processes[n];
		ngx_memzero(&target.url, sizeof(target.url));
		u = &target.url;

#define NGX_RTMP_NOTIFY_SOCKNAME "nginx-rtmp"
		ngx_file_info_t fi;
		u_char path[sizeof("unix:") + NGX_MAX_PATH];
		u_char *p = ngx_snprintf(path, sizeof(path) - 1,
			"unix:%V/" NGX_RTMP_NOTIFY_SOCKNAME ".%i",
			&nacf->socket_dir, n);
#undef NGX_RTMP_NOTIFY_SOCKNAME

		*p = 0;
		if (ngx_file_info(path + sizeof("unix:") - 1, &fi) != NGX_OK) { // 只比较"/tmp/nginx-rtmp.4"
			goto next;
		}
		u->url.data = path; // "unix:/tmp/nginx-rtmp.4"
		u->url.len = p - path;

	} else if (s->relay_type == NGX_REMOTE_RELAY) {

		ngx_str_set(&target.page_url, "nginx-remote-pull");
		ngx_memzero(&target.url, sizeof(target.url));
		u = &target.url;
		u->url.data = str_result + 7;
		u->url.len = rc - 7;
		u->default_port = 1935;
		u->uri_part = 1;
		u->no_resolve = 1; /* want ip here */

	} else if (s->relay_type == NGX_CLUSTER_RELAY) {

		ngx_str_set(&target.page_url, "nginx-cluster-pull");
		ngx_memzero(&target.url, sizeof(target.url));
		u = &target.url;
		u->url.data = cluster_url;
		u->url.len = cluster_url_len;
		u->default_port = 1935;
		u->uri_part = 1;
		u->no_resolve = 1; /* want ip here */

		ngx_memzero(&sockaddr, sizeof(sockaddr));
		local_addr.sockaddr     = &sockaddr;
		local_addr.socklen      = sizeof(sockaddr);
		target.local            = &local_addr;
		target.local->name.data = "test";
		target.local->name.len  = ngx_strlen("test");

		if (ngx_rtmp_notify_json_decode(s, str_result, target.local, &u->url, &s->app, &tmp_name) != NGX_OK) {
			goto next;
		}

		u->url.data += 7;
		u->url.len  -= 7;
	} else {
		goto error;
	}

	if (ngx_parse_url(s->connection->pool, &target.url) != NGX_OK) {
		goto next;
	}

	ngx_rtmp_relay_pull(s, &tmp_name, &target);

next:
	rc = NGX_OK;
	evt_in.buf = &evt_buf;
	evt_buf.start = &rc;
	evt_buf.end   = evt_buf.start + sizeof(rc);

	if (ngx_rtmp_fire_event(s, NGX_RTMP_AUTH_DONE,
                NULL, &evt_in) != NGX_OK)
    {
        ngx_rtmp_finalize_session(s);
    }

	return next_play(s, v);

error:
	rc = NGX_ERROR;
	evt_in.buf = &evt_buf;
	evt_buf.start = &rc;
	evt_buf.end   = evt_buf.start + sizeof(rc);

	if (ngx_rtmp_fire_event(s, NGX_RTMP_AUTH_DONE,
                NULL, &evt_in) != NGX_OK)
    {
        ngx_rtmp_finalize_session(s);
    }

	ngx_rtmp_notify_clear_flag(s, NGX_RTMP_NOTIFY_PLAYING);
	return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_notify_update_handle(ngx_rtmp_session_t *s,
        void *arg, ngx_chain_t *in)
{
    ngx_rtmp_notify_app_conf_t *nacf;
    ngx_rtmp_notify_ctx_t      *ctx;
    ngx_int_t                   rc;

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

	if ( !in ) {
		ngx_log_error(NGX_LOG_WARN, s->connection->log, 0,
			"notify: update received none!");
		return NGX_OK;
	}
	
    rc = ngx_rtmp_notify_parse_http_retcode(s, in);

    if (!nacf->update_strict && rc == NGX_ERROR)
    {
        if (s->update_fail_cnt >= nacf->update_fail_ignore)
        {
            ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                          "notify: update failed");

            return NGX_ERROR;
        }
        else
        {
            s->update_fail_cnt++;
            ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                          "notify: update_fail_cnt %d", s->update_fail_cnt);
        }
    }
    else if (nacf->update_strict && rc != NGX_OK)
    {
        ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                      "notify: update failed");

        return NGX_ERROR;
    }

    s->update_fail_cnt = 0;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "notify: schedule update %Mms",
                   nacf->update_timeout);

    ngx_add_timer(&ctx->update_evt, nacf->update_timeout);

    return NGX_OK;
}


static void
ngx_rtmp_notify_update(ngx_event_t *e)
{
    ngx_connection_t           *c;
    ngx_rtmp_session_t         *s;
    ngx_rtmp_notify_app_conf_t *nacf;
    ngx_rtmp_netcall_init_t     ci;
    ngx_url_t                  *url;
    //ngx_rtmp_notify_ctx_t      *ctx;

    c = e->data;
    s = c->hls ? c->hls_data : c->data;

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
    url = nacf->url[NGX_RTMP_NOTIFY_UPDATE];

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                  "notify: update '%V'", &url->url);

    ngx_memzero(&ci, sizeof(ci));

    ci.url = url;
    ci.create = ngx_rtmp_notify_update_create;
    ci.handle = ngx_rtmp_notify_update_handle;

    if (ngx_rtmp_netcall_create(s, &ci) == NGX_OK) {
        return;
    }

    /* schedule next update on connection error */

    ngx_rtmp_notify_update_handle(s, NULL, NULL);
}


static void
ngx_rtmp_notify_init(ngx_rtmp_session_t *s,
        u_char name[NGX_RTMP_MAX_NAME], u_char args[NGX_RTMP_MAX_ARGS],
        ngx_uint_t flags)
{
    ngx_rtmp_notify_ctx_t          *ctx;
    ngx_rtmp_notify_app_conf_t     *nacf;
    ngx_event_t                    *e;

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
    if (!nacf->active) {
        return;
    }


    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(s->connection->pool, sizeof(ngx_rtmp_notify_ctx_t));
        if (ctx == NULL) {
            return;
        }

        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_notify_module);
    }

    ngx_memcpy(ctx->name, name, NGX_RTMP_MAX_NAME);
    ngx_memcpy(ctx->args, args, NGX_RTMP_MAX_ARGS);

    ctx->flags |= flags;

    if (nacf->url[NGX_RTMP_NOTIFY_UPDATE] == NULL ||
        nacf->update_timeout == 0)
    {
        return;
    }

    //if update on playing , will core dump .
    if (flags == NGX_RTMP_NOTIFY_PLAYING)
        return;

    if (ctx->update_evt.timer_set) {
        return;
    }

    ctx->start = ngx_cached_time->sec;

    e = &ctx->update_evt;

    e->data = s->connection;
    e->log = s->connection->log;
    e->handler = ngx_rtmp_notify_update;

    ngx_add_timer(e, nacf->update_timeout);

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "notify: schedule initial update %Mms",
                   nacf->update_timeout);
}


static ngx_int_t
ngx_rtmp_notify_connect(ngx_rtmp_session_t *s, ngx_rtmp_connect_t *v)
{
    ngx_rtmp_notify_srv_conf_t     *nscf;
    ngx_rtmp_netcall_init_t         ci;
    ngx_url_t                      *url;

    if (s->auto_pushed || s->relay) {
        goto next;
    }

    nscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_notify_module);

    url = nscf->url[NGX_RTMP_NOTIFY_CONNECT];
    if (url == NULL) {
        goto next;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: connect '%V'", &url->url);

    ngx_memzero(&ci, sizeof(ci));

    ci.url = url;
    ci.create = ngx_rtmp_notify_connect_create;
    ci.handle = ngx_rtmp_notify_connect_handle;
    ci.arg = v;
    ci.argsize = sizeof(*v);

    return ngx_rtmp_netcall_create(s, &ci);

next:
    return next_connect(s, v);
}


static ngx_int_t
ngx_rtmp_notify_disconnect(ngx_rtmp_session_t *s)
{
    ngx_rtmp_notify_srv_conf_t     *nscf;
    ngx_rtmp_netcall_init_t         ci;
    ngx_url_t                      *url;

    if (s->auto_pushed || s->relay) {
        goto next;
    }

    nscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_notify_module);

    url = nscf->url[NGX_RTMP_NOTIFY_DISCONNECT];
    if (url == NULL) {
        goto next;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: disconnect '%V'", &url->url);

    ngx_memzero(&ci, sizeof(ci));

    ci.url = url;
    ci.create = ngx_rtmp_notify_disconnect_create;

    ngx_rtmp_netcall_create(s, &ci);

next:
    return next_disconnect(s);
}


static ngx_int_t
ngx_rtmp_notify_publish(ngx_rtmp_session_t *s, ngx_rtmp_publish_t *v)
{
    ngx_rtmp_notify_app_conf_t     *nacf;
    ngx_rtmp_netcall_init_t         ci;
    ngx_url_t                      *url;

    if (s->auto_pushed ) {
        goto next;
    }

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
    if (nacf == NULL) {
        goto next;
    }

    url = nacf->url[NGX_RTMP_NOTIFY_PUBLISH];

    ngx_rtmp_notify_init(s, v->name, v->args, NGX_RTMP_NOTIFY_PUBLISHING);

    if (url == NULL) {
        goto next;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: publish '%V', relay_type is %i", &url->url, s->relay_type);

    ngx_memzero(&ci, sizeof(ci));
    ci.url = url;
    ci.create = ngx_rtmp_notify_publish_create;
    ci.handle = ngx_rtmp_notify_publish_handle;
    ci.arg = v;
    ci.argsize = sizeof(*v);

    return ngx_rtmp_netcall_create(s, &ci);

next:
    return next_publish(s, v);
}


static ngx_int_t
ngx_rtmp_notify_play(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v)
{
    ngx_rtmp_notify_app_conf_t     *nacf;
    ngx_rtmp_netcall_init_t         ci;
    ngx_url_t                      *url;

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
    if (nacf == NULL) {
        goto next;
    }

    url = nacf->url[NGX_RTMP_NOTIFY_PLAY];

    if (NULL == v && ngx_strlen(s->name) == 0)
        return NGX_ERROR;


    if (v){
        ngx_rtmp_notify_init(s, v->name, v->args, NGX_RTMP_NOTIFY_PLAYING);
    }


    if (url == NULL) {
        goto next;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: play '%V'", &url->url);

    ngx_memzero(&ci, sizeof(ci));

    ci.url = url;
    ci.create = ngx_rtmp_notify_play_create;
    ci.handle = ngx_rtmp_notify_play_handle;
    ci.arg = v;
    if (v)
        ci.argsize = sizeof(*v);

    return ngx_rtmp_netcall_create(s, &ci);

next:
    if (v)
        return next_play(s, v);
    return NGX_OK;
}


ngx_int_t
ngx_rtmp_notify_play1(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v)
{
    ngx_rtmp_notify_app_conf_t     *nacf;
    ngx_rtmp_netcall_init_t         ci;
    ngx_url_t                      *url;

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
    if (nacf == NULL) {
        goto next;
    }

    url = nacf->url[NGX_RTMP_NOTIFY_PLAY];

    if (NULL == v && ngx_strlen(s->name) == 0)
        return NGX_ERROR;


    if (v){
        ngx_rtmp_notify_init(s, v->name, v->args, NGX_RTMP_NOTIFY_PLAYING);
    }


    if (url == NULL) {
        goto next;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: play '%V'", &url->url);

    ngx_memzero(&ci, sizeof(ci));

    ci.url = url;
    ci.create = ngx_rtmp_notify_play_create;
    ci.handle = ngx_rtmp_notify_play_handle;
    ci.arg = v;
    if (v)
        ci.argsize = sizeof(*v);

    return ngx_rtmp_netcall_create(s, &ci);

next:
    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_notify_close_stream(ngx_rtmp_session_t *s,
                             ngx_rtmp_close_stream_t *v)
{
    ngx_rtmp_notify_ctx_t          *ctx;
    ngx_rtmp_notify_app_conf_t     *nacf;


    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_notify_module);

    if (ctx == NULL) {
        goto next;
    }

    if (s->auto_pushed) {
        goto next;
    }

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

    if (nacf == NULL) {
        goto next;
    }

    if (ctx->update_evt.timer_set) {
        ngx_del_timer(&ctx->update_evt);
    }

    if (ctx->flags & NGX_RTMP_NOTIFY_PUBLISHING) {
        ngx_rtmp_notify_done(s, "publish_done", NGX_RTMP_NOTIFY_PUBLISH_DONE);
    }

    if (ctx->flags & NGX_RTMP_NOTIFY_PLAYING) {
        ngx_rtmp_notify_done(s, "play_done", NGX_RTMP_NOTIFY_PLAY_DONE);
    }

    if (ctx->flags) {
        ngx_rtmp_notify_done(s, "done", NGX_RTMP_NOTIFY_DONE);
    }

    ctx->flags = 0;

next:
    return next_close_stream(s, v);
}


static ngx_int_t
ngx_rtmp_notify_record_done(ngx_rtmp_session_t *s, ngx_rtmp_record_done_t *v)
{
    ngx_rtmp_netcall_init_t         ci;
    ngx_rtmp_notify_app_conf_t     *nacf;

    if (s->auto_pushed) {
        goto next;
    }

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);
    if (nacf == NULL || nacf->url[NGX_RTMP_NOTIFY_RECORD_DONE] == NULL) {
        goto next;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: record_done recorder=%V path='%V' url='%V'",
                  &v->recorder, &v->path,
                  &nacf->url[NGX_RTMP_NOTIFY_RECORD_DONE]->url);

    ngx_memzero(&ci, sizeof(ci));

    ci.url    = nacf->url[NGX_RTMP_NOTIFY_RECORD_DONE];
    ci.create = ngx_rtmp_notify_record_done_create;
    ci.arg    = v;

    ngx_rtmp_netcall_create(s, &ci);

next:
    return next_record_done(s, v);
}


static ngx_int_t
ngx_rtmp_notify_done(ngx_rtmp_session_t *s, char *cbname, ngx_uint_t url_idx)
{
    ngx_rtmp_netcall_init_t         ci;
    ngx_rtmp_notify_done_t          ds;
    ngx_rtmp_notify_app_conf_t     *nacf;
    ngx_url_t                      *url;

    nacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_notify_module);

    url = nacf->url[url_idx];
    if (url == NULL) {
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "notify: %s '%V'", cbname, &url->url);

    ds.cbname = (u_char *) cbname;
    ds.url_idx = url_idx;

    ngx_memzero(&ci, sizeof(ci));

    ci.url = url;
    ci.arg = &ds;
    ci.create = ngx_rtmp_notify_done_create;

    return ngx_rtmp_netcall_create(s, &ci);
}


static ngx_url_t *
ngx_rtmp_notify_parse_url(ngx_conf_t *cf, ngx_str_t *url)
{
    ngx_url_t  *u;
    size_t      add;

    add = 0;

    u = ngx_pcalloc(cf->pool, sizeof(ngx_url_t));
    if (u == NULL) {
        return NULL;
    }

    if (ngx_strncasecmp(url->data, (u_char *) "http://", 7) == 0) {
        add = 7;
    }

    u->url.len = url->len - add;
    u->url.data = url->data + add;
    u->default_port = 80;
    u->uri_part = 1;

    if (ngx_parse_url(cf->pool, u) != NGX_OK) {
        if (u->err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "%s in url \"%V\"", u->err, &u->url);
        }
        return NULL;
    }

    return u;
}


static char *
ngx_rtmp_notify_sp_ip(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    size_t                      n, nargs;
    ngx_str_t                  *s, *value, v;
    ngx_array_t                *iplist;

    iplist = (ngx_array_t *) (p + cmd->offset);

    nargs = cf->args->nelts - 1;
    if (ngx_array_init(iplist, cf->pool, nargs, sizeof(ngx_str_t)) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

	value = cf->args->elts;
    for (n = 1; n < cf->args->nelts; n++) {

        v = value[n];

        s = ngx_array_push(iplist);
        if (s == NULL) {
            return NGX_CONF_ERROR;
        }

        *s = v;
    }

    return NGX_CONF_OK;
}


static char *
ngx_rtmp_notify_on_srv_event(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtmp_notify_srv_conf_t     *nscf = conf;

    ngx_str_t                      *name, *value;
    ngx_url_t                      *u;
    ngx_uint_t                      n;

    value = cf->args->elts;

    u = ngx_rtmp_notify_parse_url(cf, &value[1]);
    if (u == NULL) {
        return NGX_CONF_ERROR;
    }

    name = &value[0];

    n = 0;

    switch (name->len) {
        case sizeof("on_connect") - 1:
            n = NGX_RTMP_NOTIFY_CONNECT;
            break;

        case sizeof("on_disconnect") - 1:
            n = NGX_RTMP_NOTIFY_DISCONNECT;
            break;
    }

    nscf->url[n] = u;

    return NGX_CONF_OK;
}


static char *
ngx_rtmp_notify_on_app_event(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtmp_notify_app_conf_t     *nacf = conf;

    ngx_str_t                      *name, *value;
    ngx_url_t                      *u;
    ngx_uint_t                      n;

    value = cf->args->elts;

    u = ngx_rtmp_notify_parse_url(cf, &value[1]);
    if (u == NULL) {
        return NGX_CONF_ERROR;
    }

    name = &value[0];

    n = 0;

    switch (name->len) {
        case sizeof("on_done") - 1: /* and on_play */
            if (name->data[3] == 'd') {
                n = NGX_RTMP_NOTIFY_DONE;
            } else {
                n = NGX_RTMP_NOTIFY_PLAY;
            }
            break;

        case sizeof("on_update") - 1:
            n = NGX_RTMP_NOTIFY_UPDATE;
            break;

        case sizeof("on_publish") - 1:
            n = NGX_RTMP_NOTIFY_PUBLISH;
            break;

        case sizeof("on_play_done") - 1:
            n = NGX_RTMP_NOTIFY_PLAY_DONE;
            break;

        case sizeof("on_record_done") - 1:
            n = NGX_RTMP_NOTIFY_RECORD_DONE;
            break;

        case sizeof("on_publish_done") - 1:
            n = NGX_RTMP_NOTIFY_PUBLISH_DONE;
            break;
    }

    nacf->url[n] = u;

    return NGX_CONF_OK;
}


static char *
ngx_rtmp_notify_method(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtmp_notify_app_conf_t     *nacf = conf;

    ngx_rtmp_notify_srv_conf_t     *nscf;
    ngx_str_t                      *value;

    value = cf->args->elts;
    value++;

    if (value->len == sizeof("get") - 1 &&
        ngx_strncasecmp(value->data, (u_char *) "get", value->len) == 0)
    {
        nacf->method = NGX_RTMP_NETCALL_HTTP_GET;

    } else if (value->len == sizeof("post") - 1 &&
               ngx_strncasecmp(value->data, (u_char *) "post", value->len) == 0)
    {
        nacf->method = NGX_RTMP_NETCALL_HTTP_POST;

    } else {
        return "got unexpected method";
    }

    nscf = ngx_rtmp_conf_get_module_srv_conf(cf, ngx_rtmp_notify_module);
    nscf->method = nacf->method;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_rtmp_notify_postconfiguration(ngx_conf_t *cf)
{
    next_connect = ngx_rtmp_connect;
    ngx_rtmp_connect = ngx_rtmp_notify_connect;

    next_disconnect = ngx_rtmp_disconnect;
    ngx_rtmp_disconnect = ngx_rtmp_notify_disconnect;

    next_publish = ngx_rtmp_publish;
    ngx_rtmp_publish = ngx_rtmp_notify_publish;

    next_play = ngx_rtmp_play;
    ngx_rtmp_play = ngx_rtmp_notify_play;

    next_close_stream = ngx_rtmp_close_stream;
    ngx_rtmp_close_stream = ngx_rtmp_notify_close_stream;

    next_record_done = ngx_rtmp_record_done;
    ngx_rtmp_record_done = ngx_rtmp_notify_record_done;

    return NGX_OK;
}
