
#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"

#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_play_module.h"
#include "ngx_rtmp_bandwidth.h"

#define DEFAULT_RTMP_BILLINGD_PATH 			"/usr/local/nginx/sbin/rtmpbillingd"
#define DEFAULT_RTMP_BILLINGD_PID_PATH 		"/usr/local/nginx/sbin/rtmpbillingd.pid"
#define DEFAULT_RTMP_BILLINGD_LOG_PATH 		"/usr/local/nginx/sbin/billing"
#define DEFAULT_BILLINGD_NAME 				"rtmpbillingd"

#define DEFAULT_BILLING_INTERVAL 300 	/*second*/
#define DEFAULT_BILLING_PORT 10001
#define DEFAULT_BILLING_SYNCTIME 6 		/*second*/
#define NGX_BIG_SENDBUF_SIZE 1024
#define DEFAULT_BILLING_CHECK_TIMER 10000


typedef struct {
    ngx_event_t                                 *ev;//event of billing interval
    ngx_peer_connection_t                       *pc;
    ngx_msec_t                                  timeout;
    ngx_uint_t                                  bufsize;
    ngx_chain_t                                 *send_buf;
    ngx_url_t                                   *url;
    ngx_log_t                                   *log;
    ngx_uint_t                                  send_flag;
}ngx_rtmp_billing_session_t;


typedef struct {
	ngx_rtmp_bandwidth_t 		bd_in;
	ngx_rtmp_bandwidth_t		bd_out;
	u_char                      name[128];
}ngx_rtmp_billing_bandwidth_t;


typedef struct {
    ngx_flag_t              rtmp_billing;
    ngx_str_t               rtmp_billing_name;
    ngx_str_t               rtmp_billing_log_path;
    ngx_str_t               rtmp_billing_path;
    ngx_str_t               rtmp_billing_pid_path;
    ngx_msec_t              rtmp_billing_interval;
    ngx_int_t               rtmp_billing_port;
    ngx_msec_t              rtmp_billing_timeout;
    ngx_uint_t              rtmp_billing_bufsize;

    ngx_url_t               *url;
    ngx_flag_t              connected;
    ngx_shmtx_t             billingd_shmtx;

    ngx_pool_t              *pool;
    ngx_event_t             *billingd_check_timer;
    ngx_event_t             *billing_sync_timer;

    ngx_log_t               *log;
    ngx_rtmp_billing_session_t *bs;
	
    ngx_array_t              *srv_billing_bd;
}ngx_rtmp_billing_main_conf_t;


static ngx_int_t 
ngx_rtmp_billing_init_process(ngx_cycle_t *cycle);
static void * 
ngx_rtmp_billing_create_main_conf (ngx_conf_t *cf);
static char *
ngx_rtmp_billing_init_main_conf(ngx_conf_t *cf, void *conf);
static ngx_int_t
ngx_rtmp_billing_start(ngx_cycle_t *cycle);
static void
ngx_rtmp_billing_sync (ngx_event_t *ev);
static ngx_int_t
ngx_rtmp_billing_connect_create(ngx_event_t *ev);
static void
ngx_rtmp_billing_connect_close(ngx_rtmp_billing_session_t *bs);
static ngx_chain_t* ngx_rtmp_billing_new_chain(ngx_pool_t  *pool);
static void
ngx_rtmp_billing_send(ngx_event_t *wev);
static void
ngx_rtmp_billing_recv(ngx_event_t *rev);
static ngx_url_t *
ngx_rtmp_billing_parse_url(ngx_event_t *ev);
static ngx_int_t
ngx_rtmp_billing_get_peer(ngx_peer_connection_t *pc, void *data);
static void
ngx_rtmp_billing_free_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state);
static ngx_flag_t 
ngx_rtmp_billingd_isrunning(const char *cmd_name, const char *cmd_pid_path);
static void 
ngx_rtmp_billing_server_handler(ngx_event_t *ev);
static ngx_int_t
ngx_rtmp_billing_start_billingd(int port, const char *cmd_name, const char *cmd_path, const char* log_dir, const char *cmd_pid_path, int billing_interval);
void 
ngx_rtmp_billing_get_arg(ngx_cycle_t *cycle);
static ngx_int_t 
ngx_rtmp_billing_getpid(const char *cmd_pid_path);
static void
ngx_rtmp_billing_server(ngx_log_t  *log, ngx_rtmp_core_srv_conf_t *cscf, ngx_array_t *bd);
static void
ngx_rtmp_billing_application(ngx_log_t  *log, ngx_rtmp_core_app_conf_t *cscf, ngx_array_t *bd);
static void
ngx_rtmp_billing_live(ngx_log_t  *log, ngx_rtmp_live_app_conf_t *lacf, ngx_array_t *bd);


static ngx_rtmp_module_t  ngx_rtmp_billing_module_ctx = {
    NULL,                                   /* preconfiguration */
    NULL,                                   /* postconfiguration */
    ngx_rtmp_billing_create_main_conf, /* create main configuration */
    ngx_rtmp_billing_init_main_conf,   /*  init main configuration */
    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */
    NULL,                                   /* create app configuration */
    NULL                                    /* merge app configuration */
};

ngx_module_t  ngx_rtmp_billing_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_billing_module_ctx,       /* module context */
    NULL,                                   /* module directives */
    NGX_RTMP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    ngx_rtmp_billing_init_process,      /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t 
ngx_rtmp_billing_init_process(ngx_cycle_t *cycle)
{
    ngx_rtmp_billing_get_arg(cycle);
    ngx_rtmp_billing_start(cycle);
    return NGX_OK;
}

static void * 
ngx_rtmp_billing_create_main_conf (ngx_conf_t *cf)
{
    ngx_rtmp_billing_main_conf_t  *conf;
    
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_billing_main_conf_t));
    if (conf == NULL) {
		
        return NULL;
    }
    
    conf->rtmp_billing = 0;
    conf->rtmp_billing_interval = DEFAULT_BILLING_INTERVAL * 1000;
    conf->rtmp_billing_port = DEFAULT_BILLING_PORT;
    conf->pool = cf->pool;
    
    return conf;
}

static char *
ngx_rtmp_billing_init_main_conf(ngx_conf_t *cf, void *conf)
{
    return NGX_CONF_OK;
}

static ngx_int_t
ngx_rtmp_billing_start(ngx_cycle_t *cycle)
{
    ngx_rtmp_billing_main_conf_t *bmcf = ngx_rtmp_cycle_get_module_main_conf(cycle, ngx_rtmp_billing_module);
    bmcf->log = cycle->log;
	
	ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "billing module: billiing module start...");   

    /* kill rtmpbillingd if already started */
    if (ngx_rtmp_billingd_isrunning((const char *)bmcf->rtmp_billing_name.data, (const char *)bmcf->rtmp_billing_pid_path.data)) {

        char billid_file[256] ={0};

        sprintf(billid_file, "%s/rtmpbillingd.pid", (const char *)bmcf->rtmp_billing_pid_path.data);
        ngx_int_t pid = ngx_rtmp_billing_getpid(billid_file);
        if (pid > 0) {

			ngx_log_error(NGX_LOG_INFO, cycle->log, 0, "billing module: kill rtmpbillingd:%i", pid);  
		    kill(pid,SIGTERM);
        }
    }
	
    if (!bmcf->rtmp_billing) {

		ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "billing module: bmcf->rtmp_billing is zero"); 
        return NGX_OK;
    }
	
	bmcf->srv_billing_bd = ngx_array_create(bmcf->pool, 4,
                                       sizeof(ngx_rtmp_billing_bandwidth_t));
    if (bmcf->srv_billing_bd == NULL) {

			ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "billing module: init bmcf->srv_billing_bd failed");       
            return NGX_ERROR;
    }
	
    ngx_event_t *billingd_check_timer = ngx_pcalloc(bmcf->pool, sizeof(ngx_event_t));
    if (billingd_check_timer != NULL) {
		
        billingd_check_timer->handler = ngx_rtmp_billing_server_handler;
        billingd_check_timer->data = bmcf;
        billingd_check_timer->timer_set = 0;
        billingd_check_timer->log = cycle->log;

        bmcf->billingd_check_timer = billingd_check_timer;

        ngx_rtmp_billing_server_handler(bmcf->billingd_check_timer);
    }
    
    ngx_event_t *billing_sync_timer = ngx_pcalloc(bmcf->pool, sizeof(ngx_event_t));
    if (billing_sync_timer != NULL) {
		
        billing_sync_timer->handler = ngx_rtmp_billing_sync;
        billing_sync_timer->data = bmcf;
        billing_sync_timer->timer_set = 0;
        billing_sync_timer->log = cycle->log;

        bmcf->billing_sync_timer = billing_sync_timer;

        ngx_add_timer(bmcf->billing_sync_timer, (ngx_msec_t)bmcf->rtmp_billing_interval);
    }

    return NGX_OK;
}

void
ngx_rtmp_billing_sync (ngx_event_t *ev)
{

    ngx_rtmp_billing_main_conf_t *bmcf = ev->data;
	
    if (!ngx_exiting) {
		
        ngx_add_timer(bmcf->billing_sync_timer, (ngx_msec_t)bmcf->rtmp_billing_interval); 
    }

	if (!bmcf->connected) {

        if (bmcf->url == NGX_CONF_UNSET_PTR || bmcf->url == NULL) {

            bmcf->url = ngx_rtmp_billing_parse_url(ev);

            if (!bmcf->url) {
				
                return;
            }
        }

        ngx_rtmp_billing_server_handler(bmcf->billingd_check_timer);
        ngx_rtmp_billing_connect_create(ev);
    }

   	if (bmcf->connected) {

        ngx_rtmp_core_main_conf_t       *cmcf = ngx_rtmp_core_main_conf;
        ngx_rtmp_core_srv_conf_t        **pcscf;
		
        ngx_rtmp_billing_session_t      *bs = bmcf->bs;
        ngx_uint_t i, j;
        ngx_rtmp_billing_bandwidth_t    *srv_bd;
		
        pcscf = cmcf->servers.elts;
		srv_bd = bmcf->srv_billing_bd->elts;

        for (i = 0; i < cmcf->servers.nelts; ++i, ++pcscf) {

			ngx_rtmp_billing_server(bmcf->log, *pcscf, bmcf->srv_billing_bd);			
        }
		
		/* 
		 * create one billing item 
		 * example: 1203665832   300	 rtmp://xiaoyu.com   2384234  324234
		*/ 	
	    ngx_chain_t *out = ngx_rtmp_billing_new_chain(bs->pc->connection->pool);

		if (bs->send_buf) {
			
            out->next = bs->send_buf->next;
            bs->send_buf->next = out;
        }
        else {
			
            bs->send_buf = out;
        }

        for (j = 0; j < bmcf->srv_billing_bd->nelts; j++) {
			
           out->buf->last = ngx_snprintf(out->buf->last, out->buf->end - out->buf->last,
                "%l\t%d\t%s%s\t%lu\t%lu\n",
                ngx_time(), bmcf->rtmp_billing_interval / 1000,
                "rtmp://", srv_bd[j].name, srv_bd[j].bd_in.bytes, srv_bd[j].bd_out.bytes);

           if (j == (bmcf->srv_billing_bd->nelts -1)) {

                bs->send_flag = 1;
            }
            ngx_log_error(NGX_LOG_INFO, ev->log, 0, "%l\t%d\t%s%s\t%l\t%l",
                ngx_time(), bmcf->rtmp_billing_interval / 1000,
                "rtmp://", srv_bd[j].name, srv_bd[j].bd_in.bytes, srv_bd[j].bd_out.bytes);
        }
        *(out->buf->last) = 0;

		/*clear array data*/
        for (j = 0; j < bmcf->srv_billing_bd->nelts; j++) {

            ngx_memset(&srv_bd[j].bd_in, 0, sizeof(srv_bd[j].bd_in));
            ngx_memset(&srv_bd[j].bd_out, 0, sizeof(srv_bd[j].bd_out));
        }

        if (bmcf->srv_billing_bd->nelts > 0) {

            ngx_peer_connection_t *pc = bmcf->bs->pc;
            ngx_connection_t *cc = pc->connection;
            ngx_rtmp_billing_send(cc->write);
        }
    }
}

static ngx_int_t
ngx_rtmp_billing_connect_create(ngx_event_t *ev)
{
    ngx_rtmp_billing_main_conf_t   *bmcf = ev->data;
    
    ngx_pool_t                     *pool;
    ngx_peer_connection_t          *pc;
    ngx_connection_t               *cc;
    ngx_rtmp_billing_session_t     *bs;
    ngx_int_t                      rc;

    pool = ngx_create_pool(4096, ev->log);
    if (pool == NULL) {
		
        goto error;
    }
    
    pc = ngx_pcalloc(pool, sizeof(ngx_peer_connection_t));
    if (pc == NULL) {
		
        goto error;
    }

    bs = ngx_pcalloc(pool, sizeof(ngx_rtmp_billing_session_t));
    if (bs == NULL) {
		
        goto error;
    }

    bs->ev = ev;
    bs->timeout = bmcf->rtmp_billing_timeout;
    bs->bufsize = bmcf->rtmp_billing_bufsize;
    bs->url = bmcf->url;
    bs->pc = pc;
    bs->log = ev->log;
    bs->send_flag = 0;
    
    pc->log = ev->log;
    pc->get = ngx_rtmp_billing_get_peer;
    pc->free = ngx_rtmp_billing_free_peer;
    pc->data = bs;

    /* connect */
    rc = ngx_event_connect_peer(pc);
    if (rc != NGX_OK && rc != NGX_AGAIN) {
		
        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, bmcf->log, 0,
                "RTMP billing: connection failed");
        goto error;
    }
    
    cc = pc->connection;
    cc->data = bs;
    cc->pool = pool;
    bs->pc = pc;

    cc->write->handler = ngx_rtmp_billing_send;
    cc->read->handler = ngx_rtmp_billing_recv;

    bmcf->bs = bs;
    bmcf->connected = 1;

    return NGX_OK;
    
error:
    if (pool) {
		
        ngx_destroy_pool(pool);
    }

    ngx_log_error(NGX_LOG_INFO, ev->log, 0, "billing module: connection creat failure");
    bmcf->connected = 0;
	
    return NGX_ERROR;
}

static void
ngx_rtmp_billing_connect_close(ngx_rtmp_billing_session_t *bs)
{
    ngx_pool_t                         *pool;

    ngx_event_t *ev = bs->ev;
    ngx_rtmp_billing_main_conf_t *bmcf = ev->data;
    ngx_peer_connection_t *pc = bs->pc;
    ngx_connection_t *cc = pc->connection;

    if (cc->destroyed) {
		
        return;
    }

    cc->destroyed = 1;
    pool = cc->pool;
    ngx_close_connection(cc);
    ngx_destroy_pool(pool);
    bmcf->bs = NULL;
    bmcf->connected = 0;

	return;
}

static ngx_chain_t* ngx_rtmp_billing_new_chain(ngx_pool_t *pool)
{
    ngx_chain_t                    *al;
    ngx_buf_t                      *b;

    if (!pool) {
		
        return NULL;
    }
    al = ngx_alloc_chain_link(pool);
    if (al == NULL) {
		
        return NULL;
    }
    
    b = ngx_create_temp_buf(pool, NGX_BIG_SENDBUF_SIZE);
    if (b == NULL) {
		
        return NULL;
    }
    al->buf = b;
    al->next = NULL;

    return al;
}


static void
ngx_rtmp_billing_send(ngx_event_t *wev)
{
    ngx_rtmp_billing_session_t         *bs;
    ngx_connection_t                   *cc;
    ngx_chain_t                        *cl;

    cc = wev->data;
    bs = cc->data;

    if (bs->send_flag == 0) {

        return;
    }

    if (cc->destroyed) {
		
        return;
    }

    if (wev->timedout) {
		
        ngx_log_error(NGX_LOG_INFO, cc->log, NGX_ETIMEDOUT,
                "billing module:: send timed out");
        cc->timedout = 1;
        ngx_rtmp_billing_connect_close(bs);
        return;
    }

    if (wev->timer_set) {
		
        ngx_del_timer(wev);
    }

    cl = cc->send_chain(cc, bs->send_buf, 0);

    if (cl == NGX_CHAIN_ERROR) {
		
        ngx_rtmp_billing_connect_close(bs);
        return;
    }

    bs->send_buf = cl;

    /* more data to send? */
    if (cl) {
		
        ngx_add_timer(wev, bs->timeout);
        if (ngx_handle_write_event(wev, 0) != NGX_OK) {
			
            ngx_rtmp_billing_connect_close(bs);
        }
        return;
    }

    /* we've sent everything we had.
     * now receive reply */
    ngx_del_event(wev, NGX_WRITE_EVENT, 0);

    bs->send_flag = 0;
    ngx_rtmp_billing_recv(cc->read);

    if (!bs->send_buf) {
		
        ngx_rtmp_billing_connect_close(bs);
    }
	
	return;
}

static void
ngx_rtmp_billing_recv(ngx_event_t *rev)
{
    return;
}

static ngx_url_t *
ngx_rtmp_billing_parse_url(ngx_event_t *ev)
{
    ngx_rtmp_billing_main_conf_t *bmcf = ev->data;
    ngx_url_t  *u;
    u_char sport[20] = {0};
    
    ngx_sprintf(sport, "%d", bmcf->rtmp_billing_port);
    u = ngx_pcalloc(bmcf->pool, sizeof(ngx_url_t));
    if (u == NULL) {
		
        return NULL;
    }

    u->url.len = ngx_strlen(sport) + ngx_strlen("127.0.0.1:");
    u->url.data = ngx_palloc(bmcf->pool, 1024);
    ngx_memcpy(u->url.data, "127.0.0.1:", ngx_strlen("127.0.0.1:"));
    ngx_memcpy(u->url.data + ngx_strlen("127.0.0.1:"), sport, ngx_strlen(sport));
    u->default_port = DEFAULT_BILLING_PORT;
    u->uri_part = 1;

    if (ngx_parse_url(bmcf->pool, u) != NGX_OK) {
		
        if (u->err) {
			
            ngx_log_error(NGX_LOG_ERR, ev->log, 0,
                "%s in url \"%V\"", u->err, &u->url);
        }
        return NULL;
    }

    return u;
}


static ngx_int_t
ngx_rtmp_billing_get_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_rtmp_billing_session_t   *bs = data;

    pc->sockaddr =(struct sockaddr *)&bs->url->sockaddr;
    pc->socklen = bs->url->socklen;
    pc->name = &bs->url->host;

    return NGX_OK;
}


static void
ngx_rtmp_billing_free_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
	return;
}

static void 
ngx_rtmp_billing_server_handler(ngx_event_t *ev)
{     
    ngx_rtmp_billing_main_conf_t *bmcf = ev->data;

    if (!ngx_rtmp_billingd_isrunning((const char *)bmcf->rtmp_billing_name.data, (const char *)bmcf->rtmp_billing_pid_path.data)) {

		ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "billing module: rtmpbillingd stopped! Now starting it ...");   
		if (ngx_rtmp_billing_start_billingd(bmcf->rtmp_billing_port, (const char *)bmcf->rtmp_billing_name.data, 
            (const char *)bmcf->rtmp_billing_path.data, (const char*)bmcf->rtmp_billing_log_path.data, (const char *)bmcf->rtmp_billing_pid_path.data,
            bmcf->rtmp_billing_interval / 1000) != NGX_OK) {
            
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "billing module: rtmpbillingd start failed...");
        }

    }

    if (!ngx_exiting) { 
		
        ngx_add_timer(ev, (ngx_msec_t)DEFAULT_BILLING_CHECK_TIMER); 
    }
}

static ngx_flag_t 
ngx_rtmp_billingd_isrunning(const char *cmd_name, const char *cmd_pid_path) 
{
    FILE *fd, *procfd;
    char pid[128] = {0};
    char buff[128] = {0};
    int pidnum = -1;
    ngx_flag_t ret = 0; 
	char billid_file[256] ={0};

    sprintf(billid_file, "%s/rtmpbillingd.pid", cmd_pid_path);
    if ((fd = fopen(billid_file, "r")) != NULL) {
		
        fgets(pid, sizeof(pid), fd);
        pidnum = atoi(pid);
        snprintf(buff, sizeof(buff), "/proc/%d/status", pidnum);
        if ((procfd = fopen(buff, "r")) != NULL) {
			
            fgets(pid, sizeof(pid), procfd);
            if (strstr(pid, cmd_name) != NULL) {
				
                memset(pid, 0, 128);
                fgets(pid, sizeof(pid), procfd);
                if (strstr(pid, "sleeping") || strstr(pid, "running")) {

					ret = 1;
				}    
            }   
            fclose(procfd);
        }   
        fclose(fd);
    }

    return ret;
}

static ngx_int_t
ngx_rtmp_billing_start_billingd(int port, const char *cmd_name, const char *cmd_path, const char* log_dir, const char *cmd_pid_path, int billing_interval)
{
    ngx_int_t cid = fork();
    if (cid == 0) {
		
	    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "billing module: start to create billing process: %ui", getpid());
       // close the listen port fd
        ngx_uint_t i;       
        u_char pport[128] = {0};
        u_char pinterval[128] = {0};
        ngx_listening_t  * ls = ngx_cycle->listening.elts;
        for (i = 0; i < ngx_cycle->listening.nelts; i++) {
			
            if (ls[i].fd != (ngx_socket_t) -1) {
				
                ngx_close_file(ls[i].fd);
                ls[i].fd = -1;
            }
        }
        *ngx_snprintf(pport, sizeof(pport), "%d", port) = 0;
        *ngx_snprintf(pinterval, sizeof(pinterval), "%d", billing_interval) = 0;
        execl(cmd_path, cmd_name, "-c", pport, pinterval, log_dir, cmd_pid_path, (char *)0);
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "billing module: execl rtmpbillingd:[%s]  failed...", cmd_path);
        exit(-1);
    }else if (cid < 0) {

		return  NGX_ERROR;
    } 
	
    return  NGX_OK;
}

void 
ngx_rtmp_billing_get_arg(ngx_cycle_t *cycle)
{
    char *p = NULL;
    ngx_rtmp_billing_main_conf_t *bmcf = ngx_rtmp_cycle_get_module_main_conf(cycle, ngx_rtmp_billing_module);
    ngx_rtmp_core_main_conf_t *cmcf = ngx_rtmp_cycle_get_module_main_conf(cycle, ngx_rtmp_core_module);

    if (cmcf->rtmp_billing_port && cmcf->rtmp_billing_port != NGX_CONF_UNSET_UINT) {
		
        bmcf->rtmp_billing_port = cmcf->rtmp_billing_port;
    } else {
		
        bmcf->rtmp_billing_port = DEFAULT_BILLING_PORT;
    }

    if (cmcf->rtmp_billing_interval && cmcf->rtmp_billing_interval != NGX_CONF_UNSET_UINT) {
		
        bmcf->rtmp_billing_interval = cmcf->rtmp_billing_interval;
    } else {
		
        bmcf->rtmp_billing_interval = DEFAULT_BILLING_INTERVAL * 1000;
    }

    bmcf->rtmp_billing = cmcf->rtmp_billing;

    if (cmcf->rtmp_billing_path.len) {
		
        bmcf->rtmp_billing_path.len = cmcf->rtmp_billing_path.len + 1;
        bmcf->rtmp_billing_path.data = ngx_palloc(cycle->pool, bmcf->rtmp_billing_path.len);
        ngx_memcpy(bmcf->rtmp_billing_path.data, cmcf->rtmp_billing_path.data, cmcf->rtmp_billing_path.len);
        bmcf->rtmp_billing_path.data[cmcf->rtmp_billing_path.len] = 0;
    } else {
		
        bmcf->rtmp_billing_path.len = ngx_strlen(DEFAULT_RTMP_BILLINGD_PATH) + 1;
        bmcf->rtmp_billing_path.data = ngx_palloc(cycle->pool, bmcf->rtmp_billing_path.len);
        ngx_memcpy(bmcf->rtmp_billing_path.data, DEFAULT_RTMP_BILLINGD_PATH, ngx_strlen(DEFAULT_RTMP_BILLINGD_PATH));
        bmcf->rtmp_billing_path.data[ngx_strlen(DEFAULT_RTMP_BILLINGD_PATH)] = 0;
    }
    
    if (cmcf->rtmp_billing_pid_path.len) {
		
        bmcf->rtmp_billing_pid_path.len = cmcf->rtmp_billing_pid_path.len + 1;
        bmcf->rtmp_billing_pid_path.data = ngx_palloc(cycle->pool, bmcf->rtmp_billing_pid_path.len);
        ngx_memcpy(bmcf->rtmp_billing_pid_path.data, cmcf->rtmp_billing_pid_path.data, cmcf->rtmp_billing_pid_path.len);
        bmcf->rtmp_billing_pid_path.data[cmcf->rtmp_billing_pid_path.len] = 0;
    } else {
		
        bmcf->rtmp_billing_pid_path.len = ngx_strlen(DEFAULT_RTMP_BILLINGD_PID_PATH) + 1;
        bmcf->rtmp_billing_pid_path.data = ngx_palloc(cycle->pool, bmcf->rtmp_billing_pid_path.len);
        ngx_memcpy(bmcf->rtmp_billing_pid_path.data, DEFAULT_RTMP_BILLINGD_PID_PATH, ngx_strlen(DEFAULT_RTMP_BILLINGD_PID_PATH));
        bmcf->rtmp_billing_pid_path.data[ngx_strlen(DEFAULT_RTMP_BILLINGD_PID_PATH)] = 0;
    }

    if (cmcf->rtmp_billing_log_path.len) {
		
        bmcf->rtmp_billing_log_path.len = cmcf->rtmp_billing_log_path.len + 1;
        bmcf->rtmp_billing_log_path.data = ngx_palloc(cycle->pool, bmcf->rtmp_billing_log_path.len);
        ngx_memcpy(bmcf->rtmp_billing_log_path.data, cmcf->rtmp_billing_log_path.data, cmcf->rtmp_billing_log_path.len);
        bmcf->rtmp_billing_log_path.data[cmcf->rtmp_billing_log_path.len] = 0;
    } else {
		
        bmcf->rtmp_billing_log_path.len = ngx_strlen(DEFAULT_RTMP_BILLINGD_LOG_PATH) + 1;
        bmcf->rtmp_billing_log_path.data = ngx_palloc(cycle->pool, bmcf->rtmp_billing_log_path.len);
        ngx_memcpy(bmcf->rtmp_billing_log_path.data, DEFAULT_RTMP_BILLINGD_LOG_PATH, ngx_strlen(DEFAULT_RTMP_BILLINGD_PID_PATH));
        bmcf->rtmp_billing_log_path.data[ngx_strlen(DEFAULT_RTMP_BILLINGD_LOG_PATH)] = 0;
    }

    bmcf->rtmp_billing_timeout = cmcf->rtmp_billing_timeout;
    bmcf->rtmp_billing_bufsize = cmcf->rtmp_billing_bufsize;

    p = strrchr((const char *)bmcf->rtmp_billing_path.data, '/');
    if (!p) {
		
        p = DEFAULT_BILLINGD_NAME;
    } else {
		
        p++; // pass '/'
    }
    
    bmcf->rtmp_billing_name.len = ngx_strlen(p) + 1;
    bmcf->rtmp_billing_name.data = ngx_palloc(cycle->pool, bmcf->rtmp_billing_name.len);
    ngx_memcpy(bmcf->rtmp_billing_name.data, p, ngx_strlen(p));
    bmcf->rtmp_billing_name.data[ngx_strlen(p)] = 0;

	return;
}


static ngx_int_t ngx_rtmp_billing_getpid(const char *cmd_pid_path)
{
    FILE *fd = NULL;
    char pid[128] = {0};
    int pidnum = -1; 

    if ((fd = fopen(cmd_pid_path, "r")) != NULL) {
		
		fread(pid,100,1,fd);
        pidnum = atoi(pid);  
        fclose(fd);
    }  
	
	return pidnum;
}

static void
ngx_rtmp_billing_server(ngx_log_t *log, ngx_rtmp_core_srv_conf_t *cscf, ngx_array_t *bd)
{
	ngx_rtmp_core_app_conf_t      **cacf;
	ngx_uint_t					  n;

	cacf = cscf->applications.elts;
	for (n = 0; n < cscf->applications.nelts; ++n, ++cacf) {
		
		ngx_rtmp_billing_application(log, *cacf, bd);
    }
	
	return;
}

static void
ngx_rtmp_billing_application(ngx_log_t *log, ngx_rtmp_core_app_conf_t *cacf, ngx_array_t *bd)
{
    ngx_rtmp_billing_live(log, cacf->app_conf[ngx_rtmp_live_module.ctx_index], bd);
	return;
}


static void
ngx_rtmp_billing_live(ngx_log_t *log, ngx_rtmp_live_app_conf_t *lacf, ngx_array_t *bd)
{
	ngx_uint_t  				   m, mark;
	ngx_int_t                      n;
	ngx_rtmp_live_stream_t         *stream;
	ngx_rtmp_session_t             *s;
	ngx_rtmp_live_ctx_t            *ctx;
	ngx_rtmp_billing_bandwidth_t   *p_bd, *p_curbd;
	u_char 						   name[256];
	
    if (!lacf->live) {
        return;
    }

	for (n = 0; n < lacf->nbuckets; ++n) {

		 for (stream = lacf->streams[n]; stream; stream = stream->next) {
		 	
         	ctx = stream->ctx;	
			s = ctx->session;
			ngx_log_error(NGX_LOG_INFO, log, 0, "billing module: srv_name is: %V, in is: %l, out is: %l", &s->host_in, stream->bw_billing_in.bytes,
		 	stream->bw_billing_out.bytes);
			if (bd->nelts == 0) {

				p_bd = ngx_array_push(bd);
				p_bd->bd_in = stream->bw_billing_in;
			    p_bd->bd_out = stream->bw_billing_out;
				ngx_memset(p_bd->name, 0, 128);
				*ngx_snprintf(p_bd->name, 128, "%V:%V", &s->host_in, &s->port_in)= 0;
				ngx_memset(&stream->bw_billing_in, 0, sizeof(stream->bw_billing_in));
				ngx_memset(&stream->bw_billing_out, 0, sizeof(stream->bw_billing_out));
			} else {

				p_curbd = bd->elts;
				mark = 0;
				for (m = 0; m < bd->nelts; m++) {

					*ngx_snprintf(name, 256, "%V:%V", &s->host_in, &s->port_in) = 0;
					if (ngx_strncmp((char *)name/*s->host_in.data*/, (char *)p_curbd[m].name, ngx_strlen(name)) == 0) {

						p_curbd[m].bd_in.bytes += stream->bw_billing_in.bytes;
						p_curbd[m].bd_out.bytes += stream->bw_billing_out.bytes;
					    ngx_memset(&stream->bw_billing_in, 0, sizeof(stream->bw_billing_in));
				        ngx_memset(&stream->bw_billing_out, 0, sizeof(stream->bw_billing_out));
						mark =1;
					}
				}
				if (mark == 0) {
					
					p_bd = ngx_array_push(bd);
					p_bd->bd_in = stream->bw_billing_in;
				    p_bd->bd_out = stream->bw_billing_out;
					ngx_memset(p_bd->name, 0, 128);
					*ngx_snprintf(p_bd->name, 128, "%V:%V", &s->host_in, &s->port_in)= 0;
					ngx_memset(&stream->bw_billing_in, 0, sizeof(stream->bw_billing_in));
					ngx_memset(&stream->bw_billing_out, 0, sizeof(stream->bw_billing_out));
				}
			}
		 }
	}
	
	return;
}

