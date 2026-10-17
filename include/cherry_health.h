/*
 * CherryHealth - Kernel Health & System Watchdog Monitoring Subsystem
 *
 * Copyright (c) 2026 ForgeStitch
 * SPDX-License-Identifier: MIT
 *
 * Implements real-time kernel health monitoring, per-core watchdog liveness,
 * heap integrity tracking, and proactive system health reporting.
 */

#ifndef _CHERRY_HEALTH_H_
#define _CHERRY_HEALTH_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cherry_health_stats {
    uint32_t        cores_online;       /* Number of active CPU cores */
    uint32_t        uptime_sec;         /* Kernel uptime in seconds */
    size_t          free_heap_bytes;    /* Free kernel heap memory */
    size_t          total_heap_bytes;   /* Total kernel heap memory */
    uint32_t        active_threads;     /* Active running threads */
    bool            system_healthy;     /* Overall system status */
};

/**
 * Initialize kernel health monitor daemon (khealthd).
 */
int cherry_health_init(void);

/**
 * Retrieve current kernel health statistics.
 */
int cherry_health_get_stats(struct cherry_health_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* _CHERRY_HEALTH_H_ */
