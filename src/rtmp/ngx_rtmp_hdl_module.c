
/*
 * Copyright (C) Edward.Wu
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_version.h"
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_play_module.h"
#include "ngx_rtmp_codec_module.h"


static ngx_int_t ngx_rtmp_hdl_init_process(ngx_cycle_t *cycle);
static char *ngx_rtmp_hdl(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_rtmp_hdl_postconfiguration(ngx_conf_t *cf);
static void * ngx_rtmp_hdl_create_loc_conf(ngx_conf_t *cf);
static char * ngx_rtmp_hdl_merge_loc_conf(ngx_conf_t *cf,
        void *parent, void *child);
static ngx_int_t ngx_rtmp_hdl_write_handler(ngx_http_request_t *r);

static time_t                       start_time;


#define NGX_RTMP_HDL_ALL           0xff
#define NGX_RTMP_HDL_GLOBAL        0x01
#define NGX_RTMP_HDL_LIVE          0x02
#define NGX_RTMP_HDL_CLIENTS       0x04
#define NGX_RTMP_HDL_PLAY          0x08

/*
 * global: hdl-{bufs-{total,free,used}, total bytes in/out, bw in/out} - cscf
*/


typedef struct {
    ngx_uint_t                      hdl;
    //ngx_str_t                       stylesheet;
} ngx_rtmp_hdl_loc_conf_t;


static ngx_conf_bitmask_t           ngx_rtmp_hdl_masks[] = {
    { ngx_string("all"),            NGX_RTMP_HDL_ALL           },
    { ngx_string("global"),         NGX_RTMP_HDL_GLOBAL        },
    { ngx_string("live"),           NGX_RTMP_HDL_LIVE          },
    { ngx_string("clients"),        NGX_RTMP_HDL_CLIENTS       },
    { ngx_null_string,              0 }
};


static ngx_command_t  ngx_rtmp_hdl_commands[] = {

    { ngx_string("rtmp_hdl"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
        ngx_rtmp_hdl,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_rtmp_hdl_loc_conf_t, hdl),
        ngx_rtmp_hdl_masks },
/*
    { ngx_string("rtmp_stat_stylesheet"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_rtmp_stat_loc_conf_t, stylesheet),
        NULL },
*/
    ngx_null_command
};


static ngx_http_module_t  ngx_rtmp_hdl_module_ctx = {
    NULL,                               /* preconfiguration */
    ngx_rtmp_hdl_postconfiguration,    /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    ngx_rtmp_hdl_create_loc_conf,      /* create location configuration */
    ngx_rtmp_hdl_merge_loc_conf,       /* merge location configuration */
};


ngx_module_t  ngx_rtmp_hdl_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_hdl_module_ctx,          /* module context */
    ngx_rtmp_hdl_commands,             /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    ngx_rtmp_hdl_init_process,         /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


#define NGX_RTMP_HDL_BUFSIZE           256


static ngx_int_t
ngx_rtmp_hdl_init_process(ngx_cycle_t *cycle)
{
    /*
     * HTTP process initializer is called
     * after event module initializer
     * so we can run posted events here
     */

    ngx_event_process_posted(cycle, &ngx_rtmp_init_queue);

    return NGX_OK;
}

static ngx_int_t
ngx_rtmp_hdl_handler(ngx_http_request_t *r)
{
    ngx_rtmp_hdl_loc_conf_t       *slcf;
    ngx_rtmp_core_main_conf_t      *cmcf;
    ngx_rtmp_core_srv_conf_t      **cscf;
    ngx_chain_t                    *cl, *l, **ll, ***lll;
    size_t                          n;
    off_t                           len;
    static u_char                   tbuf[NGX_TIME_T_LEN];
    static u_char                   nbuf[NGX_INT_T_LEN];

    slcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_hdl_module);
    if (slcf->hdl == 0) {
        return NGX_DECLINED;
    }

    cmcf = ngx_rtmp_core_main_conf;
    if (cmcf == NULL) {
        goto error;
    }

    ngx_int_t rc;
    ngx_buf_t *b;
    ngx_chain_t out;
    ngx_rtmp_hdl_loc_conf_t *elcf;
    elcf = ngx_http_get_module_loc_conf(r, ngx_rtmp_hdl_module);
    if (slcf->hdl == 0) {
        return NGX_DECLINED;
    }

	if(!(r->method & (NGX_HTTP_HEAD|NGX_HTTP_GET|NGX_HTTP_POST)))
    {
        return NGX_HTTP_NOT_ALLOWED;
    }
    ngx_str_t response = ngx_string("Hello World!!!");  

	//http header
    r->headers_out.content_type.len = sizeof("text/html") - 1;
    r->headers_out.content_type.data = (u_char *) "text/html";
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = 1000000;//response.len;
    if(r->method == NGX_HTTP_HEAD)
    {
        rc = ngx_http_send_header(r);
        if(rc != NGX_OK)
        {
            return rc;
        }
    }


    rc = ngx_http_send_header(r);
    if(rc != NGX_OK)
    {
        return rc;
    }

    //set rtmp info
    cscf = cmcf->servers.elts;
    for (n = 0; n < cmcf->servers.nelts; ++n, ++cscf) {
		ngx_rtmp_core_app_conf_t	  **cacf;
		size_t							n_cacf;
		cacf = (*cscf)->applications.elts;
		for (n_cacf = 0; n_cacf < (*cscf)->applications.nelts; ++n_cacf, ++cacf) {
			ngx_rtmp_live_app_conf_t *lacf = (*cacf)->app_conf[ngx_rtmp_live_module.ctx_index];
			size_t n_lacf ;
			for ( n_lacf = 0; n_lacf < lacf->nbuckets; ++n_lacf) {
			    ngx_rtmp_live_stream_t *stream;
				for (stream = lacf->streams[n_lacf]; stream; stream = stream->next) {
                    //if (strcmp(stream->name, strem_name) == 0){
                    stream->http_r = r;
                    //found_stream = 1;
                    break;
				}
			}
		}
    }

	return NGX_CUSTOME;

error:
    r->headers_out.status = NGX_HTTP_INTERNAL_SERVER_ERROR;
    r->headers_out.content_length_n = 0;
    return ngx_http_send_header(r);
}


static void *
ngx_rtmp_hdl_create_loc_conf(ngx_conf_t *cf)
{
    ngx_rtmp_hdl_loc_conf_t       *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_hdl_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->hdl = 0;

    return conf;
}


static char *
ngx_rtmp_hdl_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_hdl_loc_conf_t       *prev = parent;
    ngx_rtmp_hdl_loc_conf_t       *conf = child;

    ngx_conf_merge_bitmask_value(conf->hdl, prev->hdl, 0);
    //ngx_conf_merge_str_value(conf->stylesheet, prev->stylesheet, "");

    return NGX_CONF_OK;
}


static char *
ngx_rtmp_hdl(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_rtmp_hdl_handler;

    return ngx_conf_set_bitmask_slot(cf, cmd, conf);
}


static ngx_int_t
ngx_rtmp_hdl_av(ngx_rtmp_session_t *s, ngx_rtmp_header_t *h,
                   ngx_chain_t *in)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, s->connection->log, 0,
                   "rtmp hdl av data handler");
    return NGX_OK;
}



static ngx_int_t
ngx_rtmp_hdl_postconfiguration(ngx_conf_t *cf)
{
    ngx_rtmp_core_main_conf_t          *cmcf;
    ngx_rtmp_handler_pt                *h;

    cmcf = ngx_rtmp_core_main_conf;
    if (cmcf == NULL) {
        return NGX_ERROR;
    }

    //cmcf = ngx_rtmp_conf_get_module_main_conf(cf, ngx_rtmp_core_module);

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_AUDIO]);
    *h = ngx_rtmp_hdl_av;

    h = ngx_array_push(&cmcf->events[NGX_RTMP_MSG_VIDEO]);
    *h = ngx_rtmp_hdl_av;

    return NGX_OK;
}


