#include <stdint.h>

#define U8_MAGIC 0x55AA382D

typedef struct U8Header {
    uint32_t magic; // 0x55AA382D
    uint32_t root_node_offset; // Always (usually) 0x20?
    uint32_t meta_size; // Relative from root_node_offset. Total size of nodes and string table
    uint32_t data_offset; // I mean, really, why do we have this, it's not even used
} U8Header;

typedef struct U8Node {
    uint32_t type: 8; // 0x00: File, 0x01: Directory
    uint32_t name_offset: 24; // Relative to start of string table
    uint32_t offset; // Relative to the very start of file. Which is, silly. Like, the data offset is up there
    uint32_t size; // Directories: Pointer to end of directory (as in, &nodes[size]), or a pointer to the next child of it's parent directory. For the root node it specifies how many nodes are here
} U8Node;

typedef struct U8Context {
    U8Header header;
    union { U8Node *nodes, *root_node; };
    unsigned int node_count;
    const char* str_table;
    unsigned int str_table_size; // str_table_end = ptr + root_node_offset + meta_size
    void* ptr;
    size_t fsize;
} U8Context;

typedef struct U8File {
    U8Context* ctx;
    unsigned int index;
    unsigned int offset;
    void* ptr;
    size_t size;
} U8File;

int U8Init(void* ptr, U8Context* ctx);
void U8Examine(U8Context*);
int U8OpenFile(U8Context* ctx, const char* filepath, U8File* out);
