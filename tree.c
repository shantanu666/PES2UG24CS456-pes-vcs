// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Helper function to build a tree for a given directory prefix
static int build_tree_for_prefix(const IndexEntry *entries, int count, const char *prefix, ObjectID *id_out) {
    Tree tree = {0};
    
    // Find all entries that belong to this prefix
    for (int i = 0; i < count; i++) {
        const char *path = entries[i].path;
        
        // Check if this entry belongs to the current prefix
        if (prefix[0] == '\0') {
            // Root level: entries without '/' or with '/' but we handle subdirs separately
            const char *slash = strchr(path, '/');
            if (slash == NULL) {
                // File at root
                tree.entries[tree.count].mode = entries[i].mode;
                tree.entries[tree.count].hash = entries[i].hash;
                strcpy(tree.entries[tree.count].name, path);
                tree.count++;
            } else {
                // Directory at root, handle below
            }
        } else {
            // Subdirectory level
            if (strncmp(path, prefix, strlen(prefix)) == 0) {
                const char *remainder = path + strlen(prefix);
                const char *slash = strchr(remainder, '/');
                if (slash == NULL) {
                    // File in this directory
                    tree.entries[tree.count].mode = entries[i].mode;
                    tree.entries[tree.count].hash = entries[i].hash;
                    strcpy(tree.entries[tree.count].name, remainder);
                    tree.count++;
                } else {
                    // Subdirectory, handle below
                }
            }
        }
    }
    
    // Now handle subdirectories
    int start = 0;
    while (start < count) {
        const char *path = entries[start].path;
        const char *slash;
        
        if (prefix[0] == '\0') {
            slash = strchr(path, '/');
            if (slash == NULL) {
                start++;
                continue; // Already handled files
            }
        } else {
            if (strncmp(path, prefix, strlen(prefix)) != 0) {
                start++;
                continue;
            }
            const char *remainder = path + strlen(prefix);
            slash = strchr(remainder, '/');
            if (slash == NULL) {
                start++;
                continue; // Already handled files
            }
        }
        
        // Find the directory name
        char dir_name[256];
        if (prefix[0] == '\0') {
            size_t len = slash - path;
            memcpy(dir_name, path, len);
            dir_name[len] = '\0';
        } else {
            const char *remainder = path + strlen(prefix);
            size_t len = slash - remainder;
            memcpy(dir_name, remainder, len);
            dir_name[len] = '\0';
        }
        
        // Build the full prefix for this subdirectory
        char sub_prefix[512];
        if (prefix[0] == '\0') {
            strcpy(sub_prefix, dir_name);
            strcat(sub_prefix, "/");
        } else {
            strcpy(sub_prefix, prefix);
            strcat(sub_prefix, dir_name);
            strcat(sub_prefix, "/");
        }
        
        // Recursively build the subtree
        ObjectID sub_tree_id;
        if (build_tree_for_prefix(entries, count, sub_prefix, &sub_tree_id) != 0) {
            return -1;
        }
        
        // Add the subtree to current tree
        tree.entries[tree.count].mode = MODE_DIR;
        tree.entries[tree.count].hash = sub_tree_id;
        strcpy(tree.entries[tree.count].name, dir_name);
        tree.count++;
        
        // Skip all entries in this subdirectory
        while (start < count) {
            const char *p = entries[start].path;
            if (prefix[0] == '\0') {
                if (strncmp(p, dir_name, strlen(dir_name)) != 0 || p[strlen(dir_name)] != '/') {
                    break;
                }
            } else {
                if (strncmp(p, sub_prefix, strlen(sub_prefix)) != 0) {
                    break;
                }
            }
            start++;
        }
    }
    
    // Serialize and write the tree
    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) {
        return -1;
    }
    
    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) {
        return -1;
    }
    
    return build_tree_for_prefix(index.entries, index.count, "", id_out);
}