/*-------------------------------------------------------------------------
 *
 * pl_scanner.c
 *	  lexical scanning for PL/iSQL
 *
 *
 * Portions Copyright (c) 2023-2025, IvorySQL Global Development Team
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/pl/plisql/src/pl_scanner.c
 *
 * add the file for requirement "SQL PARSER"
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "oracle_parser/ora_scanner.h"

#include "plisql.h"
#include "pl_subproc_function.h"
#include "pl_gram.h"			/* must be after parser/scanner.h */
#include "parser/parse_param.h"
#include "catalog/pg_proc.h"


/* Klugy flag to tell scanner how to look up identifiers */
IdentifierLookup plisql_IdentifierLookup = IDENTIFIER_LOOKUP_NORMAL;

/*
 * A word about keywords:
 *
 * We keep reserved and unreserved keywords in separate headers.  Be careful
 * not to put the same word in both headers.  Also be sure that pl_gram.y's
 * unreserved_keyword production agrees with the unreserved header.  The
 * reserved keywords are passed to the core scanner, so they will be
 * recognized before (and instead of) any variable name.  Unreserved words
 * are checked for separately, usually after determining that the identifier
 * isn't a known variable name.  If plisql_IdentifierLookup is DECLARE then
 * no variable names will be recognized, so the unreserved words always work.
 * (Note in particular that this helps us avoid reserving keywords that are
 * only needed in DECLARE sections.)
 *
 * In certain contexts it is desirable to prefer recognizing an unreserved
 * keyword over recognizing a variable name.  In particular, at the start
 * of a statement we should prefer unreserved keywords unless the statement
 * looks like an assignment (i.e., first token is followed by ':=' or '[').
 * This rule allows most statement-introducing keywords to be kept unreserved.
 * (We still have to reserve initial keywords that might follow a block
 * label, unfortunately, since the method used to determine if we are at
 * start of statement doesn't recognize such cases.  We'd also have to
 * reserve any keyword that could legitimately be followed by ':=' or '['.)
 * Some additional cases are handled in pl_gram.y using tok_is_keyword().
 *
 * We try to avoid reserving more keywords than we have to; but there's
 * little point in not reserving a word if it's reserved in the core grammar.
 * Currently, the following words are reserved here but not in the core:
 * BEGIN BY DECLARE EXECUTE FOREACH IF LOOP STRICT WHILE
 */

/* ScanKeywordList lookup data for PL/iSQL keywords */
#include "pl_reserved_kwlist_d.h"
#include "pl_unreserved_kwlist_d.h"

/* Token codes for PL/iSQL keywords */
#define PG_KEYWORD(kwname, value) value,

static const uint16 ReservedPLKeywordTokens[] = {
#include "pl_reserved_kwlist.h"
};

static const uint16 UnreservedPLKeywordTokens[] = {
#include "pl_unreserved_kwlist.h"
};

#undef PG_KEYWORD

/*
 * This macro must recognize all tokens that can immediately precede a
 * PL/iSQL executable statement (that is, proc_sect or proc_stmt in the
 * grammar).  Fortunately, there are not very many, so hard-coding in this
 * fashion seems sufficient.
 */
#define AT_STMT_START(prev_token) \
	((prev_token) == ';' || \
	 (prev_token) == K_BEGIN || \
	 (prev_token) == K_THEN || \
	 (prev_token) == K_ELSE || \
	 (prev_token) == K_LOOP)


/* Auxiliary data about a token (other than the token type) */
typedef struct
{
	YYSTYPE		lval;			/* semantic information */
	YYLTYPE		lloc;			/* offset in scanbuf */
	int			leng;			/* length in bytes */
} TokenAuxData;

/*
 * Scanner working state.  At some point we might wish to fold all this
 * into a YY_EXTRA struct.  For the moment, there is no need for plisql's
 * lexer to be re-entrant, and the notational burden of passing a yyscanner
 * pointer around is great enough to not want to do it without need.
 */

/* The stuff the core lexer needs */
static ora_core_yyscan_t yyscanner = NULL;
static ora_core_yy_extra_type core_yy;

/* The original input string */
static const char *scanorig;

/* Current token's length (corresponds to plisql_yylval and plisql_yylloc) */
static int	plisql_yyleng;

/* Current token's code (corresponds to plisql_yylval and plisql_yylloc) */
static int	plisql_yytoken;

/* Token pushback stack */
#define MAX_PUSHBACKS 4

static int	num_pushbacks;
static int	pushback_token[MAX_PUSHBACKS];
static TokenAuxData pushback_auxdata[MAX_PUSHBACKS];

/* State for plisql_location_to_lineno() */
static const char *cur_line_start;
static const char *cur_line_end;
static int	cur_line_num;

/*
 * yylex used global variable in pl_scanner.c
 */
typedef struct PLiSQL_yylex_global_proper
{
	ora_core_yyscan_t yyscanner;
	ora_core_yy_extra_type core_yy;

	/* The original input string */
	const char *scanorig;

	/* Current token's length (corresponds to plsql_yylval and plsql_yylloc) */
	int	plisql_yyleng;

	/* Current token's code (corresponds to plsql_yylval and plsql_yylloc) */
	int	plisql_yytoken;

	int	num_pushbacks;
	int	pushback_token[MAX_PUSHBACKS];
	TokenAuxData pushback_auxdata[MAX_PUSHBACKS];

	/* from pl_gram.y */
	YYSTYPE	plisql_yylval;
	YYLTYPE	plisql_yylloc;

	/* State for plsql_location_to_lineno() */
	const char *cur_line_start;
	const char *cur_line_end;
	int	cur_line_num;
} PLiSQL_yylex_global_proper;

/* Internal functions */
static int	internal_yylex(TokenAuxData *auxdata);
static void push_back_token(int token, TokenAuxData *auxdata);
static void location_lineno_init(void);


/*
 * This is the yylex routine called from the PL/iSQL grammar.
 * It is a wrapper around the core lexer, with the ability to recognize
 * PL/iSQL variables and return them as special T_DATUM tokens.  If a
 * word or compound word does not match any variable name, or if matching
 * is turned off by plisql_IdentifierLookup, it is returned as
 * T_WORD or T_CWORD respectively, or as an unreserved keyword if it
 * matches one of those.
 */
int
plisql_yylex(void)
{
	int			tok1;
	TokenAuxData aux1;
	int			kwnum;
	char        buf[32];
	char		*paramname;

	tok1 = internal_yylex(&aux1);
	if (tok1 == IDENT || tok1 == PARAM || tok1 == ORAPARAM)
	{
		int			tok2;
		TokenAuxData aux2;

		paramname = aux1.lval.str;
		if (tok1 == ORAPARAM)
		{
			int num;

			/*
			 *  Similar to the following syntax:
			 *****************************************
			 * do $$
			 * begin
			 *   :x = 23;
			 *   :y = 'xiexie';
			 * end; using y inout, x inout;
			 ******************************************
			 * the origin strings :x or :y are treated as the parame name
			 */
			if (plisql_curr_compile->paramnames != NULL)
			{
				int i;

				for (i = 0; i < plisql_curr_compile->fn_nargs; i++)
					if (plisql_curr_compile->paramnames[i] != NULL &&
						strcmp(plisql_curr_compile->paramnames[i], aux1.lval.str) == 0)
						break;
				if (i != plisql_curr_compile->fn_nargs)
				{
					sprintf(buf, "%s", plisql_curr_compile->paramnames[i]);
				}
				else
				{
					num = calculate_oraparamnumber(aux1.lval.str);
					sprintf(buf, "$%d", num);
				}
			}
			else
			{
				num = calculate_oraparamnumber(aux1.lval.str);
				sprintf(buf, "$%d", num);
			}
			paramname = buf;

			/*
			 * If the compiled function is PROKIND_ANONYMOUS_BLOCK_ONLY_PARSE,
			 * we build variables dynamically.
			 */
			if (plisql_curr_compile->fn_prokind == PROKIND_ANONYMOUS_BLOCK_ONLY_PARSE)
				dynamic_build_func_vars(&plisql_curr_compile);
		}

		tok2 = internal_yylex(&aux2);
		if (tok2 == '.')
		{
			int			tok3;
			TokenAuxData aux3;

			tok3 = internal_yylex(&aux3);
			if (tok3 == IDENT)
			{
				int			tok4;
				TokenAuxData aux4;

				tok4 = internal_yylex(&aux4);
				if (tok4 == '.')
				{
					int			tok5;
					TokenAuxData aux5;

					tok5 = internal_yylex(&aux5);
					if (tok5 == IDENT)
					{
						if (plisql_parse_tripword(paramname,
												   aux1.lval.str,
												   aux3.lval.str,
												   aux5.lval.str,
												   &aux1.lval.wdatum,
												   &aux1.lval.cword))
							tok1 = T_DATUM;
						else
							tok1 = T_CWORD;
						/* Adjust token length to include A.B.C */
						aux1.leng = aux5.lloc - aux1.lloc + aux5.leng;
					}
					else
					{
						/* not A.B.C, so just process A.B */
						push_back_token(tok5, &aux5);
						push_back_token(tok4, &aux4);
						if (plisql_parse_dblword(paramname,
												  aux1.lval.str,
												  aux3.lval.str,
												  &aux1.lval.wdatum,
												  &aux1.lval.cword))
							tok1 = T_DATUM;
						else
							tok1 = T_CWORD;
						/* Adjust token length to include A.B */
						aux1.leng = aux3.lloc - aux1.lloc + aux3.leng;
					}
				}
				else
				{
					/* not A.B.C, so just process A.B */
					push_back_token(tok4, &aux4);
					if (plisql_parse_dblword(paramname,
											  aux1.lval.str,
											  aux3.lval.str,
											  &aux1.lval.wdatum,
											  &aux1.lval.cword))
						tok1 = T_DATUM;
					else
						tok1 = T_CWORD;
					/* Adjust token length to include A.B */
					aux1.leng = aux3.lloc - aux1.lloc + aux3.leng;
				}
			}
			else
			{
				/* not A.B, so just process A */
				push_back_token(tok3, &aux3);
				push_back_token(tok2, &aux2);
				if (plisql_parse_word(paramname,
									   aux1.lval.str,
									   core_yy.scanbuf + aux1.lloc,
									   true,
									   &aux1.lval.wdatum,
									   &aux1.lval.word))
					tok1 = T_DATUM;
				else if (!aux1.lval.word.quoted &&
						 (kwnum = ScanKeywordLookup(aux1.lval.word.ident,
													&UnreservedPLKeywords)) >= 0)
				{
					aux1.lval.keyword = GetScanKeyword(kwnum,
													   &UnreservedPLKeywords);
					tok1 = UnreservedPLKeywordTokens[kwnum];
				}
				else
					tok1 = T_WORD;
			}
		}
		else
		{
			/* not A.B, so just process A */
			push_back_token(tok2, &aux2);

			/*
			 * See if it matches a variable name, except in the context where
			 * we are at start of statement and the next token isn't
			 * assignment or '['.  In that case, it couldn't validly be a
			 * variable name, and skipping the lookup allows variable names to
			 * be used that would conflict with plisql or core keywords that
			 * introduce statements (e.g., "comment").  Without this special
			 * logic, every statement-introducing keyword would effectively be
			 * reserved in PL/iSQL, which would be unpleasant.
			 *
			 * If it isn't a variable name, try to match against unreserved
			 * plisql keywords.  If not one of those either, it's T_WORD.
			 *
			 * Note: we must call plisql_parse_word even if we don't want to
			 * do variable lookup, because it sets up aux1.lval.word for the
			 * non-variable cases.
			 */
			if (plisql_parse_word(paramname,
								   aux1.lval.str,
								   core_yy.scanbuf + aux1.lloc,
								   (!AT_STMT_START(plisql_yytoken) ||
									(tok2 == '=' || tok2 == COLON_EQUALS ||
									 tok2 == '[')),
								   &aux1.lval.wdatum,
								   &aux1.lval.word))
				tok1 = T_DATUM;
			else if (!aux1.lval.word.quoted &&
					 (kwnum = ScanKeywordLookup(aux1.lval.word.ident,
												&UnreservedPLKeywords)) >= 0)
			{
				aux1.lval.keyword = GetScanKeyword(kwnum,
												   &UnreservedPLKeywords);
				tok1 = UnreservedPLKeywordTokens[kwnum];
			}
			else
				tok1 = T_WORD;
		}
	}
	else
	{
		/*
		 * Not a potential plisql variable name, just return the data.
		 *
		 * Note that we also come through here if the grammar pushed back a
		 * T_DATUM, T_CWORD, T_WORD, or unreserved-keyword token returned by a
		 * previous lookup cycle; thus, pushbacks do not incur extra lookup
		 * work, since we'll never do the above code twice for the same token.
		 * This property also makes it safe to rely on the old value of
		 * plisql_yytoken in the is-this-start-of-statement test above.
		 */
	}

	plisql_yylval = aux1.lval;
	plisql_yylloc = aux1.lloc;
	plisql_yyleng = aux1.leng;
	plisql_yytoken = tok1;
	return tok1;
}

/*
 * Return the length of the token last returned by plpgsql_yylex().
 *
 * In the case of compound tokens, the length includes all the parts.
 */
int
plisql_token_length(void)
{
	return plisql_yyleng;
}

/*
 * Internal yylex function.  This wraps the core lexer and adds one feature:
 * a token pushback stack.  We also make a couple of trivial single-token
 * translations from what the core lexer does to what we want, in particular
 * interfacing from the ora_core_YYSTYPE to YYSTYPE union.
 */
static int
internal_yylex(TokenAuxData *auxdata)
{
	int			token;
	const char *yytext;

	if (num_pushbacks > 0)
	{
		num_pushbacks--;
		token = pushback_token[num_pushbacks];
		*auxdata = pushback_auxdata[num_pushbacks];
	}
	else
	{
		token = ora_core_yylex(&auxdata->lval.core_yystype,
						   &auxdata->lloc,
						   yyscanner);

		/* remember the length of yytext before it gets changed */
		yytext = core_yy.scanbuf + auxdata->lloc;
		auxdata->leng = strlen(yytext);

		/* Check for #, which the core considers operators */
		if (token == Op)
		{
#if 0
			if (strcmp(auxdata->lval.str, "<<") == 0)
				token = LESS_LESS;
			else if (strcmp(auxdata->lval.str, ">>") == 0)
				token = GREATER_GREATER;
#endif
			if (strcmp(auxdata->lval.str, "#") == 0)
				token = '#';
		}

		/* The core returns PARAM as ival, but we treat it like IDENT */
		else if (token == PARAM)
		{
			auxdata->lval.str = pstrdup(yytext);
			setdynamic_haspgparam(true);
		}
		else if (token == ORAPARAM)
		{
			calculate_oraparamnumber(auxdata->lval.str);
		}
	}

	return token;
}

/*
 * Push back a token to be re-read by next internal_yylex() call.
 */
static void
push_back_token(int token, TokenAuxData *auxdata)
{
	if (num_pushbacks >= MAX_PUSHBACKS)
		elog(ERROR, "too many tokens pushed back");
	pushback_token[num_pushbacks] = token;
	pushback_auxdata[num_pushbacks] = *auxdata;
	num_pushbacks++;
}

/*
 * Push back a single token to be re-read by next plisql_yylex() call.
 *
 * NOTE: this does not cause yylval or yylloc to "back up".  Also, it
 * is not a good idea to push back a token code other than what you read.
 */
void
plisql_push_back_token(int token)
{
	TokenAuxData auxdata;

	auxdata.lval = plisql_yylval;
	auxdata.lloc = plisql_yylloc;
	auxdata.leng = plisql_yyleng;
	push_back_token(token, &auxdata);
}

/*
 * Tell whether a token is an unreserved keyword.
 *
 * (If it is, its lowercased form was returned as the token value, so we
 * do not need to offer that data here.)
 */
bool
plisql_token_is_unreserved_keyword(int token)
{
	int			i;

	for (i = 0; i < lengthof(UnreservedPLKeywordTokens); i++)
	{
		if (UnreservedPLKeywordTokens[i] == token)
			return true;
	}
	return false;
}

/*
 * Append the function text starting at startlocation and extending to
 * (not including) endlocation onto the existing contents of "buf".
 */
void
plisql_append_source_text(StringInfo buf,
						   int startlocation, int endlocation)
{
	Assert(startlocation <= endlocation);
	appendBinaryStringInfo(buf, scanorig + startlocation,
						   endlocation - startlocation);
}

/*
 * Peek one token ahead in the input stream.  Only the token code is
 * made available, not any of the auxiliary info such as location.
 *
 * NB: no variable or unreserved keyword lookup is performed here, they will
 * be returned as IDENT. Reserved keywords are resolved as usual.
 */
int
plisql_peek(void)
{
	int			tok1;
	TokenAuxData aux1;

	tok1 = internal_yylex(&aux1);
	push_back_token(tok1, &aux1);
	return tok1;
}

/*
 * Peek two tokens ahead in the input stream. The first token and its
 * location in the query are returned in *tok1_p and *tok1_loc, second token
 * and its location in *tok2_p and *tok2_loc.
 *
 * NB: no variable or unreserved keyword lookup is performed here, they will
 * be returned as IDENT. Reserved keywords are resolved as usual.
 */
void
plisql_peek2(int *tok1_p, int *tok2_p, int *tok1_loc, int *tok2_loc)
{
	int			tok1,
				tok2;
	TokenAuxData aux1,
				aux2;

	tok1 = internal_yylex(&aux1);
	tok2 = internal_yylex(&aux2);

	*tok1_p = tok1;
	if (tok1_loc)
		*tok1_loc = aux1.lloc;
	*tok2_p = tok2;
	if (tok2_loc)
		*tok2_loc = aux2.lloc;

	push_back_token(tok2, &aux2);
	push_back_token(tok1, &aux1);
}

/*
 * plisql_scanner_errposition
 *		Report an error cursor position, if possible.
 *
 * This is expected to be used within an ereport() call.  The return value
 * is a dummy (always 0, in fact).
 *
 * Note that this can only be used for messages emitted during initial
 * parsing of a plisql function, since it requires the scanorig string
 * to still be available.
 */
int
plisql_scanner_errposition(int location)
{
	int			pos;

	if (location < 0 || scanorig == NULL)
		return 0;				/* no-op if location is unknown */

	/* Convert byte offset to character number */
	pos = pg_mbstrlen_with_len(scanorig, location) + 1;
	/* And pass it to the ereport mechanism */
	(void) internalerrposition(pos);
	/* Also pass the function body string */
	return internalerrquery(scanorig);
}

/*
 * plisql_yyerror
 *		Report a lexer or grammar error.
 *
 * The message's cursor position refers to the current token (the one
 * last returned by plisql_yylex()).
 * This is OK for syntax error messages from the Bison parser, because Bison
 * parsers report error as soon as the first unparsable token is reached.
 * Beware of using yyerror for other purposes, as the cursor position might
 * be misleading!
 */
void
plisql_yyerror(const char *message)
{
	char	   *yytext = core_yy.scanbuf + plisql_yylloc;

	if (*yytext == '\0')
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/* translator: %s is typically the translation of "syntax error" */
				 errmsg("%s at end of input", _(message)),
				 plisql_scanner_errposition(plisql_yylloc)));
	}
	else
	{
		/*
		 * If we have done any lookahead then flex will have restored the
		 * character after the end-of-token.  Zap it again so that we report
		 * only the single token here.  This modifies scanbuf but we no longer
		 * care about that.
		 */
		yytext[plisql_yyleng] = '\0';

		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/* translator: first %s is typically the translation of "syntax error" */
				 errmsg("%s at or near \"%s\"", _(message), yytext),
				 plisql_scanner_errposition(plisql_yylloc)));
	}
}

/*
 * Given a location (a byte offset in the function source text),
 * return a line number.
 *
 * We expect that this is typically called for a sequence of increasing
 * location values, so optimize accordingly by tracking the endpoints
 * of the "current" line.
 */
int
plisql_location_to_lineno(int location)
{
	const char *loc;

	if (location < 0 || scanorig == NULL)
		return 0;				/* garbage in, garbage out */
	loc = scanorig + location;

	/* be correct, but not fast, if input location goes backwards */
	if (loc < cur_line_start)
		location_lineno_init();

	while (cur_line_end != NULL && loc > cur_line_end)
	{
		cur_line_start = cur_line_end + 1;
		cur_line_num++;
		cur_line_end = strchr(cur_line_start, '\n');
	}

	return cur_line_num;
}

/* initialize or reset the state for plisql_location_to_lineno */
static void
location_lineno_init(void)
{
	cur_line_start = scanorig;
	cur_line_num = 1;

	cur_line_end = strchr(cur_line_start, '\n');
}

/* return the most recently computed lineno */
int
plisql_latest_lineno(void)
{
	return cur_line_num;
}


/*
 * Called before any actual parsing is done
 *
 * Note: the passed "str" must remain valid until plisql_scanner_finish().
 * Although it is not fed directly to flex, we need the original string
 * to cite in error messages.
 */
void
plisql_scanner_init(const char *str)
{
	/* Start up the core scanner */
	yyscanner = ora_scanner_init(str, &core_yy,
							 &ReservedPLKeywords, ReservedPLKeywordTokens);

	/*
	 * scanorig points to the original string, which unlike the scanner's
	 * scanbuf won't be modified on-the-fly by flex.  Notice that although
	 * yytext points into scanbuf, we rely on being able to apply locations
	 * (offsets from string start) to scanorig as well.
	 */
	scanorig = str;

	/* Other setup */
	plisql_IdentifierLookup = IDENTIFIER_LOOKUP_NORMAL;
	plisql_yytoken = 0;

	num_pushbacks = 0;

	location_lineno_init();
}

/*
 * Called after parsing is done to clean up after plisql_scanner_init()
 */
void
plisql_scanner_finish(void)
{
	/* release storage */
	ora_scanner_finish(yyscanner);
	/* avoid leaving any dangling pointers */
	yyscanner = NULL;
	scanorig = NULL;
}

/*
 * saved yylex global variable
 */
void *
plisql_get_yylex_global_proper(void)
{
	PLiSQL_yylex_global_proper *yylex_data;
	int i;

	yylex_data = (PLiSQL_yylex_global_proper *) palloc0(sizeof(PLiSQL_yylex_global_proper));

	yylex_data->core_yy = core_yy;
	yylex_data->cur_line_end = cur_line_end;
	yylex_data->cur_line_num = cur_line_num;
	yylex_data->cur_line_start = cur_line_start;
	yylex_data->plisql_yyleng = plisql_yyleng;
	yylex_data->plisql_yytoken = plisql_yytoken;

	yylex_data->num_pushbacks = num_pushbacks;
	for (i = 0; i < num_pushbacks; i++)
	{
		yylex_data->pushback_auxdata[i] = pushback_auxdata[i];
		yylex_data->pushback_token[i] = pushback_token[i];
	}

	yylex_data->scanorig = scanorig;
	yylex_data->yyscanner = yyscanner;
	yylex_data->plisql_yylval = plisql_yylval;
	yylex_data->plisql_yylloc = plisql_yylloc;

	return (void *) yylex_data;
}

/*
 * recover yylex data
 */
void
plisql_recover_yylex_global_proper(void *value)
{
	PLiSQL_yylex_global_proper *yylex_data;
	int i;

	Assert(value != NULL);

	yylex_data = (PLiSQL_yylex_global_proper *) value;

	core_yy = yylex_data->core_yy;
	cur_line_end = yylex_data->cur_line_end;
	cur_line_num = yylex_data->cur_line_num;
	cur_line_start = yylex_data->cur_line_start;
	plisql_yyleng = yylex_data->plisql_yyleng;
	plisql_yytoken = yylex_data->plisql_yytoken;

	num_pushbacks = yylex_data->num_pushbacks;
	for (i = 0;i < num_pushbacks; i++)
	{
		pushback_auxdata[i] = yylex_data->pushback_auxdata[i];
		pushback_token[i] = yylex_data->pushback_token[i];
	}

	scanorig = yylex_data->scanorig;
	yyscanner = yylex_data->yyscanner;
	plisql_yylval = yylex_data->plisql_yylval;
	plisql_yylloc = yylex_data->plisql_yylloc;

	return;
}

