/*
 * engine.h: running rsync commands in the Classic Mac OS application
 * (engine.c), used by the console front end and by the GUI.
 */
#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>

/* Run a typed command line (MacRoman, HFS paths, $(NAME)); exit status. */
int	engine_run_line(const char *);
/* Run prepared arguments (UTF-8, POSIX paths; argv[0] "rsync"). */
int	engine_run_argv(int, char **);
/* "trace on|off", "set NAME=VALUE": 1 if handled. */
int	engine_builtin(const char *);
/* "Disk:Folder:" -> malloc'ed "/Disk/Folder/" (NULL if not an HFS path). */
char	*engine_hfs_arg(const char *);
size_t	engine_utf8_to_macroman(const unsigned char *, size_t, char *);
void	engine_macroman_to_utf8(const char *, char *, size_t);
void	engine_grow_stack(long);
/* Run fn(arg) so that exit() inside returns its status here. */
int	classic_guarded(int (*)(void *), void *);

/* Non-zero while rsync runs (the GUI must not start another run). */
extern int engine_busy;

#endif /* ENGINE_H */
