/* Ordered host delivery of retired RSX user commands. Each command carries
 * its own cause. Storage grows by blocks; an empty queue reuses its last block.
 * The caller dispatches guest code outside this lock. */
#ifndef RSX_CALLBACK_QUEUE_H
#define RSX_CALLBACK_QUEUE_H
#include "../../runtime/platform/win32_compat.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
typedef struct rsx_callback_block {
    struct rsx_callback_block* next;
    unsigned read,used;
    uint32_t commands[64];
} rsx_callback_block;
typedef struct rsx_callback_queue {
    SRWLOCK lock;
    rsx_callback_block *head,*tail;
} rsx_callback_queue;
#define RSX_CALLBACK_QUEUE_INIT {SRWLOCK_INIT,NULL,NULL}
static void rsx_callback_queue_clear(rsx_callback_queue* q) {
    AcquireSRWLockExclusive(&q->lock);
    while(q->head){rsx_callback_block* p=q->head;q->head=p->next;free(p);}
    q->tail=NULL;
    ReleaseSRWLockExclusive(&q->lock);
}
static void rsx_callback_queue_push(rsx_callback_queue* q,uint32_t command) {
    AcquireSRWLockExclusive(&q->lock);
    if(!q->tail||q->tail->used==64) {
        rsx_callback_block* p=(rsx_callback_block*)calloc(1,sizeof(*p));
        if(!p){fputs("[gcm] user callback queue allocation failed\n",stderr);abort();}
        if(q->tail)q->tail->next=p;else q->head=p;
        q->tail=p;
    }
    q->tail->commands[q->tail->used++]=command;
    ReleaseSRWLockExclusive(&q->lock);
}
static uint64_t rsx_callback_queue_take(rsx_callback_queue* q) {
    AcquireSRWLockExclusive(&q->lock);
    uint64_t result=0;
    rsx_callback_block* p=q->head;
    if(p&&p->read<p->used) {
        result=(1ull<<32)|p->commands[p->read++];
        if(p->read==p->used) {
            if(p->next){q->head=p->next;free(p);}
            else p->read=p->used=0;
        }
    }
    ReleaseSRWLockExclusive(&q->lock);
    return result;
}
#endif
