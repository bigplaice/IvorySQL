/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/mainloop.c
 */
#include "postgres_fe.h"

#include "command.h"
#include "common.h"
#include "common/logging.h"
#include "input.h"
#include "mainloop.h"
#include "mb/pg_wchar.h"
#include "prompt.h"
#include "psqlplus.h"
#include "settings.h"
#include "stringutils.h"
#include "variables.h"
#include "oracle_fe_utils/ora_string_utils.h"
#include "oracle_fe_utils/ora_psqlscan.h"
#include "fe_utils/psqlscan_int.h"
#include "ora_prompt.h"

/* callback functions for our flex lexer */
const PsqlScanCallbacks psqlscan_callbacks = {
	psql_get_variable,
};

const Ora_psqlScanCallbacks Ora_psqlscan_callbacks = {
	ora_psql_get_variable,
};

/*
 * Main processing loop for reading lines of input
 *	and sending them to the backend.
 *
 * This loop is re-entrant. May be called by \i command
 *	which reads input from a file.
 */
int
MainLoop(FILE *source)
{
	PsqlScanState scan_state;	/* lexer working state */
	PsqlScanState ora_scan_state;	/* oracle lexer working state */
	ConditionalStack cond_stack;	/* \if status stack */
	volatile PQExpBuffer query_buf; /* buffer for query being accumulated */
	volatile PQExpBuffer previous_buf;	/* if there isn't anything in the new
										 * buffer yet, use this one for \e,
										 * etc. */
	PQExpBuffer history_buf;	/* earlier lines of a multi-line command, not
								 * yet saved to readline history */
	char	   *line;			/* current line of input */
	int			added_nl_pos;
	bool		success;
	bool		line_saved_in_history;
	volatile int successResult = EXIT_SUCCESS;
	volatile backslashResult slashCmdStatus = PSQL_CMD_UNKNOWN;
	volatile promptStatus_t prompt_status = PROMPT_READY;
	volatile bool need_redisplay = false;
	volatile int count_eof = 0;
	volatile bool die_on_error = false;
	FILE	   *prev_cmd_source;
	bool		prev_cmd_interactive;
	uint64		prev_lineno;

	/* Save the prior command source */
	prev_cmd_source = pset.cur_cmd_source;
	prev_cmd_interactive = pset.cur_cmd_interactive;
	prev_lineno = pset.lineno;
	/* pset.stmt_lineno does not need to be saved and restored */

	/* Establish new source */
	pset.cur_cmd_source = source;
	pset.cur_cmd_interactive = ((source == stdin) && !pset.notty);
	pset.lineno = 0;
	pset.stmt_lineno = 1;

	/* Create working state */
	scan_state = psql_scan_create(&psqlscan_callbacks);
	cond_stack = conditional_stack_create();
	psql_scan_set_passthrough(scan_state, (void *) cond_stack);

	/* Oracle working state */
	ora_scan_state = ora_psql_scan_create(&Ora_psqlscan_callbacks);
	ora_psql_scan_set_passthrough(ora_scan_state, (void *) cond_stack);

	query_buf = createPQExpBuffer();
	previous_buf = createPQExpBuffer();
	history_buf = createPQExpBuffer();
	if (PQExpBufferBroken(query_buf) ||
		PQExpBufferBroken(previous_buf) ||
		PQExpBufferBroken(history_buf))
		pg_fatal("out of memory");

	/* main loop to get queries and execute them */
	while (successResult == EXIT_SUCCESS)
	{
		/*
		 * Clean up after a previous Control-C
		 */
		if (cancel_pressed)
		{
			if (!pset.cur_cmd_interactive)
			{
				/*
				 * You get here if you stopped a script with Ctrl-C.
				 */
				successResult = EXIT_USER;
				break;
			}

			cancel_pressed = false;
		}

		/*
		 * Establish longjmp destination for exiting from wait-for-input. We
		 * must re-do this each time through the loop for safety, since the
		 * jmpbuf might get changed during command execution.
		 */
		if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
		{
			/* got here with longjmp */

			/* reset parsing state */
			psql_scan_finish(scan_state);
			psql_scan_reset(scan_state);

			/* reset oracle parsing state */
			ora_psql_scan_finish(ora_scan_state);
			ora_psql_scan_reset(ora_scan_state);

			resetPQExpBuffer(query_buf);
			resetPQExpBuffer(history_buf);
			count_eof = 0;
			slashCmdStatus = PSQL_CMD_UNKNOWN;
			prompt_status = PROMPT_READY;
			need_redisplay = false;
			pset.stmt_lineno = 1;
			cancel_pressed = false;

			if (pset.cur_cmd_interactive)
			{
				putc('\n', stdout);

				/*
				 * if interactive user is in an \if block, then Ctrl-C will
				 * exit from the innermost \if.
				 */
				if (!conditional_stack_empty(cond_stack))
				{
					pg_log_error("\\if: escaped");
					conditional_stack_pop(cond_stack);
				}
			}
			else
			{
				successResult = EXIT_USER;
				break;
			}
		}

		fflush(stdout);

		/*
		 * get another line
		 */
		if (pset.cur_cmd_interactive)
		{
			/* May need to reset prompt, eg after \r command */
			if (query_buf->len == 0)
			{
				if (db_mode == DB_ORACLE)
					prompt_status = ORAPROMPT_READY;
				else
					prompt_status = PROMPT_READY;
			}
			/* If query buffer came from \e, redisplay it with a prompt */
			if (need_redisplay)
			{
				if (query_buf->len > 0)
				{
					if (db_mode == DB_ORACLE)
						fputs(ora_get_prompt(ORAPROMPT_READY, cond_stack), stdout);
					else
						fputs(get_prompt(PROMPT_READY, cond_stack), stdout);
					fputs(query_buf->data, stdout);
					fflush(stdout);
				}
				need_redisplay = false;
			}
			/* Now we can fetch a line */
			if (db_mode == DB_ORACLE)
				line = gets_interactive(ora_get_prompt(prompt_status, cond_stack),
									query_buf);
			else
				line = gets_interactive(get_prompt(prompt_status, cond_stack),
									query_buf);
		}
		else
		{
			line = gets_fromFile(source);
			if (!line && ferror(source))
				successResult = EXIT_FAILURE;
		}

		/*
		 * query_buf holds query already accumulated.  line is the malloc'd
		 * new line of input (note it must be freed before looping around!)
		 */

		/* No more input.  Time to quit, or \i done */
		if (line == NULL)
		{
			if (pset.cur_cmd_interactive)
			{
				/* This tries to mimic bash's IGNOREEOF feature. */
				count_eof++;

				if (count_eof < pset.ignoreeof)
				{
					if (!pset.quiet)
						printf(_("Use \"\\q\" to leave %s.\n"), pset.progname);
					continue;
				}

				puts(pset.quiet ? "" : "\\q");
			}
			break;
		}

		count_eof = 0;

		pset.lineno++;

		/* ignore UTF-8 Unicode byte-order mark */
		if (pset.lineno == 1 && pset.encoding == PG_UTF8 && strncmp(line, "\xef\xbb\xbf", 3) == 0)
			memmove(line, line + 3, strlen(line + 3) + 1);

		/* Detect attempts to run custom-format dumps as SQL scripts */
		if (pset.lineno == 1 && !pset.cur_cmd_interactive &&
			strncmp(line, "PGDMP", 5) == 0)
		{
			free(line);
			puts(_("The input is a PostgreSQL custom-format dump.\n"
				   "Use the pg_restore command-line client to restore this dump to a database.\n"));
			fflush(stdout);
			successResult = EXIT_FAILURE;
			break;
		}

		/* no further processing of empty lines, unless within a literal */
		if (db_mode == DB_PG)
		{
			if (line[0] == '\0' && !psql_scan_in_quote(scan_state))
			{
				free(line);
				continue;
			}
		}
		else if (db_mode == DB_ORACLE)
		{
			if (line[0] == '\0' && !ora_psql_scan_in_quote(ora_scan_state))
			{
				free(line);
				continue;
			}
		}

		/* Recognize "help", "quit", "exit" only in interactive mode */
		if (pset.cur_cmd_interactive)
		{
			char	   *first_word = line;
			char	   *rest_of_line = NULL;
			bool		found_help = false;
			bool		found_exit_or_quit = false;
			bool		found_q = false;

			/*
			 * The assistance words, help/exit/quit, must have no whitespace
			 * before them, and only whitespace after, with an optional
			 * semicolon.  This prevents indented use of these words, perhaps
			 * as identifiers, from invoking the assistance behavior.
			 */
			if (pg_strncasecmp(first_word, "help", 4) == 0)
			{
				rest_of_line = first_word + 4;
				found_help = true;
			}
			else if (pg_strncasecmp(first_word, "exit", 4) == 0 ||
					 pg_strncasecmp(first_word, "quit", 4) == 0)
			{
				rest_of_line = first_word + 4;
				found_exit_or_quit = true;
			}
			else if (strncmp(first_word, "\\q", 2) == 0)
			{
				rest_of_line = first_word + 2;
				found_q = true;
			}

			/*
			 * If we found a command word, check whether the rest of the line
			 * contains only whitespace plus maybe one semicolon.  If not,
			 * ignore the command word after all.  These commands are only for
			 * compatibility with other SQL clients and are not documented.
			 */
			if (rest_of_line != NULL)
			{
				/*
				 * Ignore unless rest of line is whitespace, plus maybe one
				 * semicolon
				 */
				while (isspace((unsigned char) *rest_of_line))
					++rest_of_line;
				if (*rest_of_line == ';')
					++rest_of_line;
				while (isspace((unsigned char) *rest_of_line))
					++rest_of_line;
				if (*rest_of_line != '\0')
				{
					found_help = false;
					found_exit_or_quit = false;
				}
			}

			/*
			 * "help" is only a command when the query buffer is empty, but we
			 * emit a one-line message even when it isn't to help confused
			 * users.  The text is still added to the query buffer in that
			 * case.
			 */
			if (found_help)
			{
				if (query_buf->len != 0)
#ifndef WIN32
					puts(_("Use \\? for help or press control-C to clear the input buffer."));
#else
					puts(_("Use \\? for help."));
#endif
				else
				{
					puts(_("You are using psql, the command-line interface to PostgreSQL."));
					printf(_("Type:  \\copyright for distribution terms\n"
							 "       \\h for help with SQL commands\n"
							 "       \\? for help with psql commands\n"
							 "       \\g or terminate with semicolon to execute query\n"
							 "       \\q to quit\n"));
					free(line);
					fflush(stdout);
					continue;
				}
			}

			/*
			 * "quit" and "exit" are only commands when the query buffer is
			 * empty, but we emit a one-line message even when it isn't to
			 * help confused users.  The text is still added to the query
			 * buffer in that case.
			 */
			if (found_exit_or_quit)
			{
				if (query_buf->len != 0)
				{
					if (prompt_status == PROMPT_READY ||
						prompt_status == PROMPT_CONTINUE ||
						prompt_status == PROMPT_PAREN)
						puts(_("Use \\q to quit."));
					else
#ifndef WIN32
						puts(_("Use control-D to quit."));
#else
						puts(_("Use control-C to quit."));
#endif
				}
				else
				{
					/* exit app */
					free(line);
					fflush(stdout);
					successResult = EXIT_SUCCESS;
					break;
				}
			}

			/*
			 * If they typed "\q" in a place where "\q" is not active, supply
			 * a hint.  The text is still added to the query buffer.
			 */
			if (found_q && query_buf->len != 0 &&
				prompt_status != PROMPT_READY &&
				prompt_status != PROMPT_CONTINUE &&
				prompt_status != PROMPT_PAREN)
#ifndef WIN32
				puts(_("Use control-D to quit."));
#else
				puts(_("Use control-C to quit."));
#endif
		}

		/* echo back if flag is set, unless interactive */
		if (pset.echo == PSQL_ECHO_ALL && !pset.cur_cmd_interactive)
		{
			puts(line);
			fflush(stdout);
		}

		/* insert newlines into query buffer between source lines */
		if (query_buf->len > 0)
		{
			appendPQExpBufferChar(query_buf, '\n');
			added_nl_pos = query_buf->len;
		}
		else
			added_nl_pos = -1;	/* flag we didn't add one */

		/* Setting this will not have effect until next line. */
		die_on_error = pset.on_error_stop;

		/*
		 * Parse line, looking for command separators.
		 */
		if (db_mode == DB_PG)
		{
			psql_scan_setup(scan_state, line, strlen(line),
								pset.encoding, standard_strings());
		}
		else if (db_mode == DB_ORACLE)
		{
			ora_psql_scan_setup(ora_scan_state, line, strlen(line),
								pset.encoding, standard_strings());
		}
		/*
		 * At present, compatible-oracle client commands are all single-line
		 * commands, so we only scan the input of stmt_lineno = 1, which can
		 * avoid the performance loss caused by the parser invoked by multi-line
		 * commands.
		 *
		 * Note:
		 *	Oracle client command also extends over multiple lines, but
		 *	must using the SQL*Plus command continuation character(eg: -),
		 *	the ontinuation character we haven't implemented it yet, so for
		 *	the time being we're still assuming that these client commands
		 *	are all one-liners.
		 */
		if (db_mode == DB_ORACLE && pset.stmt_lineno == 1)
		{
			PsqlScanState	pstate;
			yyscan_t		yyscanner;
			char			*psqlplus_line = pg_strdup(line);;

			pstate = ora_psql_scan_create(&psqlplus_callbacks);
			ora_psql_scan_setup(pstate, psqlplus_line,
								strlen(psqlplus_line),
								pset.encoding,
								standard_strings());
			
			yyscanner = psqlplus_scanner_init(pstate);
			if (psqlplus_yyparse(yyscanner) == 0)
			{
				/* Parser success, i.e. this is a ora client command */
				psqlplus_cmd	*psqlpluscmd = pstate->psqlpluscmd;
				
				switch(psqlpluscmd->cmd_type)
				{
					case PSQLPLUS_CMD_VARIABLE:
						{
							psqlplus_cmd_var *bind_var = (psqlplus_cmd_var *) pstate->psqlpluscmd;

							if (bind_var->list_bind_var == true)
							{
								if (bind_var->var_name)
									ListBindVariables(pset.vars, bind_var->var_name);
								else
									ListBindVariables(pset.vars, NULL);
							}
							else if (bind_var->miss_termination_quote)
							{
								/*
								 * init_value[0] indicates whether this is a single quote
								 * or a double quote that is missing a terminating quote.
								 */
								char	quote = bind_var->init_value[0];
								char	*str_double_quote = pg_malloc0(strlen(bind_var->init_value) * 2);	/* enough */
								char	*ptr = str_double_quote;
								int		i;

								/* skip first char */
								*ptr++ = bind_var->init_value[0];
								i = 1;

								while (bind_var->init_value[i] != '\0')
								{
									/* double write quote if needed */
									if (bind_var->init_value[i] == quote)
										*ptr++ = bind_var->init_value[i];

									/* copy the character */
									*ptr++ = bind_var->init_value[i];
									i++;
								}

								*ptr = '\0';	/* Paranoid */
								
								/* report an error directly */
								pg_log_error("string \"%s\" missing terminating quote (%c).",
											str_double_quote ,
											quote);
								pg_free(str_double_quote);
							}
							else if (bind_var->assign_bind_var)
							{
								AssignBindVariable(pset.vars,
												bind_var->var_name,
												bind_var->init_value);
							}
							else
							{
								Assert(bind_var->vartype);
								SetBindVariable(pset.vars,
												bind_var->var_name,
												bind_var->vartype->oid,
												bind_var->vartype->typmod,
												bind_var->init_value,
												bind_var->initial_nonnull_value);
							}
						}
						break;
					case PSQLPLUS_CMD_PRINT:
						{
							psqlplus_cmd_print *pb = (psqlplus_cmd_print *) pstate->psqlpluscmd;
							PrintBindVariables(pset.vars, pb->print_items);
						}
						break;
					default:
						pg_log_error("Invalid PSQL*PLUS client command.");
						break;
				}

				/* save client command in history */
				if (pset.cur_cmd_interactive)
				{
					pg_append_history(psqlplus_line, history_buf);
					pg_send_history(history_buf);
				}

				/* reset */
				pset.stmt_lineno = 1;
				resetPQExpBuffer(query_buf);
				psql_scan_finish(scan_state);
				ora_psql_scan_finish(ora_scan_state);
				free(psqlplus_line);
				free(line);
				psqlplus_scanner_finish(yyscanner);
				ora_psql_scan_destroy(pstate);
				continue;
			}

			/* Syntax parsing failed, but we know it's a client command */
			if (pstate->is_sqlplus_cmd)
			{
				char	*token;
				const char	*whitespace = " \t\n\r";

				token = strtokx(pstate->scanline, whitespace, NULL, NULL,
								0, false, false, pset.encoding);

				if (token && pg_strcasecmp(token, "variable") == 0)
				{
					token = strtokx(NULL, whitespace, NULL, NULL,
									0, false, false, pset.encoding);

					/*
					 * keep in sync with 'truncate_char' in psqlplusparse.y
					 */
					if (token)
						token[strcspn(token, ",()")] = '\0';

					/*
					 * Theoretically, this token is the name of a VARIABLE variable.
					 * Check whether the reason for the parsing failure is that the
					 * name is illegal.
					 */
					if (token && !ValidBindVariableName(token))
						pg_log_error("Illegal variable name \"%s\"", token);
					else
						pg_log_error("Usage: VAR[IABLE] [ <variable> [ NUMBER | CHAR | CHAR (n [CHAR|BYTE]) |\n"
							"\t\t\t VARCHAR2 (n [CHAR|BYTE]) | NCHAR | NCHAR (n) |\n"
							"\t\t\t NVARCHAR2 (n) | BINARY_FLOAT | BINARY_DOUBLE ] ]");
				}
				else if (token && pg_strcasecmp(token, "print") == 0)
				{
					print_list	*pl = pg_malloc0(sizeof(print_list));

					pl->items = NULL;
					pl->length = 0;
					
					token = strtokx(NULL, whitespace, NULL, NULL,
									0, false, false, pset.encoding);
					while (token)
					{
						print_item *item;

						pl->length++;

						if (pl->length == 1)
							pl->items = (print_item **) pg_malloc0(sizeof(print_item *) * 1);
						else
							pl->items = (print_item **) pg_realloc(pl->items, sizeof(print_item *) * pl->length);

						/* Strips the leading and trailing quote characters of double quotes */
						if (token[0] == '"' && token[strlen(token) - 1] == '"')
						{
							token[strlen(token) - 1] = '\0';
							token++;
						}

						item = pg_malloc0(sizeof(print_item));
						item->bv_name = pg_strdup(token);
						
						if (ValidBindVariableName(item->bv_name))
							item->valid = true;
						else
							item->valid = false;
						pl->items[pl->length - 1] = item;
						token = strtokx(NULL, whitespace, NULL, NULL,
										0, false, false, pset.encoding);
					}

					if (pl->length == 0)
						PrintBindVariables(pset.vars, NULL);
					else
						PrintBindVariables(pset.vars, pl);

					while (pl->length > 0)
					{
						pl->length--;
						if (pl->items &&
							pl->items[pl->length] &&
							pl->items[pl->length]->bv_name)
						{
							pg_free(pl->items[pl->length]->bv_name);
							pg_free(pl->items[pl->length]);
						}

						if (pl->length == 0)
						{
							pg_free(pl->items);
							pg_free(pl);
							break;
						}
					}
				}

				/* save client command in history */
				if (pset.cur_cmd_interactive)
				{
					pg_append_history(psqlplus_line, history_buf);
					pg_send_history(history_buf);
				}

				/* reset */
				pset.stmt_lineno = 1;
				resetPQExpBuffer(query_buf);
				psql_scan_finish(scan_state);
				ora_psql_scan_finish(ora_scan_state);
				free(psqlplus_line);
				free(line);
				psqlplus_scanner_finish(yyscanner);
				ora_psql_scan_destroy(pstate);
				continue;
			}
			
			/* Not a compatible-oracle client command */
			psqlplus_scanner_finish(yyscanner);
			ora_psql_scan_destroy(pstate);
			free(psqlplus_line);
		}

		success = true;
		line_saved_in_history = false;

		while (success || !die_on_error)
		{
			PsqlScanResult scan_result;
			Ora_PsqlScanResult ora_scan_result; 
			promptStatus_t prompt_tmp = prompt_status;
			Ora_promptStatus_t ora_prompt_tmp = (Ora_promptStatus_t)prompt_status;
			size_t		pos_in_query;
			char	   *tmp_line;

			pos_in_query = query_buf->len;

			if (db_mode == DB_PG)
			{
				if (scan_state->scanbufhandle == NULL)
					break;
				scan_result = psql_scan(scan_state, query_buf, &prompt_tmp);
				prompt_status = prompt_tmp;

				if (PQExpBufferBroken(query_buf))
					pg_fatal("out of memory");

				/*
				 * Increase statement line number counter for each linebreak added
				 * to the query buffer by the last psql_scan() call. There only
				 * will be ones to add when navigating to a statement in
				 * readline's history containing newlines.
				 */
				tmp_line = query_buf->data + pos_in_query;
				while (*tmp_line != '\0')
				{
					if (*(tmp_line++) == '\n')
						pset.stmt_lineno++;
				}

				if (scan_result == PSCAN_EOL)
					pset.stmt_lineno++;

				/*
				 * Send command if semicolon found, or if end of line and we're in
				 * single-line mode.
				 */
				if (scan_result == PSCAN_SEMICOLON ||
					(scan_result == PSCAN_EOL && pset.singleline))
				{
					/*
					 * Save line in history.  We use history_buf to accumulate
					 * multi-line queries into a single history entry.  Note that
					 * history accumulation works on input lines, so it doesn't
					 * matter whether the query will be ignored due to \if.
					 */
					if (pset.cur_cmd_interactive && !line_saved_in_history)
					{
						pg_append_history(line, history_buf);
						pg_send_history(history_buf);
						line_saved_in_history = true;
					}

					/* execute query unless we're in an inactive \if branch */
					if (conditional_active(cond_stack))
					{
						success = SendQuery(query_buf->data);
						slashCmdStatus = success ? PSQL_CMD_SEND : PSQL_CMD_ERROR;
						pset.stmt_lineno = 1;

						/* transfer query to previous_buf by pointer-swapping */
						{
							PQExpBuffer swap_buf = previous_buf;

							previous_buf = query_buf;
							query_buf = swap_buf;
						}
						resetPQExpBuffer(query_buf);

						added_nl_pos = -1;
						/* we need not do psql_scan_reset() here */
					}
					else
					{
						/* if interactive, warn about non-executed query */
						if (pset.cur_cmd_interactive)
							pg_log_error("query ignored; use \\endif or Ctrl-C to exit current \\if block");
						/* fake an OK result for purposes of loop checks */
						success = true;
						slashCmdStatus = PSQL_CMD_SEND;
						pset.stmt_lineno = 1;
						/* note that query_buf doesn't change state */
					}
				}
				else if (scan_result == PSCAN_BACKSLASH)
				{
					/* handle backslash command */

					/*
					 * If we added a newline to query_buf, and nothing else has
					 * been inserted in query_buf by the lexer, then strip off the
					 * newline again.  This avoids any change to query_buf when a
					 * line contains only a backslash command.  Also, in this
					 * situation we force out any previous lines as a separate
					 * history entry; we don't want SQL and backslash commands
					 * intermixed in history if at all possible.
					 */
					if (query_buf->len == added_nl_pos)
					{
						query_buf->data[--query_buf->len] = '\0';
						pg_send_history(history_buf);
					}
					added_nl_pos = -1;

					/* save backslash command in history */
					if (pset.cur_cmd_interactive && !line_saved_in_history)
					{
						pg_append_history(line, history_buf);
						pg_send_history(history_buf);
						line_saved_in_history = true;
					}

					/* execute backslash command */
					slashCmdStatus = HandleSlashCmds(scan_state,
													 cond_stack,
													 query_buf,
													 previous_buf);

					success = slashCmdStatus != PSQL_CMD_ERROR;

					/*
					 * Resetting stmt_lineno after a backslash command isn't
					 * always appropriate, but it's what we've done historically
					 * and there have been few complaints.
					 */
					pset.stmt_lineno = 1;

					if (slashCmdStatus == PSQL_CMD_SEND)
					{
						/* should not see this in inactive branch */
						Assert(conditional_active(cond_stack));

						success = SendQuery(query_buf->data);

						/* transfer query to previous_buf by pointer-swapping */
						{
							PQExpBuffer swap_buf = previous_buf;

							previous_buf = query_buf;
							query_buf = swap_buf;
						}
						resetPQExpBuffer(query_buf);

						/* flush any paren nesting info after forced send */
						psql_scan_reset(scan_state);
					}
					else if (slashCmdStatus == PSQL_CMD_NEWEDIT)
					{
						/* should not see this in inactive branch */
						Assert(conditional_active(cond_stack));
						/* ensure what came back from editing ends in a newline */
						if (query_buf->len > 0 &&
							query_buf->data[query_buf->len - 1] != '\n')
							appendPQExpBufferChar(query_buf, '\n');
						/* rescan query_buf as new input */
						psql_scan_finish(scan_state);
						free(line);
						line = pg_strdup(query_buf->data);
						resetPQExpBuffer(query_buf);
						/* reset parsing state since we are rescanning whole line */
						psql_scan_reset(scan_state);
						psql_scan_setup(scan_state, line, strlen(line),
										pset.encoding, standard_strings());
						line_saved_in_history = false;
						prompt_status = PROMPT_READY;
						/* we'll want to redisplay after parsing what we have */
						need_redisplay = true;
					}
					else if (slashCmdStatus == PSQL_CMD_TERMINATE)
						break;
				}

				/* fall out of loop if lexer reached EOL */
				if (scan_result == PSCAN_INCOMPLETE ||
					scan_result == PSCAN_EOL)
					break;
			}
			else if (db_mode == DB_ORACLE)
			{
				if (ora_scan_state->scanbufhandle == NULL)
					break;
				ora_scan_result = ora_psql_scan(ora_scan_state, query_buf, &ora_prompt_tmp);
				prompt_status = ora_prompt_tmp;

				if (PQExpBufferBroken(query_buf))
					pg_fatal("out of memory");

				/*
				 * Increase statement line number counter for each linebreak added
				 * to the query buffer by the last psql_scan() call. There only
				 * will be ones to add when navigating to a statement in
				 * readline's history containing newlines.
				 */
				tmp_line = query_buf->data + pos_in_query;
				while (*tmp_line != '\0')
				{
					if (*(tmp_line++) == '\n')
						pset.stmt_lineno++;
				}

				if (ora_scan_result == ORAPSCAN_EOL)
					pset.stmt_lineno++;

				/*
				 * Send command if semicolon found, or if end of line and we're in
				 * single-line mode.
				 */
				if (ora_scan_result == ORAPSCAN_SEMICOLON ||
					(ora_scan_result == ORAPSCAN_EOL && pset.singleline))
				{
					/*
					 * Save line in history.  We use history_buf to accumulate
					 * multi-line queries into a single history entry.  Note that
					 * history accumulation works on input lines, so it doesn't
					 * matter whether the query will be ignored due to \if.
					 */
					if (pset.cur_cmd_interactive && !line_saved_in_history)
					{
						pg_append_history(line, history_buf);
						pg_send_history(history_buf);
						line_saved_in_history = true;
					}

					/* execute query unless we're in an inactive \if branch */
					if (conditional_active(cond_stack))
					{
						success = SendQuery(query_buf->data);
						slashCmdStatus = success ? PSQL_CMD_SEND : PSQL_CMD_ERROR;
						pset.stmt_lineno = 1;

						/* transfer query to previous_buf by pointer-swapping */
						{
							PQExpBuffer swap_buf = previous_buf;

							previous_buf = query_buf;
							query_buf = swap_buf;
						}
						resetPQExpBuffer(query_buf);

						added_nl_pos = -1;
						/* we need not do psql_scan_reset() here */
					}
					else
					{
						/* if interactive, warn about non-executed query */
						if (pset.cur_cmd_interactive)
							pg_log_error("query ignored; use \\endif or Ctrl-C to exit current \\if block");
						/* fake an OK result for purposes of loop checks */
						success = true;
						slashCmdStatus = PSQL_CMD_SEND;
						pset.stmt_lineno = 1;
						/* note that query_buf doesn't change state */
					}
				}
				else if (ora_scan_result == ORAPSCAN_BACKSLASH)
				{
					/* handle backslash command */

					/*
					 * If we added a newline to query_buf, and nothing else has
					 * been inserted in query_buf by the lexer, then strip off the
					 * newline again.  This avoids any change to query_buf when a
					 * line contains only a backslash command.  Also, in this
					 * situation we force out any previous lines as a separate
					 * history entry; we don't want SQL and backslash commands
					 * intermixed in history if at all possible.
					 */
					if (query_buf->len == added_nl_pos)
					{
						query_buf->data[--query_buf->len] = '\0';
						pg_send_history(history_buf);
					}
					added_nl_pos = -1;

					/* save backslash command in history */
					if (pset.cur_cmd_interactive && !line_saved_in_history)
					{
						pg_append_history(line, history_buf);
						pg_send_history(history_buf);
						line_saved_in_history = true;
					}

					/* execute backslash command */
					slashCmdStatus = HandleSlashCmds(ora_scan_state,
													 cond_stack,
													 query_buf,
													 previous_buf);

					success = slashCmdStatus != PSQL_CMD_ERROR;

					/*
					 * Resetting stmt_lineno after a backslash command isn't
					 * always appropriate, but it's what we've done historically
					 * and there have been few complaints.
					 */
					pset.stmt_lineno = 1;

					if (slashCmdStatus == PSQL_CMD_SEND)
					{
						/* should not see this in inactive branch */
						Assert(conditional_active(cond_stack));

						success = SendQuery(query_buf->data);

						/* transfer query to previous_buf by pointer-swapping */
						{
							PQExpBuffer swap_buf = previous_buf;

							previous_buf = query_buf;
							query_buf = swap_buf;
						}
						resetPQExpBuffer(query_buf);

						/* flush any paren nesting info after forced send */
						ora_psql_scan_reset(ora_scan_state);
					}
					else if (slashCmdStatus == PSQL_CMD_NEWEDIT)
					{
						/* should not see this in inactive branch */
						Assert(conditional_active(cond_stack));
						/* ensure what came back from editing ends in a newline */
						if (query_buf->len > 0 &&
							query_buf->data[query_buf->len - 1] != '\n')
							appendPQExpBufferChar(query_buf, '\n');
						/* rescan query_buf as new input */
						ora_psql_scan_finish(ora_scan_state);
						free(line);
						line = pg_strdup(query_buf->data);
						resetPQExpBuffer(query_buf);
						/* reset parsing state since we are rescanning whole line */
						ora_psql_scan_reset(ora_scan_state);
						ora_psql_scan_setup(ora_scan_state, line, strlen(line),
										pset.encoding, standard_strings());
						line_saved_in_history = false;
						prompt_status = PROMPT_READY;
						/* we'll want to redisplay after parsing what we have */
						need_redisplay = true;
					}
					else if (slashCmdStatus == PSQL_CMD_TERMINATE)
						break;
				}

				/* fall out of loop if lexer reached EOL */
				if (ora_scan_result == ORAPSCAN_INCOMPLETE ||
					ora_scan_result == ORAPSCAN_EOL)
					break;
			}
		}

		/*
		 * Add line to pending history if we didn't do so already.  Then, if
		 * the query buffer is still empty, flush out any unsent history
		 * entry.  This means that empty lines (containing only whitespace and
		 * perhaps a dash-dash comment) that precede a query will be recorded
		 * as separate history entries, not as part of that query.
		 */
		if (pset.cur_cmd_interactive)
		{
			if (!line_saved_in_history)
				pg_append_history(line, history_buf);
			if (query_buf->len == 0)
				pg_send_history(history_buf);
		}

		psql_scan_finish(scan_state);
		ora_psql_scan_finish(ora_scan_state);
		free(line);

		if (slashCmdStatus == PSQL_CMD_TERMINATE)
		{
			successResult = EXIT_SUCCESS;
			break;
		}

		if (!pset.cur_cmd_interactive)
		{
			if (!success && die_on_error)
				successResult = EXIT_USER;
			/* Have we lost the db connection? */
			else if (!pset.db)
				successResult = EXIT_BADCONN;
		}
	}							/* while !endoffile/session */

	/*
	 * If we have a non-semicolon-terminated query at the end of file, we
	 * process it unless the input source is interactive --- in that case it
	 * seems better to go ahead and quit.  Also skip if this is an error exit.
	 */
	if (query_buf->len > 0 && !pset.cur_cmd_interactive &&
		successResult == EXIT_SUCCESS)
	{
		/* save query in history */
		/* currently unneeded since we don't use this block if interactive */
#ifdef NOT_USED
		if (pset.cur_cmd_interactive)
			pg_send_history(history_buf);
#endif

		/* execute query unless we're in an inactive \if branch */
		if (conditional_active(cond_stack))
		{
			success = SendQuery(query_buf->data);
		}
		else
		{
			if (pset.cur_cmd_interactive)
				pg_log_error("query ignored; use \\endif or Ctrl-C to exit current \\if block");
			success = true;
		}

		if (!success && die_on_error)
			successResult = EXIT_USER;
		else if (pset.db == NULL)
			successResult = EXIT_BADCONN;
	}

	/*
	 * Check for unbalanced \if-\endifs unless user explicitly quit, or the
	 * script is erroring out
	 */
	if (slashCmdStatus != PSQL_CMD_TERMINATE &&
		successResult != EXIT_USER &&
		!conditional_stack_empty(cond_stack))
	{
		pg_log_error("reached EOF without finding closing \\endif(s)");
		if (die_on_error && !pset.cur_cmd_interactive)
			successResult = EXIT_USER;
	}

	/*
	 * Let's just make real sure the SIGINT handler won't try to use
	 * sigint_interrupt_jmp after we exit this routine.  If there is an outer
	 * MainLoop instance, it will reset sigint_interrupt_jmp to point to
	 * itself at the top of its loop, before any further interactive input
	 * happens.
	 */
	sigint_interrupt_enabled = false;

	destroyPQExpBuffer(query_buf);
	destroyPQExpBuffer(previous_buf);
	destroyPQExpBuffer(history_buf);

	psql_scan_destroy(scan_state);
	ora_psql_scan_destroy(ora_scan_state);
	conditional_stack_destroy(cond_stack);

	pset.cur_cmd_source = prev_cmd_source;
	pset.cur_cmd_interactive = prev_cmd_interactive;
	pset.lineno = prev_lineno;

	return successResult;
}								/* MainLoop() */
