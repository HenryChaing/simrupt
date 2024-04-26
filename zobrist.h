#pragma once

#include "game.h"
#include <linux/list.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#define HASH_TABLE_SIZE ((int) 1e6 + 3)  // choose a large prime number

extern u64 zobrist_table[N_GRIDS][2];

typedef struct {
    u64 key;
    int score;
    int move;
    struct hlist_node ht_list;
} zobrist_entry_t;

void zobrist_init(void);
zobrist_entry_t *zobrist_get(u64 key);
void zobrist_put(u64 key, int score, int move);
void zobrist_clear(void);
