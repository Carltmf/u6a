/*
 * codegen.c - Unlambda VM bytecode generator
 * 
 * Copyright (C) 2020  CismonX <admin@cismon.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "codegen.h"
#include "logging.h"
#include "vm_defs.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <arpa/inet.h>

#define OPTIMIZE_STR_MIN_LEN 0x04

#define WRITE_SECION(buffer, type_size, len, ostream)                \
    if (UNLIKELY(len != fwrite(buffer, type_size, len, ostream))) {  \
        write_len = len * type_size;                                 \
        goto codegen_failed;                                         \
    }

static FILE*       output_stream;
static const char* file_name;
static bool        optimize_const;

static const char* err_codegen = "codegen error";
static const char* info_codegen = "codegen";

struct ins_with_offset {
    struct u6a_vm_ins ins;
    uint32_t          offset;
};

static inline bool
write_bc_header(FILE* restrict output_stream, uint32_t text_len, uint32_t rodata_len) {
    struct u6a_bc_header header = {
        .file = {
            .magic            = U6A_MAGIC,
            .ver_major        = U6A_VER_MAJOR,
            .ver_minor        = U6A_VER_MINOR,
            .prog_header_size = U6A_BC_PROG_HEADER_SIZE
        },
        .prog = {
            .text_size        = htonl(text_len * sizeof(uint8_t)),
            .rodata_size      = htonl(rodata_len * sizeof(struct u6a_token))
        }
    };
    return 1 == fwrite(&header, sizeof(struct u6a_bc_header), 1, output_stream);
}

bool
u6a_write_prefix(const char* prefix_string) {
    if (prefix_string == NULL) {
        return true;
    }
    uint32_t write_length = strlen(prefix_string);
    if (UNLIKELY(write_length != fwrite(prefix_string, sizeof(char), write_length, output_stream))) {
        u6a_err_write_failed(err_codegen, write_length, file_name);
        return false;
    }
    u6a_info_verbose(info_codegen, "prefix string written, %" PRIu32 " chars total", write_length);
    return true;
}

void
u6a_codegen_init(FILE* output_stream_, const char* file_name_, bool optimize_const_) {
    output_stream = output_stream_;
    file_name = file_name_;
    optimize_const = optimize_const_;
}

bool
u6a_codegen(struct u6a_ast_node* ast_arr, uint32_t ast_len) {
    void* bc_buffer = calloc(ast_len, sizeof(struct u6a_vm_ins) + sizeof(char));
    if (UNLIKELY(bc_buffer == NULL)) {
        u6a_err_bad_alloc(err_codegen, ast_len * (sizeof(struct u6a_vm_ins) + sizeof(char)));
        return false;
    }
    struct u6a_vm_ins* text_buffer = bc_buffer;
    char* rodata_buffer = (char*)(text_buffer + ast_len);
    uint32_t text_len = 0;
    uint32_t rodata_len = 0;
    struct ins_with_offset* stack = malloc(ast_len * sizeof(struct ins_with_offset));
    if (UNLIKELY(stack == NULL)) {
        u6a_err_bad_alloc(err_codegen, ast_len * sizeof(struct ins_with_offset));
        free(bc_buffer);
        return false;
    }
    uint32_t stack_top = UINT32_MAX;
    for (uint32_t node_idx = 0; node_idx < ast_len; ++node_idx) {
        struct u6a_ast_node* node = ast_arr + node_idx;
        if (U6A_AN_FN(node) != u6a_tf_app) {
            continue;
        }
        struct u6a_ast_node* lchild = U6A_AN_LEFT(node);
        struct u6a_ast_node* rchild = U6A_AN_RIGHT(node, ast_arr);
        if (U6A_AN_FN(lchild) == u6a_tf_app) {
            if (U6A_AN_FN(rchild) == u6a_tf_app) {
                stack[stack_top++].ins.opcode = u6a_vo_sa;
            } else {
                stack[stack_top++].ins = (struct u6a_vm_ins) {
                    .opcode = u6a_vo_app,
                    .operand.fn.second = rchild->value
                };
            }
        } else {
            if (U6A_AN_FN(rchild) == u6a_tf_app) {
                if (U6A_AN_FN(lchild) == u6a_tf_d) {
                    text_buffer[text_len].opcode = u6a_vo_del;
                    stack[stack_top++] = (struct ins_with_offset) {
                        .ins.opcode = u6a_vo_la,
                        .offset = text_len++
                    };
                } else {
                    stack[stack_top++].ins = (struct u6a_vm_ins) {
                        .opcode = u6a_vo_app,
                        .operand.fn.first = lchild->value
                    };
                }
            } else {
                if (optimize_const && U6A_AN_FN(lchild) == u6a_tf_out) {
                    uint32_t old_rodata_len = rodata_len;
                    uint32_t old_stack_top = stack_top;
                    rodata_buffer[rodata_len++] = U6A_AN_CH(lchild);
                    while (stack_top < UINT32_MAX) {
                        struct u6a_vm_ins peek_ins = stack[stack_top--].ins;
                        struct u6a_token operand_first = peek_ins.operand.fn.first;
                        struct u6a_token operand_second = peek_ins.operand.fn.second;
                        if (peek_ins.opcode == u6a_vo_app && operand_first.fn == u6a_tf_out && !operand_second.fn) {
                            rodata_buffer[rodata_len++] = operand_first.ch;
                        } else {
                            break;
                        }
                    }
                    // Ignore short strings, as they don't optimize much
                    if (rodata_len - old_rodata_len < OPTIMIZE_STR_MIN_LEN) {
                        rodata_len = old_rodata_len;
                        stack_top = old_stack_top;
                        goto no_optimize_str;
                    } else {
                        rodata_buffer[rodata_len++] = '\0';
                        text_buffer[text_len++] = (struct u6a_vm_ins) {
                            .opcode = u6a_vo_lc,
                            .opcode_ex = u6a_vo_ex_print,
                            .operand.offset = htonl(old_rodata_len)
                        };
                        text_buffer[text_len++] = (struct u6a_vm_ins) {
                            .opcode = u6a_vo_app,
                            .operand.fn.second = rchild->value
                        };
                    }
                } else {
                    no_optimize_str:
                    text_buffer[text_len++] = (struct u6a_vm_ins) {
                        .opcode = u6a_vo_app,
                        .operand.fn = {
                            .first = lchild->value,
                            .second = rchild->value
                        }
                    };
                }
                while (stack_top < UINT32_MAX) {
                    struct ins_with_offset* top_elem = stack + stack_top--;
                    if (top_elem->ins.opcode == u6a_vo_sa) {
                        text_buffer[text_len].opcode = u6a_vo_sa;
                        stack[stack_top++] = (struct ins_with_offset) {
                            .ins.opcode = u6a_vo_la,
                            .offset = text_len++
                        };
                        break;
                    } else {
                        text_buffer[text_len++] = top_elem->ins;
                        if (top_elem->ins.opcode == u6a_vo_la) {
                            text_buffer[top_elem->offset].operand.offset = htonl(text_len);
                        }
                    }
                }
            }
        }
    }
    uint32_t write_len;
    if (UNLIKELY(!write_bc_header(output_stream, text_len, rodata_len))) {
        write_len = sizeof(struct u6a_bc_header);
        goto codegen_failed;
    }
    WRITE_SECION(text_buffer, sizeof(struct u6a_vm_ins), text_len, output_stream);
    WRITE_SECION(rodata_buffer, sizeof(char), rodata_len, output_stream);
    free(bc_buffer);
    free(stack);
    u6a_info_verbose(info_codegen, "completed, text: %" PRIu32 ", rodata: %" PRIu32, text_len, rodata_len);
    return true;

    codegen_failed:
    u6a_err_write_failed(err_codegen, write_len, file_name);
    free(bc_buffer);
    free(stack);
    return false;
}
