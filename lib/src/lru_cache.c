#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../lib/include/lru_cache.h"

LRUCache* lru_cache_create(void) {
    LRUCache *cache = (LRUCache*)malloc(sizeof(LRUCache));
    if (!cache) return NULL;
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    memset(cache->nodes, 0, sizeof(cache->nodes));
    return cache;
}

static LRUNode* find_node(LRUCache *cache, const char *key) {
    for (int i = 0; i < cache->count; i++) {
        if (cache->nodes[i] && strcmp(cache->nodes[i]->key, key) == 0) {
            return cache->nodes[i];
        }
    }
    return NULL;
}

static void move_to_front(LRUCache *cache, LRUNode *node) {
    if (cache->head == node) return;
    
    // Remove from current position
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (cache->tail == node) cache->tail = node->prev;
    
    // Move to front
    node->prev = NULL;
    node->next = cache->head;
    if (cache->head) cache->head->prev = node;
    cache->head = node;
    if (!cache->tail) cache->tail = node;
}

int lru_cache_get(LRUCache *cache, const char *key) {
    if (!cache || !key) return -1;
    
    LRUNode *node = find_node(cache, key);
    if (node) {
        move_to_front(cache, node);
        return node->value;
    }
    return -1;
}

void lru_cache_put(LRUCache *cache, const char *key, int value) {
    if (!cache || !key) return;
    
    LRUNode *node = find_node(cache, key);
    if (node) {
        // Update existing
        node->value = value;
        move_to_front(cache, node);
        return;
    }
    
    // Add new node
    if (cache->count >= LRU_CACHE_SIZE) {
        // Remove least recently used (tail)
        LRUNode *lru = cache->tail;
        if (lru) {
            if (lru->prev) lru->prev->next = NULL;
            cache->tail = lru->prev;
            if (cache->head == lru) cache->head = NULL;
            
            // Find and remove from nodes array
            for (int i = 0; i < cache->count; i++) {
                if (cache->nodes[i] == lru) {
                    free(lru->key);
                    free(lru);
                    cache->nodes[i] = NULL;
                    break;
                }
            }
            cache->count--;
        }
    }
    
    // Create new node
    node = (LRUNode*)malloc(sizeof(LRUNode));
    if (!node) return;
    node->key = strdup(key);
    node->value = value;
    node->prev = NULL;
    node->next = cache->head;
    
    if (cache->head) cache->head->prev = node;
    cache->head = node;
    if (!cache->tail) cache->tail = node;
    
    // Add to nodes array
    for (int i = 0; i < LRU_CACHE_SIZE; i++) {
        if (!cache->nodes[i]) {
            cache->nodes[i] = node;
            break;
        }
    }
    cache->count++;
}

void lru_cache_free(LRUCache *cache) {
    if (!cache) return;
    LRUNode *node = cache->head;
    while (node) {
        LRUNode *next = node->next;
        free(node->key);
        free(node);
        node = next;
    }
    free(cache);
}

