// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Huawei Technologies Co., Ltd. All Rights Reserved.

#ifndef PROXY_OPT_SCHE_H
#define PROXY_OPT_SCHE_H

#ifdef ENABLE_OPT_LOG
#define LogFile ngx_log_error
#else
#define LogFile(...) ((void)sizeof((_Bool[]){__VA_ARGS__})) 
#endif

#define PREFILL_SHM_NAME "opt_prefill_shared_memory"
#define NUM_PREFILL_SHARED_MEMORY_BLOCKS 64

static ngx_shm_zone_t *prefill_shm_zone = NULL;
static ngx_uint_t      prefill_shm_zone_size = (128 * 1024);
static ngx_uint_t      prefill_upstream_count = 0;
static ngx_uint_t      prefill_batch_size = 0;

typedef struct {
    ngx_uint_t  total_length_sum;
    ngx_uint_t  total_request_sum;
} prefill_server_data_t;

typedef struct {
    ngx_uint_t  scheduler_num;
    ngx_uint_t  worker_num;
    ngx_uint_t  batch_size;
    ngx_uint_t  num_shm_blocks;
    prefill_server_data_t server[1];
} prefill_upstream_into_t;

typedef struct {
    ngx_http_upstream_rr_peer_data_t  *rrp;
    ngx_uint_t                         chosen;
} prefill_peer_data_t;

static ngx_int_t get_upstream_count(const char*upstream_name, ngx_conf_t *cf, ngx_int_t *ups_count) {

    ngx_http_upstream_main_conf_t  *umcf = NULL;
    ngx_http_upstream_srv_conf_t  *uscf = NULL, **uscfp;
    ngx_array_t *servers = NULL;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    if (umcf == NULL) {
        LogFile(NGX_LOG_ERR, cf->log, 0, "Failed to get upstream main conf");
        return NGX_ERROR;
    }
    uscfp = umcf->upstreams.elts;
    for (ngx_int_t i = 0; i < umcf->upstreams.nelts; i++) {
        uscf = uscfp[i];
        if (uscf == NULL) { 
            continue;
        }
        if (ngx_strcmp(uscf->host.data, (u_char *)upstream_name) == 0) {
            servers = uscf->servers;
            break;
        }
    }
    if (servers != NULL) {
        LogFile(NGX_LOG_EMERG, cf->log, 0, "Upstream has %ui servers\n", servers->nelts);
        *ups_count = servers->nelts;
    }
    return NGX_OK;
}


#endif
