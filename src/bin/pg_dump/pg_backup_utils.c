/*-------------------------------------------------------------------------
 *
 * pg_backup_utils.c
 *	Utility routines shared by pg_dump and pg_restore
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_backup_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "pg_backup_utils.h"
#include "parallel.h"

#ifdef WIN32

#ifndef VA_COPY
# ifdef HAVE_VA_COPY
#  define VA_COPY(dest, src) va_copy(dest, src)
# else
#  ifdef HAVE___VA_COPY
#   define VA_COPY(dest, src) __va_copy(dest, src)
#  else
#   define VA_COPY(dest, src) (dest) = (src)
#  endif
# endif
#endif

#define INIT_SZ 128

int
vasprintf(char **str, const char *fmt, va_list ap)
{
    int ret = -1;
    va_list ap2;
    char *string, *newstr;
    size_t len;
    
    VA_COPY(ap2, ap);
    if ((string = malloc(INIT_SZ)) == NULL)
        goto fail;
    
    ret = vsnprintf(string, INIT_SZ, fmt, ap2);
    if (ret >= 0 && ret < INIT_SZ) { /* succeeded with initial alloc */
        *str = string;
    } else if (ret == INT_MAX || ret < 0) { /* Bad length */
        goto fail;
    } else {        /* bigger than initial, realloc allowing for nul */
        len = (size_t)ret + 1;
        if ((newstr = realloc(string, len)) == NULL) {
            free(string);
            goto fail;
        } else {
            va_end(ap2);
            VA_COPY(ap2, ap);
            ret = vsnprintf(newstr, len, fmt, ap2);
            if (ret >= 0 && (size_t)ret < len) {
                *str = newstr;
            } else { /* failed with realloc'ed string, give up */
                free(newstr);
                goto fail;
            }
        }
    }
    va_end(ap2);
    return (ret);
    
fail:
    *str = NULL;
    errno = ENOMEM;
    va_end(ap2);
    return (-1);
}

int asprintf(char **str, const char *fmt, ...)
{
    va_list ap;
    int ret;
    
    *str = NULL;
    va_start(ap, fmt);
    ret = vasprintf(str, fmt, ap);
    va_end(ap);
    
    return ret;
}
#endif // WIN32


char **g_outMsgBuf = NULL;
jmp_buf* g_jmpEnv = NULL;

/* Globals exported by this file */
const char *progname = NULL;

#define MAX_ON_EXIT_NICELY				20

static struct
{
	on_exit_nicely_callback function;
	void	   *arg;
}	on_exit_nicely_list[MAX_ON_EXIT_NICELY];

static int	on_exit_nicely_index;

/*
 * Parse a --section=foo command line argument.
 *
 * Set or update the bitmask in *dumpSections according to arg.
 * dumpSections is initialised as DUMP_UNSECTIONED by pg_dump and
 * pg_restore so they can know if this has even been called.
 */
void
set_dump_section(const char *arg, int *dumpSections)
{
	/* if this is the first call, clear all the bits */
	if (*dumpSections == DUMP_UNSECTIONED)
		*dumpSections = 0;

	if (strcmp(arg, "pre-data") == 0)
		*dumpSections |= DUMP_PRE_DATA;
	else if (strcmp(arg, "data") == 0)
		*dumpSections |= DUMP_DATA;
	else if (strcmp(arg, "post-data") == 0)
		*dumpSections |= DUMP_POST_DATA;
	else
	{
		write_msg( NULL, _("%s: unrecognized section name: \"%s\"\n"),
                  progname, arg);
		write_msg( NULL, _("Try \"%s --help\" for more information.\n"),
                  progname);
		exit_nicely(1);
	}
}


/*
 * Write a printf-style message to stderr.
 *
 * The program name is prepended, if "progname" has been set.
 * Also, if modulename isn't NULL, that's included too.
 * Note that we'll try to translate the modulename and the fmt string.
 */
void
write_msg(const char *modulename, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vwrite_msg(modulename, fmt, ap);
	va_end(ap);
}

/*
 * As write_msg, but pass a va_list not variable arguments.
 */
void
vwrite_msg(const char *modulename, const char *fmt, va_list ap)
{
    char* title     = NULL;
    char* message   = NULL;
    char* newBuf    = NULL;
    
    size_t titleLen   = 0;
    size_t messageLen = 0;
    size_t bufLen     = 0;
    
    if( g_outMsgBuf == NULL )
        return;
    
    if( modulename )
		asprintf( &title, "%s: [%s] ", progname, _(modulename) );
	else
		asprintf( &title, "%s: ", progname);
    
    vasprintf (&message, _(fmt), ap);
    
    titleLen   = strlen( title );
    messageLen = strlen( message );
    
    if( *g_outMsgBuf == NULL )
    {
        *g_outMsgBuf = (char*) malloc( titleLen + messageLen + 1 );
        if( *g_outMsgBuf )
        {
            strcpy( *g_outMsgBuf, title );
            strcat( *g_outMsgBuf, message );
        }
    }
    else
    {
        bufLen = strlen( *g_outMsgBuf );
        newBuf = (char*) realloc( *g_outMsgBuf, bufLen + titleLen + messageLen + 1 );
        if( newBuf )
        {
            *g_outMsgBuf = newBuf;
            strcat( *g_outMsgBuf, title );
            strcat( *g_outMsgBuf, message );
        }
    }
    
	free( title );
	free( message );
}

/* Register a callback to be run when exit_nicely is invoked. */
void
on_exit_nicely(on_exit_nicely_callback function, void *arg)
{
	if (on_exit_nicely_index >= MAX_ON_EXIT_NICELY)
		exit_horribly(NULL, "out of on_exit_nicely slots\n");
	on_exit_nicely_list[on_exit_nicely_index].function = function;
	on_exit_nicely_list[on_exit_nicely_index].arg = arg;
	on_exit_nicely_index++;
}

/*
 * Run accumulated on_exit_nicely callbacks in reverse order and then exit
 * quietly.  This needs to be thread-safe.
 */
void
exit_nicely(int code)
{
	int			i;

	for (i = on_exit_nicely_index - 1; i >= 0; i--)
		(*on_exit_nicely_list[i].function) (code,
											on_exit_nicely_list[i].arg);

#ifdef WIN32
	if (parallel_init_done && GetCurrentThreadId() != mainThreadId)
		ExitThread(code);
#endif

    on_exit_nicely_index = 0;
    
    CleanDumpable();
    
	if( code && g_jmpEnv )
		longjmp( *g_jmpEnv, 1 );
}
