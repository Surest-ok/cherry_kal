/*
 * CherryWorkqueue - Linux-Compatible Deferred Execution Workqueue Subsystem
 *
 * Copyright (c) 2026 ForgeStitch
 * SPDX-License-Identifier: MIT
 *
 * Implements Linux-standard deferred workqueues (work_struct, workqueue_struct,
 * queue_work, schedule_work) for asynchronous driver interrupts and deferred tasks.
 */

#ifndef _CHERRY_WORKQUEUE_H_
#define _CHERRY_WORKQUEUE_H_

#include "cherry_kal.h"

#ifdef __cplusplus
extern "C" {
#endif

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
    atomic_t                data;       /* Pending status / flags */
    struct work_struct     *next;       /* Linked list chain */
    work_func_t             func;       /* Worker callback */
};

#define WORK_DATA_PENDING   (1 << 0)

#define INIT_WORK(_work, _func)                 \
    do {                                        \
        (_work)->data.counter = 0;             \
        (_work)->next = NULL;                  \
        (_work)->func = (_func);               \
    } while (0)

struct workqueue_struct;

/**
 * Initialize system default workqueue (system_wq).
 */
int cherry_workqueue_init(void);

/**
 * Create a dedicated kernel workqueue with specified name.
 */
struct workqueue_struct *create_workqueue(const char *name);
struct workqueue_struct *create_singlethread_workqueue(const char *name);

/**
 * Destroy a workqueue and release all allocated resources.
 */
void destroy_workqueue(struct workqueue_struct *wq);

/**
 * Queue work onto a specific workqueue. Returns true if work was successfully queued.
 */
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);

/**
 * Queue work onto the default system workqueue (system_wq).
 */
bool schedule_work(struct work_struct *work);

/**
 * Flush all pending work items in a workqueue and wait for completion.
 */
void flush_workqueue(struct workqueue_struct *wq);

/**
 * Self-test suite verifying workqueue deferred execution.
 */
int cherry_workqueue_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* _CHERRY_WORKQUEUE_H_ */
