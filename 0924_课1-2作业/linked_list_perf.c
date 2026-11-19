#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { MIN_SIZE = 32, MAX_SIZE = 512 };

typedef struct LinkData
{
    struct LinkData *prev;
    struct LinkData *next;
    int type;
    int dataSize;
    char *data;
} LinkData;

static LinkData *new_node(int size, const char *mem)
{
    if (size < MIN_SIZE || size > MAX_SIZE || !mem)
    {
        errno = EINVAL;
        return NULL;
    }

    // One allocation keeps the node and its payload together.
    LinkData *node = malloc(sizeof(*node) + (size_t)size);
    if (!node)
        return NULL;

    node->type = 0;
    node->dataSize = size;
    node->data = (char *)(node + 1);
    memcpy(node->data, mem, (size_t)size);
    return node;
}

LinkData *InitLink(int size, char *mem)
{
    LinkData *first = new_node(size, mem);
    if (first)
        first->prev = first->next = first;
    return first;
}

// The required void interface reports failure through errno.
void AppendNode(LinkData *firstNode, int size, char *mem)
{
    errno = 0;
    if (!firstNode)
    {
        errno = EINVAL;
        return;
    }

    LinkData *node = new_node(size, mem);
    if (!node)
        return;

    LinkData *last = firstNode->prev;
    node->prev = last;
    node->next = firstNode;
    last->next = node;
    firstNode->prev = node;
}

// Keep repeated benchmark calls visible to GCC at -O2.
__attribute__((noipa))
LinkData *FindNode(LinkData *firstNode, int size, char *mem)
{
    if (!firstNode || !mem || size < MIN_SIZE || size > MAX_SIZE)
        return NULL;

    LinkData *node = firstNode;
    do
    {
        if (node->dataSize == size && memcmp(node->data, mem, (size_t)size) == 0)
            return node;
        node = node->next;
    } while (node != firstNode);

    return NULL;
}

static void free_link(LinkData *first)
{
    LinkData *node = first->next;
    while (node != first)
    {
        LinkData *next = node->next;
        free(node);
        node = next;
    }
    free(first);
}

struct EventPair
{
    const char *name;
    int leader;
    int member;
    bool active;
};

// PERF_FORMAT_GROUP layout: nr, time_enabled, time_running, values.
struct PairRead
{
    uint64_t nr;
    uint64_t enabled;
    uint64_t running;
    uint64_t values[2];
};

static int open_event(uint32_t type, uint64_t config, int leader)
{
    struct perf_event_attr attr = {
        .type = type,
        .size = sizeof(attr),
        .config = config,
        .disabled = leader == -1,
        .exclude_kernel = 1,
        .exclude_hv = 1,
        .read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                       PERF_FORMAT_TOTAL_TIME_RUNNING,
    };
    return (int)syscall(SYS_perf_event_open, &attr, 0, -1, leader,
                        PERF_FLAG_FD_CLOEXEC);
}

static struct EventPair open_pair(const char *name, uint32_t type,
                                  uint64_t first, uint64_t second)
{
    struct EventPair pair = {.name = name, .leader = -1, .member = -1};
    pair.leader = open_event(type, first, -1);
    if (pair.leader >= 0)
        pair.member = open_event(type, second, pair.leader);
    if (pair.member < 0)
        fprintf(stderr, "%s perf_event_open: %s\n", name, strerror(errno));
    return pair;
}

static void start_pair(struct EventPair *pair)
{
    if (pair->member < 0)
        return;
    pair->active =
        ioctl(pair->leader, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) == 0 &&
        ioctl(pair->leader, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) == 0;
    if (!pair->active)
        fprintf(stderr, "%s start: %s\n", pair->name, strerror(errno));
}

static void stop_pair(struct EventPair *pair)
{
    if (pair->active &&
        ioctl(pair->leader, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) < 0)
    {
        fprintf(stderr, "%s stop: %s\n", pair->name, strerror(errno));
        pair->active = false;
    }
}

static bool read_pair(struct EventPair pair, struct PairRead *result)
{
    bool available = false;
    if (pair.active)
    {
        ssize_t bytes = read(pair.leader, result, sizeof(*result));
        available = bytes == (ssize_t)sizeof(*result) &&
                    result->nr == 2 && result->running > 0;
        if (!available)
            fprintf(stderr, "%s counters could not be read or scheduled\n", pair.name);
    }

    if (pair.member >= 0)
        close(pair.member);
    if (pair.leader >= 0)
        close(pair.leader);
    return available;
}

static long double scaled(const struct PairRead *result, int i)
{
    return (long double)result->values[i] * result->enabled / result->running;
}

static uint64_t l1_read_event(uint64_t result)
{
    return PERF_COUNT_HW_CACHE_L1D |
           ((uint64_t)PERF_COUNT_HW_CACHE_OP_READ << 8) | (result << 16);
}

static int self_test(void)
{
    char a[MIN_SIZE] = {1};
    char b[MAX_SIZE] = {2};
    char c[MIN_SIZE] = {3};
    LinkData *first = InitLink(sizeof(a), a);
    if (!first)
        return 1;

    bool ok = first->next == first && first->prev == first &&
              FindNode(first, sizeof(a), a) == first;
    AppendNode(first, sizeof(b), b);
    ok = ok && errno == 0;
    LinkData *second = first->next;
    AppendNode(first, sizeof(a), a);
    ok = ok && errno == 0;
    AppendNode(first, sizeof(c) - 1, c);
    ok = ok && errno == EINVAL;
    LinkData *last = first->prev;
    ok = ok && second != first && last != second && last != first &&
         second->prev == first && second->next == last &&
         last->prev == second && last->next == first &&
         FindNode(first, sizeof(a), a) == first &&
         FindNode(first, sizeof(b), b) == second &&
         FindNode(first, sizeof(c), c) == NULL;
    b[0] = 0;
    ok = ok && second->data[0] == 2;
    free_link(first);

    printf("self-test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static bool parse_count(const char *text, uint64_t *count)
{
    if (*text < '0' || *text > '9')
        return false;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value == 0 || value >= UINT64_MAX)
        return false;
    *count = (uint64_t)value;
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();

    uint64_t nodes = 10000;
    uint64_t repetitions = 2000;
    const char *mode = argc > 3 ? argv[3] : "tail";
    bool miss = strcmp(mode, "miss") == 0;
    bool head = strcmp(mode, "head") == 0;
    if (argc > 4 || (argc > 1 && !parse_count(argv[1], &nodes)) ||
        (argc > 2 && !parse_count(argv[2], &repetitions)) ||
        (!miss && !head && strcmp(mode, "tail") != 0))
    {
        fprintf(stderr, "Usage: %s [nodes [repetitions [head|tail|miss]]]\n", argv[0]);
        return 1;
    }

    char payload[64] = {0};
    LinkData *first = InitLink(sizeof(payload), payload);
    if (!first)
    {
        perror("InitLink");
        return 1;
    }
    for (uint64_t i = 1; i < nodes; ++i)
    {
        memcpy(payload, &i, sizeof(i));
        AppendNode(first, sizeof(payload), payload);
        if (errno)
        {
            perror("AppendNode");
            free_link(first);
            return 1;
        }
    }

    uint64_t key = head ? 0 : (miss ? nodes : nodes - 1);
    memcpy(payload, &key, sizeof(key));
    LinkData *expected = miss ? NULL : (head ? first : first->prev);
    if (FindNode(first, sizeof(payload), payload) != expected)
    {
        fputs("Search warm-up returned an unexpected result\n", stderr);
        free_link(first);
        return 1;
    }

    struct EventPair core = open_pair("CPU", PERF_TYPE_HARDWARE,
                         PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_CPU_CYCLES);
    struct EventPair l1 = open_pair("L1D", PERF_TYPE_HW_CACHE,
                         l1_read_event(PERF_COUNT_HW_CACHE_RESULT_ACCESS),
                         l1_read_event(PERF_COUNT_HW_CACHE_RESULT_MISS));

    uint64_t hits = 0;
    start_pair(&core);
    start_pair(&l1);
    for (uint64_t i = 0; i < repetitions; ++i)
        hits += FindNode(first, sizeof(payload), payload) != NULL;
    stop_pair(&l1);
    stop_pair(&core);

    struct PairRead cpu_result, l1_result;
    bool have_core = read_pair(core, &cpu_result);
    bool have_l1 = read_pair(l1, &l1_result);
    printf("nodes=%" PRIu64 " repetitions=%" PRIu64 " mode=%s hits=%" PRIu64 "\n",
           nodes, repetitions, mode, hits);
    if (have_core)
    {
        printf("instructions: %.0Lf\ncycles: %.0Lf\n",
               scaled(&cpu_result, 0), scaled(&cpu_result, 1));
    }
    else
        puts("instructions: N/A\ncycles: N/A");

    if (have_l1)
    {
        printf("memory accesses (L1D reads): %.0Lf\nL1D read misses: %.0Lf\n",
               scaled(&l1_result, 0), scaled(&l1_result, 1));
    }
    else
        puts("memory accesses (L1D reads): N/A\nL1D read misses: N/A");

    bool have_rate = have_l1 && l1_result.values[0] > 0 &&
                     l1_result.values[1] <= l1_result.values[0];
    if (have_rate)
        printf("L1D read hit rate: %.2Lf%%\n",
               100.0L * (1.0L - (long double)l1_result.values[1] / l1_result.values[0]));
    else
        puts("L1D read hit rate: N/A");

    free_link(first);
    if (hits != (miss ? 0 : repetitions))
        return 1;
    return have_core && have_rate ? 0 : 2;
}
