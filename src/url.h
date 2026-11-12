/* XLT - URLs for the web browser: resolving links, the address bar's
 * "address or search" guess, form encoding. Portable (host-tested). */
#ifndef URL_H
#define URL_H

#define URL_MAX 1024

/* ref resolved against base (RFC 3986, with dot segments removed) into out.
 * Returns 0, or -1 if the result does not fit or base is not absolute. */
int url_resolve(const char *base, const char *ref, char *out, int n);

/* The scheme in lower case ("https"), "" if there is none. */
void url_scheme(const char *url, char *out, int n);
/* The host, without user info or port ("en.wikipedia.org"). */
void url_host(const char *url, char *out, int n);
/* 1 for http:// and https:// URLs (what the browser can open). */
int url_is_web(const char *url);
/* 1 if a and b differ only after '#' (a jump within the page). */
int url_same_page(const char *a, const char *b);
/* The part after '#', "" if none (points into url). */
const char *url_fragment(const char *url);

/* What was typed in the address bar: an address (example.com, http://...,
 * localhost:8080/x) becomes a URL; anything else a search with the given
 * engine prefix (the query is appended, form-encoded). Returns 1 for an
 * address, 0 for a search, -1 if empty. */
int url_from_input(const char *text, const char *search_prefix, char *out, int n);

/* Appends s form-encoded (application/x-www-form-urlencoded: space -> '+').
 * Returns -1 if it did not fit (out is left terminated). */
int url_encode_append(char *out, int n, const char *s);

/* A search engine's click-through link (duckduckgo.com/l/?uddg=<target>): the target,
 * decoded, into out. Returns 1 if url was one, else 0 (out untouched). */
int url_unwrap_redirect(const char *url, char *out, int n);

/* 1 if the path (before '?' or '#') ends in .svg: a picture the browser can't draw. */
int url_is_svg(const char *url);

/* A file name for saving what url points at: the filename= of a Content-Disposition
 * header if there is one (may be NULL), else the last part of the path, %-decoded;
 * characters FAT can't take become '_'. "download" if nothing is left. */
void url_file_name(const char *url, const char *disposition, char *out, int n);

/* A short form for showing: no scheme, no "www.", no trailing '/'. */
void url_pretty(const char *url, char *out, int n);

#endif
