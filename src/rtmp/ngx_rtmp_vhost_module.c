
/*
 * Copyright (C) Edward.Wu
 * date: 2015-01-15
 * 
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"
#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_netcall_module.h"
#include "ngx_rtmp_codec_module.h"




static ngx_int_t
ngx_is_valid_ip( u_char* str )
{
    
    ngx_int_t a;
    u_char* p, * p1;
    u_char temp[NGX_RTMP_MAX_NAME] = {0};
    size_t len;
    size_t count = 0;

    p1 = str;
    p = NULL;
    while (1){
        p = (u_char *)ngx_strchr(p1, '.');

        ngx_memset(temp, 0, NGX_RTMP_MAX_NAME);
        len = (size_t)(p ? p - p1 : ngx_strlen(p1));
        ngx_memcpy(temp, p1, len);
        a = ngx_atoi(temp, len);
	    if (a < 0 || a > 255){
            return NGX_ERROR;
        }       

        count ++;
        if(count > 4){
            return NGX_ERROR;
        }
        if (!p)
        	break;
        p1 = p;
        p1 ++;
    }
    return NGX_OK;
        
/*        
    //the last param 'd' is always wrong value, I don't know why.
    int len = sscanf(str, "%d.%d.%d.%d",&a,&b,&c,&d);
    if( (4 == len)
	    &&   (a>=0   &&   a<=255)
		&&   (b>=0   &&   b<=255)
		&&   (c>=0   &&   c<=255)
		&&   (d>=0   &&   d<=255))
	{
	    return NGX_OK;
	}
	return NGX_ERROR;
	*/
}

static ngx_rtmp_connect_pt          next_connect;


static ngx_int_t ngx_rtmp_vhost_postconfiguration(ngx_conf_t *cf);
static void * ngx_rtmp_vhost_create_app_conf(ngx_conf_t *cf);
static char * ngx_rtmp_vhost_merge_app_conf(ngx_conf_t *cf,
       void *parent, void *child);


typedef struct {
     ngx_str_t                          vhost;
} ngx_rtmp_vhost_app_conf_t;






static ngx_command_t  ngx_rtmp_vhost_commands[] = {

    { ngx_string("vhost"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|
                         NGX_RTMP_REC_CONF|NGX_CONF_1MORE,
      ngx_conf_set_str_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_vhost_app_conf_t, vhost),
      NULL },


      ngx_null_command
};


static ngx_rtmp_module_t  ngx_rtmp_vhost_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_rtmp_vhost_postconfiguration,      /* postconfiguration */
    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */
    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */
    ngx_rtmp_vhost_create_app_conf,        /* create app configuration */
    ngx_rtmp_vhost_merge_app_conf          /* merge app configuration */
};


ngx_module_t  ngx_rtmp_vhost_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_vhost_module_ctx,            /* module context */
    ngx_rtmp_vhost_commands,               /* module directives */
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
ngx_rtmp_vhost_create_app_conf(ngx_conf_t *cf)
{
    ngx_rtmp_vhost_app_conf_t      *racf;

    racf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_vhost_app_conf_t));

    if (racf == NULL) {
        return NULL;
    }


    return racf;
}


static char *
ngx_rtmp_vhost_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_vhost_app_conf_t     *prev = parent;
    ngx_rtmp_vhost_app_conf_t     *conf = child;
 
    ngx_conf_merge_str_value(conf->vhost, prev->vhost, "");
    

    return NGX_CONF_OK;
}



static ngx_int_t
ngx_rtmp_vhost_connect(ngx_rtmp_session_t *s, ngx_rtmp_connect_t *v)
{
    ngx_rtmp_vhost_app_conf_t     *racf;
    size_t len ;
    size_t vhost_len ;
    ngx_rtmp_core_srv_conf_t   *cscf;
    ngx_rtmp_core_app_conf_t  **cacfp;
    ngx_uint_t                  n;
    u_char * p, *p1;
    u_char domain_name[NGX_RTMP_MAX_NAME] = {0};
    u_char param[NGX_RTMP_MAX_NAME] = {0};
    u_char param_name[NGX_RTMP_MAX_NAME] = {0};
    u_char param_value[NGX_RTMP_MAX_NAME] = {0};

    if (s->auto_pushed) {
        goto next;
    }
    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);
    /* find application & set app_conf */
    cacfp = cscf->applications.elts;
    for(n = 0; n < cscf->applications.nelts; ++n, ++cacfp) {
        if ((*cacfp)->name.len == ngx_strlen(v->app) &&
            ngx_strncmp((*cacfp)->name.data, v->app, ngx_strlen(v->app)) == 0)
        {
            /* found app! */
            s->app_conf = (*cacfp)->app_conf;
            break;
        }
    }

    racf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_vhost_module);

    if ( NULL == racf ) {
        goto next;
    }

    vhost_len = racf->vhost.len;
    //check vhost, 
    if ( vhost_len > 0 )
    {
        //1:check the url 
        //get ip or domain name
        p = v->tc_url;
        p += 7;// skip rtmp://
        p1 = p;
        p = (u_char *)ngx_strchr(p1, '/');
        if (NULL == p) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                         "vhost: tc_url[%s] has only doname, why?.",
                         v->tc_url);
            return NGX_ERROR;
        }
        ngx_memcpy(domain_name, p1, p - p1);
        if (NGX_ERROR == ngx_is_valid_ip(domain_name)){

            if( ngx_strlen(domain_name) != racf->vhost.len 
                || ngx_strcmp(domain_name, racf->vhost.data) != 0){
            
                ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                             "vhost: the domain name [%s] is not equl to conf's [%s].",
                             domain_name, racf->vhost.data);
                return NGX_ERROR;
            }
            
            goto next;
        }
        
        
        //2:check the args
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "vhost: check the vhost in url args [%s].", v->args);
        
        len = ngx_strlen(v->args);
        if (0 == len) 
        {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                         "vhost: fail to find vhost in url args which is empty.");
            return NGX_ERROR;
        }
        //get the vhost param
        p = (u_char *)ngx_strchr(v->args, '&');
        if (NULL != p ) {
            ngx_memcpy(param,v->args,v->args - p);
        }
        else{
            ngx_memcpy(param, v->args, len);    
        }
        

        len = ngx_strlen(param);
        if (0 == len){ 
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                         "vhost: url args is [%s], but fail to get vhost param, why?.",
                         v->args);

            return NGX_ERROR;
        }
        

        //split the name and value
        p = (u_char *)ngx_strchr(param, '=');   
        if (NULL == p){
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                         "vhost: fail to find '=' in [%s].",
                         param);

            return NGX_ERROR;           
        }
        *p++ = 0;
        
        ngx_cpystrn(param_name, param, NGX_RTMP_MAX_ARGS);
        ngx_cpystrn(param_value, p, NGX_RTMP_MAX_ARGS);
        
        if( ngx_strlen(param_name) != ngx_strlen("vhost") 
            || ngx_strcmp(param_name,"vhost") != 0){
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                         "vhost: the param name must be 'vhost', but [%s].",
                         param);
            return NGX_ERROR;
        }
        if( ngx_strlen(param_value) != racf->vhost.len 
            || ngx_strcmp(param_value,racf->vhost.data) != 0){
        
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                         "vhost: the url'vhost[%s] is not equl to conf's [%s].",
                         param_value, racf->vhost.data);
            return NGX_ERROR;
        }
        

    }



next:
    return next_connect(s, v);
}


static ngx_int_t
ngx_rtmp_vhost_postconfiguration(ngx_conf_t *cf)
{

    //next_publish = ngx_rtmp_publish;
    next_connect = ngx_rtmp_connect;
    ngx_rtmp_connect = ngx_rtmp_vhost_connect;

    return NGX_OK;
}

