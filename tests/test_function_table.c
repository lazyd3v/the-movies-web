/* Tests the table extracted from the patched pinned runtime, not a duplicate. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

typedef void (*GuestFn)(void *);
static int fail_alloc;
static void *table_calloc(size_t n, size_t bytes) {
    return fail_alloc ? NULL : calloc(n, bytes);
}
#define calloc table_calloc
#include FUNCTION_TABLE_SOURCE
#undef calloc

static void first(void *unused) { (void)unused; }
static void second(void *unused) { (void)unused; }

static void expect_abort(int scenario) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        if (scenario == 0) { fail_alloc = 1; function_table_grow(); }
        if (scenario == 1) { g_fn_cap = 0x80000000u; function_table_grow(); }
        if (scenario == 2) register_function(3, NULL);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int main(void) {
    assert(lookup_function(0) == NULL);
    /* Deliberately collide at the initial capacity, including address zero. */
    for (uint32_t i = 0; i < 1024; i++) register_function(i << 16, first);
    for (uint32_t i = 0; i < 1024; i++) assert(lookup_function(i << 16) == first);
    assert(lookup_function(2048u << 16) == NULL);
    /* More than the old table's entire capacity; several growth steps. */
    for (uint32_t i = 0; i < 100000; i++) register_function((i << 4) | 1u, first);
    assert(g_fn_count == 101024);
    assert(g_fn_count <= g_fn_cap / 2u);
    assert(g_fn_cap >= 262144u);
    for (uint32_t i = 0; i < 1024; i++) assert(lookup_function(i << 16) == first);
    for (uint32_t i = 0; i < 100000; i++) assert(lookup_function((i << 4) | 1u) == first);
    uint32_t old_count = g_fn_count, old_capacity = g_fn_cap;
    for (uint32_t i = 0; i < 100000; i += 3) register_function((i << 4) | 1u, second);
    assert(g_fn_count == old_count && g_fn_cap == old_capacity);
    for (uint32_t i = 0; i < 100000; i++)
        assert(lookup_function((i << 4) | 1u) == (i % 3 ? first : second));
    assert(lookup_function(UINT32_MAX) == NULL);
    expect_abort(0); expect_abort(1); expect_abort(2);
    free(g_fns);
    puts("function table: collisions, 101024 registrations, growth, replacement and failure guards passed");
    return 0;
}
