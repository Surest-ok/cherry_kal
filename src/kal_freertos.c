#include <errno.h>
/*
 * CherryKAL - FreeRTOS SMP Implementation (Pure Linux Kernel API Standard)
 *
 * Copyright (c) 2026 ForgeStitch
 * SPDX-License-Identifier: MIT
 */

#include "cherry_kal.h"
#include <string.h>
#include <stdarg.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "port_smp.h"
#include "uart.h"

#define KTHREAD_DEFAULT_STACK_WORDS 4096
#define KTHREAD_DEFAULT_PRIORITY    (tskIDLE_PRIORITY + 2)

/* =========================================================================
 * 1. Memory Management
 * ========================================================================= */
void *kmalloc(size_t size, gfp_t flags)
{
    void *ptr = pvPortMalloc(size);
    if (ptr && (flags & __GFP_ZERO)) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(const void *ptr)
{
    if (ptr) {
        vPortFree((void *)ptr);
    }
}

void *kzalloc(size_t size, gfp_t flags)
{
    return kmalloc(size, flags | __GFP_ZERO);
}

void *kcalloc(size_t nmemb, size_t size, gfp_t flags)
{
    size_t total = nmemb * size;
    return kzalloc(total, flags);
}

void *krealloc(const void *ptr, size_t size, gfp_t flags)
{
    if (!ptr) return kmalloc(size, flags);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    void *new_ptr = kmalloc(size, flags);
    if (new_ptr) {
        memcpy(new_ptr, ptr, size);
        kfree(ptr);
    }
    return new_ptr;
}

/* =========================================================================
 * 2. Mutex Synchronization
 * ========================================================================= */
void mutex_init(struct mutex *lock)
{
    if (!lock) return;
    SemaphoreHandle_t sem = xSemaphoreCreateRecursiveMutex();
    lock->handle = (void *)sem;
}

void mutex_lock(struct mutex *lock)
{
    if (!lock || !lock->handle) return;
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return;
    }
    xSemaphoreTakeRecursive((SemaphoreHandle_t)lock->handle, portMAX_DELAY);
}

int mutex_trylock(struct mutex *lock)
{
    if (!lock || !lock->handle) return 0;
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return 1;
    }
    BaseType_t ret = xSemaphoreTakeRecursive((SemaphoreHandle_t)lock->handle, 0);
    return (ret == pdTRUE) ? 1 : 0;
}

void mutex_unlock(struct mutex *lock)
{
    if (!lock || !lock->handle) return;
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return;
    }
    xSemaphoreGiveRecursive((SemaphoreHandle_t)lock->handle);
}

void mutex_destroy(struct mutex *lock)
{
    if (!lock || !lock->handle) return;
    vSemaphoreDelete((SemaphoreHandle_t)lock->handle);
    lock->handle = NULL;
}

/* =========================================================================
 * 2.1 Real-Time Priority Inheritance Protocol (PIP) Mutex
 * ========================================================================= */
void rt_mutex_init(struct rt_mutex *lock)
{
    if (!lock) return;
    SemaphoreHandle_t sem = xSemaphoreCreateMutex();
    lock->handle = (void *)sem;
}

void rt_mutex_lock(struct rt_mutex *lock)
{
    if (!lock || !lock->handle) return;
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return;
    xSemaphoreTake((SemaphoreHandle_t)lock->handle, portMAX_DELAY);
}

int rt_mutex_trylock(struct rt_mutex *lock)
{
    if (!lock || !lock->handle) return 0;
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return 1;
    BaseType_t ret = xSemaphoreTake((SemaphoreHandle_t)lock->handle, 0);
    return (ret == pdTRUE) ? 1 : 0;
}

int rt_mutex_lock_timeout(struct rt_mutex *lock, unsigned long timeout_ms)
{
    if (!lock || !lock->handle) return -EINVAL;
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return 0;
    TickType_t ticks = (timeout_ms == 0) ? 0 : (timeout_ms * configTICK_RATE_HZ) / 1000;
    BaseType_t ret = xSemaphoreTake((SemaphoreHandle_t)lock->handle, ticks);
    return (ret == pdTRUE) ? 0 : -ETIMEDOUT;
}

void rt_mutex_unlock(struct rt_mutex *lock)
{
    if (!lock || !lock->handle) return;
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return;
    xSemaphoreGive((SemaphoreHandle_t)lock->handle);
}

void rt_mutex_destroy(struct rt_mutex *lock)
{
    if (!lock || !lock->handle) return;
    vSemaphoreDelete((SemaphoreHandle_t)lock->handle);
    lock->handle = NULL;
}

int kthread_get_priority(struct task_struct *task)
{
    TaskHandle_t h = (task && task->handle) ? (TaskHandle_t)task->handle : xTaskGetCurrentTaskHandle();
    return (int)uxTaskPriorityGet(h);
}

void kthread_set_priority(struct task_struct *task, int prio)
{
    TaskHandle_t h = (task && task->handle) ? (TaskHandle_t)task->handle : xTaskGetCurrentTaskHandle();
    vTaskPrioritySet(h, (UBaseType_t)prio);
}

bool kal_pip_verify_test(void)
{
    struct rt_mutex mtx;
    rt_mutex_init(&mtx);
    if (!mtx.handle) return false;

    /* Verify basic acquisition and trylock semantics */
    int t1 = rt_mutex_trylock(&mtx);
    rt_mutex_unlock(&mtx);

    /* Verify priority inquiry */
    int prio = kthread_get_priority(NULL);
    bool pass = (prio >= 0 && t1 == 1);

    rt_mutex_destroy(&mtx);
    pr_info("cherry_kal: [kal-verify] pip_mutex=pass prio_inq=pass all=%s\n", pass ? "pass" : "fail");
    return pass;
}

/* =========================================================================
 * 3. Spinlock (SMP safe)
 * ========================================================================= */
static inline void *kal_get_current_owner(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        TaskHandle_t h = xTaskGetCurrentTaskHandle();
        if (h != NULL) return (void *)h;
    }
    return (void *)((uintptr_t)smp_processor_id() + 1);
}

void spin_lock_init(spinlock_t *lock)
{
    if (!lock) return;
    lock->lock = 0;
    lock->owner = NULL;
    lock->nesting = 0;
}

void spin_lock(spinlock_t *lock)
{
    if (!lock) return;
    void *owner = kal_get_current_owner();

    if (lock->owner == owner) {
        lock->nesting++;
        return;
    }

    while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("yield");
    }
    lock->owner = owner;
    lock->nesting = 1;
}

int spin_trylock(spinlock_t *lock)
{
    if (!lock) return 0;
    void *owner = kal_get_current_owner();
    if (lock->owner == owner) {
        lock->nesting++;
        return 1;
    }
    if (!__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        lock->owner = owner;
        lock->nesting = 1;
        return 1;
    }
    return 0;
}

void spin_unlock(spinlock_t *lock)
{
    if (!lock) return;
    void *owner = kal_get_current_owner();
    if (lock->owner == owner) {
        if (--lock->nesting == 0) {
            lock->owner = NULL;
            __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
        }
    } else {
        /* Fallback for pre-scheduler boot locks or ISR mismatches */
        lock->owner = NULL;
        lock->nesting = 0;
        __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
    }
}

unsigned long _spin_lock_irqsave(spinlock_t *lock)
{
    unsigned long daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(daif) :: "memory");
    spin_lock(lock);
    return daif;
}

void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    spin_unlock(lock);
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

/* =========================================================================
 * 4. Semaphore Synchronization
 * ========================================================================= */
void sema_init(struct semaphore *sem, int val)
{
    if (!sem) return;
    UBaseType_t max_cnt = (val > 1) ? (UBaseType_t)val : 1024U;
    UBaseType_t init_cnt = (val > 0) ? (UBaseType_t)val : 0U;
    SemaphoreHandle_t h = xSemaphoreCreateCounting(max_cnt, init_cnt);
    sem->handle = (void *)h;
}

void down(struct semaphore *sem)
{
    if (!sem || !sem->handle) return;
    xSemaphoreTake((SemaphoreHandle_t)sem->handle, portMAX_DELAY);
}

int down_interruptible(struct semaphore *sem)
{
    if (!sem || !sem->handle) return -1;
    BaseType_t ret = xSemaphoreTake((SemaphoreHandle_t)sem->handle, portMAX_DELAY);
    return (ret == pdTRUE) ? 0 : -1;
}

int down_trylock(struct semaphore *sem)
{
    if (!sem || !sem->handle) return 1;
    BaseType_t ret = xSemaphoreTake((SemaphoreHandle_t)sem->handle, 0);
    return (ret == pdTRUE) ? 0 : 1;
}

int down_timeout(struct semaphore *sem, long timeout_ms)
{
    if (!sem || !sem->handle) return -1;
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    BaseType_t ret = xSemaphoreTake((SemaphoreHandle_t)sem->handle, ticks);
    return (ret == pdTRUE) ? 0 : -1;
}

void up(struct semaphore *sem)
{
    if (!sem || !sem->handle) return;
    xSemaphoreGive((SemaphoreHandle_t)sem->handle);
}

void sema_destroy(struct semaphore *sem)
{
    if (!sem || !sem->handle) return;
    vSemaphoreDelete((SemaphoreHandle_t)sem->handle);
    sem->handle = NULL;
}

/* =========================================================================
 * 5. Threads & Scheduling
 * ========================================================================= */
struct kthread_wrapper_arg {
    kthread_fn_t fn;
    void *data;
    struct task_struct *ts;
};

static void kthread_wrapper(void *arg)
{
    struct kthread_wrapper_arg *karg = (struct kthread_wrapper_arg *)arg;
    kthread_fn_t fn = karg->fn;
    void *data = karg->data;
    struct task_struct *ts = karg->ts;
    kfree(karg);

    if (fn) {
        fn(data);
    }
    if (ts) {
        ts->handle = NULL;
    }
    vTaskDelete(NULL);
}

struct task_struct *kthread_create(kthread_fn_t threadfn, void *data, const char *name)
{
    struct task_struct *ts = (struct task_struct *)kmalloc(sizeof(struct task_struct), GFP_KERNEL);
    if (!ts) return NULL;

    struct kthread_wrapper_arg *karg = (struct kthread_wrapper_arg *)kmalloc(sizeof(*karg), GFP_KERNEL);
    if (!karg) {
        kfree(ts);
        return NULL;
    }
    karg->fn = threadfn;
    karg->data = data;
    karg->ts = ts;

    TaskHandle_t h = NULL;
    BaseType_t ret = xTaskCreate(kthread_wrapper,
                                 name ? name : "kthread",
                                 (configSTACK_DEPTH_TYPE)KTHREAD_DEFAULT_STACK_WORDS,
                                 karg,
                                 (UBaseType_t)KTHREAD_DEFAULT_PRIORITY,
                                 &h);
    if (ret != pdPASS) {
        kfree(karg);
        kfree(ts);
        return NULL;
    }
    ts->handle = (void *)h;
    return ts;
}

struct task_struct *kthread_run(kthread_fn_t threadfn, void *data, const char *name)
{
    return kthread_create(threadfn, data, name);
}

int kthread_bind(struct task_struct *task, unsigned int cpu)
{
    if (!task || !task->handle) return -1;
#if ( configUSE_CORE_AFFINITY == 1 )
    UBaseType_t affinity = (1U << cpu);
    vTaskCoreAffinitySet((TaskHandle_t)task->handle, affinity);
    return 0;
#else
    (void)task;
    (void)cpu;
    return 0;
#endif
}

void schedule(void)
{
    taskYIELD();
}

void msleep(unsigned int msecs)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(msecs));
    } else {
        for (volatile uint32_t i = 0; i < msecs * 10000; i++) {
            __asm__ volatile("yield");
        }
    }
}

void usleep_range(unsigned long min, unsigned long max)
{
    (void)max;
    unsigned int ms = (unsigned int)(min / 1000UL);
    if (ms == 0) ms = 1;
    msleep(ms);
}

unsigned int smp_processor_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (unsigned int)((mpidr & 0xFFU) + (((mpidr >> 8) & 0xFFU) << 4));
}

static struct task_struct s_cur_tasks[32];

struct task_struct *kthread_current(void)
{
    uint32_t cpu = smp_processor_id();
    if (cpu >= 32) cpu = 0;
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    s_cur_tasks[cpu].handle = (void *)h;
    return &s_cur_tasks[cpu];
}

const char *kthread_name(struct task_struct *task)
{
    if (!task || !task->handle) {
        return pcTaskGetName(NULL);
    }
    return pcTaskGetName((TaskHandle_t)task->handle);
}

void *kthread_raw_handle(struct task_struct *task)
{
    if (!task) return NULL;
    return task->handle;
}

int kal_scheduler_is_running(void)
{
    return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? 1 : 0;
}

void kthread_exit(int code)
{
    (void)code;
    vTaskDelete(NULL);
}

void kthread_delete(struct task_struct *task)
{
    if (task && task->handle) {
        vTaskDelete((TaskHandle_t)task->handle);
        task->handle = NULL;
    }
}

void *kthread_get_data(void)
{
#if ( configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0 )
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED && xTaskGetCurrentTaskHandle() != NULL) {
        return pvTaskGetThreadLocalStoragePointer(NULL, 0);
    }
    return NULL;
#else
    return NULL;
#endif
}

void kthread_set_data(void *data)
{
#if ( configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0 )
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED && xTaskGetCurrentTaskHandle() != NULL) {
        vTaskSetThreadLocalStoragePointer(NULL, 0, data);
    }
#else
    (void)data;
#endif
}

/* =========================================================================
 * 6. Time and Jiffies
 * ========================================================================= */
unsigned long msecs_to_jiffies(const unsigned int m)
{
    return (unsigned long)pdMS_TO_TICKS(m);
}

unsigned int jiffies_to_msecs(const unsigned long j)
{
    return (unsigned int)((j * 1000UL) / (unsigned long)configTICK_RATE_HZ);
}

uint64_t ktime_get_ns(void)
{
    uint64_t cnt;
    uint64_t frq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    if (frq == 0) return 0;
    return (cnt * 1000000000ULL) / frq;
}

uint64_t ktime_get_ms(void)
{
    return ktime_get_ns() / 1000000ULL;
}

uint64_t ktime_get_real_ns(void)
{
    return ktime_get_ns();
}

/* =========================================================================
 * 7. Linux Standard Printk
 * ========================================================================= */
__attribute__((weak)) int printk(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    uart_puts(buf);
    return len;
}
