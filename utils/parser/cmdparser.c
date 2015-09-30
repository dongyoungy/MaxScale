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
