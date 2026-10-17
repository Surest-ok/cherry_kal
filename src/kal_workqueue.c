/*
 * CherryWorkqueue - Linux-Compatible Deferred Execution Workqueue Implementation
 *
 * Copyright (c) 2026 ForgeStitch
 * SPDX-License-Identifier: MIT
 */

#include "cherry_workqueue.h"
#include <string.h>
#include <errno.h>

struct workqueue_struct {
    char                    name[32];
    struct task_struct     *worker_task;
    struct semaphore        sem;
    struct semaphore        exit_sem;
    spinlock_t              lock;
    struct work_struct     *head;
    struct work_struct     *tail;
    volatile bool           active;
};

static struct workqueue_struct *g_system_wq = NULL;

static int workqueue_worker(void *data)
{
    struct workqueue_struct *wq = (struct workqueue_struct *)data;
    if (!wq) return -1;

    while (wq->active) {
        down(&wq->sem);
        if (!wq->active) break;

        for (;;) {
            unsigned long flags;
            spin_lock_irqsave(&wq->lock, flags);

            struct work_struct *work = wq->head;
            if (!work) {
                spin_unlock_irqrestore(&wq->lock, flags);
                break;
            }

            wq->head = work->next;
            if (!wq->head) {
                wq->tail = NULL;
            }
            work->next = NULL;
            atomic_set(&work->data, 0);

            spin_unlock_irqrestore(&wq->lock, flags);

            if (work->func) {
                work->func(work);
            }
        }
    }

    up(&wq->exit_sem);
    return 0;
}

struct workqueue_struct *create_workqueue(const char *name)
{
    struct workqueue_struct *wq = (struct workqueue_struct *)kzalloc(sizeof(*wq), GFP_KERNEL);
    if (!wq) return NULL;

    strncpy(wq->name, name ? name : "kworker", sizeof(wq->name) - 1);
    sema_init(&wq->sem, 0);
    sema_init(&wq->exit_sem, 0);
    spin_lock_init(&wq->lock);
    wq->active = true;

    wq->worker_task = kthread_run(workqueue_worker, wq, wq->name);
    if (!wq->worker_task) {
        sema_destroy(&wq->sem);
        sema_destroy(&wq->exit_sem);
        kfree(wq);
        return NULL;
    }

    return wq;
}

struct workqueue_struct *create_singlethread_workqueue(const char *name)
{
    return create_workqueue(name);
}

void flush_workqueue(struct workqueue_struct *wq)
{
    if (!wq) return;

    while (wq->head != NULL) {
        msleep(1);
    }
}

void destroy_workqueue(struct workqueue_struct *wq)
{
    if (!wq) return;

    flush_workqueue(wq);

    wq->active = false;
    up(&wq->sem);
    down(&wq->exit_sem);

    msleep(10);

    if (wq->worker_task) {
        kfree(wq->worker_task);
        wq->worker_task = NULL;
    }

    sema_destroy(&wq->sem);
    sema_destroy(&wq->exit_sem);
    kfree(wq);
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
    if (!wq || !work || !work->func || !wq->active) {
        return false;
    }

    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);

    /* If work is already pending in the queue, skip duplicate insertion */
    if (atomic_read(&work->data) & WORK_DATA_PENDING) {
        spin_unlock_irqrestore(&wq->lock, flags);
        return false;
    }

    atomic_set(&work->data, WORK_DATA_PENDING);
    work->next = NULL;

    if (wq->tail) {
        wq->tail->next = work;
        wq->tail = work;
    } else {
        wq->head = work;
        wq->tail = work;
    }

    spin_unlock_irqrestore(&wq->lock, flags);

    up(&wq->sem);
    return true;
}

int cherry_workqueue_init(void)
{
    if (g_system_wq) return 0;

    g_system_wq = create_workqueue("events/0");
    if (!g_system_wq) {
        pr_err("cherry_workqueue: failed to create system default workqueue\n");
        return -ENOMEM;
    }

    pr_info("cherry_workqueue: initialized system default deferred workqueue (events/0)\n");
    return 0;
}

bool schedule_work(struct work_struct *work)
{
    if (!g_system_wq) {
        if (cherry_workqueue_init() != 0) {
            return false;
        }
    }
    return queue_work(g_system_wq, work);
}

struct test_work_ctx {
    struct work_struct work;
    atomic_t          *target;
};

static void dummy_work_fn(struct work_struct *work)
{
    struct test_work_ctx *ctx = container_of(work, struct test_work_ctx, work);
    if (ctx && ctx->target) {
        atomic_inc(ctx->target);
    }
}

int cherry_workqueue_run_tests(void)
{
    struct workqueue_struct *wq = create_workqueue("test_wq");
    if (!wq) return -1;

    atomic_t test_val = ATOMIC_INIT(0);
    struct test_work_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    INIT_WORK(&ctx.work, dummy_work_fn);
    ctx.target = &test_val;

    bool queued = queue_work(wq, &ctx.work);
    if (!queued) {
        destroy_workqueue(wq);
        return -1;
    }

    /* Wait for execution */
    int limit = 100;
    while (atomic_read(&test_val) == 0 && --limit > 0) {
        msleep(2);
    }

    int res = atomic_read(&test_val);
    destroy_workqueue(wq);

    if (res == 1) {
        pr_info("cherry_workqueue: [workqueue-verify] async deferred execution verified (completed=%d)\n", res);
        return 0;
    }

    pr_err("cherry_workqueue: self-test failed (res=%d)\n", res);
    return -1;
}
