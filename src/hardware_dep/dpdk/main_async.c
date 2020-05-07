// Copyright 2019 Eotvos Lorand University, Budapest, Hungary
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dpdk_lib.h"
#include "gen_include.h"
#include "dpdkx_crypto.h"

// -----------------------------------------------------------------------------
// GLOBALS

struct rte_mempool *context_pool;
struct rte_mempool *async_pool;
struct rte_ring    *context_buffer;

// -----------------------------------------------------------------------------
// INTERFACE

void async_init_storage();
void async_handle_packet(struct lcore_data* lcdata, packet_descriptor_t* pd, unsigned pkt_idx, uint32_t port_id, void (*handler_function)(void));
void main_loop_async(struct lcore_data* lcdata, packet_descriptor_t* pd);
void main_loop_fake_crypto(struct lcore_data* lcdata);
void do_async_op(packet_descriptor_t* pd, enum async_op_type op);
void do_blocking_sync_op(packet_descriptor_t* pd, enum async_op_type op);

// -----------------------------------------------------------------------------
// DEBUG

#define DBG_CONTEXT_SWAP_TO_MAIN        debug(T4LIT(Swapping to main context...,warning) "\n");
#define DBG_CONTEXT_SWAP_TO_PACKET(ctx) debug(T4LIT(Swapping to packet context (%p)...,warning) "\n", ctx);

// -----------------------------------------------------------------------------
// EXTERNS

extern struct lcore_conf   lcore_conf[RTE_MAX_LCORE];

// defined in dataplane.c
void init_headers(packet_descriptor_t* pd, lookup_table_t** tables);
void reset_headers(packet_descriptor_t* pd, lookup_table_t** tables);
void parse_packet(packet_descriptor_t* pd, lookup_table_t** tables, parser_state_t* pstate);
void emit_packet(packet_descriptor_t* pd, lookup_table_t** tables, parser_state_t* pstate);
void control_DeparserImpl(packet_descriptor_t* pd, lookup_table_t** tables, parser_state_t* pstate);
extern void free_packet(packet_descriptor_t* pd);

// -----------------------------------------------------------------------------
// SERIALIZATION AND DESERIALIZATION

static void reset_pd(packet_descriptor_t *pd)
{
    pd->dropped=0;
    pd->parsed_length = 0;
    pd->payload_length = rte_pktmbuf_pkt_len(pd->wrapper) - pd->parsed_length;
    pd->emit_hdrinst_count = 0;
    pd->is_emit_reordering = false;
}

static void resume_packet_handling(struct rte_mbuf *mbuf, struct lcore_data* lcdata, packet_descriptor_t *pd)
{
    debug_mbuf(mbuf, "Data after async function: ");

    // Extracting extra content from the mbuf

    int packet_length = *(rte_pktmbuf_mtod(mbuf, uint32_t*));
    rte_pktmbuf_adj(mbuf, sizeof(uint32_t));
    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        void* context = *(rte_pktmbuf_mtod(mbuf, void**));
        rte_pktmbuf_adj(mbuf, sizeof(void*));

        // Resetting the pd
        init_headers(pd, 0);
        reset_headers(pd, 0);
        reset_pd(pd);
    #endif

    pd->wrapper = mbuf;
    pd->data = rte_pktmbuf_mtod(pd->wrapper, uint8_t*);

    pd->wrapper->pkt_len = packet_length;

    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        pd->context = context;

        DBG_CONTEXT_SWAP_TO_PACKET(context)
        swapcontext(&lcdata->conf->main_loop_context, context);
        debug("Swapped back to main context.\n");
    #endif
}


void create_crypto_op(struct async_op **op_out, packet_descriptor_t* pd, enum async_op_type op_type, void* extraInformationForAsyncHandling){
    unsigned encryption_offset = 0;//14; // TODO

    int ret = rte_mempool_get(async_pool, (void**)op_out);
    if(ret < 0){
        rte_exit(EXIT_FAILURE, "Mempool get failed!\n");
        //TODO: it should be a packet drop, not total fail
    }
    struct async_op *op = *op_out;
    op->op = op_type;
    op->data = pd->wrapper;

    uint32_t packet_length = op->data->pkt_len;
    int encrypted_length = packet_length - encryption_offset;
    int extra_length = 0;
    debug_mbuf(op->data, "Packet: ");

    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        if(extraInformationForAsyncHandling != NULL){
            void* context = extraInformationForAsyncHandling;
            rte_pktmbuf_prepend(op->data, sizeof(void*));
            *(rte_pktmbuf_mtod(op->data, void**)) = context;
            extra_length += sizeof(void*);
        }
    #endif

    rte_pktmbuf_prepend(op->data, sizeof(uint32_t));
    *(rte_pktmbuf_mtod(op->data, uint32_t*)) = packet_length;
    extra_length += sizeof(int);

    debug_mbuf(op->data, "Prepared for encryption (added extra informations):");

    op->offset = extra_length + encryption_offset;
    debug("encr_len%d %d\n",encrypted_length,packet_length)
    // This is extremely important, believe me. The pkt_len has to be a multiple of the cipher block size, otherwise the crypto device won't do the operation on the mbuf.
    if(encrypted_length%16 != 0) rte_pktmbuf_append(op->data, 16-encrypted_length%16);
}

void enqueue_packet_for_async(packet_descriptor_t* pd, enum async_op_type op_type, void* extraInformationForAsyncHandling)
{
    struct async_op *op;
    create_crypto_op(&op,pd,op_type,extraInformationForAsyncHandling);

    rte_ring_enqueue(lcore_conf[rte_lcore_id()].async_queue, op);
    debug_mbuf(op->data, "Enqueued for async");
}

// -----------------------------------------------------------------------------
// CALLBACKS

#ifdef DEBUG__COUNT_CONTEXT_MISSING_CAUSED_PACKET_DROP
int debugContextMissingCausedDroppedPacketCounter[RTE_MAX_LCORE];
uint64_t debugContextMissingCausedStartCycleCount[RTE_MAX_LCORE];
pthread_mutex_t contextMissingMutex;
#endif

void async_init_storage()
{
    context_pool = rte_mempool_create("context_pool", (unsigned)1023, sizeof(ucontext_t) + CONTEXT_STACKSIZE, MEMPOOL_CACHE_SIZE, 0, NULL, NULL, NULL, NULL, 0, 0);
    if (context_pool == NULL) rte_exit(EXIT_FAILURE, "Cannot create context pool\n");
    async_pool = rte_mempool_create("async_pool", (unsigned)1024*1024-1, sizeof(struct async_op), MEMPOOL_CACHE_SIZE, 0, NULL, NULL, NULL, NULL, 0, 0);
    if (async_pool == NULL) rte_exit(EXIT_FAILURE, "Cannot create async op pool\n");
    context_buffer = rte_ring_create("context_ring", (unsigned)32*1024, SOCKET_ID_ANY, 0 /*RING_F_SP_ENQ | RING_F_SC_DEQ */);
    for(int a=0;a<NUMBER_OF_CORES;a++) {
        char rxName[100];
        char txName[100];
        sprintf(rxName,"fake_crypto_rx_ring_%d",a);
        sprintf(txName,"fake_crypto_tx_ring_%d",a);
        lcore_conf[a].fake_crypto_rx = rte_ring_create(rxName, (unsigned) 32*1024, SOCKET_ID_ANY,
                                                              0 /*RING_F_SP_ENQ | RING_F_SC_DEQ */);
        lcore_conf[a].fake_crypto_tx = rte_ring_create(txName, (unsigned) 32*1024, SOCKET_ID_ANY,
                                                              0 /*RING_F_SP_ENQ | RING_F_SC_DEQ */);

        #ifdef DEBUG__COUNT_CONTEXT_MISSING_CAUSED_PACKET_DROP
            debugContextMissingCausedDroppedPacketCounter[a] = 0;
            debugContextMissingCausedStartCycleCount[a] = 0;
        #endif


        #ifdef DEBUG__CRYPTO_EVERY_N
            extern int run_blocking_encryption_counter[RTE_MAX_LCORE];
            run_blocking_encryption_counter[a] = 0;
        #endif
        #ifdef DEBUG__CONTEXT_SWITCH_FOR_EVERY_N_PACKET
            extern int packet_required_counter[RTE_MAX_LCORE];
            packet_required_counter[a] = -1;
        #endif
    }
}


void async_handle_packet(struct lcore_data* lcdata, packet_descriptor_t* pd, unsigned pkt_idx, uint32_t port_id, void (*handler_function)(void))
{
    ucontext_t *context;
    if(rte_mempool_get(context_pool, (void**)&context) != 0) {
        //rte_exit(1,"Cannot create new context!\n");
        pd->dropped = 1;
        free_packet(pd);
        pd->context = NULL;
        #ifdef DEBUG__COUNT_CONTEXT_MISSING_CAUSED_PACKET_DROP
            if(debugContextMissingCausedStartCycleCount[rte_lcore_id()] == 0) {
                debugContextMissingCausedStartCycleCount[rte_lcore_id()] = rte_get_tsc_cycles();
            }else if(rte_get_tsc_cycles() - debugContextMissingCausedStartCycleCount[rte_lcore_id()] > rte_get_timer_hz()){
                pthread_mutex_lock(&contextMissingMutex);
                fprintf(stderr, "----------------dropped number of packets caused by packet drop on core [%d]:%d\n",rte_lcore_id(),debugContextMissingCausedDroppedPacketCounter[rte_lcore_id()]);
                pthread_mutex_unlock(&contextMissingMutex);

                debugContextMissingCausedStartCycleCount[rte_lcore_id()] = rte_get_tsc_cycles();
                debugContextMissingCausedDroppedPacketCounter[rte_lcore_id()] = 0;
            }else{
                debugContextMissingCausedDroppedPacketCounter[rte_lcore_id()]++;
            }

        #endif
        return;
    }

    context->uc_stack.ss_sp = (ucontext_t*)context + 1; // the stack is supposed to be placed right after the context description
    context->uc_stack.ss_size = CONTEXT_STACKSIZE;
    context->uc_stack.ss_flags = 0;
    pd->context = context;
    debug("Packet being handled, context reference is %p\n", context);

    getcontext(context);
    context->uc_link = &lcdata->conf->main_loop_context;
    makecontext(context, handler_function, 4, lcdata, pd, port_id);
    DBG_CONTEXT_SWAP_TO_PACKET(context)
    swapcontext(&lcdata->conf->main_loop_context, context);
    debug("Swapped back to main context.\n");
}

void do_async_op(packet_descriptor_t* pd, enum async_op_type op)
{
    void* extraInformationForAsyncHandling = NULL;
    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        if(pd->context == NULL) return;

        extraInformationForAsyncHandling = pd->context;

        // saving standard metadata into context
        int metadata_length = pd->headers[header_instance_standard_metadata].length;
        uint8_t standard_metadata[metadata_length];
        memcpy(standard_metadata,
               pd->headers[header_instance_standard_metadata].pointer,
               metadata_length);
    #endif

    // deparse
    control_DeparserImpl(pd, 0, 0);
    emit_packet(pd, 0, 0);

    // enqueue mbuf to async operation buffer
    enqueue_packet_for_async(pd, op, extraInformationForAsyncHandling);

    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        void* context = extraInformationForAsyncHandling;
        // suspend processing of packet and go back to the main context
        DBG_CONTEXT_SWAP_TO_MAIN
        swapcontext(context, &lcore_conf[rte_lcore_id()].main_loop_context);
        debug("Swapped back to packet context %p.\n", context);
    #endif


    // parse
    reset_pd(pd);
    parse_packet(pd, 0, 0);


    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        // restoring standard metadata from context
        memcpy(pd->headers[header_instance_standard_metadata].pointer,
               standard_metadata,
               metadata_length);
    #endif
}
#if ASYNC_MODE == ASYNC_MODE_CONTEXT
    ucontext_t* cs[RTE_MAX_LCORE][CRYPTO_BURST_SIZE];
#endif

struct async_op *async_ops[RTE_MAX_LCORE][CRYPTO_BURST_SIZE];
struct rte_crypto_op* enqueued_ops[RTE_MAX_LCORE][CRYPTO_BURST_SIZE];
struct rte_crypto_op* dequeued_ops[RTE_MAX_LCORE][CRYPTO_BURST_SIZE];

#include <unistd.h>

void do_blocking_sync_op(packet_descriptor_t* pd, enum async_op_type op){
    unsigned int lcore_id = rte_lcore_id();
    int c;

    control_DeparserImpl(pd, 0, 0);
    emit_packet(pd, 0, 0);

    create_crypto_op(async_ops[lcore_id],pd,op,NULL);
    if (rte_crypto_op_bulk_alloc(lcore_conf[lcore_id].crypto_pool, RTE_CRYPTO_OP_TYPE_SYMMETRIC, enqueued_ops[lcore_id], 1) == 0){
        rte_exit(EXIT_FAILURE, "Not enough crypto operations available\n");
    }
    async_op_to_crypto_op(async_ops[lcore_id][0], enqueued_ops[lcore_id][0]);
    rte_mempool_put_bulk(async_pool, (void **) async_ops[lcore_id], 1);

    #ifdef START_CRYPTO_NODE
        if (rte_ring_enqueue_burst(lcore_conf[lcore_id].fake_crypto_rx, (void**)enqueued_ops[lcore_id], 1, NULL) <= 0){
            debug(T4LIT(Enqueing ops in blocking sync op failed... skipping encryption,error) "\n");
            return;
        }
        while(rte_ring_dequeue_burst(lcore_conf[lcore_id].fake_crypto_tx, (void**)dequeued_ops[lcore_id], 1, NULL) == 0);
    #else
        if(rte_cryptodev_enqueue_burst(cdev_id, lcore_id,enqueued_ops[lcore_id], 1) <= 0){
            debug(T4LIT(Enqueing ops in blocking sync op failed... skipping encryption,error) "\n");
            return;
        }
        while(rte_cryptodev_dequeue_burst(cdev_id, lcore_id, dequeued_ops[lcore_id], 1) == 0);
    #endif
    struct rte_mbuf *mbuf = dequeued_ops[lcore_id][0]->sym->m_src;
    int packet_length = *(rte_pktmbuf_mtod(mbuf, int*));

    rte_pktmbuf_adj(mbuf, sizeof(int));
    pd->wrapper = mbuf;
    pd->data = rte_pktmbuf_mtod(pd->wrapper, uint8_t*);
    pd->wrapper->pkt_len = packet_length;
    debug_mbuf(mbuf, "Result of encryption\n");

    rte_mempool_put_bulk(lcore_conf[lcore_id].crypto_pool, (void **)dequeued_ops[lcore_id], 1);

    reset_pd(pd);
    parse_packet(pd, 0, 0);
}

static inline void
wait_for_cycles(uint64_t cycles)
{
    uint64_t now = rte_get_tsc_cycles();
    uint64_t then = now;
    while((now - then) < cycles)
        now = rte_get_tsc_cycles();
}
void main_loop_fake_crypto(struct lcore_data* lcdata){
    unsigned lcore_id = rte_lcore_id();
    for(int a=0;a<NUMBER_OF_CORES;a++){
        if(lcore_conf[a].fake_crypto_rx != NULL){
            unsigned int n = rte_ring_dequeue_burst(lcore_conf[a].fake_crypto_rx, (void*)enqueued_ops[lcore_id], CRYPTO_BURST_SIZE, NULL);
            if (n>0){
                #if CRYPTO_NODE_MODE == CRYPTO_NODE_MODE_OPENSSL
                    rte_cryptodev_enqueue_burst(cdev_id, lcore_id, enqueued_ops[lcore_id], n);
                    int already_dequed_ops = 0;
                    while(already_dequed_ops < n){
                        already_dequed_ops += rte_cryptodev_dequeue_burst(cdev_id, lcore_id, dequeued_ops[lcore_id], n - already_dequed_ops);
                    }
                #elif CRYPTO_NODE_MODE == CRYPTO_NODE_MODE_FAKE
                    wait_for_cycles(FAKE_CRYPTO_SLEEP_MULTIPLIER*n);
                #endif

                for(int b=0;b<n;b++){
                    enqueued_ops[lcore_id][b]->status = RTE_CRYPTO_OP_STATUS_SUCCESS;
                }
                if (rte_ring_enqueue_burst(lcore_conf[a].fake_crypto_tx, (void*)enqueued_ops[lcore_id], n, NULL) <= 0){
                    debug(T4LIT(Enqueing from fake crypto core failed,error) "\n");
                }
            }
        }
    }
}

void main_loop_async(struct lcore_data* lcdata, packet_descriptor_t *pd)
{
    unsigned lcore_id = rte_lcore_id();
    unsigned n, i;
    #if ASYNC_MODE == ASYNC_MODE_CONTEXT
        if(rte_ring_count(context_buffer) > CRYPTO_BURST_SIZE)
        {
            n = rte_ring_dequeue_burst(context_buffer, (void**)cs[lcore_id], CRYPTO_BURST_SIZE, NULL);
            for(i = 0; i < n; i++)
                debug(T4LIT(Packet context %p is being freed up.,warning) "\n", cs[lcore_id][i]);
            rte_mempool_put_bulk(context_pool, (void**)cs[lcore_id], n);
        }
    #endif

    if(CRYPTO_DEVICE_AVAILABLE)
    {
        if(rte_ring_count(lcdata->conf->async_queue) >= CRYPTO_BURST_SIZE)
        {
            n = rte_ring_dequeue_burst(lcdata->conf->async_queue, (void**)async_ops[lcore_id], CRYPTO_BURST_SIZE, NULL);
            if(n > 0)
            {
                if (rte_crypto_op_bulk_alloc(lcdata->conf->crypto_pool, RTE_CRYPTO_OP_TYPE_SYMMETRIC, enqueued_ops[lcore_id], n) == 0)
                    rte_exit(EXIT_FAILURE, "Not enough crypto operations available\n");
                for(i = 0; i < n; i++)
                    async_op_to_crypto_op(async_ops[lcore_id][i], enqueued_ops[lcore_id][i]);
                rte_mempool_put_bulk(async_pool, (void**)async_ops[lcore_id], n);
                #ifdef START_CRYPTO_NODE
                    lcdata->conf->pending_crypto += rte_ring_enqueue_burst(lcore_conf[lcore_id].fake_crypto_rx, (void**)enqueued_ops[lcore_id], n, NULL);
                #else
                    lcdata->conf->pending_crypto += rte_cryptodev_enqueue_burst(cdev_id, lcore_id, enqueued_ops[lcore_id], n);
                #endif
            }
        }

        if(lcdata->conf->pending_crypto >= CRYPTO_BURST_SIZE)
        {
            #ifdef START_CRYPTO_NODE
                n = rte_ring_dequeue_burst(lcore_conf[lcore_id].fake_crypto_tx, (void**)dequeued_ops[lcore_id], CRYPTO_BURST_SIZE, NULL);
            #else
                n = rte_cryptodev_dequeue_burst(cdev_id, lcore_id, dequeued_ops[lcore_id], CRYPTO_BURST_SIZE);
            #endif
            for (i = 0; i < n; i++)
            {
                if (dequeued_ops[lcore_id][i]->status != RTE_CRYPTO_OP_STATUS_SUCCESS){
                    rte_exit(EXIT_FAILURE, "Some operations were not processed correctly");
                }
                else{
                    dequeued_ops[lcore_id][i]->status = RTE_CRYPTO_OP_STATUS_SUCCESS;
                    resume_packet_handling(dequeued_ops[lcore_id][i]->sym->m_src, lcdata, pd);
                }
            }
            rte_mempool_put_bulk(lcdata->conf->crypto_pool, (void **)dequeued_ops[lcore_id], n);
            lcdata->conf->pending_crypto -= n;
        }
    }
}
