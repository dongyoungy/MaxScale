#include <parser.h>

#define TEST(a,b) if(!(a)){printf(b"\n");exit(1);}

int main(int argc, char** argv)
{
    const char* test1 = "This is a test.";
    const char* test2 = "'this is a quoted string'";
    const char* test3 = "\"this is a quoted string\"";
    const char* test4 = "/this/is/a/path/to/source.c";
    const char* test5 = "123456";
    const char* test6 = "123456.654321";
    const char* test7 = "key=value";
    const char* test8 = "disable_sescmd_history=true,connection_limit=300,optimize_wildard=false,option_value";
    const char* test9 = "router_options(disable_sescmd_history=true,connection_limit=300,optimize_wildard=false,disable (log_trace,log_debug,log_error) enable (log_error (error_verbose,error_fatal) ) ) ";
    const char* test10 = "this (has (many (sub (strings (in (a (single (string)))))))) ";

    struct parser_token_t* head = NULL;
    struct parser_token_t* ptr = NULL;
    int i;

    /** Test 1 */
    tokenize_string(&head,(char*)test1,strlen(test1));
    TEST(head != NULL, "Parsing failed.");
    ptr = head;
    for(i=0;i<4 && ptr != NULL;i++)
    {
        TEST(ptr->type == PARSER_STRING,"Token was of wrong type.");
        ptr = ptr->next;
    }
    free_all_tokens(head);

    /** Test 2 */
    tokenize_string(&head,(char*)test2,strlen(test2));
    TEST(head != NULL,"Parsing failed.");
    TEST(head->type == PARSER_QUOTED_STRING,"Single-quoted string was not detected.");
    free_all_tokens(head);

    /** Test 3 */
    tokenize_string(&head,(char*)test3,strlen(test3));
    TEST(head != NULL,"Parsing failed.");
    TEST(head->type == PARSER_QUOTED_STRING,"Double-quoted string was not detected.");
    free_all_tokens(head);

    /** Test 4 */
    tokenize_string(&head,(char*)test4,strlen(test4));
    TEST(head != NULL,"Parsing failed.");
    TEST(head->type == PARSER_ABS_PATH,"Absolute path was not detected.");
    free_all_tokens(head);

    /** Test 5 */
    tokenize_string(&head,(char*)test5,strlen(test5));
    TEST(head != NULL,"Parsing failed.");
    TEST(head->type == PARSER_INT,"Integer value was not detected.");
    free_all_tokens(head);

    /** Test 6 */
    tokenize_string(&head,(char*)test6,strlen(test6));
    TEST(head != NULL,"Parsing failed.");
    TEST(head->type == PARSER_FLOAT,"Floating point value was not detected.");
    free_all_tokens(head);

    /** Test 7 */
    tokenize_string(&head,(char*)test7,strlen(test7));
    TEST(head != NULL,"Parsing failed.");
    TEST(head->type == PARSER_PAIR,"Key-value pair was not detected.");
    free_all_tokens(head);

    /** Test 8 */
    tokenize_string(&head,(char*)test8,strlen(test8));
    TEST(head != NULL,"Parsing failed.");
    TEST(parser_get_keyvalue(head,"disable_sescmd_history") != NULL,"\"disable_sescmd_history=true\" was not detected.");
    TEST(strcmp(parser_get_keyvalue(head,"disable_sescmd_history"),"true") == 0,"\"disable_sescmd_history=true\" returned wrong value.");
    TEST(parser_get_keyvalue(head,"connection_limit") != NULL,"\"connection_limit=300\" was not detected.");
    TEST(strcmp(parser_get_keyvalue(head,"connection_limit"),"300") == 0,"\"connection_limit=300\" returned wrong value.");
    TEST(parser_get_keyvalue(head,"optimize_wildard") != NULL,"\"optimize_wildard=false\" was not detected.");
    TEST(strcmp(parser_get_keyvalue(head,"optimize_wildard"),"false") == 0,"\"optimize_wildard=false\" returned wrong value.");
    free_all_tokens(head);

    /** Test 9 */
    tokenize_string(&head,(char*)test9,strlen(test9));
    TEST(head != NULL,"Parsing failed.");
    print_all_tokens(stdout,head);
    free_all_tokens(head);    

    /** Test 10 */
    printf("\n");
    tokenize_string(&head,(char*)test10,strlen(test10));
    TEST(head != NULL,"Parsing failed.");
    print_all_tokens(stdout,head);
    free_all_tokens(head);    
    return 0;
}
