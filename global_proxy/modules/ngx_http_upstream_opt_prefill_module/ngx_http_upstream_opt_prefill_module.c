// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Huawei Technologies Co., Ltd. All Rights Reserved.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_upstream.h>
#include <math.h>
#include "opt_sche.h"

#define PREFILL_UPSTREAM_NAME "prefill_servers"

typedef struct {
    ngx_flag_t  enable;
    ngx_uint_t  batch_size;
} upstream_opt_prefill_conf_t;

static ngx_command_t  ngx_http_prefill_commands[] = {
    { ngx_string("opt_prefill"),
    NGX_HTTP_UPS_CONF|NGX_CONF_FLAG,
    ngx_conf_set_flag_slot,
    NGX_HTTP_SRV_CONF_OFFSET,
    offsetof(upstream_opt_prefill_conf_t, enable),
    NULL },

    { ngx_string("batch_size_prefill"),
    NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_SRV_CONF_OFFSET,
    offsetof(upstream_opt_prefill_conf_t, batch_size),
    NULL },

    ngx_null_command
};

static void *ngx_http_prefill_create_srv_conf(ngx_conf_t *cf);
static char *ngx_http_prefill_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_http_prefill_postconfig(ngx_conf_t *cf);
static ngx_int_t ngx_http_prefill_upstream_init(ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *uscf);
static ngx_int_t ngx_http_prefill_get_peer(ngx_peer_connection_t *pc, void *data);
static void      ngx_http_prefill_free_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state);

static ngx_http_module_t  ngx_http_prefill_module_ctx = {
    NULL,
    ngx_http_prefill_postconfig,
    NULL, NULL,
    ngx_http_prefill_create_srv_conf,
    ngx_http_prefill_merge_srv_conf,
    NULL, NULL
};

ngx_module_t  ngx_http_upstream_opt_prefill_module = {
    NGX_MODULE_V1,
    &ngx_http_prefill_module_ctx,
    ngx_http_prefill_commands,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

static void * ngx_http_prefill_create_srv_conf(ngx_conf_t *cf) {
    upstream_opt_prefill_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) {
        return NULL;
    }
    conf->enable  = NGX_CONF_UNSET;
    conf->batch_size  = NGX_CONF_UNSET_UINT;
    return conf;
}
static char * ngx_http_prefill_merge_srv_conf(ngx_conf_t *cf, void *parent, void *child) {
    upstream_opt_prefill_conf_t *prev = parent;
    upstream_opt_prefill_conf_t *conf = child;
    ngx_conf_merge_value(conf->enable,  prev->enable,  0);
    ngx_conf_merge_uint_value(conf->batch_size, prev->batch_size, 5);
    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_prefill_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data) {
    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    ngx_slab_pool_t *pool_ptr = NULL;
    pool_ptr = (ngx_slab_pool_t *) shm_zone->shm.addr;
    LogFile(NGX_LOG_EMERG, shm_zone->shm.log, 0, "\n@opt_prefill call: init shm zone = %p, %p\n", pool_ptr, shm_zone);

    prefill_upstream_info_t *shm_block = NULL;
    ngx_uint_t num_prefill_shm_blocks = NUM_PREFILL_SHARED_MEMORY_BLOCKS;
    size_t size_of_shared_memory = sizeof(prefill_upstream_info_t) + (num_prefill_shm_blocks - 1) * sizeof(prefill_server_data_t);
    shm_block = ngx_slab_alloc(pool_ptr, size_of_shared_memory);
    if (shm_block == NULL) {
        return NGX_ERROR;
    }
    LogFile(NGX_LOG_EMERG, shm_zone->shm.log, 0, "\n@opt_prefill call: shm blocks %ui\n", num_prefill_shm_blocks);

    shm_block->num_shm_blocks = num_prefill_shm_blocks;
    shm_block->scheduler_num = 1;
    shm_block->worker_num = prefill_upstream_count;
    shm_block->batch_size = 16;
    for (ngx_uint_t i = 0; i < num_prefill_shm_blocks; ++i) {
        shm_block->server[i].total_length_sum = 0;
        shm_block->server[i].total_request_sum = 0;
    }
    shm_zone->data = shm_block;
    return NGX_OK;
}

static ngx_int_t ngx_http_prefill_postconfig(ngx_conf_t *cf) {
    const char*upstream_name = PREFILL_UPSTREAM_NAME;
    get_upstream_count(upstream_name, cf, &prefill_upstream_count);
    ngx_str_t shm_name = ngx_string(PREFILL_SHM_NAME);
    prefill_shm_zone = ngx_shared_memory_add(cf, &shm_name, prefill_shm_zone_size, NULL);
    if (prefill_shm_zone == NULL) {
        return NGX_ERROR;
    }

    ngx_http_upstream_main_conf_t  *upcf;
    ngx_http_upstream_srv_conf_t  **uscfp;
    upstream_opt_prefill_conf_t    *conf;
    ngx_uint_t                      i;

    upcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    if (upcf == NULL) return NGX_OK;
    uscfp = upcf->upstreams.elts;
    for (i = 0; i < upcf->upstreams.nelts; ++i) {
        conf = ngx_http_conf_upstream_srv_conf(uscfp[i],
            ngx_http_upstream_opt_prefill_module);
        if (conf->enable == 1) {
            uscfp[i]->peer.init = ngx_http_prefill_upstream_init;
            prefill_batch_size = conf->batch_size;
        }
    }
    if (prefill_batch_size == 0) {
        prefill_batch_size = 16;
    }
    LogFile(NGX_LOG_EMERG, cf->log, 0, "\n@opt_prefill call: %p, prefill batch_size = %ui\n", prefill_shm_zone, prefill_batch_size);
    prefill_shm_zone->init = ngx_http_prefill_init_shm_zone;

    return NGX_OK;
}

static ngx_int_t select_prefill_solver(prefill_upstream_info_t *prefill_shm, ngx_uint_t req_length, ngx_uint_t *chosen) {
    ngx_uint_t worker_num = ngx_atomic_fetch_add(&(prefill_shm->worker_num), 0);
    ngx_uint_t scheduler_num = ngx_atomic_fetch_add(&(prefill_shm->scheduler_num), 0);
    ngx_uint_t count = 0;
    double min_score = 0;
    const ngx_uint_t max_tie = worker_num;
    ngx_uint_t min_peers[max_tie];

    for (ngx_uint_t i = 0; i < worker_num; ++i) {
        ngx_uint_t length_sum_workers = 0;
        ngx_uint_t request_sum_workers = 0;
        for (ngx_uint_t rank = 0; rank < scheduler_num; ++rank) {
            ngx_atomic_t r_length = ngx_atomic_fetch_add(&(prefill_shm->server[rank * worker_num + i].total_length_sum), 0);
            ngx_atomic_t r_count = ngx_atomic_fetch_add(&(prefill_shm->server[rank * worker_num + i].total_request_sum), 0);
            length_sum_workers += r_length;
            request_sum_workers += r_count;
        }
        double score = length_sum_workers * ( (ceil(request_sum_workers / (1.0 * prefill_batch_size)) + 1.0) / 2.0);
        if (count == 0 || score < min_score) {
            min_score = score;
            count = 0;
            min_peers[count++] = i;
        } else if (score == min_score) {
            if (count < max_tie) {
                min_peers[count++] = i;
            }
        }
    }
    ngx_uint_t rand_idx = ngx_random() % count;
    *chosen = min_peers[rand_idx];

    ngx_uint_t worker_id = *chosen;
    ngx_atomic_fetch_add(&(prefill_shm->server[worker_id].total_length_sum), req_length);
    ngx_atomic_fetch_add(&(prefill_shm->server[worker_id].total_request_sum), 1);
}

static ngx_int_t ngx_http_prefill_upstream_init(ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *uscf) {
    ngx_http_upstream_t              *u = r->upstream;
    ngx_http_upstream_rr_peer_data_t *rrp = NULL;
    ngx_uint_t                        chosen = 0;

    if (ngx_http_upstream_init_round_robin_peer(r, uscf) != NGX_OK)
        return NGX_ERROR;
    rrp = u->peer.data;

    prefill_upstream_info_t *prefill_shm = prefill_shm_zone->data;

    if (ngx_atomic_fetch_add(&(prefill_shm->worker_num), 0) == 0) {
        ngx_uint_t  n = rrp->peers->number;
        if (n > prefill_shm->num_shm_blocks) {
            n = prefill_shm->num_shm_blocks;
        } 
        ngx_atomic_fetch_add(&(prefill_shm->worker_num), n);
    }
    select_prefill_solver(prefill_shm, r->request_length, &chosen);
    { // print
        ngx_uint_t worker_num = ngx_atomic_fetch_add(&(prefill_shm->worker_num), 0);
        LogFile(NGX_LOG_EMERG, r->connection->log, 0, "\n@opt_prefill* select: chosen = %ui, worker_num = %ui\n", chosen, worker_num);

        for (ngx_uint_t i = 0; i < worker_num; ++i) {
            LogFile(NGX_LOG_EMERG, r->connection->log, 0, "\n@opt_prefill* select: i = %ui, total_length_sum = %ui, total_request_sum = %ui\n", 
                i,
                ngx_atomic_fetch_add(&(prefill_shm->server[i].total_length_sum), 0),
                ngx_atomic_fetch_add(&(prefill_shm->server[i].total_request_sum), 0)
            );
        }
    }

    ngx_http_prefill_peer_data_t *prefill_peer_data = NULL;
    prefill_peer_data = ngx_palloc(r->pool, sizeof(*prefill_peer_data));
    prefill_peer_data->rrp    = rrp;
    prefill_peer_data->chosen = chosen;
    u->peer.data  = prefill_peer_data;
    u->peer.get   = ngx_http_prefill_get_peer;
    u->peer.free  = ngx_http_prefill_free_peer;

    struct sockaddr_in *sin = (struct sockaddr_in *)rrp->peers->peer[chosen].sockaddr;
    ngx_uint_t port = ntohs(sin->sin_port);

    LogFile(NGX_LOG_WARN, r->connection->log, 0,
        "[OPT_PREFILL Scheduler]: request assigned to port=%ui, request_length=%O",
        port, r->request_length);

    return NGX_OK;
}

static ngx_int_t
ngx_http_prefill_get_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_prefill_peer_data_t     *prefill_peer_data = data;
    ngx_http_upstream_rr_peer_data_t *rrp   = prefill_peer_data->rrp;
    ngx_http_upstream_rr_peers_t     *peers = rrp->peers;
    ngx_uint_t                       idx   = prefill_peer_data->chosen;

    if (idx >= peers->number)
        return ngx_http_upstream_get_round_robin_peer(pc, rrp);

    if (peers->peer[idx].down)
        return NGX_BUSY;

    pc->sockaddr = peers->peer[idx].sockaddr;
    pc->socklen  = peers->peer[idx].socklen;
    pc->name     = &peers->peer[idx].name;
    rrp->current = &peers->peer[idx];
    LogFile(NGX_LOG_DEBUG_HTTP, pc->log, 0,
        "@opt_prefill: get_peer chosen=%ui addr=\"%V\"", idx, pc->name);
    return NGX_OK;
}

static void
ngx_http_prefill_free_peer(ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
    ngx_http_prefill_peer_data_t     *prefill_peer_data = data;
    ngx_http_upstream_rr_peer_data_t *rrp   = prefill_peer_data->rrp;
    ngx_http_upstream_free_round_robin_peer(pc, rrp, state);
}