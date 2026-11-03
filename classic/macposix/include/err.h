/* Classic Mac OS: err(3) family, printing to the console window. */
#ifndef MP_ERR_H
#define MP_ERR_H
#include <stdarg.h>
#include "macposix.h"
void	err(int, const char *, ...) __attribute__((noreturn));
void	errx(int, const char *, ...) __attribute__((noreturn));
void	warn(const char *, ...);
void	warnx(const char *, ...);
void	verr(int, const char *, va_list) __attribute__((noreturn));
void	verrx(int, const char *, va_list) __attribute__((noreturn));
void	vwarn(const char *, va_list);
void	vwarnx(const char *, va_list);
#endif
