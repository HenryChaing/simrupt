#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/math.h>
#include <linux/log2.h>


#include "game.h"
#include "mcts.h"
#include "mt19937-64.h"
#include "util.h"

struct node {
    int move;
    char player;
    int n_visits;
    u32 score;
    struct node *parent;
    struct node *children[N_GRIDS];
};

static struct node *new_node(int move, char player, struct node *parent)
{
    struct node *node = vmalloc(sizeof(struct node));
    node->move = move;
    node->player = player;
    node->n_visits = 0;
    node->score = 0;
    node->parent = parent;
    memset(node->children, 0, sizeof(node->children));
    return node;
}

static void free_node(struct node *node)
{
    for (int i = 0; i < N_GRIDS; i++)
        if (node->children[i])
            free_node(node->children[i]);
    vfree(node);
}

static inline u64 uct_score(int n_total, int n_visits, u32 score)
{
    if (n_visits == 0)
        return 117813; // DBL_MAX * 65536;
    return (score) / n_visits +
           int_sqrt64(2) * int_sqrt64(__ilog2_u64(n_total) / n_visits) * 65536;
}

static struct node *select_move(struct node *node)
{
    struct node *best_node = NULL;
    u64 best_score = 0;
    for (int i = 0; i < N_GRIDS; i++) {
        if (!node->children[i])
            continue;
        u64 score = uct_score(node->n_visits, node->children[i]->n_visits,
                                 node->children[i]->score);
        if (score > best_score) {
            best_score = score;
            best_node = node->children[i];
        }
    }
    return best_node;
}

static u32 simulate(char *table, char player)
{
    char current_player = player;
    char temp_table[N_GRIDS];
    memcpy(temp_table, table, N_GRIDS);
    while (1) {
        int *moves = available_moves(temp_table);
        if (moves[0] == -1) {
            vfree(moves);
            break;
        }
        int n_moves = 0;
        while (n_moves < N_GRIDS && moves[n_moves] != -1)
            ++n_moves;
        int move = moves[mt19937_rand() % n_moves];
        vfree(moves);
        temp_table[move] = current_player;
        if ((check_win(temp_table)) != ' ')
            return calculate_win_value(check_win(temp_table), player);
        current_player ^= 'O' ^ 'X';
    }
    return 32768;
}

static void backpropagate(struct node *node, u32 score)
{
    while (node) {
        node->n_visits++;
        node->score += score;
        node = node->parent;
        score = 65536 - score;
    }
}

static void expand(struct node *node, char *table)
{
    int *moves = available_moves(table);
    int n_moves = 0;
    while (n_moves < N_GRIDS && moves[n_moves] != -1)
        ++n_moves;
    for (int i = 0; i < n_moves; i++) {
        node->children[i] = new_node(moves[i], node->player ^ 'O' ^ 'X', node);
    }
    vfree(moves);
}

int mcts(char *table, char player)
{
    char win;
    struct node *root = new_node(-1, player, NULL);
    for (int i = 0; i < 1000; i++) {
        struct node *node = root;
        char temp_table[N_GRIDS];
        memcpy(temp_table, table, N_GRIDS);
        while (1) {
            if ((win = check_win(temp_table)) != ' ') {
                u32 score =
                    calculate_win_value(win, node->player ^ 'O' ^ 'X');
                backpropagate(node, score);
                break;
            }
            if (node->n_visits == 0) {
                u32 score = simulate(temp_table, node->player);
                backpropagate(node, score);
                break;
            }
            if (node->children[0] == NULL)
                expand(node, temp_table);
            node = select_move(node);
            BUG_ON(!node);
            temp_table[node->move] = node->player ^ 'O' ^ 'X';
        }
    }
    struct node *best_node = NULL;
    int most_visits = -1;
    for (int i = 0; i < N_GRIDS; i++) {
        if (root->children[i] && root->children[i]->n_visits > most_visits) {
            most_visits = root->children[i]->n_visits;
            best_node = root->children[i];
        }
    }
    if (best_node) {
        int best_move = best_node->move;
        free_node(root);
        return best_move;
    }
    return -1;
}
