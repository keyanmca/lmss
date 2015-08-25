
/*
 * Copyright (C) Gino Hu
 */


#ifndef _NGX_RTMP_HLS_H_INCLUDED_
#define _NGX_RTMP_HLS_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_rtmp_cmd_module.h>
#include <ngx_rtmp_relay_module.h>
#include <ngx_rtmp_netcall_module.h>
#include "ngx_rtmp.h"

typedef struct ngx_rtmp_hls_ctx_s ngx_rtmp_hls_ctx_t;
typedef struct ngx_rtmp_hls_stream_s ngx_rtmp_hls_stream_t;
typedef struct ngx_rtmp_hls_session_s ngx_rtmp_hls_session_t;


typedef struct {
    uint64_t                            id;
    double                              duration;
    unsigned                            active:1;
    unsigned                            discont:1; /* before */
} ngx_rtmp_hls_frag_t;


typedef struct {
    ngx_str_t                           suffix;
    ngx_array_t                         args;
} ngx_rtmp_hls_variant_t;


struct ngx_rtmp_hls_ctx_s {
    ngx_rtmp_session_t                 *session;
    ngx_rtmp_hls_ctx_t                 *next;
	ngx_rtmp_hls_stream_t              *hls_stream;
	ngx_rtmp_hls_session_t             *hls_session;

	unsigned                            publisher:1;

    unsigned                            opened:1;
    unsigned                            sliced:1;

    ngx_file_t                          file;

	ngx_str_t                           relay_url;
	ngx_str_t                           relay_uri;
	ngx_str_t                           relay_vhost;
	ngx_str_t                           relay_host;
    ngx_str_t                           playlist;
    ngx_str_t                           playlist_bak;
    ngx_str_t                           var_playlist;
    ngx_str_t                           var_playlist_bak;
    ngx_str_t                           stream;
    ngx_str_t                           name;
    ngx_str_t                           serv_name;

    uint64_t                            frag;
    uint64_t                            frag_ts;
    ngx_uint_t                          nfrags;
    ngx_rtmp_hls_frag_t                *frags; /* circular 2 * winfrags + 1 */

    ngx_uint_t                          audio_cc;
    ngx_uint_t                          video_cc;

    uint64_t                            aframe_base;
    uint64_t                            aframe_num;

    ngx_buf_t                          *aframe;
    uint64_t                            aframe_pts;

    ngx_rtmp_hls_variant_t             *var;
    uint32_t                            base_timestamp;
    unsigned                            is_first_packge:1;
    ngx_event_t                         retry_evt;
    ngx_msec_t                          retry_evt_msec;
    ngx_int_t                           retry_times;
};


struct ngx_rtmp_hls_stream_s {
    u_char                              name[NGX_RTMP_MAX_NAME];
    ngx_rtmp_hls_stream_t              *next;
    ngx_rtmp_hls_ctx_t                 *ctx;
};


struct ngx_rtmp_hls_session_s {
    ngx_chain_t                        *out;
    ngx_msec_t                          timeout;
};


typedef struct {
	ngx_int_t                           nbuckets;
    ngx_rtmp_hls_stream_t             **hls_streams;
    ngx_pool_t                         *pool;
	ngx_rtmp_hls_stream_t              *free_hls_streams;
    ngx_flag_t                          hls;
    ngx_msec_t                          fraglen;
    ngx_msec_t                          max_fraglen;
    ngx_msec_t                          muxdelay;
    ngx_msec_t                          sync;
    ngx_msec_t                          playlen;
    ngx_uint_t                          winfrags;
    ngx_flag_t                          continuous;
    ngx_flag_t                          nested;
    ngx_flag_t                          slicing_way;  //0: according to key frame, 1:according to timestamp, default:0
    ngx_str_t                           path;
    ngx_uint_t                          naming;
    ngx_uint_t                          slicing;
    ngx_uint_t                          type;
    ngx_path_t                         *slot;
    ngx_msec_t                          max_audio_delay;
    size_t                              audio_buffer_size;
    ngx_flag_t                          cleanup;
    ngx_array_t                        *variant;
    ngx_str_t                           base_url;
    ngx_int_t                           granularity;
} ngx_rtmp_hls_app_conf_t;


#endif /* _NGX_RTMP_HLS_H_INCLUDED_ */

