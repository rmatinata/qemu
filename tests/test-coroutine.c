/*
 * Coroutine tests
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include <glib.h>
#include <alloca.h>
#include <fcntl.h>
#include <pthread.h>
#include "qemu/coroutine.h"
#include "qemu/coroutine_int.h"

/*
 * Check that qemu_in_coroutine() works
 */

static void coroutine_fn verify_in_coroutine(void *opaque)
{
    g_assert(qemu_in_coroutine());
}

static void test_in_coroutine(void)
{
    Coroutine *coroutine;

    g_assert(!qemu_in_coroutine());

    coroutine = qemu_coroutine_create(verify_in_coroutine);
    qemu_coroutine_enter(coroutine, NULL);
}

/*
 * Check that qemu_coroutine_self() works
 */

static void coroutine_fn verify_self(void *opaque)
{
    g_assert(qemu_coroutine_self() == opaque);
}

static void test_self(void)
{
    Coroutine *coroutine;

    coroutine = qemu_coroutine_create(verify_self);
    qemu_coroutine_enter(coroutine, coroutine);
}

/*
 * Check that coroutines may nest multiple levels
 */

typedef struct {
    unsigned int n_enter;   /* num coroutines entered */
    unsigned int n_return;  /* num coroutines returned */
    unsigned int max;       /* maximum level of nesting */
} NestData;

static void coroutine_fn nest(void *opaque)
{
    NestData *nd = opaque;

    nd->n_enter++;

    if (nd->n_enter < nd->max) {
        Coroutine *child;

        child = qemu_coroutine_create(nest);
        qemu_coroutine_enter(child, nd);
    }

    nd->n_return++;
}

static void test_nesting(void)
{
    Coroutine *root;
    NestData nd = {
        .n_enter  = 0,
        .n_return = 0,
        .max      = 128,
    };

    root = qemu_coroutine_create(nest);
    qemu_coroutine_enter(root, &nd);

    /* Must enter and return from max nesting level */
    g_assert_cmpint(nd.n_enter, ==, nd.max);
    g_assert_cmpint(nd.n_return, ==, nd.max);
}

/*
 * Check that yield/enter transfer control correctly
 */

static void coroutine_fn yield_5_times(void *opaque)
{
    bool *done = opaque;
    int i;

    for (i = 0; i < 5; i++) {
        qemu_coroutine_yield();
    }
    *done = true;
}

static void test_yield(void)
{
    Coroutine *coroutine;
    bool done = false;
    int i = -1; /* one extra time to return from coroutine */

    coroutine = qemu_coroutine_create(yield_5_times);
    while (!done) {
        qemu_coroutine_enter(coroutine, &done);
        i++;
    }
    g_assert_cmpint(i, ==, 5); /* coroutine must yield 5 times */
}

static void coroutine_fn c2_fn(void *opaque)
{
    qemu_coroutine_yield();
}

static void coroutine_fn c1_fn(void *opaque)
{
    Coroutine *c2 = opaque;
    qemu_coroutine_enter(c2, NULL);
}

static void test_co_queue(void)
{
    Coroutine *c1;
    Coroutine *c2;

    c1 = qemu_coroutine_create(c1_fn);
    c2 = qemu_coroutine_create(c2_fn);

    qemu_coroutine_enter(c1, c2);
    memset(c1, 0xff, sizeof(Coroutine));
    qemu_coroutine_enter(c2, NULL);
    memset(c1, 0, sizeof(Coroutine));
}

/*
 * Check that creation, enter, and return work
 */

static void coroutine_fn set_and_exit(void *opaque)
{
    bool *done = opaque;

    *done = true;
}

static void test_lifecycle(void)
{
    Coroutine *coroutine;
    bool done = false;

    /* Create, enter, and return from coroutine */
    coroutine = qemu_coroutine_create(set_and_exit);
    qemu_coroutine_enter(coroutine, &done);
    g_assert(done); /* expect done to be true (first time) */

    /* Repeat to check that no state affects this test */
    done = false;
    coroutine = qemu_coroutine_create(set_and_exit);
    qemu_coroutine_enter(coroutine, &done);
    g_assert(done); /* expect done to be true (second time) */
}


#define RECORD_SIZE 10 /* Leave some room for expansion */
struct coroutine_position {
    int func;
    int state;
};
static struct coroutine_position records[RECORD_SIZE];
static unsigned record_pos;

static void record_push(int func, int state)
{
    struct coroutine_position *cp = &records[record_pos++];
    g_assert_cmpint(record_pos, <, RECORD_SIZE);
    cp->func = func;
    cp->state = state;
}

static void coroutine_fn co_order_test(void *opaque)
{
    record_push(2, 1);
    g_assert(qemu_in_coroutine());
    qemu_coroutine_yield();
    record_push(2, 2);
    g_assert(qemu_in_coroutine());
}

static void do_order_test(void)
{
    Coroutine *co;

    co = qemu_coroutine_create(co_order_test);
    record_push(1, 1);
    qemu_coroutine_enter(co, NULL);
    record_push(1, 2);
    g_assert(!qemu_in_coroutine());
    qemu_coroutine_enter(co, NULL);
    record_push(1, 3);
    g_assert(!qemu_in_coroutine());
}

static void test_order(void)
{
    int i;
    const struct coroutine_position expected_pos[] = {
        {1, 1,}, {2, 1}, {1, 2}, {2, 2}, {1, 3}
    };
    do_order_test();
    g_assert_cmpint(record_pos, ==, 5);
    for (i = 0; i < record_pos; i++) {
        g_assert_cmpint(records[i].func , ==, expected_pos[i].func );
        g_assert_cmpint(records[i].state, ==, expected_pos[i].state);
    }
}

static void recursive_stack_growth(int n, int fd, int stack_size)
{
    int ret;
    void *buf;

    if (n == 0) {
        return;
    }

    qemu_coroutine_yield();

    buf = alloca(stack_size);
    // Do something interesting with buf so gcc doesn't optimize it away.
    ret = read(fd, buf, stack_size);
    assert(ret >= 0);
    recursive_stack_growth(n - 1, fd, stack_size);
    // Do some more stuff so gcc doesn't convert into tail call.
    ret = write(fd, buf, stack_size);
    assert(ret >= 0);
}

static void coroutine_fn co_stack_growth(void *opaque)
{
    int *ret = opaque;
    int fd = open("/dev/null", O_RDWR); // So read()/write() become no-ops.
    // Grow stack to close to 1M in roughly 4K chunks.
    recursive_stack_growth(250, fd, 4000);
    *ret = 1;
}

static void *test_stack_growth_thr(void *arg)
{
    int ret[2] = {} ;
    Coroutine *co[2];

    co[0] = qemu_coroutine_create(co_stack_growth);
    co[1] = qemu_coroutine_create(co_stack_growth);

    while (ret[0] == 0 || ret[1] == 0) {
        if (ret[0] == 0) {
            qemu_coroutine_enter(co[0], &ret[0]);
        }
        if (ret[1] == 0) {
            qemu_coroutine_enter(co[1], &ret[1]);
        }
    }
    return NULL;
}

static void test_stack_growth(void)
{
    const int num_threads = 4;
    pthread_t thr[num_threads];
    int i;

    for (i = 0; i < num_threads; i++) {
        pthread_create(&thr[i], NULL, test_stack_growth_thr, NULL);
    }

    for (i = 0; i < num_threads; i++) {
        pthread_join(thr[i], NULL);
    }
}

/*
 * Lifecycle benchmark
 */

static void coroutine_fn empty_coroutine(void *opaque)
{
    /* Do nothing */
}

static void perf_lifecycle(void)
{
    Coroutine *coroutine;
    unsigned int i, max;
    double duration;

    max = 1000000;

    g_test_timer_start();
    for (i = 0; i < max; i++) {
        coroutine = qemu_coroutine_create(empty_coroutine);
        qemu_coroutine_enter(coroutine, NULL);
    }
    duration = g_test_timer_elapsed();

    g_test_message("Lifecycle %u iterations: %f s\n", max, duration);
}

static void perf_nesting(void)
{
    unsigned int i, maxcycles, maxnesting;
    double duration;

    maxcycles = 10000;
    maxnesting = 1000;
    Coroutine *root;

    g_test_timer_start();
    for (i = 0; i < maxcycles; i++) {
        NestData nd = {
            .n_enter  = 0,
            .n_return = 0,
            .max      = maxnesting,
        };
        root = qemu_coroutine_create(nest);
        qemu_coroutine_enter(root, &nd);
    }
    duration = g_test_timer_elapsed();

    g_test_message("Nesting %u iterations of %u depth each: %f s\n",
        maxcycles, maxnesting, duration);
}

/*
 * Yield benchmark
 */

static void coroutine_fn yield_loop(void *opaque)
{
    unsigned int *counter = opaque;

    while ((*counter) > 0) {
        (*counter)--;
        qemu_coroutine_yield();
    }
}

static void perf_yield(void)
{
    unsigned int i, maxcycles;
    double duration;

    maxcycles = 100000000;
    i = maxcycles;
    Coroutine *coroutine = qemu_coroutine_create(yield_loop);

    g_test_timer_start();
    while (i > 0) {
        qemu_coroutine_enter(coroutine, &i);
    }
    duration = g_test_timer_elapsed();

    g_test_message("Yield %u iterations: %f s\n",
        maxcycles, duration);
}

static __attribute__((noinline)) void dummy(unsigned *i)
{
    (*i)--;
}

static void perf_baseline(void)
{
    unsigned int i, maxcycles;
    double duration;

    maxcycles = 100000000;
    i = maxcycles;

    g_test_timer_start();
    while (i > 0) {
        dummy(&i);
    }
    duration = g_test_timer_elapsed();

    g_test_message("Function call %u iterations: %f s\n",
        maxcycles, duration);
}

static __attribute__((noinline)) void perf_cost_func(void *opaque)
{
    qemu_coroutine_yield();
}

static void perf_cost(void)
{
    const unsigned long maxcycles = 40000000;
    unsigned long i = 0;
    double duration;
    unsigned long ops;
    Coroutine *co;

    g_test_timer_start();
    while (i++ < maxcycles) {
        co = qemu_coroutine_create(perf_cost_func);
        qemu_coroutine_enter(co, &i);
        qemu_coroutine_enter(co, NULL);
    }
    duration = g_test_timer_elapsed();
    ops = (long)(maxcycles / (duration * 1000));

    g_test_message("Run operation %lu iterations %f s, %luK operations/s, "
                   "%luns per coroutine",
                   maxcycles,
                   duration, ops,
                   (unsigned long)(1000000000.0 * duration / maxcycles));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/basic/co_queue", test_co_queue);
    g_test_add_func("/basic/lifecycle", test_lifecycle);
    g_test_add_func("/basic/yield", test_yield);
    g_test_add_func("/basic/nesting", test_nesting);
    g_test_add_func("/basic/self", test_self);
    g_test_add_func("/basic/in_coroutine", test_in_coroutine);
    g_test_add_func("/basic/order", test_order);
    g_test_add_func("/basic/stack_growth", test_stack_growth);
    if (g_test_perf()) {
        g_test_add_func("/perf/lifecycle", perf_lifecycle);
        g_test_add_func("/perf/nesting", perf_nesting);
        g_test_add_func("/perf/yield", perf_yield);
        g_test_add_func("/perf/function-call", perf_baseline);
        g_test_add_func("/perf/cost", perf_cost);
    }
    return g_test_run();
}
