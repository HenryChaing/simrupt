#pragma once

#include <linux/types.h>

/* initializes mt[NN] with a seed */
void mt19937_init(u64 seed);

/* generates a random number on [0, 2^64-1]-interval */
u64 mt19937_rand(void);
