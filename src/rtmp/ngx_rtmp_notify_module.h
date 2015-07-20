
/*
 * Copyright (C) Roman Arutyunyan
 */


#ifndef _NGX_RTMP_NOTIFY_H_INCLUDED_
#define _NGX_RTMP_NOTIFY_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_rtmp.h"
#include "json-c/json.h"


ngx_int_t ngx_rtmp_notify_play1(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v);


#endif /* _NGX_RTMP_NOTIFY_H_INCLUDED_ */
