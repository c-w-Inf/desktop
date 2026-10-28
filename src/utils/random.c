#include "random.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

static u64 randomState = 0;
static bool randomSeeded = false;

u64 RandomNext (void) {
    if (!randomSeeded) {
        u64 seed = (u64)time (NULL) ^ ((u64)(uintptr_t)&randomState << 17) ^
                   (u64)clock ();
        if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;
        randomState = seed;
        randomSeeded = true;
    }
    u64 x = randomState;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    randomState = x;
    return x * 2685821657736338717ULL;
}

u64 RandomBounded (u64 limit) {
    u64 bound = UINT64_MAX - (UINT64_MAX % limit);
    u64 value;
    do {
        value = RandomNext ();
    } while (value >= bound);
    return value % limit;
}
