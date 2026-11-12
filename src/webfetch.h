/* XLT - the web browser's engine: two threads behind the browser screen.
 *
 *   network thread: joins Wi-Fi when needed, then downloads with curl
 *                   (HTTPS through mbedTLS, cookies kept for the session,
 *                   connections reused), one request at a time;
 *   worker thread:  converts the page to UTF-8, parses and lays it out,
 *                   decodes pictures into textures.
 *
 * The browser (web.c) asks with wf_* calls and collects what is finished
 * with wf_poll() once per frame. Everything is tagged with a generation;
 * wf_cancel() makes everything of older generations stop or be dropped. */
#ifndef WEBFETCH_H
#define WEBFETCH_H

#include <stdint.h>
#include "html.h"
#include "weblay.h"
#include "url.h"

enum { WP_HTML, WP_TEXT, WP_IMAGE, WP_FILE, WP_ERROR };

typedef struct {
    int kind;                 /* WP_* */
    char url[URL_MAX];        /* where it ended up (after redirects) */
    char ctype[64];
    int status;               /* HTTP status, 0 if none */
    int64_t size;             /* bytes received (or announced, for WP_FILE) */
    char err[128];            /* WP_ERROR, WP_FILE: what to tell */
    HtmlDoc *doc;             /* WP_HTML / WP_TEXT / WP_IMAGE (a page with the one picture) */
    WebLayout *lay;
    WebImgState *imgs;        /* per image of doc */
    int ms_fetch, ms_parse;   /* timings for the log */
    char filename[128];       /* WP_FILE: the name to save it under */
} WebPage;

enum { WR_PAGE, WR_IMG, WR_LAYOUT, WR_DOWNLOAD };

typedef struct {
    int kind, gen;
    WebPage *page;            /* WR_PAGE: now the caller's (free with wf_free_page) */
    int img;                  /* WR_IMG: which image */
    int ok;
    void *tex;                /* WR_IMG: texture (the caller's; free()), written back from the cache */
    int psm, tw, th, tbw, w, h;
    WebLayout *lay;           /* WR_LAYOUT: the caller's */
    char msg[160];            /* WR_DOWNLOAD: the file saved, or why not (ok says which) */
} WebResult;

/* The look the layout is made for; set before navigating (copied). */
void wf_set_layout(const WebLayCfg *cfg);

void wf_init(const char *app_dir);
/* A line in cache/web.log. */
void wf_note(const char *fmt, ...);
/* Starts loading url (post: form body for a POST, or NULL). Bumps nothing: pass the generation to use. */
void wf_navigate(int gen, const char *url, const char *post, const char *referer);
/* Queues a picture of page `gen`; it is fitted into max_w x 512. */
void wf_image(int gen, int img, const char *url, int max_w, const char *referer);
/* A new layout of doc with these image sizes (both copied / kept as they are until WR_LAYOUT comes). */
void wf_relayout(int gen, const HtmlDoc *doc, const WebImgState *imgs, int nimgs);
/* Everything older than gen is stopped and dropped. */
void wf_cancel(int gen);
/* 1 if a result was taken. */
int wf_poll(WebResult *out);
/* Frees a page through the worker, after any layout job that still reads it. */
void wf_free_page(WebPage *p);

/* Saves url into the file at path (made here; removed again if it fails or is stopped).
 * One at a time; pages wait behind it. The answer is a WR_DOWNLOAD. */
void wf_download(const char *url, const char *referer, const char *path);
void wf_download_stop(void);
/* 1 while one runs; its bytes so far, the total (0 if unknown), KB/s. */
int wf_download_progress(int64_t *got, int64_t *total, int *kbps);

/* What the network thread is doing (for the progress line). */
enum { WF_IDLE, WF_WIFI, WF_CONNECTING, WF_RECEIVING, WF_WORKING };
int wf_busy(void);            /* a page or pictures still on the way */
void wf_progress(int *phase, int64_t *got, int64_t *total);
/* Wi-Fi: which saved connection to use next (0 = the last one). */
void wf_use_profile(int id);
/* Drops the connection (when the browser has been closed for a while, and before quitting). */
void wf_disconnect(void);
void wf_shutdown(void);

#endif
