/*
 * This file is distributed as part of the MariaDB Corporation MaxScale.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright MariaDB Corporation Ab 2015
 */

#ifndef _PARSER_H
#define _PARSER_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
enum {
    PARSER_UNDEFINED,
    PARSER_STRING,
    PARSER_QUOTED_STRING,
    PARSER_INT,
    PARSER_FLOAT,
    PARSER_PAIR,
    PARSER_ABS_PATH,
    PARSER_SUBSTRING
};

struct pair{
    char* key;
    char* value;
};

struct parser_token_t{
    int type;
    union tokval{
        struct pair pairval;
        char* stringval;
        int intval;
        double floatval;
        struct parser_token_t* substring;
    }value;
    struct parser_token_t* next;
    struct parser_token_t* head;
    struct parser_token_t* parent;
};


int tokenize_string(struct parser_token_t** tok, char* param, size_t len);
char* parser_get_keyvalue(struct parser_token_t* head, char* token);
bool parser_has_string(struct parser_token_t* head, char* token);
void free_all_tokens(struct parser_token_t* tok);
void print_all_tokens(FILE* dest,struct parser_token_t* head);
#endif
