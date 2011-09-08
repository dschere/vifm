/* vifm
 * Copyright (C) 2001 Ken Steen.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

#include <regex.h>

#include <curses.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifndef _WIN32
#include <pwd.h> /* getpwnam() */
#endif
#include <unistd.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "../config.h"

#include "config.h"
#include "log.h"
#include "macros.h"
#include "status.h"
#include "version.h"
#include "ui.h"

#include "utils.h"

struct Fuse_List *fuse_mounts = NULL;

int
S_ISEXE(mode_t mode)
{
#ifndef _WIN32
	return ((S_IXUSR & mode) || (S_IXGRP & mode) || (S_IXOTH & mode));
#else
	return 0;
#endif
}

int
is_dir(const char *file)
{
#ifndef _WIN32
	struct stat statbuf;
	if(stat(file, &statbuf) != 0)
	{
		LOG_SERROR_MSG(errno, "Can't stat \"%s\"", file);
		log_cwd();
		return 0;
	}

	return S_ISDIR(statbuf.st_mode);
#else
	DWORD attr;

	attr = GetFileAttributesA(file);
	if(attr == INVALID_FILE_ATTRIBUTES)
	{
		LOG_SERROR_MSG(errno, "Can't get attributes of \"%s\"", file);
		log_cwd();
		return 0;
	}

	return (attr & FILE_ATTRIBUTE_DIRECTORY);
#endif
}

/*
 * Escape the filename for the purpose of inserting it into the shell.
 *
 * Automatically calculates string length when len == 0.
 * quote_percent means prepend percent sign with a percent sign
 *
 * Returns new string, caller should free it.
 */
char *
escape_filename(const char *string, int quote_percent)
{
#ifndef _WIN32
	size_t len;
	size_t i;
	char *ret, *dup;

	len = strlen(string);

	dup = ret = malloc(len*2 + 2 + 1);

	if(*string == '-')
	{
		*dup++ = '.';
		*dup++ = '/';
	}
	else if(*string == '~')
	{
		*dup++ = *string++;
	}

	for(i = 0; i < len; i++, string++, dup++)
	{
		switch(*string)
		{
			case '%':
				if(quote_percent)
					*dup++ = '%';
				break;
			case '\'':
			case '\\':
			case '\r':
			case '\n':
			case '\t':
			case '"':
			case ';':
			case ' ':
			case '?':
			case '|':
			case '[':
			case ']':
			case '{':
			case '}':
			case '<':
			case '>':
			case '`':
			case '!':
			case '$':
			case '&':
			case '*':
			case '(':
			case ')':
				*dup++ = '\\';
				break;
			case '~':
			case '#':
				if(dup == ret)
					*dup++ = '\\';
				break;
		}
		*dup = *string;
	}
	*dup = '\0';
	return ret;
#else
	return strdup(string);
#endif
}

void
chomp(char *text)
{
	size_t len;

	if(text[0] == '\0')
		return;

	len = strlen(text);
	if(text[len - 1] == '\n')
		text[len - 1] = '\0';
}

/* like chomp() but removes trailing slash */
void
chosp(char *text)
{
	size_t len;

	if(text[0] == '\0')
		return;

	len = strlen(text);
	if(text[len - 1] == '/')
		text[len - 1] = '\0';
}

/* returns width of utf8 character */
size_t
get_char_width(const char* string)
{
	if((string[0] & 0xe0) == 0xc0 && (string[1] & 0xc0) == 0x80)
		return 2;
	else if((string[0] & 0xf0) == 0xe0 && (string[1] & 0xc0) == 0x80 &&
			(string[2] & 0xc0) == 0x80)
		return 3;
	else if((string[0] & 0xf8) == 0xf0 && (string[1] & 0xc0) == 0x80 &&
			(string[2] & 0xc0) == 0x80 && (string[3] & 0xc0) == 0x80)
		return 4;
	else if(string[0] == '\0')
		return 0;
	else
		return 1;
}

/* returns count of bytes of whole string or of first max_len utf8 characters */
size_t
get_real_string_width(char *string, size_t max_len)
{
	size_t width = 0;
	while(*string != '\0' && max_len-- != 0)
	{
		size_t char_width = get_char_width(string);
		width += char_width;
		string += char_width;
	}
	return width;
}

static size_t
guess_char_width(char c)
{
	if((c & 0xe0) == 0xc0)
		return 2;
	else if((c & 0xf0) == 0xe0)
		return 3;
	else if((c & 0xf8) == 0xf0)
		return 4;
	else
		return 1;
}

/* returns count utf8 characters excluding incomplete utf8 characters */
size_t
get_normal_utf8_string_length(const char *string)
{
	size_t length = 0;
	while(*string != '\0')
	{
		size_t char_width = guess_char_width(*string);
		if(char_width <= strlen(string))
			length++;
		else
			break;
		string += char_width;
	}
	return length;
}

/* returns count of bytes excluding incomplete utf8 characters */
size_t
get_normal_utf8_string_widthn(const char *string, size_t max)
{
	size_t length = 0;
	while(*string != '\0' && max-- > 0)
	{
		size_t char_width = guess_char_width(*string);
		if(char_width <= strlen(string))
			length += char_width;
		else
			break;
		string += char_width;
	}
	return length;
}

/* returns count of bytes excluding incomplete utf8 characters */
size_t
get_normal_utf8_string_width(const char *string)
{
	size_t length = 0;
	while(*string != '\0')
	{
		size_t char_width = guess_char_width(*string);
		if(char_width <= strlen(string))
			length += char_width;
		else
			break;
		string += char_width;
	}
	return length;
}

/* returns count of utf8 characters in string */
size_t
get_utf8_string_length(const char *string)
{
	size_t length = 0;
	while(*string != '\0')
	{
		size_t char_width = get_char_width(string);
		string += char_width;
		length++;
	}
	return length;
}

/* returns (string_width - string_length) */
size_t
get_utf8_overhead(const char *string)
{
	size_t overhead = 0;
	while(*string != '\0')
	{
		size_t char_width = get_char_width(string);
		string += char_width;
		overhead += char_width - 1;
	}
	return overhead;
}

wchar_t *
to_wide(const char *s)
{
	wchar_t *result;
	int len;

	len = mbstowcs(NULL, s, 0);
	result = malloc((len + 1)*sizeof(wchar_t));
	if(result != NULL)
		mbstowcs(result, s, len + 1);
	return result;
}

/* if err == 1 then use stderr and close stdin and stdout */
void _gnuc_noreturn
run_from_fork(int pipe[2], int err, char *cmd)
{
	char *args[4];
	int nullfd;

	close(err ? STDERR_FILENO : STDOUT_FILENO);
	if(dup(pipe[1]) == -1) /* Redirect stderr or stdout to write end of pipe. */
		exit(1);
	close(pipe[0]);        /* Close read end of pipe. */
	close(STDIN_FILENO);
	close(err ? STDOUT_FILENO : STDERR_FILENO);

	/* Send stdout, stdin to /dev/null */
	if((nullfd = open("/dev/null", O_RDONLY)) != -1)
	{
		if(dup2(nullfd, STDIN_FILENO) == -1)
			exit(1);
		if(dup2(nullfd, err ? STDOUT_FILENO : STDERR_FILENO) == -1)
			exit(1);
	}

	args[0] = cfg.shell;
	args[1] = "-c";
	args[2] = cmd;
	args[3] = NULL;

#ifndef _WIN32
	execvp(args[0], args);
#else
	execvp(args[0], (const char **)args);
#endif
	exit(1);
}

/* I'm really worry about the portability... */
wchar_t *
my_wcsdup(const wchar_t *ws)
{
	wchar_t *result;

	result = (wchar_t *) malloc((wcslen(ws) + 1) * sizeof(wchar_t));
	if(result == NULL)
		return NULL;
	wcscpy(result, ws);
	return result;
}

char *
uchar2str(wchar_t *c, size_t *len)
{
	static char buf[8];

	*len = 1;
	switch(*c)
	{
		case L' ':
			strcpy(buf, "<space>");
			break;
		case L'\033':
			if(c[1] == L'[' && c[2] == 'Z')
			{
				strcpy(buf, "<s-tab>");
				*len += 2;
				break;
			}
			if(c[1] != L'\0' && c[1] != L'\033')
			{
				strcpy(buf, "<m-a>");
				buf[3] += c[1] - L'a';
				++*len;
				break;
			}
			strcpy(buf, "<esc>");
			break;
		case L'\177':
			strcpy(buf, "<del>");
			break;
		case KEY_HOME:
			strcpy(buf, "<home>");
			break;
		case KEY_END:
			strcpy(buf, "<end>");
			break;
		case KEY_LEFT:
			strcpy(buf, "<left>");
			break;
		case KEY_RIGHT:
			strcpy(buf, "<right>");
			break;
		case KEY_UP:
			strcpy(buf, "<up>");
			break;
		case KEY_DOWN:
			strcpy(buf, "<down>");
			break;
		case KEY_BACKSPACE:
			strcpy(buf, "<bs>");
			break;
		case KEY_BTAB:
			strcpy(buf, "<s-tab>");
			break;
		case KEY_DC:
			strcpy(buf, "<delete>");
			break;
		case KEY_PPAGE:
			strcpy(buf, "<pageup>");
			break;
		case KEY_NPAGE:
			strcpy(buf, "<pagedown>");
			break;

		default:
			if(*c == L'\n' || (*c > L' ' && *c < 256))
			{
				buf[0] = *c;
				buf[1] = '\0';
			}
			else if(*c >= KEY_F0 && *c < KEY_F0 + 10)
			{
				strcpy(buf, "<f0>");
				buf[2] += *c - KEY_F0;
			}
			else if(*c >= KEY_F0 + 10 && *c < KEY_F0 + 63)
			{
				strcpy(buf, "<f00>");
				buf[2] += *c/10 - KEY_F0;
				buf[3] += *c%10 - KEY_F0;
			}
			else
			{
				strcpy(buf, "<c-A>");
				buf[3] = tolower(buf[3] + *c - 1);
			}
			break;
	}
	return buf;
}

void
get_perm_string(char * buf, int len, mode_t mode)
{
#ifndef _WIN32
	char *perm_sets[] =
	{ "---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx" };
	int u, g, o;

	u = (mode & S_IRWXU) >> 6;
	g = (mode & S_IRWXG) >> 3;
	o = (mode & S_IRWXO);

	snprintf(buf, len, "-%s%s%s", perm_sets[u], perm_sets[g], perm_sets[o]);

	if(S_ISLNK(mode))
		buf[0] = 'l';
	else if(S_ISDIR(mode))
		buf[0] = 'd';
	else if(S_ISBLK(mode))
		buf[0] = 'b';
	else if(S_ISCHR(mode))
		buf[0] = 'c';
	else if(S_ISFIFO(mode))
		buf[0] = 'p';
	else if(S_ISSOCK(mode))
		buf[0] = 's';

	if(mode & S_ISVTX)
		buf[9] = (buf[9] == '-') ? 'T' : 't';
	if(mode & S_ISGID)
		buf[6] = (buf[6] == '-') ? 'S' : 's';
	if(mode & S_ISUID)
		buf[3] = (buf[3] == '-') ? 'S' : 's';
#else
	snprintf(buf, len, "--WINDOWS--");
#endif
}

/* When list is NULL returns maximum number of lines, otherwise returns number
 * of filled lines */
int
fill_version_info(char **list)
{
	int x = 0;

	if(list == NULL)
		return 9;

	list[x++] = strdup("Version: " VERSION);
	list[x] = malloc(sizeof("Git commit hash: ") + strlen(GIT_HASH) + 1);
	sprintf(list[x++], "Git commit hash: %s", GIT_HASH);
	list[x++] = strdup("Compiled at: " __DATE__ " " __TIME__);
	list[x++] = strdup("");

#ifdef ENABLE_COMPATIBILITY_MODE
	list[x++] = strdup("Compatibility mode is on");
#else
	list[x++] = strdup("Compatibility mode is off");
#endif

#ifdef ENABLE_EXTENDED_KEYS
	list[x++] = strdup("Support of extended keys is on");
#else
	list[x++] = strdup("Support of extended keys is off");
#endif

#ifdef HAVE_LIBGTK
	list[x++] = strdup("With GTK+ library");
#else
	list[x++] = strdup("Without GTK+ library");
#endif

#ifdef HAVE_LIBMAGIC
	list[x++] = strdup("With magic library");
#else
	list[x++] = strdup("Without magic library");
#endif

#ifdef HAVE_FILE_PROG
	list[x++] = strdup("With file program");
#else
	list[x++] = strdup("Without file program");
#endif

	return x;
}

int
path_starts_with(const char *path, const char *begin)
{
	size_t len = strlen(begin);

	if(len > 0 && begin[len - 1] == '/')
		len--;

	if(strncmp(path, begin, len) != 0)
		return 0;

	return (path[len] == '\0' || path[len] == '/');
}

void
friendly_size_notation(unsigned long long num, int str_size, char *str)
{
	static const char* iec_units[] = {
		"  B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"
	};
	static const char* si_units[] = {
		"B", "K", "M", "G", "T", "P", "E", "Z", "Y"
	};

	static int _gnuc_unused units_size_guard[
		(ARRAY_LEN(iec_units) == ARRAY_LEN(si_units)) ? 1 : -1
	];

	const char** units;
	size_t u;
	double d = num;

	if(cfg.use_iec_prefixes)
		units = iec_units;
	else
		units = si_units;

	u = 0;
	while(d >= 1023.5 && u < ARRAY_LEN(iec_units) - 1)
	{
		d /= 1024.0;
		u++;
	}
	if(u == 0)
	{
		snprintf(str, str_size, "%.0f %s", d, units[u]);
	}
	else
	{
		if(d > 9)
			snprintf(str, str_size, "%.0f %s", d, units[u]);
		else
		{
			size_t len = snprintf(str, str_size, "%.1f %s", d, units[u]);
			if(str[len - strlen(units[u]) - 1 - 1] == '0')
				snprintf(str, str_size, "%.0f %s", d, units[u]);
		}
	}
}

int
check_link_is_dir(const char *filename)
{
	char linkto[PATH_MAX + NAME_MAX];
	int saved_errno;
	char *filename_copy;
	char *p;

	filename_copy = strdup(filename);
	chosp(filename_copy);

	p = realpath(filename, linkto);
	saved_errno = errno;

	free(filename_copy);

	if(p == linkto)
	{
		return is_dir(linkto);
	}
	else
	{
		LOG_SERROR_MSG(saved_errno, "Can't readlink \"%s\"", filename);
		log_cwd();
	}

	return 0;
}

int
add_to_string_array(char ***array, int len, int count, ...)
{
	char **p;
	va_list va;

	p = realloc(*array, sizeof(char *)*(len + count));
	if(p == NULL)
		return count;
	*array = p;

	va_start(va, count);
	while(count-- > 0)
	{
		if((p[len] = strdup(va_arg(va, char *))) == NULL)
			break;
		len++;
	}
	va_end(va);

	return len;
}

int
is_in_string_array(char **array, size_t len, const char *key)
{
	int i;
	for(i = 0; i < len; i++)
		if(strcmp(array[i], key) == 0)
			return 1;
	return 0;
}

int
is_in_string_array_case(char **array, size_t len, const char *key)
{
	int i;
	for(i = 0; i < len; i++)
		if(strcasecmp(array[i], key) == 0)
			return 1;
	return 0;
}

int
string_array_pos(char **array, size_t len, const char *key)
{
	int i;
	for(i = 0; i < len; i++)
		if(strcmp(array[i], key) == 0)
			return i;
	return -1;
}

int
string_array_pos_case(char **array, size_t len, const char *key)
{
	int i;
	for(i = 0; i < len; i++)
		if(strcasecmp(array[i], key) == 0)
			return i;
	return -1;
}

void
free_string_array(char **array, size_t len)
{
	int i;

	for(i = 0; i < len; i++)
		free(array[i]);
	free(array);
}

void
free_wstring_array(wchar_t **array, size_t len)
{
	free_string_array((char **)array, len);
}

/* Removes excess slashes, "../" and "./" from the path */
void
canonicalize_path(const char *directory, char *buf, size_t buf_size)
{
	const char *p; /* source string pointer */
	char *q; /* destination string pointer */

	buf[0] = '\0';

	q = buf - 1;
	p = directory;

#ifdef _WIN32
	if(p[0] == '/' && p[1] == '/' && p[2] != '/')
	{
		strcpy(buf, "//");
		q = buf + 1;
		p += 2;
		while(*p != '\0' && *p != '/')
			*++q = *p++;
		buf = q + 1;
	}
#endif

	while(*p != '\0' && (size_t)((q + 1) - buf) < buf_size - 1)
	{
		int prev_dir_present;

		prev_dir_present = (q != buf - 1 && *q == '/');
		if(prev_dir_present && strncmp(p, "./", 2) == 0)
			p++;
		else if(prev_dir_present && strcmp(p, ".") == 0)
			;
		else if(prev_dir_present &&
				(strncmp(p, "../", 3) == 0 || strcmp(p, "..") == 0) &&
				strcmp(buf, "../") != 0)
		{
#ifdef _WIN32
			if(*(q - 1) != ':')
#endif
			{
				p++;
				q--;
				while(q >= buf && *q != '/')
					q--;
			}
		}
		else if(*p == '/')
		{
			if(!prev_dir_present)
				*++q = '/';
		}
		else
		{
			*++q = *p;
		}

		p++;
	}

	if(*q != '/')
		*++q = '/';

	*++q = '\0';
}

const char *
make_rel_path(const char *path, const char *base)
{
	static char buf[PATH_MAX];

	const char *p = path, *b = base;
	int i;
	int nslashes;

#ifdef _WIN32
	if(path[1] == ':' && base[1] == ':' && path[0] != base[0])
	{
		canonicalize_path(path, buf, sizeof(buf));
		return buf;
	}
#endif

	while(p[0] != '\0' && p[1] != '\0' && b[0] != '\0' && b[1] != '\0')
	{
		const char *op = p, *ob = b;
		if((p = strchr(p + 1, '/')) == NULL)
			p = path + strlen(path);
		if((b = strchr(b + 1, '/')) == NULL)
			b = base + strlen(base);
		if(p - path != b - base || strncmp(path, base, p - path) != 0)
		{
			p = op;
			b = ob;
			break;
		}
	}

	canonicalize_path(b, buf, sizeof(buf));
	chosp(buf);

	nslashes = 0;
	for(i = 0; buf[i] != '\0'; i++)
		if(buf[i] == '/')
			nslashes++;

	buf[0] = '\0';
	while(nslashes-- > 0)
		strcat(buf, "../");
	if(*p == '/')
		p++;
	canonicalize_path(p, buf + strlen(buf), sizeof(buf) - strlen(buf));
	chosp(buf);

	if(buf[0] == '\0')
		strcpy(buf, ".");

	return buf;
}

const char *
replace_home_part(const char *directory)
{
	static char buf[PATH_MAX];
	size_t len;

	len = strlen(cfg.home_dir) - 1;
	if(strncmp(directory, cfg.home_dir, len) == 0 &&
			(directory[len] == '\0' || directory[len] == '/'))
		strncat(strcpy(buf, "~"), directory + len, sizeof(buf));
	else
		strncpy(buf, directory, sizeof(buf));
	if(!is_root_dir(buf))
		chosp(buf);

	return buf;
}

char *
expand_tilde(char *path)
{
#ifndef _WIN32
	char name[NAME_MAX];
	char *p, *result;
	struct passwd *pw;

	if(path[0] != '~')
		return path;

	if(path[1] == '\0' || path[1] == '/')
	{
		char *result;

		result = malloc((strlen(cfg.home_dir) + strlen(path) + 1));
		if(result == NULL)
			return NULL;

		sprintf(result, "%s%s", cfg.home_dir, (path[1] == '/') ? (path + 2) : "");
		free(path);
		return result;
	}

	if((p = strchr(path, '/')) == NULL)
	{
		p = path + strlen(path);
		strcpy(name, path + 1);
	}
	else
	{
		snprintf(name, p - (path + 1) + 1, "%s", path + 1);
		p++;
	}

	if((pw = getpwnam(name)) == NULL)
		return path;

	chosp(pw->pw_dir);
	result = malloc(strlen(pw->pw_dir) + strlen(path) + 1);
	if(result == NULL)
		return NULL;
	sprintf(result, "%s/%s", pw->pw_dir, p);
	free(path);

	return result;
#else
	return path;
#endif
}

int
get_regexp_cflags(const char *pattern)
{
	int result;

	result = REG_EXTENDED;
	if(cfg.ignore_case)
		result |= REG_ICASE;

	if(cfg.ignore_case && cfg.smart_case)
	{
		wchar_t *wstring, *p;
		wstring = to_wide(pattern);
		p = wstring - 1;
		while(*++p != L'\0')
			if(iswupper(*p))
			{
				result &= ~REG_ICASE;
				break;
			}
		free(wstring);
	}
	return result;
}

const char *
get_regexp_error(int err, regex_t *re)
{
	static char buf[360];

	regerror(err, re, buf, sizeof(buf));
	return buf;
}

int
is_root_dir(const char *path)
{
#ifdef _WIN32
	if(isalpha(path[0]) && strcmp(path + 1, ":/") == 0)
		return 1;

	if(path[0] == '/' && path[1] == '/' && path[2] != '\0')
	{
		char *p = strchr(path + 2, '/');
		if(p == NULL || p[1] == '\0')
			return 1;
	}
#endif
	return (path[0] == '/' && path[1] == '\0');
}

int
is_path_absolute(const char *path)
{
#ifdef _WIN32
	if(isalpha(path[0]) && path[1] == ':')
		return 1;
	if(path[0] == '/' && path[1] == '/')
		return 1;
#endif
	return (path[0] == '/');
}

int
ends_with(const char* str, const char* suffix)
{
	size_t str_len = strlen(str);
	size_t suf_len = strlen(suffix);

	if(str_len < suf_len)
		return 0;
	else
		return (strcmp(suffix, str + str_len - suf_len) == 0);
}

char *
strchar2str(const char *str)
{
	static char buf[8];

	size_t len = get_char_width(str);
	if(len != 1 || str[0] >= ' ' || str[0] == '\n')
	{
		memcpy(buf, str, len);
		buf[len] = '\0';
	}
	else
	{
		buf[0] = '^';
		buf[1] = ('A' - 1) + str[0];
		buf[2] = '\0';
	}
	return buf;
}

char *
to_multibyte(const wchar_t *s)
{
	size_t len;
	char *result;

	len = wcstombs(NULL, s, 0) + 1;
	if((result = malloc(len*sizeof(char))) == NULL)
		return NULL;

	wcstombs(result, s, len);
	return result;
}

int
get_link_target(const char *link, char *buf, size_t buf_len)
{
#ifndef _WIN32
	char *filename;
	ssize_t len;

	filename = strdup(link);
	chosp(filename);

	len = readlink(filename, buf, buf_len);

	free(filename);

	if(len == -1)
		return -1;

	buf[len] = '\0';
	return 0;
#else
	static char filename[PATH_MAX];

	DWORD attr;
	HANDLE hfind;
	WIN32_FIND_DATAA ffd;
	HANDLE hfile;
	char rdb[2048];
	char *t;
	REPARSE_DATA_BUFFER *sbuf;
	WCHAR *path;

	attr = GetFileAttributes(link);
	if(attr == INVALID_FILE_ATTRIBUTES)
		return -1;

	if(!(attr & FILE_ATTRIBUTE_REPARSE_POINT))
		return -1;

	snprintf(filename, sizeof(filename), "%s", link);
	chosp(filename);
	hfind = FindFirstFileA(filename, &ffd);
	if(hfind == INVALID_HANDLE_VALUE)
		return -1;

	FindClose(hfind);

	if(ffd.dwReserved0 != IO_REPARSE_TAG_SYMLINK)
		return -1;

	hfile = CreateFileA(filename, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			NULL);
	if(hfile == INVALID_HANDLE_VALUE)
		return -1;

	if(!DeviceIoControl(hfile, FSCTL_GET_REPARSE_POINT, NULL, 0, rdb,
				sizeof(rdb), &attr, NULL))
	{
		CloseHandle(hfile);
		return -1;
	}
	CloseHandle(hfile);
	
	sbuf = (REPARSE_DATA_BUFFER *)rdb;
	path = sbuf->SymbolicLinkReparseBuffer.PathBuffer;
	path[sbuf->SymbolicLinkReparseBuffer.PrintNameOffset/sizeof(WCHAR) +
			sbuf->SymbolicLinkReparseBuffer.PrintNameLength/sizeof(WCHAR)] = L'\0';
	t = to_multibyte(path +
			sbuf->SymbolicLinkReparseBuffer.PrintNameOffset/sizeof(WCHAR));
	if(strncmp(t, "\\??\\", 4) == 0)
		strncpy(buf, t + 4, buf_len);
	else
		strncpy(buf, t, buf_len);
	buf[buf_len - 1] = '\0';
	free(t);
	return 0;
#endif
}

void
strtoupper(char *s)
{
	while(*s != '\0')
	{
		*s = toupper(*s);
		s++;
	}
}

#ifdef _WIN32

int
wcwidth(wchar_t c)
{
	return 1;
}

int
wcswidth(const wchar_t *str, size_t len)
{
	return MIN(len, wcslen(str));
}

int
S_ISLNK(mode_t mode)
{
	return 0;
}

int
readlink(const char *path, char *buf, size_t len)
{
	return -1;
}

char *
realpath(const char *path, char *buf)
{
	if(get_link_target(path, buf, PATH_MAX) != 0)
		strcpy(buf, path);
	return buf;
}

int
is_unc_path(const char *path)
{
	return (path[0] == '/' && path[1] == '/' && path[2] != '/');
}

int
is_unc_root(const char *path)
{
	if(is_unc_path(path))
	{
		char *p = strchr(path + 2, '/');
		if(p == NULL || p[1] == '\0')
			return 1;
	}
	return 0;
}

int
exec_program(TCHAR *cmd)
{
	BOOL ret;
	DWORD exitcode;
	STARTUPINFO startup = {};
	PROCESS_INFORMATION pinfo;

	ret = CreateProcess(NULL, cmd, NULL, NULL, 0, 0, NULL, NULL, &startup,
			&pinfo);
	if(ret == 0)
		return -1;

	CloseHandle(pinfo.hThread);

	if(WaitForSingleObject(pinfo.hProcess, INFINITE) != WAIT_OBJECT_0)
	{
		CloseHandle(pinfo.hProcess);
		return -1;
	}
	if(GetExitCodeProcess(pinfo.hProcess, &exitcode) == 0)
	{
		CloseHandle(pinfo.hProcess);
		return -1;
	}
	CloseHandle(pinfo.hProcess);
	return exitcode;
}

int
is_win_executable(const char *name)
{
	char *path, *p, *q;
	char name_buf[NAME_MAX];

	path = getenv("PATHEXT");
	if(path == NULL || path[0] == '\0')
		path = ".bat;.exe;.com";

	snprintf(name_buf, sizeof(name_buf), "%s", name);
	strtoupper(name_buf);

	p = path - 1;
	do
	{
		char ext_buf[16];

		p++;
		q = strchr(p, ';');
		if(q == NULL)
			q = p + strlen(p);

		snprintf(ext_buf, q - p + 1, "%s", p);
		strtoupper(ext_buf);
		p = q;

		if(ends_with(name_buf, ext_buf))
			return 1;
	} while(q[0] != '\0');
	return 0;
}

#endif

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 : */
