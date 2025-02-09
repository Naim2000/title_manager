#define _XOPEN_SOURCE 600L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ogc/video.h>

#include "u8.h"

#define ERROR(str, ...) fprintf(stderr, "%s:%i: \n" str "\n", __FILE__, __LINE__, ##__VA_ARGS__)

int U8Init(void* ptr, U8Context* ctx) {
    assert(ptr != NULL);

    if (ctx) memset(ctx, 0, sizeof(U8Context));

    U8Header* header = (U8Header*)ptr;

    if (header->magic != U8_MAGIC) {
        ERROR("U8 header magic is invalid (%#08x != %#08x)", header->magic, U8_MAGIC);
        return -1;
    }
    
    U8Node* nodes = (U8Node*)(ptr + header->root_node_offset);
    U8Node* root_node = &nodes[0];

    if (root_node->type != 0x01) {
        ERROR("Root node is not a directory? What?");
        return -2;
    }

    unsigned int node_count = root_node->size;

    const char* str_table = (const char*)(nodes + node_count);
    unsigned int str_table_size = header->meta_size - (node_count * sizeof(U8Node));

    for (unsigned int index = 1; index < node_count; index++) {
        U8Node* node = &nodes[index];

        if (node->type != 0x00 && node->type != 0x01) {
            ERROR("Node #%u has invalid node type (%#x)", index, node->type);
            return -3;
        }

        if (node->name_offset >= str_table_size) {
            ERROR("Name offset for node #%u is out of range (%#x >= %#x)", index, node->name_offset, str_table_size);
            return -4;
        }

        const char* node_name = str_table + node->name_offset;

        if (node->type == 0x00) {
            if (node->offset < header->data_offset) {
                ERROR("Data offset for file node #%u (%s) is out of bounds (%#x < %#x)", index, node_name, node->offset, header->data_offset); // Only use for data_offset lmao
                return -5;
            }       
        } else {
            if (node->size > node_count) {
                ERROR("End marker for directory node #%u (%s) is out of bounds (%#x > %#x)", index, node_name, node->offset, node_count);
                return -5;
            }
        }
    }

    if (ctx) {
        ctx->header = *header;
        ctx->nodes = nodes;
        ctx->node_count = node_count;
        ctx->str_table = str_table;
        ctx->str_table_size = str_table_size;

        ctx->ptr = ptr;
    }

    return 0;
}

#define next_sibling(N, n) ((((N[n]).type == 0x00) ? n + 1 : ((N[n]).size)))
#define has_child(N, n) ((N[n]).type == 0x01 && (n + 1) != (N[n].size))

void U8Examine(U8Context* ctx) {
    unsigned int dir_stack[16] = { ctx->node_count };
    int dir_lvl = 0;
    char string[128];
    const char
        dir_skip = 0xb3,
        dir_child = 0xc3,
        dir_last_child = 0xc0,
        dir_start = 0xc2,
        child = 0xc4;


    printf("/ (node count=%u)\n", ctx->node_count);

    for (unsigned int index = 1; index < ctx->node_count; index++) {
        U8Node* node = &ctx->nodes[index];
        const char* name = ctx->str_table + node->name_offset;

        char* ptr = string;
        for (int i = 0; i < dir_lvl; i++) {
            *ptr++ = (next_sibling(ctx->nodes, index) == dir_stack[i]) ? ' ' : dir_skip;
        }
        *ptr++ = (next_sibling(ctx->nodes, index) == dir_stack[dir_lvl]) ? dir_last_child : dir_child;
        *ptr++ = has_child(ctx->nodes, index) ? dir_start : child;
        ptr += sprintf(ptr, node->type == 0x00 ? "%s " : "%s/ ", name);
        if (node->type == 0x00) {
            sprintf(ptr, "(%#x)", node->size);
        }
        else {
            dir_stack[++dir_lvl] = node->size;
        }

        puts(string);
        VIDEO_WaitVSync();
        VIDEO_WaitVSync();

        while (index + 1 == dir_stack[dir_lvl] && dir_lvl > 0) dir_lvl--;
    }
}

static unsigned int U8FindChild(U8Context* ctx, unsigned int parent, const char* name) {
    if (ctx->nodes[parent].type != 0x01) {
        ERROR("Tried to find child in a non-directory node");
        return 0;
    }

    unsigned int dir_cur = parent + 1;
    unsigned int dir_end = ctx->nodes[parent].size;

    while (dir_cur < dir_end) {
        U8Node* node = &ctx->nodes[dir_cur];
        const char* node_name = ctx->str_table + node->name_offset;

        if (!strcmp(name, node_name))
            return dir_cur;

        // Next child node
        dir_cur = next_sibling(ctx->nodes, dir_cur);
    }

    return 0;
}

int U8OpenFile(U8Context* ctx, const char* filepath, U8File* out) {
    assert(ctx != NULL && filepath != NULL); // out? Eh not so much

    char* _filepath = strdup(filepath);
    if (!_filepath) //??
        return -1;

    char* saveptr;
    char* elem = strtok_r(_filepath, "/", &saveptr);
    unsigned int index = 0;

    if (!elem) {
        ERROR("strtok() came back with nothing on the first run?\n" "'%s' <-- Does this path have a slash?", filepath);
        free(_filepath);
        return -2;
    }

    while (elem) {
        index = U8FindChild(ctx, index, elem);
        if (!index)
            break;

        elem = strtok_r(NULL, "/", &saveptr);
    }

    free(_filepath);
    if (!index)
        return -2;

    U8Node* node = &ctx->nodes[index];
    if (node->type != 0x00)
        return -3;

    if (out) {
        out->ctx    = ctx;
        out->index  = index;
        out->offset = node->offset;
        out->ptr    = ctx->ptr + node->offset;
        out->size   = node->size;
    }

    return 0;
}
