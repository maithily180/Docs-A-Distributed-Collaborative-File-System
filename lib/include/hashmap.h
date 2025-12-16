#ifndef HASHMAP_H
#define HASHMAP_H

// Simple hashmap for file name -> index mapping
// Provides O(1) average case lookup

#define HASHMAP_SIZE 1024
#define HASHMAP_LOAD_FACTOR 0.75

typedef struct HashNode {
    char *key;
    int value;
    struct HashNode *next;
} HashNode;

typedef struct {
    HashNode *buckets[HASHMAP_SIZE];
    int size;
    int capacity;
} HashMap;

// Initialize hashmap
HashMap* hashmap_create(void);
// Insert key-value pair
int hashmap_put(HashMap *map, const char *key, int value);
// Get value by key, returns -1 if not found
int hashmap_get(HashMap *map, const char *key);
// Remove key-value pair
int hashmap_remove(HashMap *map, const char *key);
// Free hashmap
void hashmap_free(HashMap *map);
// Get hash value for string
unsigned int hash_string(const char *str);

#endif

