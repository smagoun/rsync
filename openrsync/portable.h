/*
 * portable.h: shims for building openrsync on systems other than macOS and
 * the BSDs (Linux for testing, Classic Mac OS via Retro68).  Included from
 * extern.h after the system headers.  Every definition is guarded, so this
 * is a no-op wherever the native facility exists.
 */
#ifndef PORTABLE_H
#define PORTABLE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef SIZE_T_MAX
#define SIZE_T_MAX SIZE_MAX
#endif

#ifndef O_RESOLVE_BENEATH
/* FreeBSD/macOS only; the closest portable restriction. */
#define O_RESOLVE_BENEATH O_NOFOLLOW
#endif

#ifndef STAILQ_HEAD
#define STAILQ_HEAD(name, type)						\
struct name {								\
	struct type *stqh_first;					\
	struct type **stqh_last;					\
}
#define STAILQ_ENTRY(type)						\
struct {								\
	struct type *stqe_next;						\
}
#define STAILQ_FIRST(head)	((head)->stqh_first)
#define STAILQ_EMPTY(head)	((head)->stqh_first == NULL)
#define STAILQ_NEXT(elm, field)	((elm)->field.stqe_next)
#define STAILQ_INIT(head) do {						\
	(head)->stqh_first = NULL;					\
	(head)->stqh_last = &(head)->stqh_first;			\
} while (0)
#define STAILQ_INSERT_TAIL(head, elm, field) do {			\
	(elm)->field.stqe_next = NULL;					\
	*(head)->stqh_last = (elm);					\
	(head)->stqh_last = &(elm)->field.stqe_next;			\
} while (0)
#define STAILQ_REMOVE_HEAD(head, field) do {				\
	if (((head)->stqh_first =					\
	    (head)->stqh_first->field.stqe_next) == NULL)		\
		(head)->stqh_last = &(head)->stqh_first;		\
} while (0)
#define STAILQ_FOREACH(var, head, field)				\
	for ((var) = STAILQ_FIRST(head); (var);				\
	    (var) = STAILQ_NEXT(var, field))
#endif

/* Restart getopt(3) from argv[1]. */
#if defined(__GLIBC__)
#define PORTABLE_GETOPT_RESET()	do { optind = 0; } while (0)
#else
#define PORTABLE_GETOPT_RESET()	do { optreset = 1; optind = 1; } while (0)
#endif

/* --netatalk (netatalk.c); macOS has native forks and -E instead. */
#ifndef HAVE_NETATALK
#if defined(__APPLE__)
#define HAVE_NETATALK 0
#else
#define HAVE_NETATALK 1
#endif
#endif

#if !defined(__APPLE__) && !defined(__FreeBSD__)
#define PORTABLE_NEED_SBUF 1
#define PORTABLE_NEED_HUMANIZE 1
#endif

#ifdef PORTABLE_NEED_SBUF
/* Minimal FreeBSD sbuf(9) work-alike: auto-extending buffers only. */
struct sbuf {
	char	*s_buf;
	size_t	 s_len;
	size_t	 s_size;
	int	 s_error;
	int	 s_finished;
};
struct sbuf	*sbuf_new_auto(void);
int		 sbuf_bcat(struct sbuf *, const void *, size_t);
int		 sbuf_cat(struct sbuf *, const char *);
int		 sbuf_putc(struct sbuf *, int);
int		 sbuf_printf(struct sbuf *, const char *, ...)
		    __attribute__((format(printf, 2, 3)));
int		 sbuf_vprintf(struct sbuf *, const char *, va_list);
int		 sbuf_finish(struct sbuf *);
char		*sbuf_data(struct sbuf *);
ssize_t		 sbuf_len(struct sbuf *);
void		 sbuf_clear(struct sbuf *);
void		 sbuf_delete(struct sbuf *);
#endif

#ifdef PORTABLE_NEED_HUMANIZE
#ifndef HN_DECIMAL
#define HN_DECIMAL		0x01
#define HN_NOSPACE		0x02
#define HN_B			0x04
#define HN_DIVISOR_1000		0x08
#define HN_IEC_PREFIXES		0x10
#define HN_GETSCALE		0x10
#define HN_AUTOSCALE		0x20
#endif
int	humanize_number(char *, size_t, int64_t, const char *, int, int);
#endif

#endif /* PORTABLE_H */
