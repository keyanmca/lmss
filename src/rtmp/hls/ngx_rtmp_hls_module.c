
/*
 * Copyright (C) Roman Arutyunyan
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_rtmp.h>
#include <ngx_rtmp_cmd_module.h>
#include <ngx_rtmp_codec_module.h>
#include "ngx_rtmp_hls_module.h"
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_mpegts.h"


static ngx_rtmp_publish_pt              next_publish;
static ngx_rtmp_play_pt                 next_play;
static ngx_rtmp_close_stream_pt         next_close_stream;
static ngx_rtmp_stream_begin_pt         next_stream_begin;
static ngx_rtmp_stream_eof_pt           next_stream_eof;
static ngx_rtmp_start_hls_slice_pt      next_start_hls_slice;


static char * ngx_rtmp_hls_variant(ngx_conf_t *cf, ngx_command_t *cmd,
       void *conf);
static ngx_int_t ngx_rtmp_hls_postconfiguration(ngx_conf_t *cf);
static void * ngx_rtmp_hls_create_app_conf(ngx_conf_t *cf);
static char * ngx_rtmp_hls_merge_app_conf(ngx_conf_t *cf,
       void *parent, void *child);
static ngx_int_t ngx_rtmp_hls_flush_audio(ngx_rtmp_session_t *s);
static ngx_int_t ngx_rtmp_hls_ensure_directory(ngx_rtmp_session_t *s);

static void * ngx_rtmp_http_hls_create_conf(ngx_conf_t *cf);
static char * ngx_rtmp_http_hls_merge_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_rtmp_http_hls_match_app(ngx_http_request_t *r, ngx_int_t t,
	ngx_rtmp_core_srv_conf_t **out_cscf, ngx_rtmp_core_app_conf_t **out_cacf,
	ngx_rtmp_conf_port_t **out_cfport, ngx_rtmp_hls_app_conf_t **out_hacf,
	ngx_str_t *out_host, ngx_str_t *out_stream_name);
static ngx_int_t ngx_rtmp_http_hls_play_local(ngx_http_request_t *r, ngx_str_t *stream_name);
static ngx_int_t ngx_rtmp_http_hls_handler(ngx_http_request_t *r);
static ngx_int_t ngx_rtmp_http_hls_init(ngx_conf_t *cf);
static ngx_int_t ngx_rtmp_hls_open_file(ngx_http_request_t *r, ngx_chain_t *out);
extern void ngx_rtmp_close_connection(ngx_connection_t *c);


#define ngx_rtmp_hls_get_module_app_conf(app_conf, module)  (app_conf ? \
					app_conf[module.ctx_index] : NULL)

ngx_uint_t                  ngx_rtmp_hls_naccepted;


#define NGX_RTMP_HLS_BUFSIZE            (1024*1024)
#define NGX_RTMP_HLS_DIR_ACCESS         0744

typedef struct {
    ngx_flag_t                          hls;
} ngx_rtmp_http_hls_loc_conf_t;

typedef struct {
    ngx_str_t                           path;
    ngx_msec_t                          playlen;
} ngx_rtmp_hls_cleanup_t;



#define NGX_RTMP_HLS_NAMING_SEQUENTIAL  1
#define NGX_RTMP_HLS_NAMING_TIMESTAMP   2
#define NGX_RTMP_HLS_NAMING_SYSTEM      3


#define NGX_RTMP_HLS_SLICING_PLAIN      1
#define NGX_RTMP_HLS_SLICING_ALIGNED    2


#define NGX_RTMP_HLS_TYPE_LIVE          1
#define NGX_RTMP_HLS_TYPE_EVENT         2


static ngx_conf_enum_t                  ngx_rtmp_hls_naming_slots[] = {
    { ngx_string("sequential"),         NGX_RTMP_HLS_NAMING_SEQUENTIAL },
    { ngx_string("timestamp"),          NGX_RTMP_HLS_NAMING_TIMESTAMP  },
    { ngx_string("system"),             NGX_RTMP_HLS_NAMING_SYSTEM     },
    { ngx_null_string,                  0 }
};


static ngx_conf_enum_t                  ngx_rtmp_hls_slicing_slots[] = {
    { ngx_string("plain"),              NGX_RTMP_HLS_SLICING_PLAIN },
    { ngx_string("aligned"),            NGX_RTMP_HLS_SLICING_ALIGNED  },
    { ngx_null_string,                  0 }
};


static ngx_conf_enum_t                  ngx_rtmp_hls_type_slots[] = {
    { ngx_string("live"),               NGX_RTMP_HLS_TYPE_LIVE  },
    { ngx_string("event"),              NGX_RTMP_HLS_TYPE_EVENT },
    { ngx_null_string,                  0 }
};


typedef struct {
    size_t                buffer_size;
    size_t                max_buffer_size;
} ngx_http_mp4_conf_t;


static ngx_command_t  ngx_rtmp_http_hls_commands[] = {

    { ngx_string("hls"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_rtmp_http_hls_loc_conf_t, hls),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_rtmp_http_hls_module_ctx = {
    NULL,                          /* preconfiguration */
    ngx_rtmp_http_hls_init,        /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    ngx_rtmp_http_hls_create_conf, /* create location configuration */
    ngx_rtmp_http_hls_merge_conf   /* merge location configuration */
};


ngx_module_t  ngx_rtmp_http_hls_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_http_hls_module_ctx, /* module context */
    ngx_rtmp_http_hls_commands,    /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_command_t ngx_rtmp_hls_commands[] = {

    { ngx_string("hls"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, hls),
      NULL },

    { ngx_string("hls_fragment"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, fraglen),
      NULL },

    { ngx_string("hls_max_fragment"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, max_fraglen),
      NULL },

    { ngx_string("hls_path"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, path),
      NULL },

    { ngx_string("hls_playlist_length"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, playlen),
      NULL },

    { ngx_string("hls_muxdelay"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, muxdelay),
      NULL },

    { ngx_string("hls_sync"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, sync),
      NULL },

    { ngx_string("hls_continuous"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, continuous),
      NULL },

    { ngx_string("hls_nested"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, nested),
      NULL },

    { ngx_string("hls_fragment_naming"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, naming),
      &ngx_rtmp_hls_naming_slots },

    { ngx_string("hls_fragment_slicing"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, slicing),
      &ngx_rtmp_hls_slicing_slots },

    { ngx_string("hls_type"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, type),
      &ngx_rtmp_hls_type_slots },

    { ngx_string("hls_max_audio_delay"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, max_audio_delay),
      NULL },

    { ngx_string("hls_audio_buffer_size"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, audio_buffer_size),
      NULL },

    { ngx_string("hls_cleanup"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, cleanup),
      NULL },

    { ngx_string("hls_variant"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_1MORE,
      ngx_rtmp_hls_variant,
      NGX_RTMP_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("hls_base_url"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, base_url),
      NULL },

    { ngx_string("hls_fragment_naming_granularity"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, granularity),
      NULL },

	{ ngx_string("hls_stream_buckets"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, nbuckets),
      NULL },

	{ ngx_string("hls_idle_streams"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_hls_app_conf_t, idle_streams),
      NULL },

    ngx_null_command
};


static ngx_rtmp_module_t  ngx_rtmp_hls_module_ctx = {
    NULL,                               /* preconfiguration */
    ngx_rtmp_hls_postconfiguration,     /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    ngx_rtmp_hls_create_app_conf,       /* create application configuration */
    ngx_rtmp_hls_merge_app_conf,        /* merge application configuration */
};


ngx_module_t  ngx_rtmp_hls_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_hls_module_ctx,           /* module context */
    ngx_rtmp_hls_commands,              /* module directives */
    NGX_RTMP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_rtmp_hls_frag_t *
ngx_rtmp_hls_get_frag(ngx_rtmp_session_t *s, ngx_int_t n)
{
    ngx_rtmp_hls_ctx_t         *ctx;
    ngx_rtmp_hls_app_conf_t    *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    return &ctx->frags[(ctx->frag + n) % (hacf->winfrags * 2 + 1)];
}


static void
ngx_rtmp_hls_next_frag(ngx_rtmp_session_t *s)
{
    ngx_rtmp_hls_ctx_t         *ctx;
    ngx_rtmp_hls_app_conf_t    *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    if (ctx->nfrags == hacf->winfrags) {
        ctx->frag++;
    } else {
        ctx->nfrags++;
    }
}


static ngx_int_t
ngx_rtmp_hls_rename_file(u_char *src, u_char *dst)
{
    /* rename file with overwrite */

#if (NGX_WIN32)
    return MoveFileEx((LPCTSTR) src, (LPCTSTR) dst, MOVEFILE_REPLACE_EXISTING);
#else
    return ngx_rename_file(src, dst);
#endif
}


static ngx_int_t
ngx_rtmp_hls_write_variant_playlist(ngx_rtmp_session_t *s)
{
    static u_char             buffer[1024];

    u_char                   *p, *last;
    ssize_t                   rc;
    ngx_fd_t                  fd;
    ngx_str_t                *arg;
    ngx_uint_t                n, k;
    ngx_rtmp_hls_ctx_t       *ctx;
    ngx_rtmp_hls_variant_t   *var;
    ngx_rtmp_hls_app_conf_t  *hacf;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    fd = ngx_open_file(ctx->var_playlist_bak.data, NGX_FILE_WRONLY,
                       NGX_FILE_TRUNCATE, NGX_FILE_DEFAULT_ACCESS);

    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: " ngx_open_file_n " failed: '%V'",
                      &ctx->var_playlist_bak);

        return NGX_ERROR;
    }

#define NGX_RTMP_HLS_VAR_HEADER "#EXTM3U\n#EXT-X-VERSION:3\n"

    rc = ngx_write_fd(fd, NGX_RTMP_HLS_VAR_HEADER,
                      sizeof(NGX_RTMP_HLS_VAR_HEADER) - 1);
    if (rc < 0) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: " ngx_write_fd_n " failed: '%V'",
                      &ctx->var_playlist_bak);
        ngx_close_file(fd);
        return NGX_ERROR;
    }

    var = hacf->variant->elts;
    for (n = 0; n < hacf->variant->nelts; n++, var++)
    {
        p = buffer;
        last = buffer + sizeof(buffer);

        p = ngx_slprintf(p, last, "#EXT-X-STREAM-INF:PROGRAM-ID=1");

        arg = var->args.elts;
        for (k = 0; k < var->args.nelts; k++, arg++) {
            p = ngx_slprintf(p, last, ",%V", arg);
        }

        if (p < last) {
            *p++ = '\n';
        }

        p = ngx_slprintf(p, last, "%V%*s%V",
                         &hacf->base_url,
                         ctx->name.len - ctx->var->suffix.len, ctx->name.data,
                         &var->suffix);
        if (hacf->nested) {
            p = ngx_slprintf(p, last, "%s", "/index");
        }

        p = ngx_slprintf(p, last, "%s", ".m3u8\n");

        rc = ngx_write_fd(fd, buffer, p - buffer);
        if (rc < 0) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                          "hls: " ngx_write_fd_n " failed '%V'",
                          &ctx->var_playlist_bak);
            ngx_close_file(fd);
            return NGX_ERROR;
        }
    }

    ngx_close_file(fd);

    if (ngx_rtmp_hls_rename_file(ctx->var_playlist_bak.data,
                                 ctx->var_playlist.data)
        == NGX_FILE_ERROR)
    {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: rename failed: '%V'->'%V'",
                      &ctx->var_playlist_bak, &ctx->var_playlist);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_write_playlist(ngx_rtmp_session_t *s)
{
    static u_char                   buffer[1024];
    ngx_fd_t                        fd;
    u_char                         *p;
    ngx_rtmp_hls_ctx_t             *ctx;
    ssize_t                         n;
    ngx_rtmp_hls_app_conf_t        *hacf;
    ngx_rtmp_hls_frag_t            *f;
    ngx_uint_t                      i, max_frag;
    ngx_str_t                       name_part;
    const char                     *sep;
	u_char                         *dot;


    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    fd = ngx_open_file(ctx->playlist_bak.data, NGX_FILE_WRONLY,
                       NGX_FILE_TRUNCATE, NGX_FILE_DEFAULT_ACCESS);

    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: " ngx_open_file_n " failed: '%V'",
                      &ctx->playlist_bak);
        return NGX_ERROR;
    }

    max_frag = hacf->fraglen / 1000;

    for (i = 0; i < ctx->nfrags; i++) {
        f = ngx_rtmp_hls_get_frag(s, i);
        if (f->duration > max_frag) {
            max_frag = (ngx_uint_t) (f->duration + .5);
        }
    }

    p = ngx_snprintf(buffer, sizeof(buffer),
                     "#EXTM3U\n"
                     "#EXT-X-VERSION:3\n"
                     "#EXT-X-MEDIA-SEQUENCE:%uL\n"
                     "#EXT-X-TARGETDURATION:%ui\n"
                     "%s",
                     ctx->frag, max_frag,
                     hacf->type == NGX_RTMP_HLS_TYPE_EVENT ?
                     "#EXT-X-PLAYLIST-TYPE: EVENT\n" : "");

    n = ngx_write_fd(fd, buffer, p - buffer);
    if (n < 0) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: " ngx_write_fd_n " failed: '%V'",
                      &ctx->playlist_bak);
        ngx_close_file(fd);
        return NGX_ERROR;
    }

    sep = hacf->nested ? (hacf->base_url.len ? "/" : "") : "";

    name_part.len = 0;
    if (!hacf->nested || hacf->base_url.len) {
        name_part = ctx->name;
    }

	dot = hacf->nested ? "" : ".";

    for (i = 0; i < ctx->nfrags; i++) {
        f = ngx_rtmp_hls_get_frag(s, i);

        p = ngx_snprintf(buffer, sizeof(buffer),
                         "%s"
                         "#EXTINF:%.3f,\n"
                         "%V%V%s%s%uL.ts\n",
                         f->discont ? "#EXT-X-DISCONTINUITY\n" : "",
                         f->duration, &hacf->base_url, &name_part, dot, sep, f->id);

        ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "hls: fragment frag=%uL, n=%ui/%ui, duration=%.3f, "
                       "discont=%i",
                       ctx->frag, i + 1, ctx->nfrags, f->duration, f->discont);

        n = ngx_write_fd(fd, buffer, p - buffer);
        if (n < 0) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                          "hls: " ngx_write_fd_n " failed '%V'",
                          &ctx->playlist_bak);
            ngx_close_file(fd);
            return NGX_ERROR;
        }
    }

    ngx_close_file(fd);

    if (ngx_rtmp_hls_rename_file(ctx->playlist_bak.data, ctx->playlist.data)
        == NGX_FILE_ERROR)
    {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: rename failed: '%V'->'%V'",
                      &ctx->playlist_bak, &ctx->playlist);
        return NGX_ERROR;
    }

    if (ctx->var) {
        return ngx_rtmp_hls_write_variant_playlist(s);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_copy(ngx_rtmp_session_t *s, void *dst, u_char **src, size_t n,
    ngx_chain_t **in)
{
    u_char  *last;
    size_t   pn;

    if (*in == NULL) {
        return NGX_ERROR;
    }

    for ( ;; ) {
        last = (*in)->buf->last;

        if ((size_t)(last - *src) >= n) {
            if (dst) {
                ngx_memcpy(dst, *src, n);
            }

            *src += n;

            while (*in && *src == (*in)->buf->last) {
                *in = (*in)->next;
                if (*in) {
                    *src = (*in)->buf->pos;
                }
            }

            return NGX_OK;
        }

        pn = last - *src;

        if (dst) {
            ngx_memcpy(dst, *src, pn);
            dst = (u_char *)dst + pn;
        }

        n -= pn;
        *in = (*in)->next;

        if (*in == NULL) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "hls: failed to read %uz byte(s)", n);
            return NGX_ERROR;
        }

        *src = (*in)->buf->pos;
    }
}


static ngx_int_t
ngx_rtmp_hls_append_aud(ngx_rtmp_session_t *s, ngx_buf_t *out)
{
    static u_char   aud_nal[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xf0 };

    if (out->last + sizeof(aud_nal) > out->end) {
        return NGX_ERROR;
    }

    out->last = ngx_cpymem(out->last, aud_nal, sizeof(aud_nal));

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_append_sps_pps(ngx_rtmp_session_t *s, ngx_buf_t *out)
{
    ngx_rtmp_codec_ctx_t           *codec_ctx;
    u_char                         *p;
    ngx_chain_t                    *in;
    ngx_rtmp_hls_ctx_t             *ctx;
    int8_t                          nnals;
    uint16_t                        len, rlen;
    ngx_int_t                       n;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    if (ctx == NULL || codec_ctx == NULL) {
        return NGX_ERROR;
    }

    in = codec_ctx->avc_header;
    if (in == NULL) {
        return NGX_ERROR;
    }

    p = in->buf->pos;

    /*
     * Skip bytes:
     * - flv fmt
     * - H264 CONF/PICT (0x00)
     * - 0
     * - 0
     * - 0
     * - version
     * - profile
     * - compatibility
     * - level
     * - nal bytes
     */

    if (ngx_rtmp_hls_copy(s, NULL, &p, 10, &in) != NGX_OK) {
        return NGX_ERROR;
    }

    /* number of SPS NALs */
    if (ngx_rtmp_hls_copy(s, &nnals, &p, 1, &in) != NGX_OK) {
        return NGX_ERROR;
    }

    nnals &= 0x1f; /* 5lsb */

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: SPS number: %uz", nnals);

    /* SPS */
    for (n = 0; ; ++n) {
        for (; nnals; --nnals) {

            /* NAL length */
            if (ngx_rtmp_hls_copy(s, &rlen, &p, 2, &in) != NGX_OK) {
                return NGX_ERROR;
            }

            ngx_rtmp_rmemcpy(&len, &rlen, 2);

            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "hls: header NAL length: %uz", (size_t) len);

            /* AnnexB prefix */
            if (out->end - out->last < 4) {
                ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                              "hls: too small buffer for header NAL size");
                return NGX_ERROR;
            }

            *out->last++ = 0;
            *out->last++ = 0;
            *out->last++ = 0;
            *out->last++ = 1;

            /* NAL body */
            if (out->end - out->last < len) {
                ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                              "hls: too small buffer for header NAL");
                return NGX_ERROR;
            }

            if (ngx_rtmp_hls_copy(s, out->last, &p, len, &in) != NGX_OK) {
                return NGX_ERROR;
            }

            out->last += len;
        }

        if (n == 1) {
            break;
        }

        /* number of PPS NALs */
        if (ngx_rtmp_hls_copy(s, &nnals, &p, 1, &in) != NGX_OK) {
            return NGX_ERROR;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "hls: PPS number: %uz", nnals);
    }

    return NGX_OK;
}


static uint64_t
ngx_rtmp_hls_get_fragment_id(ngx_rtmp_session_t *s, uint64_t ts)
{
    ngx_rtmp_hls_ctx_t         *ctx;
    ngx_rtmp_hls_app_conf_t    *hacf;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);

    switch (hacf->naming) {

    case NGX_RTMP_HLS_NAMING_TIMESTAMP:
        return ts;

    case NGX_RTMP_HLS_NAMING_SYSTEM:
        return (uint64_t) ngx_cached_time->sec * 1000 + ngx_cached_time->msec;

    default: /* NGX_RTMP_HLS_NAMING_SEQUENTIAL */
        return ctx->frag + ctx->nfrags;
    }
}


static ngx_int_t
ngx_rtmp_hls_send_start_slice(ngx_rtmp_session_t *s)
{
	ngx_rtmp_live_ctx_t            *lctx, *relay_ctx;
	ngx_rtmp_hls_ctx_t             *hctx;
	ngx_rtmp_session_t             *rs;

	lctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_live_module);
    if (lctx == NULL || lctx->stream == NULL) {
        return NGX_OK;
    }

	hctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    if (hctx == NULL) {
		return NGX_OK;
    }

	for (relay_ctx = lctx->stream->relay_ctx; relay_ctx; relay_ctx = relay_ctx->next) {
        if (relay_ctx == lctx || relay_ctx->paused || relay_ctx->hls) {
            continue;
        }

		ngx_log_error(NGX_LOG_ERR, s->connection->log, 0, "send_start_slice %p", relay_ctx);

		rs = relay_ctx->session;
		ngx_rtmp_send_start_hls_slice(rs, hctx->frag, hctx->frag_ts);
	}

	return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_close_fragment(ngx_rtmp_session_t *s)
{
    ngx_rtmp_hls_ctx_t         *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    if (ctx == NULL || !ctx->opened) {
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: close fragment n=%uL", ctx->frag);

    ngx_close_file(ctx->file.fd);

    ctx->opened = 0;
    ctx->file.fd = NGX_INVALID_FILE;

    ngx_rtmp_hls_next_frag(s);

    ngx_rtmp_hls_write_playlist(s);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_open_fragment(ngx_rtmp_session_t *s, uint64_t ts,
    ngx_int_t discont)
{
    uint64_t                  id;
    ngx_uint_t                g;
    ngx_rtmp_hls_ctx_t       *ctx;
    ngx_rtmp_hls_frag_t      *f;
    ngx_rtmp_hls_app_conf_t  *hacf;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    if (ctx->opened) {
        return NGX_OK;
    }

    if (ngx_rtmp_hls_ensure_directory(s) != NGX_OK) {
        return NGX_ERROR;
    }

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);

    id = ngx_rtmp_hls_get_fragment_id(s, ts);

    if (hacf->granularity) {
        g = (ngx_uint_t) hacf->granularity;
        id = (uint64_t) (id / g) * g;
    }

	if (hacf->nested)
    	*ngx_sprintf(ctx->stream.data + ctx->stream.len, "%uL.ts", id) = 0;
	else
		*ngx_sprintf(ctx->stream.data + ctx->stream.len, ".%uL.ts", id) = 0;

    ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: open fragment file='%s', frag=%uL, n=%ui, time=%uL, "
                   "discont=%i",
                   ctx->stream.data, ctx->frag, ctx->nfrags, ts, discont);

    ngx_memzero(&ctx->file, sizeof(ctx->file));

    ctx->file.log = s->connection->log;

    ngx_str_set(&ctx->file.name, "hls");

    ctx->file.fd = ngx_open_file(ctx->stream.data, NGX_FILE_WRONLY,
                                 NGX_FILE_TRUNCATE, NGX_FILE_DEFAULT_ACCESS);

    if (ctx->file.fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: error creating fragment file");
        return NGX_ERROR;
    }

    if (ngx_rtmp_mpegts_write_header(&ctx->file) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: error writing fragment header");
        ngx_close_file(ctx->file.fd);
        return NGX_ERROR;
    }

    ctx->opened = 1;

    f = ngx_rtmp_hls_get_frag(s, ctx->nfrags);

    ngx_memzero(f, sizeof(*f));

    f->active = 1;
    f->discont = discont;
    f->id = id;

    ctx->frag_ts = ts;

    /* start fragment with audio to make iPhone happy */

    ngx_rtmp_hls_flush_audio(s);

    return NGX_OK;
}


static void
ngx_rtmp_hls_restore_stream(ngx_rtmp_session_t *s)
{
    ngx_rtmp_hls_ctx_t             *ctx;
    ngx_file_t                      file;
    ssize_t                         ret;
    off_t                           offset;
    u_char                         *p, *last, *end, *next, *pa;
    ngx_rtmp_hls_frag_t            *f;
    double                          duration;
    ngx_int_t                       discont;
    uint64_t                        mag;
    static u_char                   buffer[4096];

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    ngx_memzero(&file, sizeof(file));

    file.log = s->connection->log;

    ngx_str_set(&file.name, "m3u8");

    file.fd = ngx_open_file(ctx->playlist.data, NGX_FILE_RDONLY, NGX_FILE_OPEN,
                            0);
    if (file.fd == NGX_INVALID_FILE) {
        return;
    }

    offset = 0;
    ctx->nfrags = 0;
    f = NULL;
    duration = 0;
    discont = 0;

    for ( ;; ) {

        ret = ngx_read_file(&file, buffer, sizeof(buffer), offset);
        if (ret <= 0) {
            goto done;
        }

        p = buffer;
        end = buffer + ret;

        for ( ;; ) {
            last = ngx_strlchr(p, end, '\n');

            if (last == NULL) {
                if (p == buffer) {
                    goto done;
                }
                break;
            }

            next = last + 1;
            offset += (next - p);

            if (p != last && last[-1] == '\r') {
                last--;
            }


#define NGX_RTMP_MSEQ           "#EXT-X-MEDIA-SEQUENCE:"
#define NGX_RTMP_MSEQ_LEN       (sizeof(NGX_RTMP_MSEQ) - 1)


            if (ngx_memcmp(p, NGX_RTMP_MSEQ, NGX_RTMP_MSEQ_LEN) == 0) {

                ctx->frag = (uint64_t) strtod((const char *)
                                              &p[NGX_RTMP_MSEQ_LEN], NULL);

                ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                               "hls: restore sequence frag=%uL", ctx->frag);
            }


#define NGX_RTMP_EXTINF         "#EXTINF:"
#define NGX_RTMP_EXTINF_LEN     (sizeof(NGX_RTMP_EXTINF) - 1)


            if (ngx_memcmp(p, NGX_RTMP_EXTINF, NGX_RTMP_EXTINF_LEN) == 0) {

                duration = strtod((const char *) &p[NGX_RTMP_EXTINF_LEN], NULL);

                ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                               "hls: restore durarion=%.3f", duration);
            }


#define NGX_RTMP_DISCONT        "#EXT-X-DISCONTINUITY"
#define NGX_RTMP_DISCONT_LEN    (sizeof(NGX_RTMP_DISCONT) - 1)


            if (ngx_memcmp(p, NGX_RTMP_DISCONT, NGX_RTMP_DISCONT_LEN) == 0) {

                discont = 1;

                ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                               "hls: discontinuity");
            }

            /* find '.ts\r' */

            if (p + 4 <= last &&
                last[-3] == '.' && last[-2] == 't' && last[-1] == 's')
            {
                f = ngx_rtmp_hls_get_frag(s, ctx->nfrags);

                ngx_memzero(f, sizeof(*f));

                f->duration = duration;
                f->discont = discont;
                f->active = 1;
                f->id = 0;

                discont = 0;

                mag = 1;
                for (pa = last - 4; pa >= p; pa--) {
                    if (*pa < '0' || *pa > '9') {
                        break;
                    }
                    f->id += (*pa - '0') * mag;
                    mag *= 10;
                }

                ngx_rtmp_hls_next_frag(s);

                ngx_log_debug6(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                               "hls: restore fragment '%*s' id=%uL, "
                               "duration=%.3f, frag=%uL, nfrags=%ui",
                               (size_t) (last - p), p, f->id, f->duration,
                               ctx->frag, ctx->nfrags);
            }

            p = next;
        }
    }

done:
    ngx_close_file(file.fd);
}


static ngx_int_t
ngx_rtmp_hls_ensure_directory(ngx_rtmp_session_t *s)
{
    ngx_file_info_t           fi;
    ngx_rtmp_hls_ctx_t       *ctx;
    ngx_rtmp_hls_app_conf_t  *hacf;
	u_char                   *p;
	ngx_int_t                 plen = 0;

    static u_char             path[NGX_MAX_PATH + 1];

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);

	ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

	ngx_memzero(path, sizeof(path));
	p = ngx_cpymem(path, hacf->path.data, hacf->path.len);
	plen += hacf->path.len;
	if (p[-1] != '/') {
        *p++ = '/';
		plen++;
    }

	// path = /tmp/hls/xiaoyi/live
	p = ngx_cpymem(p, ctx->serv_name.data, ctx->serv_name.len);
	*p++ = '/';

	p = ngx_cpymem(p, s->app.data, s->app.len);
	*p++ = '/';

    if (ngx_file_info(path, &fi) == NGX_FILE_ERROR) {

        if (ngx_errno != NGX_ENOENT) {
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, ngx_errno,
                          "hls: " ngx_file_info_n " failed on '%s'",
                          path);
            return NGX_ERROR;
        }

        /* ENOENT */

        if (ngx_create_full_path(path, NGX_RTMP_HLS_DIR_ACCESS) == NGX_FILE_ERROR) {
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, ngx_errno,
                          "hls: " ngx_create_dir_n " failed on '%s'",
                          path);
            return NGX_ERROR;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "hls: directory '%s' created", path);

    } else {

        if (!ngx_is_dir(&fi)) {
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                          "hls: '%s' exists and is not a directory",
                          path);
            return  NGX_ERROR;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "hls: directory '%s' exists", path);
    }

    if (!hacf->nested) {
        return NGX_OK;
    }

	p = ngx_cpymem(p, ctx->name.data, ctx->name.len);
	*p++ = '/';

    if (ngx_file_info(path, &fi) != NGX_FILE_ERROR) {

        if (ngx_is_dir(&fi)) {
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "hls: directory '%s' exists", path);
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "hls: '%s' exists and is not a directory", path);

        return  NGX_ERROR;
    }

    if (ngx_errno != NGX_ENOENT) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: " ngx_file_info_n " failed on '%s'", path);
        return NGX_ERROR;
    }

    /* NGX_ENOENT */

    if (ngx_create_dir(path, NGX_RTMP_HLS_DIR_ACCESS) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "hls: " ngx_create_dir_n " failed on '%s'", path);
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: directory '%s' created", path);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_publish(ngx_rtmp_session_t *s, ngx_rtmp_publish_t *v)
{
    ngx_rtmp_hls_app_conf_t        *hacf;
    ngx_rtmp_hls_ctx_t             *ctx;
    u_char                         *p, *pp;
    ngx_rtmp_hls_frag_t            *f;
    ngx_buf_t                      *b;
    size_t                          len;
    ngx_rtmp_hls_variant_t         *var;
    ngx_uint_t                      n;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);
    if (hacf == NULL || !hacf->hls || hacf->path.len == 0) {
        goto next;
    }

    if (s->auto_pushed) {
        goto next;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                   "hls_publish: name='%s' type='%s'",
                   v->name, v->type);

	ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    if (ctx == NULL) {

        ctx = ngx_pcalloc(s->connection->pool, sizeof(ngx_rtmp_hls_ctx_t));
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_hls_module);

    } else {

        f = ctx->frags;
        b = ctx->aframe;

		ngx_memzero(ctx, sizeof(ngx_rtmp_hls_ctx_t));

        ctx->frags = f;
        ctx->aframe = b;

        if (b) {
            b->pos = b->last = b->start;
        }
    }

    if (ctx->frags == NULL) {
        ctx->frags = ngx_pcalloc(s->connection->pool,
                                 sizeof(ngx_rtmp_hls_frag_t) *
                                 (hacf->winfrags * 2 + 1));
        if (ctx->frags == NULL) {
            return NGX_ERROR;
        }
    }

    if (ngx_strstr(v->name, "..")) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "hls: bad stream name: '%s'", v->name);
        return NGX_ERROR;
    }

    ctx->name.len = ngx_strlen(v->name);
    ctx->name.data = ngx_palloc(s->connection->pool, ctx->name.len + 1);

    if (ctx->name.data == NULL) {
        return NGX_ERROR;
    }

    *ngx_cpymem(ctx->name.data, v->name, ctx->name.len) = 0;

	ctx->serv_name.data = ngx_strchr(s->host_in.data, '.');
	if (ctx->serv_name.data == NULL) {
		return NGX_ERROR;
	}
	ctx->serv_name.len = ctx->serv_name.data - s->host_in.data;
	ctx->serv_name.data = s->host_in.data;

    len = hacf->path.len + 1 + ctx->serv_name.len + 1
			+ s->app.len + 1 + ctx->name.len + sizeof(".m3u8");
    if (hacf->nested) {
        len += sizeof("/index") - 1;
    }

    ctx->playlist.data = ngx_palloc(s->connection->pool, len);

	// playlist = /tmp/hls/
    p = ngx_cpymem(ctx->playlist.data, hacf->path.data, hacf->path.len);
	if (p[-1] != '/') {
        *p++ = '/';
    }

	// playlist = /tmp/hls/xiaoyi/
	p = ngx_cpymem(p, ctx->serv_name.data, ctx->serv_name.len);
    *p++ = '/';

	// playlist = /tmp/hls/xiaoyi/app/
	p = ngx_cpymem(p, s->app.data, s->app.len);
    *p++ = '/';

	// playlist = /tmp/hls/xiaoyi/app/huzilong_demo_1
    p = ngx_cpymem(p, ctx->name.data, ctx->name.len);

    /*
     * ctx->stream holds initial part of stream file path
     * however the space for the whole stream path
     * is allocated
     */

	// /tmp/hls/xiaoyi/app/huzilong_demo_1 + . or / + tsnumber + .ts
    ctx->stream.len = p - ctx->playlist.data + 1;
    ctx->stream.data = ngx_palloc(s->connection->pool,
                                  ctx->stream.len + 1 +
                                  NGX_INT64_LEN + ngx_strlen(".ts"));

	// stream = /tmp/hls/xiaoyi/app/huzilong_demo_1/
    ngx_memcpy(ctx->stream.data, ctx->playlist.data, ctx->stream.len - 1);
	if (hacf->nested) {
		ctx->stream.data[ctx->stream.len - 1] = '/';
	} else {
		ctx->stream.len --;
	}

    /* varint playlist path */

    if (hacf->variant) {
        var = hacf->variant->elts;
        for (n = 0; n < hacf->variant->nelts; n++, var++) {
            if (ctx->name.len > var->suffix.len &&
                ngx_memcmp(var->suffix.data,
                           ctx->name.data + ctx->name.len - var->suffix.len,
                           var->suffix.len)
                == 0)
            {
                ctx->var = var;

                len = (size_t) (p - ctx->playlist.data);

                ctx->var_playlist.len = len - var->suffix.len + sizeof(".m3u8")
                                        - 1;
                ctx->var_playlist.data = ngx_palloc(s->connection->pool,
                                                    ctx->var_playlist.len + 1);

                pp = ngx_cpymem(ctx->var_playlist.data, ctx->playlist.data,
                               len - var->suffix.len);
                pp = ngx_cpymem(pp, ".m3u8", sizeof(".m3u8") - 1);
                *pp = 0;

                ctx->var_playlist_bak.len = ctx->var_playlist.len +
                                            sizeof(".bak") - 1;
                ctx->var_playlist_bak.data = ngx_palloc(s->connection->pool,
                                                 ctx->var_playlist_bak.len + 1);

                pp = ngx_cpymem(ctx->var_playlist_bak.data,
                                ctx->var_playlist.data,
                                ctx->var_playlist.len);
                pp = ngx_cpymem(pp, ".bak", sizeof(".bak") - 1);
                *pp = 0;

                break;
            }
        }
    }


    /* playlist path */

	// playlist = /tmp/hls/xiaoyi/app/huzilong_demo_1/index.m3u8
    if (hacf->nested) {
        p = ngx_cpymem(p, "/index.m3u8", sizeof("/index.m3u8") - 1);
    } else {
        p = ngx_cpymem(p, ".m3u8", sizeof(".m3u8") - 1);
    }

    ctx->playlist.len = p - ctx->playlist.data;

    *p = 0;

    /* playlist bak (new playlist) path */

    ctx->playlist_bak.data = ngx_palloc(s->connection->pool,
                                        ctx->playlist.len + sizeof(".bak"));
    p = ngx_cpymem(ctx->playlist_bak.data, ctx->playlist.data,
                   ctx->playlist.len);
    p = ngx_cpymem(p, ".bak", sizeof(".bak") - 1);

    ctx->playlist_bak.len = p - ctx->playlist_bak.data;

    *p = 0;

    ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: playlist='%V' playlist_bak='%V' stream_pattern='%V'",
                   &ctx->playlist, &ctx->playlist_bak, &ctx->stream);

    if (hacf->continuous) {
        ngx_rtmp_hls_restore_stream(s);
    }

next:
    return next_publish(s, v);
}


static ngx_int_t
ngx_rtmp_hls_play(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v)
{
	ngx_rtmp_hls_app_conf_t        *hacf;
    ngx_rtmp_hls_ctx_t             *ctx;

	if (!s->hls) {
		goto next;
	}

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);
    if (hacf == NULL || !hacf->hls || hacf->path.len == 0) {
        goto next;
    }

	ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    if (ctx == NULL) {
        ctx = ngx_pcalloc(s->connection->pool, sizeof(ngx_rtmp_hls_ctx_t));
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_hls_module);
		ctx->retry_evt_msec = 2000;
    }

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                   "hls_play: name='%s' start=%uD duration=%uD reset=%d page_url='%V' addr_text='%V' tc_url='%V' flashver='%V'",
                   v->name, (uint32_t) v->start,
                   (uint32_t) v->duration, (uint32_t) v->reset, 
                   &s->page_url, s->addr_text, &s->tc_url, &s->flashver);

next:
    return next_play(s, v);
}


static ngx_int_t
ngx_rtmp_hls_open_file(ngx_http_request_t *r, ngx_chain_t *out)
{
	u_char					  *last;
	off_t					   start, len;
	size_t					   root;
	ngx_int_t				   rc;
	ngx_uint_t				   level;
	ngx_str_t				   path;
	ngx_log_t				  *log;
	ngx_buf_t				  *b;
	ngx_open_file_info_t	   of;
	ngx_http_core_loc_conf_t  *clcf;
	ngx_rtmp_session_t        *s;
	ngx_connection_t          *c;

	c = r->connection;
	s = c->hls ? c->hls_data : c->data;

	last = ngx_http_map_uri_to_path(r, &path, &root, 0);
	if (last == NULL) {
		return NGX_ERROR;
	}

	log = r->connection->log;

	path.len = last - path.data;

	ngx_log_error(NGX_LOG_INFO, log, 0,
			"http hls open filename: \"%V\"", &path);

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

	ngx_memzero(&of, sizeof(ngx_open_file_info_t));

	of.read_ahead = clcf->read_ahead;
	of.directio = clcf->directio;
	of.valid = clcf->open_file_cache_valid;
	of.min_uses = clcf->open_file_cache_min_uses;
	of.errors = clcf->open_file_cache_errors;
	of.events = clcf->open_file_cache_events;

	if (ngx_http_set_disable_symlinks(r, clcf, &path, &of) != NGX_OK) {
		return NGX_ERROR;
	}

	if (ngx_open_cached_file(clcf->open_file_cache, &path, &of, r->pool)
		!= NGX_OK)
	{
		switch (of.err) {

		case 0:
			return NGX_HTTP_INTERNAL_SERVER_ERROR;

		case NGX_ENOENT:
		case NGX_ENOTDIR:
		case NGX_ENAMETOOLONG:

			level = NGX_LOG_ERR;
			rc = NGX_HTTP_NOT_FOUND;
			break;

		case NGX_EACCES:
#if (NGX_HAVE_OPENAT)
		case NGX_EMLINK:
		case NGX_ELOOP:
#endif

			level = NGX_LOG_ERR;
			rc = NGX_HTTP_FORBIDDEN;
			break;

		default:

			level = NGX_LOG_CRIT;
			rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
			break;
		}

		if (rc != NGX_HTTP_NOT_FOUND || clcf->log_not_found) {
			ngx_log_error(level, log, of.err,
						  "%s \"%s\" failed", of.failed, path.data);
		}

		return NGX_ERROR;
	}

	if (!of.is_file) {

		if (ngx_close_file(of.fd) == NGX_FILE_ERROR) {
			ngx_log_error(NGX_LOG_ALERT, log, ngx_errno,
						  ngx_close_file_n " \"%s\" failed", path.data);
		}

		return NGX_ERROR;
	}

	r->root_tested = !r->error_page;

	start = 0;
	len = of.size;

	log->action = "sending hls file to client";

	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = len;
	r->headers_out.last_modified_time = of.mtime;

	if (ngx_http_set_etag(r) != NGX_OK) {
		return NGX_ERROR;
	}

	if (ngx_http_set_content_type(r) != NGX_OK) {
		return NGX_ERROR;
	}

	b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
	if (b == NULL) {
		return NGX_ERROR;
	}

	b->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
	if (b->file == NULL) {
		return NGX_ERROR;
	}

	if (!r->header_sent) {
		ngx_http_send_header(r);
	}

	b->file_pos = start;
	b->file_last = of.size;

	b->in_file = b->file_last ? 1: 0;
	b->last_buf = (r == r->main) ? 1 : 0;
	b->last_in_chain = 1;

	b->file->fd = of.fd;
	b->file->name = path;
	b->file->log = log;
	b->file->directio = of.is_directio;

	out->buf = b;
	out->next = NULL;

	return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_close_stream(ngx_rtmp_session_t *s, ngx_rtmp_close_stream_t *v)
{
    ngx_rtmp_hls_app_conf_t        *hacf;
    ngx_rtmp_hls_ctx_t             *ctx;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    if (hacf == NULL || !hacf->hls || ctx == NULL) {
        goto next;
    }

#if 0
	if (ctx->hls_stream == NULL) {
		goto next;
	}

	if (ctx->hls_stream->publishing && ctx->publishing) {
        ctx->hls_stream->publishing = 0;
    }

    for (cctx = &ctx->hls_stream->ctx; *cctx; cctx = &(*cctx)->next) {
        if (*cctx == ctx) {
            *cctx = ctx->next;
            break;
        }
    }

	if (ctx->hls_stream->ctx) {
		ctx->hls_stream = NULL;
		goto next;
	}

	hls_stream = ngx_rtmp_hls_get_stream(s, ctx->hls_stream->name, 0);
    if (hls_stream == NULL) {
        goto next;
    }
    *hls_stream = (*hls_stream)->next;

    ctx->hls_stream->next = hacf->free_streams;
    hacf->free_streams    = ctx->hls_stream;
    ctx->hls_stream = NULL;
#endif

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: close stream");

    ngx_rtmp_hls_close_fragment(s);

next:
    return next_close_stream(s, v);
}


static ngx_int_t
ngx_rtmp_hls_parse_aac_header(ngx_rtmp_session_t *s, ngx_uint_t *objtype,
    ngx_uint_t *srindex, ngx_uint_t *chconf)
{
    ngx_rtmp_codec_ctx_t   *codec_ctx;
    ngx_chain_t            *cl;
    u_char                 *p, b0, b1;

    codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    cl = codec_ctx->aac_header;

    p = cl->buf->pos;

    if (ngx_rtmp_hls_copy(s, NULL, &p, 2, &cl) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_rtmp_hls_copy(s, &b0, &p, 1, &cl) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_rtmp_hls_copy(s, &b1, &p, 1, &cl) != NGX_OK) {
        return NGX_ERROR;
    }

    *objtype = b0 >> 3;
    if (*objtype == 0 || *objtype == 0x1f) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "hls: unsupported adts object type:%ui", *objtype);
        return NGX_ERROR;
    }

    if (*objtype > 4) {

        /*
         * Mark all extended profiles as LC
         * to make Android as happy as possible.
         */

        *objtype = 2;
    }

    *srindex = ((b0 << 1) & 0x0f) | ((b1 & 0x80) >> 7);
    if (*srindex == 0x0f) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "hls: unsupported adts sample rate:%ui", *srindex);
        return NGX_ERROR;
    }

    *chconf = (b1 >> 3) & 0x0f;

    ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: aac object_type:%ui, sample_rate_index:%ui, "
                   "channel_config:%ui", *objtype, *srindex, *chconf);

    return NGX_OK;
}


static void
ngx_rtmp_hls_update_fragment(ngx_rtmp_session_t *s, uint64_t ts,
    ngx_int_t boundary, ngx_uint_t flush_rate)
{
    ngx_rtmp_hls_ctx_t         *ctx;
    ngx_rtmp_hls_app_conf_t    *hacf;
    ngx_rtmp_hls_frag_t        *f;
    ngx_msec_t                  ts_frag_len;
    ngx_int_t                   same_frag, force,discont;
    ngx_buf_t                  *b;
    int64_t                     d;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);
    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    f = NULL;
    force = 0;
    discont = 1;

    if (ctx->opened) {
        f = ngx_rtmp_hls_get_frag(s, ctx->nfrags);
        d = (int64_t) (ts - ctx->frag_ts);

        if (d > (int64_t) hacf->max_fraglen * 90 || d < -90000) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "hls: force fragment split: %.3f sec, ", d / 90000.);
            force = 1;

        } else {
            f->duration = (ts - ctx->frag_ts) / 90000.;
            discont = 0;
        }
    }

    switch (hacf->slicing) {
        case NGX_RTMP_HLS_SLICING_PLAIN:
            if (f && f->duration < hacf->fraglen / 1000.) {
                boundary = 0;
            }
            break;

        case NGX_RTMP_HLS_SLICING_ALIGNED:

            ts_frag_len = hacf->fraglen * 90;
            same_frag = ctx->frag_ts / ts_frag_len == ts / ts_frag_len;

            if (f && same_frag) {
                boundary = 0;
            }

            if (f == NULL && (ctx->frag_ts == 0 || same_frag)) {
                ctx->frag_ts = ts;
                boundary = 0;
            }

            break;
    }

    if (boundary || force) {
		ngx_rtmp_hls_send_start_slice(s);
        ngx_rtmp_hls_close_fragment(s);
        ngx_rtmp_hls_open_fragment(s, ts, discont);
    }

    b = ctx->aframe;
    if (ctx->opened && b && b->last > b->pos &&
        ctx->aframe_pts + (uint64_t) hacf->max_audio_delay * 90 / flush_rate
        < ts)
    {
        ngx_rtmp_hls_flush_audio(s);
    }
}


static ngx_int_t
ngx_rtmp_hls_flush_audio(ngx_rtmp_session_t *s)
{
    ngx_rtmp_hls_ctx_t             *ctx;
    ngx_rtmp_mpegts_frame_t         frame;
    ngx_int_t                       rc;
    ngx_buf_t                      *b;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    if (ctx == NULL || !ctx->opened) {
        return NGX_OK;
    }

    b = ctx->aframe;

    if (b == NULL || b->pos == b->last) {
        return NGX_OK;
    }

    ngx_memzero(&frame, sizeof(frame));

    frame.dts = ctx->aframe_pts;
    frame.pts = frame.dts;
    frame.cc = ctx->audio_cc;
    frame.pid = 0x101;
    frame.sid = 0xc0;

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: flush audio pts=%uL", frame.pts);

    rc = ngx_rtmp_mpegts_write_frame(&ctx->file, &frame, b);

    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "hls: audio flush failed");
    }

    ctx->audio_cc = frame.cc;
    b->pos = b->last = b->start;

    return rc;
}


static ngx_int_t
ngx_rtmp_hls_audio(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
    ngx_chain_t *in)
{
    ngx_rtmp_hls_app_conf_t        *hacf;
    ngx_rtmp_hls_ctx_t             *ctx;
    ngx_rtmp_codec_ctx_t           *codec_ctx;
    uint64_t                        pts, est_pts;
    int64_t                         dpts;
    size_t                          bsize;
    ngx_buf_t                      *b;
    u_char                         *p;
    ngx_uint_t                      objtype, srindex, chconf, size;

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    if (hacf == NULL || !hacf->hls || ctx == NULL ||
        codec_ctx == NULL  || h->mlen < 2)
    {
        return NGX_OK;
    }

	if (s->relay && !ctx->sliced) {
		return NGX_OK;
	}

    if (codec_ctx->audio_codec_id != NGX_RTMP_AUDIO_AAC ||
        codec_ctx->aac_header == NULL || ngx_rtmp_is_codec_header(in))
    {
        return NGX_OK;
    }

    b = ctx->aframe;

    if (b == NULL) {

        b = ngx_pcalloc(s->connection->pool, sizeof(ngx_buf_t));
        if (b == NULL) {
            return NGX_ERROR;
        }

        ctx->aframe = b;

        b->start = ngx_palloc(s->connection->pool, hacf->audio_buffer_size);
        if (b->start == NULL) {
            return NGX_ERROR;
        }

        b->end = b->start + hacf->audio_buffer_size;
        b->pos = b->last = b->start;
    }

    size = h->mlen - 2 + 7;
    pts = (uint64_t) h->timestamp * 90;

    if (b->start + size > b->end) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "hls: too big audio frame");
        return NGX_OK;
    }

    /*
     * start new fragment here if
     * there's no video at all, otherwise
     * do it in video handler
     */

    ngx_rtmp_hls_update_fragment(s, pts, codec_ctx->avc_header == NULL, 2);

    if (b->last + size > b->end) {
        ngx_rtmp_hls_flush_audio(s);
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: audio pts=%uL", pts);

    if (b->last + 7 > b->end) {
        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "hls: not enough buffer for audio header");
        return NGX_OK;
    }

    p = b->last;
    b->last += 5;

    /* copy payload */

    for (; in && b->last < b->end; in = in->next) {

        bsize = in->buf->last - in->buf->pos;
        if (b->last + bsize > b->end) {
            bsize = b->end - b->last;
        }

        b->last = ngx_cpymem(b->last, in->buf->pos, bsize);
    }

    /* make up ADTS header */

    if (ngx_rtmp_hls_parse_aac_header(s, &objtype, &srindex, &chconf)
        != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "hls: aac header error");
        return NGX_OK;
    }

    /* we have 5 free bytes + 2 bytes of RTMP frame header */

    p[0] = 0xff;
    p[1] = 0xf1;
    p[2] = (u_char) (((objtype - 1) << 6) | (srindex << 2) |
                     ((chconf & 0x04) >> 2));
    p[3] = (u_char) (((chconf & 0x03) << 6) | ((size >> 11) & 0x03));
    p[4] = (u_char) (size >> 3);
    p[5] = (u_char) ((size << 5) | 0x1f);
    p[6] = 0xfc;

    if (p != b->start) {
        ctx->aframe_num++;
        return NGX_OK;
    }

    ctx->aframe_pts = pts;

    if (!hacf->sync || codec_ctx->sample_rate == 0) {
        return NGX_OK;
    }

    /* align audio frames */

    /* TODO: We assume here AAC frame size is 1024
     *       Need to handle AAC frames with frame size of 960 */

    est_pts = ctx->aframe_base + ctx->aframe_num * 90000 * 1024 /
                                 codec_ctx->sample_rate;
    dpts = (int64_t) (est_pts - pts);

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: audio sync dpts=%L (%.5fs)",
                   dpts, dpts / 90000.);

    if (dpts <= (int64_t) hacf->sync * 90 &&
        dpts >= (int64_t) hacf->sync * -90)
    {
        ctx->aframe_num++;
        ctx->aframe_pts = est_pts;
        return NGX_OK;
    }

    ctx->aframe_base = pts;
    ctx->aframe_num  = 1;

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: audio sync gap dpts=%L (%.5fs)",
                   dpts, dpts / 90000.);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_video(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
    ngx_chain_t *in)
{
    ngx_rtmp_hls_app_conf_t        *hacf;
    ngx_rtmp_hls_ctx_t             *ctx;
    ngx_rtmp_codec_ctx_t           *codec_ctx;
    u_char                         *p;
    uint8_t                         fmt, ftype, htype, nal_type, src_nal_type;
    uint32_t                        len, rlen;
    ngx_buf_t                       out, *b;
    uint32_t                        cts;
    ngx_rtmp_mpegts_frame_t         frame;
    ngx_uint_t                      nal_bytes;
    ngx_int_t                       aud_sent, sps_pps_sent, boundary;
    static u_char                   buffer[NGX_RTMP_HLS_BUFSIZE];

    hacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_hls_module);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

    codec_ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_codec_module);

    if (hacf == NULL || !hacf->hls || ctx == NULL || codec_ctx == NULL ||
        codec_ctx->avc_header == NULL || h->mlen < 1)
    {
        return NGX_OK;
    }

	if (s->relay && !ctx->sliced) {
		return NGX_OK;
	}

    /* Only H264 is supported */
    if (codec_ctx->video_codec_id != NGX_RTMP_VIDEO_H264) {
        return NGX_OK;
    }

    p = in->buf->pos;
    if (ngx_rtmp_hls_copy(s, &fmt, &p, 1, &in) != NGX_OK) {
        return NGX_ERROR;
    }

    /* 1: keyframe (IDR)
     * 2: inter frame
     * 3: disposable inter frame */

    ftype = (fmt & 0xf0) >> 4;

    /* H264 HDR/PICT */

    if (ngx_rtmp_hls_copy(s, &htype, &p, 1, &in) != NGX_OK) {
        return NGX_ERROR;
    }

    /* proceed only with PICT */

    if (htype != 1) {
        return NGX_OK;
    }

    /* 3 bytes: decoder delay */

    if (ngx_rtmp_hls_copy(s, &cts, &p, 3, &in) != NGX_OK) {
        return NGX_ERROR;
    }

    cts = ((cts & 0x00FF0000) >> 16) | ((cts & 0x000000FF) << 16) |
          (cts & 0x0000FF00);

    ngx_memzero(&out, sizeof(out));

    out.start = buffer;
    out.end = buffer + sizeof(buffer);
    out.pos = out.start;
    out.last = out.pos;

    nal_bytes = codec_ctx->avc_nal_bytes;
    aud_sent = 0;
    sps_pps_sent = 0;

    while (in) {
        if (ngx_rtmp_hls_copy(s, &rlen, &p, nal_bytes, &in) != NGX_OK) {
            return NGX_OK;
        }

        len = 0;
        ngx_rtmp_rmemcpy(&len, &rlen, nal_bytes);

        if (len == 0) {
            continue;
        }

        if (ngx_rtmp_hls_copy(s, &src_nal_type, &p, 1, &in) != NGX_OK) {
            return NGX_OK;
        }

        nal_type = src_nal_type & 0x1f;

        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "hls: h264 NAL type=%ui, len=%uD",
                       (ngx_uint_t) nal_type, len);

        if (nal_type >= 7 && nal_type <= 9) {
            if (ngx_rtmp_hls_copy(s, NULL, &p, len - 1, &in) != NGX_OK) {
                return NGX_ERROR;
            }
            continue;
        }

        if (!aud_sent) {
            switch (nal_type) {
                case 1:
                case 5:
                case 6:
                    if (ngx_rtmp_hls_append_aud(s, &out) != NGX_OK) {
                        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                                      "hls: error appending AUD NAL");
                    }
                case 9:
                    aud_sent = 1;
                    break;
            }
        }

        switch (nal_type) {
            case 1:
                sps_pps_sent = 0;
                break;
            case 5:
                if (sps_pps_sent) {
                    break;
                }
                if (ngx_rtmp_hls_append_sps_pps(s, &out) != NGX_OK) {
                    ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                                  "hls: error appenging SPS/PPS NALs");
                }
                sps_pps_sent = 1;
                break;
        }

        /* AnnexB prefix */

        if (out.end - out.last < 5) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "hls: not enough buffer for AnnexB prefix");
            return NGX_OK;
        }

        /* first AnnexB prefix is long (4 bytes) */

        if (out.last == out.pos) {
            *out.last++ = 0;
        }

        *out.last++ = 0;
        *out.last++ = 0;
        *out.last++ = 1;
        *out.last++ = src_nal_type;

        /* NAL body */

        if (out.end - out.last < (ngx_int_t) len) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "hls: not enough buffer for NAL");
            return NGX_OK;
        }

        if (ngx_rtmp_hls_copy(s, out.last, &p, len - 1, &in) != NGX_OK) {
            return NGX_ERROR;
        }

        out.last += (len - 1);
    }

    ngx_memzero(&frame, sizeof(frame));

    frame.cc = ctx->video_cc;
    frame.dts = (uint64_t) h->timestamp * 90;
    frame.pts = frame.dts + cts * 90;
    frame.pid = 0x100;
    frame.sid = 0xe0;
    frame.key = (ftype == 1);

    /*
     * start new fragment if
     * - we have video key frame AND
     * - we have audio buffered or have no audio at all or stream is closed
     */

    b = ctx->aframe;
    boundary = frame.key && (codec_ctx->aac_header == NULL || !ctx->opened ||
                             (b && b->last > b->pos));

    ngx_rtmp_hls_update_fragment(s, frame.dts, boundary, 1);

    if (!ctx->opened) {
        return NGX_OK;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "hls: video pts=%uL, dts=%uL", frame.pts, frame.dts);

    if (ngx_rtmp_mpegts_write_frame(&ctx->file, &frame, &out) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "hls: video frame failed");
    }

    ctx->video_cc = frame.cc;

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_stream_begin(ngx_rtmp_session_t *s, ngx_rtmp_stream_begin_t *v)
{
    return next_stream_begin(s, v);
}


static ngx_int_t
ngx_rtmp_hls_stream_eof(ngx_rtmp_session_t *s, ngx_rtmp_stream_eof_t *v)
{
    ngx_rtmp_hls_flush_audio(s);

    ngx_rtmp_hls_close_fragment(s);

    return next_stream_eof(s, v);
}


static ngx_int_t
ngx_rtmp_hls_start_hls_slice(ngx_rtmp_session_t *s, ngx_rtmp_start_hls_slice_t *v)
{
	ngx_rtmp_hls_ctx_t             *ctx;

	if (s->relay != 0 && v != NULL) {
		goto next;
	}

	ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);

	if (ctx == NULL) {
		goto next;
	}

	ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
               "start_hls_slice: ctx->sliced='%d' frag=%uD frag_ts=%uD",
               ctx->sliced, v->frag, v->frag_ts);

	if (ctx->sliced == 0) {
		ctx->frag = v->frag;
		ctx->frag_ts = v->frag_ts;
		ctx->sliced = 1;
	}

next:
	return next_start_hls_slice(s, v);
}


static ngx_int_t
ngx_rtmp_hls_cleanup_dir(ngx_str_t *ppath, ngx_msec_t playlen)
{
    ngx_dir_t               dir;
    time_t                  mtime, max_age;
    ngx_err_t               err;
    ngx_str_t               name, spath;
    u_char                 *p;
    ngx_int_t               nentries, nerased;
    u_char                  path[NGX_MAX_PATH + 1];

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, ngx_cycle->log, 0,
                   "hls: cleanup path='%V' playlen=%M",
                   ppath, playlen);

    if (ngx_open_dir(ppath, &dir) != NGX_OK) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, ngx_cycle->log, ngx_errno,
                      "hls: cleanup open dir failed '%V'", ppath);
        return NGX_ERROR;
    }

    nentries = 0;
    nerased = 0;

    for ( ;; ) {
        ngx_set_errno(0);

        if (ngx_read_dir(&dir) == NGX_ERROR) {
            err = ngx_errno;

            if (ngx_close_dir(&dir) == NGX_ERROR) {
                ngx_log_error(NGX_LOG_CRIT, ngx_cycle->log, ngx_errno,
                              "hls: cleanup " ngx_close_dir_n " \"%V\" failed",
                              ppath);
            }

            if (err == NGX_ENOMOREFILES) {
                return nentries - nerased;
            }

            ngx_log_error(NGX_LOG_CRIT, ngx_cycle->log, err,
                          "hls: cleanup " ngx_read_dir_n
                          " '%V' failed", ppath);
            return NGX_ERROR;
        }

        name.data = ngx_de_name(&dir);
        if (name.data[0] == '.') {
            continue;
        }

        name.len = ngx_de_namelen(&dir);

        p = ngx_snprintf(path, sizeof(path) - 1, "%V/%V", ppath, &name);
        *p = 0;

        spath.data = path;
        spath.len = p - path;

        nentries++;

        if (!dir.valid_info && ngx_de_info(path, &dir) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_CRIT, ngx_cycle->log, ngx_errno,
                          "hls: cleanup " ngx_de_info_n " \"%V\" failed",
                          &spath);

            continue;
        }

        if (ngx_de_is_dir(&dir)) {

            if (ngx_rtmp_hls_cleanup_dir(&spath, playlen) == 0) {
                ngx_log_debug1(NGX_LOG_DEBUG_RTMP, ngx_cycle->log, 0,
                               "hls: cleanup dir '%V'", &name);

                /*
                 * null-termination gets spoiled in win32
                 * version of ngx_open_dir
                 */

                *p = 0;

                if (ngx_delete_dir(path) == NGX_FILE_ERROR) {
                    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
                                  "hls: cleanup " ngx_delete_dir_n
                                  " failed on '%V'", &spath);
                } else {
                    nerased++;
                }
            }

            continue;
        }

        if (!ngx_de_is_file(&dir)) {
            continue;
        }

        if (name.len >= 3 && name.data[name.len - 3] == '.' &&
                             name.data[name.len - 2] == 't' &&
                             name.data[name.len - 1] == 's')
        {
            max_age = playlen / 500;

        } else if (name.len >= 5 && name.data[name.len - 5] == '.' &&
                                    name.data[name.len - 4] == 'm' &&
                                    name.data[name.len - 3] == '3' &&
                                    name.data[name.len - 2] == 'u' &&
                                    name.data[name.len - 1] == '8')
        {
            max_age = playlen / 1000;

        } else {
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, ngx_cycle->log, 0,
                           "hls: cleanup skip unknown file type '%V'", &name);
            continue;
        }

        mtime = ngx_de_mtime(&dir);
        if (mtime + max_age > ngx_cached_time->sec) {
            continue;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, ngx_cycle->log, 0,
                       "hls: cleanup '%V' mtime=%T age=%T",
                       &name, mtime, ngx_cached_time->sec - mtime);

        if (ngx_delete_file(path) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
                          "hls: cleanup " ngx_delete_file_n " failed on '%V'",
                          &spath);
            continue;
        }

        nerased++;
    }
}


static time_t
ngx_rtmp_hls_cleanup(void *data)
{
    ngx_rtmp_hls_cleanup_t *cleanup = data;

    ngx_rtmp_hls_cleanup_dir(&cleanup->path, cleanup->playlen);

    return cleanup->playlen / 500;
}


static char *
ngx_rtmp_hls_variant(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_rtmp_hls_app_conf_t  *hacf = conf;

    ngx_str_t                *value, *arg;
    ngx_uint_t                n;
    ngx_rtmp_hls_variant_t   *var;

    value = cf->args->elts;

    if (hacf->variant == NULL) {
        hacf->variant = ngx_array_create(cf->pool, 1,
                                         sizeof(ngx_rtmp_hls_variant_t));
        if (hacf->variant == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    var = ngx_array_push(hacf->variant);
    if (var == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(var, sizeof(ngx_rtmp_hls_variant_t));

    var->suffix = value[1];

    if (cf->args->nelts == 2) {
        return NGX_CONF_OK;
    }

    if (ngx_array_init(&var->args, cf->pool, cf->args->nelts - 2,
                       sizeof(ngx_str_t))
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    arg = ngx_array_push_n(&var->args, cf->args->nelts - 2);
    if (arg == NULL) {
        return NGX_CONF_ERROR;
    }

    for (n = 2; n < cf->args->nelts; n++) {
        *arg++ = value[n];
    }

    return NGX_CONF_OK;
}


static void *
ngx_rtmp_hls_create_app_conf(ngx_conf_t *cf)
{
    ngx_rtmp_hls_app_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_hls_app_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->hls = NGX_CONF_UNSET;
    conf->fraglen = NGX_CONF_UNSET_MSEC;
    conf->max_fraglen = NGX_CONF_UNSET_MSEC;
    conf->muxdelay = NGX_CONF_UNSET_MSEC;
    conf->sync = NGX_CONF_UNSET_MSEC;
    conf->playlen = NGX_CONF_UNSET_MSEC;
    conf->continuous = NGX_CONF_UNSET;
    conf->nested = NGX_CONF_UNSET;
    conf->naming = NGX_CONF_UNSET_UINT;
    conf->slicing = NGX_CONF_UNSET_UINT;
    conf->type = NGX_CONF_UNSET_UINT;
    conf->max_audio_delay = NGX_CONF_UNSET_MSEC;
    conf->audio_buffer_size = NGX_CONF_UNSET_SIZE;
    conf->cleanup = NGX_CONF_UNSET;
    conf->granularity = NGX_CONF_UNSET;
	conf->nbuckets = NGX_CONF_UNSET;
	conf->idle_streams = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_rtmp_hls_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_hls_app_conf_t    *prev = parent;
    ngx_rtmp_hls_app_conf_t    *conf = child;
    ngx_rtmp_hls_cleanup_t     *cleanup;

    ngx_conf_merge_value(conf->hls, prev->hls, 0);
    ngx_conf_merge_msec_value(conf->fraglen, prev->fraglen, 5000);
    ngx_conf_merge_msec_value(conf->max_fraglen, prev->max_fraglen,
                              conf->fraglen * 10);
    ngx_conf_merge_msec_value(conf->muxdelay, prev->muxdelay, 700);
    ngx_conf_merge_msec_value(conf->sync, prev->sync, 2);
    ngx_conf_merge_msec_value(conf->playlen, prev->playlen, 30000);
    ngx_conf_merge_value(conf->continuous, prev->continuous, 1);
    ngx_conf_merge_value(conf->nested, prev->nested, 0);
    ngx_conf_merge_uint_value(conf->naming, prev->naming,
                              NGX_RTMP_HLS_NAMING_SEQUENTIAL);
    ngx_conf_merge_uint_value(conf->slicing, prev->slicing,
                              NGX_RTMP_HLS_SLICING_PLAIN);
    ngx_conf_merge_uint_value(conf->type, prev->type,
                              NGX_RTMP_HLS_TYPE_LIVE);
    ngx_conf_merge_msec_value(conf->max_audio_delay, prev->max_audio_delay,
                              300);
    ngx_conf_merge_size_value(conf->audio_buffer_size, prev->audio_buffer_size,
                              NGX_RTMP_HLS_BUFSIZE);
    ngx_conf_merge_value(conf->cleanup, prev->cleanup, 1);
    ngx_conf_merge_str_value(conf->base_url, prev->base_url, "");
    ngx_conf_merge_value(conf->granularity, prev->granularity, 0);
	ngx_conf_merge_value(conf->nbuckets, prev->nbuckets, 1024);
	ngx_conf_merge_value(conf->idle_streams, prev->idle_streams, 1);

	conf->pool = ngx_create_pool(4096, &cf->cycle->new_log);
    if (conf->pool == NULL) {
        return NGX_CONF_ERROR;
    }

	conf->streams = ngx_pcalloc(cf->pool,
            sizeof(ngx_rtmp_hls_stream_t *) * conf->nbuckets);

    if (conf->fraglen) {
        conf->winfrags = conf->playlen / conf->fraglen;
    }

    /* schedule cleanup */

    if (conf->hls && conf->path.len && conf->cleanup &&
        conf->type != NGX_RTMP_HLS_TYPE_EVENT)
    {
        if (conf->path.data[conf->path.len - 1] == '/') {
            conf->path.len--;
        }

        cleanup = ngx_pcalloc(cf->pool, sizeof(*cleanup));
        if (cleanup == NULL) {
            return NGX_CONF_ERROR;
        }

        cleanup->path = conf->path;
        cleanup->playlen = conf->playlen;

        conf->slot = ngx_pcalloc(cf->pool, sizeof(*conf->slot));
        if (conf->slot == NULL) {
            return NGX_CONF_ERROR;
        }

        conf->slot->manager = ngx_rtmp_hls_cleanup;
        conf->slot->name = conf->path;
        conf->slot->data = cleanup;
        conf->slot->conf_file = cf->conf_file->file.name.data;
        conf->slot->line = cf->conf_file->line;

        if (ngx_add_path(cf, &conf->slot) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    ngx_conf_merge_str_value(conf->path, prev->path, "");

    return NGX_CONF_OK;
}


static void
ngx_rtmp_hls_close_connection(ngx_connection_t *c)
{
	ngx_rtmp_session_t		   *s;
	ngx_rtmp_core_srv_conf_t   *cscf;

	s = c->hls ? c->hls_data : c->data;
	c = s->connection;

	cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    ngx_log_error(NGX_LOG_ERR, c->log, 0, "hls close connection");

    ngx_rtmp_fire_event(s, NGX_RTMP_DISCONNECT, NULL, NULL);

    if (s->ping_evt.timer_set) {
        ngx_del_timer(&s->ping_evt);
    }

    while (s->out_pos != s->out_last) {
        ngx_rtmp_free_shared_chain(cscf, s->out[s->out_pos++]);
        s->out_pos %= s->out_queue;
    }

	ngx_destroy_pool(c->hls_pool);

}


ngx_rtmp_session_t *
ngx_rtmp_hls_init_session(ngx_connection_t *c, ngx_rtmp_addr_conf_t *addr_conf)
{
    ngx_rtmp_session_t             *s;
	ngx_rtmp_core_srv_conf_t       *cscf;

    s = ngx_pcalloc(c->hls_pool, sizeof(ngx_rtmp_session_t) +
            sizeof(ngx_chain_t *) * ((ngx_rtmp_core_srv_conf_t *)
                addr_conf->ctx-> srv_conf[ngx_rtmp_core_module
                    .ctx_index])->out_queue);
    if (s == NULL) {
		ngx_rtmp_close_connection(c);
        return NULL;
    }

	s->addr_conf = addr_conf;

	s->main_conf = addr_conf->ctx->main_conf;
    s->srv_conf = addr_conf->ctx->srv_conf;

	s->addr_text = &addr_conf->addr_text;

    c->hls_data = s;
	s->connection = c;
	c->hls_closer = ngx_rtmp_hls_close_connection;

	c->hls = 1;

    s->ctx = ngx_pcalloc(c->hls_pool, sizeof(void *) * ngx_rtmp_max_module);
    if (s->ctx == NULL) {
        ngx_rtmp_close_connection(c);
        return NULL;
    }

	cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    s->out_queue = cscf->out_queue;
    s->out_cork = cscf->out_cork;
    s->in_streams = ngx_pcalloc(c->hls_pool, sizeof(ngx_rtmp_stream_t)
            * cscf->max_streams);
    if (s->in_streams == NULL) {
        ngx_rtmp_close_connection(c);
        return NULL;
    }

	s->epoch = ngx_current_msec;
	s->timeout = cscf->timeout;
	s->buflen = cscf->buflen;
	s->connected = 1;
	ngx_rtmp_set_chunk_size(s, NGX_RTMP_DEFAULT_CHUNK_SIZE);

	if (ngx_rtmp_fire_event(s, NGX_RTMP_CONNECT, NULL, NULL) != NGX_OK) {
        ngx_rtmp_finalize_session(s);
        return NULL;
    }

    return s;
}


static ngx_int_t
ngx_rtmp_http_hls_change_uri(ngx_http_request_t *r, ngx_str_t host,
	ngx_rtmp_hls_app_conf_t *hacf)
{
	u_char                   *p, *colon;
	ngx_str_t                 fact;
	ngx_str_t                 uri;
	ngx_http_core_loc_conf_t *clcf;
	ngx_connection_t         *c;

	c = r->connection;

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	if (clcf == NULL) {
        return NGX_ERROR;
    }

	colon = ngx_strchr(host.data, ':');
	if (colon != NULL) {
		host.len = host.data - colon;
	}

	fact.data = ngx_strchr(host.data, '.');
	fact.len = fact.data - host.data;
	fact.data = host.data;

	ngx_int_t offset = hacf->path.len - clcf->root.len;
	uri.len = r->uri.len + fact.len + 1;
	uri.data = ngx_palloc(c->pool, uri.len);

	p = ngx_cpymem(uri.data, r->uri.data, offset);
	if (hacf->path.data[hacf->path.len - 1] != '/') {
		*p++ = '/';
	}

	p = ngx_cpymem(p, fact.data, fact.len);

	p = ngx_cpymem(p, r->uri.data + offset, r->uri.len - offset);

	r->uri = uri;

	return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_init_connection(ngx_http_request_t *r, ngx_int_t t, ngx_str_t host, 
	ngx_str_t *stream_name, ngx_rtmp_core_srv_conf_t *cscf, ngx_rtmp_core_app_conf_t *cacf,
	ngx_rtmp_hls_app_conf_t *hacf, ngx_rtmp_conf_port_t *cf_port)
{
	ngx_uint_t             i;
	ngx_rtmp_port_t       *port;
    ngx_rtmp_session_t    *s;
	ngx_rtmp_addr_conf_t  *addr_conf;
	ngx_connection_t      *c;
	ngx_rtmp_hls_ctx_t    *ctx;
	struct sockaddr       *sa;
	struct sockaddr_in    *sin;
    ngx_rtmp_in_addr_t    *addr;
	ngx_int_t              unix_socket;
	ngx_http_core_loc_conf_t *clcf;
	u_char                *colon;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6   *sin6;
    ngx_rtmp_in6_addr_t   *addr6;
#endif

	c = r->connection;

	++ngx_rtmp_hls_naccepted;

	port = cf_port->ports.elts;
    unix_socket = 0;

    if (port->naddrs > 1) {

        /*
         * There are several addresses on this port and one of them
         * is the "*:port" wildcard so getsockname() is needed to determine
         * the server address.
         *
         * AcceptEx() already gave this address.
         */

        if (ngx_connection_local_sockaddr(c, NULL, 0) != NGX_OK) {
            ngx_rtmp_close_connection(c);
            return NGX_ERROR;
        }

        sa = c->local_sockaddr;

        switch (sa->sa_family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) sa;

            addr6 = port->addrs;

            /* the last address is "*" */

            for (i = 0; i < port->naddrs - 1; i++) {
                if (ngx_memcmp(&addr6[i].addr6, &sin6->sin6_addr, 16) == 0) {
                    break;
                }
            }

            addr_conf = &addr6[i].conf;

            break;
#endif

        case AF_UNIX:
            unix_socket = 1;

        default: /* AF_INET */
            sin = (struct sockaddr_in *) sa;

            addr = port->addrs;

            /* the last address is "*" */

            for (i = 0; i < port->naddrs - 1; i++) {
                if (addr[i].addr == sin->sin_addr.s_addr) {
                    break;
                }
            }

            addr_conf = &addr[i].conf;

            break;
        }

    } else {
        switch (c->local_sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            addr6 = port->addrs;
            addr_conf = &addr6[0].conf;
            break;
#endif

        case AF_UNIX:
            unix_socket = 1;

        default: /* AF_INET */
            addr = port->addrs;
            addr_conf = &addr[0].conf;
            break;
        }
    }

	ngx_log_error(NGX_LOG_INFO, c->log, 0, "*%ui hls client connected '%V'",
                  c->number, &c->addr_text);

	c->hls_pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, c->log);

    s = ngx_rtmp_hls_init_session(c, addr_conf);
    if (s == NULL) {
        return NGX_ERROR;
    }

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	if (clcf == NULL) {
        return NGX_ERROR;
    }

	s->auto_pushed = unix_socket;

    s->hls = 1;
	s->rdata = r;

	s->app_conf = cacf->app_conf;

	colon = ngx_strchr(host.data, ':');
	if (colon != NULL) {
		host.len = host.data - colon;
	}

	s->host_in.data = ngx_palloc(c->hls_pool, host.len);
	if (s->host_in.data == NULL) {
		return NGX_ERROR;
	}
	s->host_in.len = host.len;
	ngx_memcpy(s->host_in.data, host.data, host.len);

	ngx_memzero(s->name, sizeof(s->name));
	ngx_memcpy(s->name, stream_name->data, stream_name->len);
	s->app  = cacf->name;
	s->args = r->args;

	ngx_str_set(&s->flashver, "HLS 17,0,0,188");
	s->swf_url  = r->headers_in.user_agent->value;
	s->page_url = r->headers_in.user_agent->value;
	s->tc_url.len = ngx_strlen("http://") + r->headers_in.host->value.len + s->app.len + 1;
	s->tc_url.data = ngx_palloc(c->hls_pool, s->tc_url.len);
	*ngx_snprintf(s->tc_url.data, s->tc_url.len, "http://%V/%V",
		&r->headers_in.host->value, &s->app) = 0;

	ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    if (ctx == NULL) {
        ctx = ngx_pcalloc(c->hls_pool, sizeof(ngx_rtmp_hls_ctx_t));
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_hls_module);
    }
	ctx->request_type = t;

	return NGX_OK;
}


static void
ngx_rtmp_hls_retry_m3u8(ngx_event_t *e)
{
	ngx_rtmp_hls_ctx_t   *ctx;
	ngx_http_request_t   *r;
	ngx_rtmp_session_t   *s;
	ngx_int_t             rc, opened;
	ngx_chain_t           out;

	s = e->data;
	r = s->rdata;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    if (ctx == NULL) {
		ngx_del_timer(&ctx->retry_evt);
        return;
    }

	opened = ngx_rtmp_hls_open_file(r, &out);
	if (opened == NGX_OK) {
		ngx_del_timer(&ctx->retry_evt);
		rc = ngx_http_output_filter(r, &out);
		ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "ngx_rtmp_hls_retry_m3u8 output_filter rc: %d", rc);
		ngx_http_finalize_request(r, rc);
	} else {
		e = &ctx->retry_evt;
		e->data = s;
	    e->log = r->connection->log;
	    e->handler = ngx_rtmp_hls_retry_m3u8;

		ngx_add_timer(e, 2000);
	}

 	ngx_log_debug(NGX_LOG_DEBUG, s->connection->log, 0, "ngx_rtmp_hls_retry_m3u8 callbacked");

	return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_retry_m3u8_timer(ngx_rtmp_session_t *s)
{
	ngx_rtmp_hls_ctx_t   *ctx;
	ngx_http_request_t   *r;
	ngx_event_t          *e;;

	r = s->rdata;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    if (ctx == NULL) {
        return NGX_OK;
    }

	e = &ctx->retry_evt;
	e->data = s;
    e->log = r->connection->log;
    e->handler = ngx_rtmp_hls_retry_m3u8;

	ngx_add_timer(e, 2000);

	ngx_log_debug(NGX_LOG_DEBUG, s->connection->log, 0,
               "ngx_rtmp_hls_retry_m3u8_timer: call ngx_rtmp_hls_retry_m3u8 after %Mms",
               ctx->retry_evt_msec);

	return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_auth_done(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
        ngx_chain_t *in)
{
    ngx_rtmp_hls_ctx_t   *ctx;
	ngx_http_request_t   *r;
	ngx_int_t             rc, opened;
	ngx_chain_t           out;
	ngx_int_t            *prc = (ngx_int_t *)in->buf->start;

	r = s->rdata;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_hls_module);
    if (ctx == NULL || !s->hls) {
        return NGX_OK;
    }

	ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "http_hls auth done rc: %d", *prc);

    if (*prc != NGX_OK) {
    	ngx_rtmp_finalize_session(s);
    } else {
    	opened = ngx_rtmp_hls_open_file(r, &out);
		if (opened == NGX_OK) {
			rc = ngx_http_output_filter(r, &out);
			ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "http_hls auth done output_filter rc: %d", rc);
			ngx_http_finalize_request(r, rc);
		} else {
			ngx_rtmp_hls_retry_m3u8_timer(s);
		}
    }

	return NGX_OK;
}


static ngx_int_t
ngx_rtmp_http_hls_handler(ngx_http_request_t *r)
{
	ngx_rtmp_http_hls_loc_conf_t        *hlcf;
	ngx_rtmp_core_srv_conf_t            *cscf;
	ngx_rtmp_core_app_conf_t            *cacf;
	ngx_rtmp_hls_app_conf_t             *hacf;
	ngx_rtmp_conf_port_t                *cf_port;
	ngx_rtmp_session_t                  *s;
	ngx_int_t                            t;
    ngx_int_t                  	         rc, opened;
	ngx_str_t                            host;
	ngx_str_t                            stream_name;
	ngx_chain_t                          out;

	hlcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_http_hls_module);
	if (hlcf == NULL || !hlcf->hls) {
		return NGX_DECLINED;
	}

	if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    if (r->uri.data[r->uri.len - 1] == '/' &&
		r->uri.len > ngx_strlen(".ts")) {
        return NGX_DECLINED;
    }

	if (r->uri.data[r->uri.len - 1] == 's' &&
		r->uri.data[r->uri.len - 2] == 't' &&
		r->uri.data[r->uri.len - 3] == '.') {
		t = NGX_RTMP_HTTP_HLS_ACCESS_TS;
	} else if (r->uri.len > ngx_strlen(".m3u8") &&
		r->uri.data[r->uri.len - 1] == '8' &&
		r->uri.data[r->uri.len - 2] == 'u' &&
		r->uri.data[r->uri.len - 3] == '3' &&
		r->uri.data[r->uri.len - 4] == 'm' &&
		r->uri.data[r->uri.len - 5] == '.') {
		t = NGX_RTMP_HTTP_HLS_ACCESS_M3U8;
	} else {
		return NGX_DECLINED;
	}

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

	ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
              "http_hls handle uri: '%V' args: '%V' r->out: '%d'", &r->uri, &r->args, r->out == NULL);

	if (ngx_rtmp_http_hls_match_app(r, t, &cscf, &cacf, &cf_port, &hacf, &host, &stream_name) != NGX_OK) {
		goto error;
	}

	ngx_rtmp_http_hls_change_uri(r, host, hacf);

	ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
              "http_hls handle uri changed uri: '%V' args: '%V'", &r->uri, &r->args);

	s = r->connection->hls_data;
	if (!(s == NULL && t == NGX_RTMP_HTTP_HLS_ACCESS_M3U8)) {
		opened = ngx_rtmp_hls_open_file(r, &out);

		ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
              "http_hls open file finished rc: '%d' request_type:'%s'",
              rc, t == NGX_RTMP_HTTP_HLS_ACCESS_M3U8 ? "m3u8" : "ts");
	}

	if (s != NULL) {
		ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "http_hls handle keep-alived request");
		if (opened != NGX_OK) {
			goto error;
		}
		return ngx_http_output_filter(r, &out);
	}

	switch(t) {
		case NGX_RTMP_HTTP_HLS_ACCESS_TS:
		{
			if (opened != NGX_OK) {
				goto error;
			}
			return ngx_http_output_filter(r, &out);
		}
		case NGX_RTMP_HTTP_HLS_ACCESS_M3U8:
		{
			ngx_rtmp_hls_init_connection(r, t, host, &stream_name, cscf, cacf, hacf, cf_port);

			ngx_rtmp_http_hls_play_local(r, &stream_name);

			return NGX_CUSTOME;
		}
	}

error:
	r->headers_out.status = NGX_HTTP_FORBIDDEN;
	r->headers_out.content_length_n = 0;
	return ngx_http_send_header(r);
}


static void *
ngx_rtmp_http_hls_create_conf(ngx_conf_t *cf)
{
    ngx_rtmp_http_hls_loc_conf_t  *hlcf;

    hlcf = ngx_palloc(cf->pool, sizeof(ngx_rtmp_http_hls_loc_conf_t));
    if (hlcf == NULL) {
        return NULL;
    }

	hlcf->hls = NGX_CONF_UNSET;

    return hlcf;
}


static char *
ngx_rtmp_http_hls_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
	ngx_rtmp_http_hls_loc_conf_t *prev = parent;
    ngx_rtmp_http_hls_loc_conf_t *conf = child;

	ngx_conf_merge_value(conf->hls, prev->hls, 0);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_rtmp_http_hls_play_local(ngx_http_request_t *r, ngx_str_t *stream_name)
{
	ngx_rtmp_play_t 	        v;
	ngx_rtmp_session_t         *s;

	s = (ngx_rtmp_session_t *)r->connection->hls_data;

	ngx_memzero(&v, sizeof(ngx_rtmp_play_t));

    *(ngx_cpymem(v.name, stream_name->data, ngx_min(sizeof(v.name) - 1, stream_name->len))) = 0;

	if (r->args.len > 0)
		*(ngx_cpymem(v.args, r->args.data, ngx_min(sizeof(v.args) - 1, r->args.len))) = 0;

	return ngx_rtmp_cmd_start_play(s, &v);
}


static ngx_int_t
ngx_rtmp_http_hls_match_app(ngx_http_request_t *r, ngx_int_t t,
	ngx_rtmp_core_srv_conf_t **out_cscf, ngx_rtmp_core_app_conf_t **out_cacf,
	ngx_rtmp_conf_port_t **out_cfport, ngx_rtmp_hls_app_conf_t **out_hacf,
	ngx_str_t *out_host, ngx_str_t *out_stream_name)
{
	size_t                          i, j, k;
	ngx_str_t                       name;
	ngx_http_core_loc_conf_t       *clcf;
	ngx_rtmp_core_main_conf_t      *cmcf;
    ngx_rtmp_core_srv_conf_t      **cscf;
	ngx_rtmp_core_app_conf_t	  **cacf;
	ngx_rtmp_conf_port_t           *cf_ports;
	ngx_rtmp_hls_app_conf_t        *hacf;
	ngx_rtmp_server_name_t         *srna;

	cmcf = ngx_rtmp_core_main_conf;
    if (cmcf == NULL) {
        return NGX_ERROR;
    }

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	if (clcf == NULL) {
        return NGX_ERROR;
    }

	cf_ports = cmcf->ports.elts;
	cscf  = cmcf->servers.elts;
    for (i = 0; i < cmcf->servers.nelts; ++i, ++cscf) {

		srna = (*cscf)->server_names.elts;
		for (j = 0; j < (*cscf)->server_names.nelts; ++j, ++srna) {

			if (ngx_strncmp(srna->name.data, r->headers_in.host->value.data, srna->name.len) == 0) {

				cacf = (*cscf)->applications.elts;
				for (k = 0; k < (*cscf)->applications.nelts; ++k, ++cacf) {

					hacf = ngx_rtmp_hls_get_module_app_conf((*cacf)->app_conf, ngx_rtmp_hls_module);
					if (hacf != NULL && hacf->hls && hacf->path.len > 0) {

				        if (ngx_strncmp(clcf->root.data, hacf->path.data, clcf->root.len) == 0 && 
							ngx_strncmp(hacf->path.data + clcf->root.len, r->uri.data, hacf->path.len - clcf->root.len) == 0 &&
							ngx_strncmp((*cacf)->name.data, r->uri.data + hacf->path.len + 1 - clcf->root.len, (*cacf)->name.len) == 0) {

							name.data = r->uri.data + (hacf->path.len - clcf->root.len) + 1 + (*cacf)->name.len + 1;
							name.len = r->uri.len - (hacf->path.len - clcf->root.len) - 1 - (*cacf)->name.len - 1;
							switch(t) {
								case NGX_RTMP_HTTP_HLS_ACCESS_TS:
								{
									name.len -= ngx_strlen(".ts");
									while (name.len > 0 && *(name.data + (name.len--) - 1) != (hacf->nested ? '/' : '.'));
									break;
								}
								case NGX_RTMP_HTTP_HLS_ACCESS_M3U8:
									name.len -= ngx_strlen(hacf->nested ? "/index.m3u8" : ".m3u8");
									break;
							}

							*out_cscf = *cscf;
							*out_cacf = *cacf;
							*out_cfport = &cf_ports[i];
							*out_hacf = hacf;
							*out_host = srna->name;
							*out_stream_name = name;

							ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
		              					"match_app success host: '%V' app: '%V' name: '%V'",
		              					out_host, &(*out_cacf)->name, out_stream_name);

							return NGX_OK;
				        }
				    }
				}
			}
		}
    }

	ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
				"match_app failed");
	
	return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_http_hls_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_rtmp_http_hls_handler;

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_hls_postconfiguration(ngx_conf_t *cf)
{
    ngx_rtmp_core_main_conf_t   *cmcf;
    ngx_rtmp_handler_pt         *h;

    cmcf = ngx_rtmp_conf_get_module_main_conf(cf, ngx_rtmp_core_module);

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_VIDEO]);
    *h = ngx_rtmp_hls_video;

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_AUDIO]);
    *h = ngx_rtmp_hls_audio;

	h = ngx_array_push(&cmcf->events[NGX_RTMP_AUTH_DONE]);
    *h = ngx_rtmp_hls_auth_done;

    next_publish = ngx_rtmp_publish;
    ngx_rtmp_publish = ngx_rtmp_hls_publish;

	next_play = ngx_rtmp_play;
    ngx_rtmp_play = ngx_rtmp_hls_play;

    next_close_stream = ngx_rtmp_close_stream;
    ngx_rtmp_close_stream = ngx_rtmp_hls_close_stream;

    next_stream_begin = ngx_rtmp_stream_begin;
    ngx_rtmp_stream_begin = ngx_rtmp_hls_stream_begin;

    next_stream_eof = ngx_rtmp_stream_eof;
    ngx_rtmp_stream_eof = ngx_rtmp_hls_stream_eof;

	next_start_hls_slice = ngx_rtmp_start_hls_slice;
    ngx_rtmp_start_hls_slice = ngx_rtmp_hls_start_hls_slice;

    return NGX_OK;
}
