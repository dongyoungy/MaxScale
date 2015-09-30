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

#include <stdio.h>
#include <string.h>
#include <parser.h>

int main(int argc, char** argv)
{
    struct parser_token_t* head = NULL;
    char* argstr;
    int arglen = 1;

    if(argc < 2)
    {
        printf("Usage: %s STRING\n",argv[0]);
        return 1;
    }

    for(int i = 1;i<argc;i++)
    {
        arglen += strlen(argv[i]) + 1;
    }

    argstr = malloc(arglen);
    *argstr = '\0';


    for(int i = 1;i<argc;i++)
    {
        strcat(argstr,argv[i]);
        strcat(argstr," ");
    }
    argstr[strlen(argstr)-1] = '\0';
    tokenize_string(&head,(char*)argstr,strlen(argstr));
    if(head == NULL)
    {
        printf("Error: parsing failed.\n");
        return 1;
    }
    print_all_tokens(stdout,head);
    free_all_tokens(head);
 
    return 0;
}
