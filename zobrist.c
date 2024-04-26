#include <linux/kernel.h> /* We are doing kernel work */ 
#include <linux/module.h> /* Specifically, a module  */ 
#include "mt19937-64.h"
#include "zobrist.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("A device that simulates interrupts");

u64 zobrist_table[N_GRIDS][2];

#define HASH(key) ((key) % HASH_TABLE_SIZE)

static struct hlist_head *hash_table;

void zobrist_init(void)
{
    int i;
    for (i = 0; i < N_GRIDS; i++) {
        zobrist_table[i][0] = mt19937_rand();
        zobrist_table[i][1] = mt19937_rand();
    }
    hash_table = vmalloc(sizeof(struct hlist_head) * HASH_TABLE_SIZE);
    BUG_ON(!hash_table);
    // assert(hash_table);
    for (i = 0; i < HASH_TABLE_SIZE; i++)
        INIT_HLIST_HEAD(&hash_table[i]);
}

zobrist_entry_t *zobrist_get(u64 key)
{
    unsigned long long hash_key = HASH(key);

    if (hlist_empty(&hash_table[hash_key]))
        return NULL;

    zobrist_entry_t *entry = NULL;
    hlist_for_each_entry (entry, &hash_table[hash_key], ht_list)
    {
        if (entry->key == key)
            return entry;
    }
    return NULL;
}

void zobrist_put(u64 key, int score, int move)
{
    unsigned long long hash_key = HASH(key);
    zobrist_entry_t *new_entry = vmalloc(sizeof(zobrist_entry_t));
    BUG_ON(!new_entry);
    new_entry->key = key;
    new_entry->move = move;
    new_entry->score = score;
    hlist_add_head(&new_entry->ht_list, &hash_table[hash_key]);
}

void zobrist_clear(void)
{
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        while (!hlist_empty(&hash_table[i])) {
            zobrist_entry_t *entry;
            entry = hlist_entry(hash_table[i].first, zobrist_entry_t, ht_list);
            hlist_del(&entry->ht_list);
            vfree(entry);
        }
        INIT_HLIST_HEAD(&hash_table[i]);
    }
}