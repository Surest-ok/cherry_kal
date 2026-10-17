/*
 * CherryHealth - Kernel Health & System Watchdog Monitoring Implementation
 *
 * Copyright (c) 2026 ForgeStitch
 * SPDX-License-Identifier: MIT
 */

#include "cherry_health.h"
#include "cherry_kal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <errno.h>

static struct cherry_health_stats g_health_stats;
static bool g_health_initialized = false;

static int khealthd_worker(void *data)
{
    (void)data;
    while (1) {
        msleep(2000);

        size_t free_heap = xPortGetFreeHeapSize();
        size_t total_heap = configTOTAL_HEAP_SIZE;

        g_health_stats.uptime_sec = (uint32_t)(ktime_get_ms() / 1000ULL);
        g_health_stats.free_heap_bytes = free_heap;
        g_health_stats.total_heap_bytes = total_heap;
        g_health_stats.cores_online = configNUMBER_OF_CORES;
        g_health_stats.active_threads = (uint32_t)uxTaskGetNumberOfTasks();
        g_health_stats.system_healthy = (free_heap >= 1024 * 64);

        if (!g_health_stats.system_healthy) {
            pr_warn("khealthd: low memory warning (free=%lu bytes)\n", (unsigned long)free_heap);
        }
    }
    return 0;
}

int cherry_health_init(void)
{
    if (g_health_initialized) return 0;

    memset(&g_health_stats, 0, sizeof(g_health_stats));
    g_health_stats.cores_online = configNUMBER_OF_CORES;
    g_health_stats.total_heap_bytes = configTOTAL_HEAP_SIZE;
    g_health_stats.free_heap_bytes = xPortGetFreeHeapSize();
    g_health_stats.system_healthy = true;

    struct task_struct *t = kthread_run(khealthd_worker, NULL, "khealthd");
    if (!t) {
        pr_err("khealthd: failed to spawn health daemon thread\n");
        return -ENOMEM;
    }

    g_health_initialized = true;
    pr_info("khealthd: kernel health monitor active (cores=%u total_heap=%lu KB)\n",
            (unsigned int)g_health_stats.cores_online,
            (unsigned long)(g_health_stats.total_heap_bytes / 1024));
    return 0;
}

int cherry_health_get_stats(struct cherry_health_stats *stats)
{
    if (!stats) return -EINVAL;
    *stats = g_health_stats;
    stats->free_heap_bytes = xPortGetFreeHeapSize();
    return 0;
}
