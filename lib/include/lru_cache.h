#ifndef LRU_CACHE_H
#define LRU_CACHE_H

// LRU Cache for recent file searches
#define LRU_CACHE_SIZE 64

typedef struct LRUNode {
    char *key;
    int value;
    struct LRUNode *prev;
    struct LRUNode *next;
} LRUNode;

typedef struct {
    LRUNode *head;
    LRUNode *tail;
    LRUNode *nodes[LRU_CACHE_SIZE];
    int count;
} LRUCache;

// Initialize LRU cache
LRUCache* lru_cache_create(void);
// Get value (moves to front)
int lru_cache_get(LRUCache *cache, const char *key);
// Put key-value (adds/moves to front)
void lru_cache_put(LRUCache *cache, const char *key, int value);
// Free cache
void lru_cache_free(LRUCache *cache);

#endif

