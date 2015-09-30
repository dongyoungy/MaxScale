%{
#include <parser.h>
struct parser_token_t* create_token(int type, char* val);
struct parser_token_t* alloc_token();
void begin_substring(struct parser_token_t** tok);
void end_substring(struct parser_token_t** tok);
void free_token(struct parser_token_t*);
void free_all_tokens(struct parser_token_t*);
void link_tokens(struct parser_token_t** a, struct parser_token_t* b);
%}
%option noyywrap reentrant
%option extra-type="struct parser_token_t**"

num			[0-9]
alpha 		[a-zA-Z]
alnum 		[a-zA-Z0-9]
sq_str 		['][^']+[']
dq_str 		["][^"]+["]
float		{num}+[.]{num}+
posix	    [a-zA-Z0-9._-]
keyvalue1	{posix}+=['][^']+[']
keyvalue2	{posix}+=["][^"]+["]
keyvalue3	{posix}+={posix}+
beginparen  [(]
endparen    [)]
%%

{beginparen}    {
                begin_substring(yyextra);
                }

{endparen}      {
                end_substring(yyextra);
                }

{dq_str}	{
            struct parser_token_t* tok = create_token(PARSER_QUOTED_STRING,yytext);
            link_tokens(yyextra,tok);
            }

{sq_str}	{
            struct parser_token_t* tok = create_token(PARSER_QUOTED_STRING,yytext);
            link_tokens(yyextra,tok);
            }

[.]?"/"({posix}+|"/")+ {
            struct parser_token_t* tok = create_token(PARSER_ABS_PATH,yytext);
            link_tokens(yyextra,tok);
            }

{keyvalue1}	{
            struct parser_token_t* tok = create_token(PARSER_PAIR,yytext);
            link_tokens(yyextra,tok);
            }

{keyvalue2}	{
            struct parser_token_t* tok = create_token(PARSER_PAIR,yytext);
            link_tokens(yyextra,tok);
            }

{keyvalue3}	{
            struct parser_token_t* tok = create_token(PARSER_PAIR,yytext);
            link_tokens(yyextra,tok);
            }

{float}		{
            struct parser_token_t* tok = create_token(PARSER_FLOAT,yytext);
            link_tokens(yyextra,tok);
            }

{num}+      {
            struct parser_token_t* tok = create_token(PARSER_INT,yytext);
            link_tokens(yyextra,tok);
            }

{posix}+	{
            struct parser_token_t* tok = create_token(PARSER_STRING,yytext);
            link_tokens(yyextra,tok);
            }
.

%%

int tokenize_string(struct parser_token_t** tok, char* param, size_t len)
{
	yyscan_t scanner;
	struct parser_token_t* head = NULL;
	YY_BUFFER_STATE buf;
	yylex_init_extra(&head,&scanner);
	buf = yy_scan_bytes(param,len,scanner);
	yylex(scanner);
	yy_delete_buffer(buf,scanner);
	yylex_destroy(scanner);
    *tok = head->head;
	return 0;
}
