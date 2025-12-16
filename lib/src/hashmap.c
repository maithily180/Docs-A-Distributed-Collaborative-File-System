#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../lib/include/hashmap.h"

// Hash function for strings (djb2)
unsigned int hash_string(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

HashMap* hashmap_create(void) {
    HashMap *map = (HashMap*)malloc(sizeof(HashMap));
    if (!map) return NULL;
    memset(map->buckets, 0, sizeof(map->buckets));
    map->size = 0;
    map->capacity = HASHMAP_SIZE;
    return map;
}

int hashmap_put(HashMap *map, const char *key, int value) {
    if (!map || !key) return -1;
    
    unsigned int hash = hash_string(key) % HASHMAP_SIZE;
    HashNode *node = map->buckets[hash];
    
    // Check if key already exists
    while (node) {
        if (strcmp(node->key, key) == 0) {
            node->value = value; // Update existing
            return 0;
        }
        node = node->next;
    }
    
    // Create new node
    HashNode *new_node = (HashNode*)malloc(sizeof(HashNode));
    if (!new_node) return -1;
    new_node->key = strdup(key);
    new_node->value = value;
    new_node->next = map->buckets[hash];
    map->buckets[hash] = new_node;
    map->size++;
    return 0;
}

int hashmap_get(HashMap *map, const char *key) {
    if (!map || !key) return -1;
    
    unsigned int hash = hash_string(key) % HASHMAP_SIZE;
    HashNode *node = map->buckets[hash];
    
    while (node) {
        if (strcmp(node->key, key) == 0) {
            return node->value;
        }
        node = node->next;
    }
    return -1; // Not found
}

int hashmap_remove(HashMap *map, const char *key) {
    if (!map || !key) return -1;
    
    unsigned int hash = hash_string(key) % HASHMAP_SIZE;
    HashNode *node = map->buckets[hash];
    HashNode *prev = NULL;
    
    while (node) {
        if (strcmp(node->key, key) == 0) {
            if (prev) {
                prev->next = node->next;
            } else {
                map->buckets[hash] = node->next;
            }
            free(node->key);
            free(node);
            map->size--;
            return 0;
        }
        prev = node;
        node = node->next;
    }
    return -1; // Not found
}

void hashmap_free(HashMap *map) {
    if (!map) return;
    for (int i = 0; i < HASHMAP_SIZE; i++) {
        HashNode *node = map->buckets[i];
        while (node) {
            HashNode *next = node->next;
            free(node->key);
            free(node);
            node = next;
        }
    }
    free(map);
}

