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
static ngx_shm_zone_t *prefill_shm_zone = NULL;
static ngx_uint_t      prefill_shm_zone_size = (128 * 1024);

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

static ngx_int_t add_opt_prefill_shared_memory_addr(ngx_conf_t *cf) {

    if (prefill_shm_zone != NULL) {
        return NGX_OK;
    }
    ngx_str_t shm_name = ngx_string(PREFILL_SHM_NAME);
    LogFile(NGX_LOG_EMERG, cf->log, 0, "\n@P* call:add prefill shared memory size = %ui\n", prefill_shm_zone_size);

    prefill_shm_zone = ngx_shared_memory_add(cf, &shm_name, prefill_shm_zone_size, NULL);
    if (prefill_shm_zone == NULL) {
        LogFile(NGX_LOG_EMERG, cf->log, 0, "\n@P* error: add shared memory\n");
        return NGX_ERROR;
    }
    return NGX_OK;
}

static ngx_int_t update_at_prefill_finished(ngx_shm_zone_t *shm_zone, ngx_http_request_t *r) {
    if (shm_zone == NULL || shm_zone->shm.addr == NULL) {
        LogFile(NGX_LOG_EMERG, r->connection->log, 0, "\n@P error: prefill done, get global memory failed, NULL pointer\n");
    }
    prefill_upstream_into_t *shm_block = shm_zone->data;
    prefill_peer_data_t *prefill_peer_data = NULL;
    prefill_peer_data = r->upstream->peer.data;
    if (shm_block == NULL || prefill_peer_data == NULL) {
        LogFile(NGX_LOG_EMERG, r->connection->log, 0, "\n@P error: prefill done, peer NULL pointer\n");
        return NGX_ERROR;
    }

    ngx_uint_t worker_num = ngx_atomic_fetch_add(&(prefill_shm->worker_num), 0);
    ngx_uint_t worker_id = prefill_peer_data->chosen;
    ngx_atomic_fetch_add(&(prefill_shm->server[worker_id].total_length_sum), -(r->request_length));
    ngx_atomic_fetch_add(&(prefill_shm->server[worker_id].total_request_sum), -1);
    { // print
        for (ngx_uint_t i = 0; i < worker_num; ++i) {
            LogFile(NGX_LOG_EMERG, r->connection->log, 0, "\n@P* prefill done: i = %ui, total_length_sum = %ui, total_request_sum = %ui\n", 
                i,
                ngx_atomic_fetch_add(&(prefill_shm->server[i].total_length_sum), 0),
                ngx_atomic_fetch_add(&(prefill_shm->server[i].total_request_sum), 0)
            );
        }
    }
    return NGX_OK;
}

#endif
