/* XLT - the web browser's network and worker threads. See webfetch.h. */
#define _GNU_SOURCE /* strcasestr */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <ctype.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <curl/curl.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include "webfetch.h"
#include "imgdecode.h"
#include "net.h"
#include "textenc.h"
#include "cacerts_data.h"

#define PAGE_CAP (3 << 20)
#define IMG_CAP (3 << 20)
#define IMG_MAX_W 480
#define IMG_MAX_H 512
#define UA "Mozilla/5.0 (PlayStation Portable; XLT) AppleWebKit/537.36 (KHTML, like Gecko) Mobile Safari/537.36"

/* ------------------------------------------------------------- queues */

enum { J_PAGE, J_IMG, J_PARSE, J_DECODE, J_LAYOUT, J_FREE, J_DISCONNECT, J_DOWNLOAD };

typedef struct Job {
    struct Job *next;
    int kind, gen;
    int img, max_w;
    char url[URL_MAX];
    char referer[URL_MAX];
    char *post;
    /* J_PARSE / J_DECODE: what was received */
    char *data;
    int len;
    char ctype[64];
    int status, ms_fetch;
    int64_t announced;
    char err[128];
    int is_file;
    /* J_LAYOUT */
    const HtmlDoc *doc;
    WebImgState *imgs;
    /* J_FREE */
    WebPage *page;
    /* J_PARSE: the Content-Disposition header; J_DOWNLOAD: where to save */
    char disposition[256];
    char path[256];
} Job;

typedef struct {
    Job *head, *tail;
    SceUID lock, work;
} Queue;

static Queue s_net, s_cpu;
static SceUID s_res_lock = -1;
static WebResult s_res[64];
static int s_res_head, s_res_n;

static volatile int s_cancel_gen;  /* jobs of older generations are dropped */
static volatile int s_phase;
static volatile int64_t s_got, s_total;
static volatile int s_pending;     /* page and image jobs not yet answered */
static volatile int s_profile;
static WebLayCfg s_cfg;
static char s_log[256];

static void q_init(Queue *q, const char *name)
{
    q->head = q->tail = NULL;
    q->lock = sceKernelCreateSema(name, 0, 1, 1, NULL);
    q->work = sceKernelCreateSema(name, 0, 0, 100000, NULL);
}

static void q_put(Queue *q, Job *j)
{
    j->next = NULL;
    sceKernelWaitSema(q->lock, 1, NULL);
    if (q->tail) q->tail->next = j;
    else q->head = j;
    q->tail = j;
    sceKernelSignalSema(q->lock, 1);
    sceKernelSignalSema(q->work, 1);
}

static Job *q_take(Queue *q)
{
    sceKernelWaitSema(q->work, 1, NULL);
    sceKernelWaitSema(q->lock, 1, NULL);
    Job *j = q->head;
    if (j) {
        q->head = j->next;
        if (!q->head) q->tail = NULL;
    }
    sceKernelSignalSema(q->lock, 1);
    return j;
}

static void post_result(const WebResult *r)
{
    for (;;) {
        sceKernelWaitSema(s_res_lock, 1, NULL);
        if (s_res_n < (int)(sizeof(s_res) / sizeof(s_res[0]))) {
            s_res[(s_res_head + s_res_n) % 64] = *r;
            s_res_n++;
            sceKernelSignalSema(s_res_lock, 1);
            return;
        }
        sceKernelSignalSema(s_res_lock, 1);
        sceKernelDelayThread(5000);
    }
}

int wf_poll(WebResult *out)
{
    if (s_res_lock < 0)
        return 0;
    sceKernelWaitSema(s_res_lock, 1, NULL);
    int got = s_res_n > 0;
    if (got) {
        *out = s_res[s_res_head];
        s_res_head = (s_res_head + 1) % 64;
        s_res_n--;
    }
    sceKernelSignalSema(s_res_lock, 1);
    if (got && (out->kind == WR_PAGE || out->kind == WR_IMG))
        __sync_fetch_and_sub(&s_pending, 1);
    return got;
}

static Job *job_new(int kind, int gen)
{
    Job *j = calloc(1, sizeof(Job));
    if (j) {
        j->kind = kind;
        j->gen = gen;
    }
    return j;
}

static void job_free(Job *j)
{
    if (!j) return;
    free(j->post);
    free(j->data);
    free(j->imgs);
    free(j);
}

static int stale(const Job *j) { return j->gen < s_cancel_gen; }

static int ms_now(void)
{
    return (int)(sceKernelGetSystemTimeWide() / 1000);
}

/* --------------------------------------------------------------- log */

/* The log is made at full size when XLT starts and lines are written into it in place:
 * a file created (or grown) just before the PSP freezes never reaches the card. */
#define LOG_SIZE (32 * 1024)
static int s_log_pos;

static void wlog(const char *fmt, ...)
{
    if (!s_log[0] || s_log_pos >= LOG_SIZE - 2) return;
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    if (s_log_pos + n > LOG_SIZE - 1) n = LOG_SIZE - 1 - s_log_pos;
    SceUID fd = sceIoOpen(s_log, PSP_O_WRONLY, 0777);
    if (fd >= 0) {
        sceIoLseek32(fd, s_log_pos, PSP_SEEK_SET);
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
        s_log_pos += n;
    }
}

void wf_note(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    wlog("%s", line);
}

static void log_create(void)
{
    SceUID fd = sceIoOpen(s_log, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    static char blank[1024];
    memset(blank, ' ', sizeof(blank));
    blank[sizeof(blank) - 1] = '\n';
    for (int i = 0; i < LOG_SIZE / (int)sizeof(blank); i++)
        sceIoWrite(fd, blank, sizeof(blank));
    sceIoClose(fd);
}

/* ---------------------------------------------------------------- curl */

static CURL *s_curl;
static mbedtls_x509_crt s_ca;
static int s_ca_ok;

/* Set when the last chain failed on its dates only. */
static volatile int s_cert_date_only;

/* 1 if the PSP's clock is plainly behind: earlier than the day this XLT was built. */
static int clock_behind(void)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__; /* "Nov  7 2026" */
    char mon[4] = {d[0], d[1], d[2], 0};
    const char *m = strstr(months, mon);
    struct tm b = {0};
    b.tm_year = atoi(d + 7) - 1900;
    b.tm_mon = m ? (int)(m - months) / 3 : 0;
    b.tm_mday = atoi(d + 4);
    return time(NULL) < mktime(&b);
}

/* Intermediate certificates of the chain being checked that verified; kept as trusted
 * once the whole handshake succeeds (keep_intermediates), so the next chain through
 * them stops there and skips their signature checks (ECDSA P-384 is slow on a PSP). */
#define PENDING_MAX 4
static unsigned char *s_pend[PENDING_MAX];
static int s_pend_len[PENDING_MAX], s_pend_n;
static char s_trust_path[256];
static int s_trust_added;

static void pending_clear(void)
{
    for (int i = 0; i < s_pend_n; i++) free(s_pend[i]);
    s_pend_n = 0;
}

static int in_trust(const unsigned char *der, int len)
{
    for (const mbedtls_x509_crt *c = &s_ca; c && c->raw.p; c = c->next)
        if ((int)c->raw.len == len && !memcmp(c->raw.p, der, len)) return 1;
    return 0;
}

static void keep_intermediates(void)
{
    for (int i = 0; i < s_pend_n; i++) {
        if (in_trust(s_pend[i], s_pend_len[i]) || mbedtls_x509_crt_parse_der(&s_ca, s_pend[i], s_pend_len[i]) != 0)
            continue;
        s_trust_added++;
        SceUID fd = sceIoOpen(s_trust_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            unsigned char l[2] = {(unsigned char)(s_pend_len[i] >> 8), (unsigned char)s_pend_len[i]};
            sceIoWrite(fd, l, 2);
            sceIoWrite(fd, s_pend[i], s_pend_len[i]);
            sceIoClose(fd);
        }
    }
    pending_clear();
}

/* The intermediates kept by earlier sessions. */
static int load_trusted(void)
{
    SceUID fd = sceIoOpen(s_trust_path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = 0;
    unsigned char l[2];
    static unsigned char der[8192];
    while (sceIoRead(fd, l, 2) == 2) {
        int len = l[0] << 8 | l[1];
        if (len <= 0 || len > (int)sizeof(der) || sceIoRead(fd, der, len) != len) break;
        if (!in_trust(der, len) && mbedtls_x509_crt_parse_der(&s_ca, der, len) == 0) n++;
    }
    sceIoClose(fd);
    return n;
}

/* Logs each certificate of a chain that doesn't verify, with mbedTLS's reasons. */
static int verify_cb(void *user, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)user;
    if (depth > 0 && !*flags && s_pend_n < PENDING_MAX &&
        !(crt->issuer_raw.len == crt->subject_raw.len && !memcmp(crt->issuer_raw.p, crt->subject_raw.p, crt->subject_raw.len)) &&
        !in_trust(crt->raw.p, (int)crt->raw.len)) { /* an intermediate, not a root, not known yet */
        unsigned char *c = malloc(crt->raw.len);
        if (c) {
            memcpy(c, crt->raw.p, crt->raw.len);
            s_pend[s_pend_n] = c;
            s_pend_len[s_pend_n++] = (int)crt->raw.len;
        }
    }
    if (*flags) {
        uint32_t dates = MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED;
        if (*flags & dates) s_cert_date_only = !(*flags & ~dates & ~MBEDTLS_X509_BADCERT_NOT_TRUSTED) || s_cert_date_only;
        char name[160], why[256];
        mbedtls_x509_dn_gets(name, sizeof(name), &crt->subject);
        mbedtls_x509_crt_verify_info(why, sizeof(why), "", *flags);
        for (char *q = why; *q; q++) if (*q == '\n') *q = ';';
        wlog("  cert %d: %s (valid %04d-%02d-%02d to %04d-%02d-%02d): flags %08X %s", depth, name,
             crt->valid_from.year, crt->valid_from.mon, crt->valid_from.day,
             crt->valid_to.year, crt->valid_to.mon, crt->valid_to.day, (unsigned)*flags, why);
    }
    return 0;
}

/* The cheapest first: X25519 for the key exchange (mbedTLS's default list starts at
 * P-521, very slow here), ChaCha20 (fast in software), then AES-128; the rest for
 * older servers. */
static const mbedtls_ecp_group_id s_curves[] = {
    MBEDTLS_ECP_DP_CURVE25519, MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_SECP384R1, MBEDTLS_ECP_DP_NONE,
};
static const int s_suites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,
    MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA,
    0,
};

static CURLcode ssl_ctx_cb(CURL *c, void *ssl_ctx, void *user)
{
    (void)c; (void)user;
    mbedtls_ssl_config *cfg = ssl_ctx;
    if (s_ca_ok)
        mbedtls_ssl_conf_ca_chain(cfg, &s_ca, NULL);
    mbedtls_ssl_conf_verify(cfg, verify_cb, NULL);
    mbedtls_ssl_conf_curves(cfg, s_curves);
    mbedtls_ssl_conf_ciphersuites(cfg, s_suites);
    return CURLE_OK;
}

typedef struct {
    Job *job;
    char *buf;
    int len, cap, limit;
    int file;      /* a page job that turned out to be a download: stopped */
    int too_big;
    char disposition[256];
} Xfer;

static size_t on_header(char *p, size_t size, size_t n, void *user)
{
    Xfer *x = user;
    size_t len = size * n;
    if (len > 20 && !strncasecmp(p, "content-disposition:", 20)) {
        size_t l = len - 20 < sizeof(x->disposition) - 1 ? len - 20 : sizeof(x->disposition) - 1;
        memcpy(x->disposition, p + 20, l);
        x->disposition[l] = 0;
    }
    return len;
}

static int is_page_type(const char *ct)
{
    if (!ct || !ct[0]) return 1; /* unknown: try it */
    return !strncasecmp(ct, "text/", 5) || strcasestr(ct, "html") || strcasestr(ct, "xml") ||
           !strncasecmp(ct, "image/", 6) || strcasestr(ct, "json");
}

static size_t on_data(char *p, size_t size, size_t n, void *user)
{
    Xfer *x = user;
    size_t bytes = size * n;
    if (x->len == 0 && x->job->kind == J_PAGE) {
        char *ct = NULL;
        curl_easy_getinfo(s_curl, CURLINFO_CONTENT_TYPE, &ct);
        if (!is_page_type(ct)) {
            x->file = 1;
            return 0; /* stops the transfer */
        }
    }
    if (x->len + (int)bytes > x->limit) {
        x->too_big = 1;
        bytes = x->limit - x->len;
        if (bytes == 0) return 0;
    }
    if (x->len + (int)bytes + 1 > x->cap) {
        int nc = x->cap ? x->cap * 2 : 64 * 1024;
        while (nc < x->len + (int)bytes + 1) nc *= 2;
        if (nc > x->limit + 1) nc = x->limit + 1;
        char *nb = realloc(x->buf, nc);
        if (!nb) return 0;
        x->buf = nb;
        x->cap = nc;
    }
    memcpy(x->buf + x->len, p, bytes);
    x->len += (int)bytes;
    x->buf[x->len] = 0;
    s_got = x->len;
    s_phase = WF_RECEIVING;
    return x->too_big ? 0 : size * n;
}

static int on_progress(void *user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    Xfer *x = user;
    (void)dlnow; (void)ultotal; (void)ulnow;
    if (dltotal > 0) s_total = dltotal;
    return stale(x->job) ? 1 : 0;
}

static int curl_ready(void)
{
    if (s_curl)
        return 1;
    static int once;
    if (once) { /* again after a lost connection: the certificates are still parsed */
        s_curl = curl_easy_init();
        return s_curl != NULL;
    }
    once = 1;
    int t0 = ms_now();
    curl_global_init(CURL_GLOBAL_ALL);
    mbedtls_x509_crt_init(&s_ca);
    int off = 0, good = 0;
    for (int i = 0; i < CACERT_COUNT; i++) {
        if (mbedtls_x509_crt_parse_der(&s_ca, cacert_der + off, cacert_len[i]) == 0) good++;
        off += cacert_len[i];
    }
    s_ca_ok = good > 0;
    int kept = load_trusted();
    s_curl = curl_easy_init();
    if (!s_curl)
        return 0;
    wlog("curl %s, %d of %d root certificates (+%d kept intermediates) in %d ms", curl_version(), good, CACERT_COUNT,
         kept, ms_now() - t0);
    {
        time_t now = time(NULL);
        struct tm *g = gmtime(&now);
        wlog("clock: time() = %ld (%04d-%02d-%02d %02d:%02d UTC)", (long)now, g ? g->tm_year + 1900 : 0,
             g ? g->tm_mon + 1 : 0, g ? g->tm_mday : 0, g ? g->tm_hour : 0, g ? g->tm_min : 0);
    }
    return 1;
}

static void set_common(CURL *c, Xfer *x, const Job *j, struct curl_slist *hdr)
{
    curl_easy_reset(c); /* keeps connections, cookies and the DNS cache */
    curl_easy_setopt(c, CURLOPT_URL, j->url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, ""); /* cookies, in memory, for this session */
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 16L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_cb);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, x);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, x);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, x);
    if (j->referer[0])
        curl_easy_setopt(c, CURLOPT_REFERER, j->referer);
    if (j->post) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, j->post);
    }
}

static void say_error(CURLcode rc, const char *url, char *err, int n)
{
    char host[128];
    url_host(url, host, sizeof(host));
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST: snprintf(err, n, "Couldn't find %.60s", host); break;
    case CURLE_COULDNT_CONNECT: snprintf(err, n, "Couldn't reach %.60s", host); break;
    case CURLE_OPERATION_TIMEDOUT: snprintf(err, n, "%.60s took too long to answer", host); break;
    case CURLE_PEER_FAILED_VERIFICATION:
        if (s_cert_date_only && clock_behind())
            snprintf(err, n, "The PSP's date is wrong, so no secure site can be checked: set it in Settings > Date & Time");
        else
            snprintf(err, n, "%.60s's certificate couldn't be verified: the connection isn't safe", host);
        break;
    case CURLE_SSL_CONNECT_ERROR:
        snprintf(err, n, "A secure connection to %.60s failed (it may need newer TLS than the PSP has)", host); break;
    case CURLE_TOO_MANY_REDIRECTS: snprintf(err, n, "%.60s keeps redirecting", host); break;
    case CURLE_UNSUPPORTED_PROTOCOL: snprintf(err, n, "This kind of address can't be opened"); break;
    case CURLE_GOT_NOTHING: snprintf(err, n, "%.60s sent nothing back", host); break;
    case CURLE_RECV_ERROR: case CURLE_SEND_ERROR: snprintf(err, n, "The connection to %.60s was lost", host); break;
    default: snprintf(err, n, "%.100s", curl_easy_strerror(rc)); break;
    }
}

/* Joins Wi-Fi if needed. 0 when ready. (A join is not cancelled by a newer
 * page: that page needs it too.) */
static int ensure_net(char *err, int n)
{
    if (net_is_up())
        return 0;
    s_phase = WF_WIFI;
    int t0 = ms_now();
    if (net_connect(s_profile, NULL) < 0) {
        snprintf(err, n, "%s", net_error());
        wlog("wifi: failed: %s", net_error());
        return -1;
    }
    wlog("wifi: connected to %s in %d ms", net_profile_name(), ms_now() - t0);
    return 0;
}

static void do_fetch(Job *j)
{
    WebResult res;
    memset(&res, 0, sizeof(res));
    res.gen = j->gen;
    char err[128] = "";
    if (stale(j)) goto dropped;
    if (ensure_net(err, sizeof(err)) < 0 || !curl_ready()) {
        if (!err[0]) snprintf(err, sizeof(err), "The browser couldn't start");
        goto failed;
    }
    if (stale(j)) goto dropped;
    int attempt = 0;
retry:
    s_phase = WF_CONNECTING;
    s_got = s_total = 0;
    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, j->kind == J_PAGE
        ? "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,text/plain;q=0.8,*/*;q=0.5"
        : "Accept: image/jpeg,image/png,image/gif;q=0.9,*/*;q=0.1");
    hdr = curl_slist_append(hdr, "Accept-Language: en-US,en;q=0.9,el;q=0.6");
    Xfer x = {j, NULL, 0, 0, j->kind == J_PAGE ? PAGE_CAP : IMG_CAP, 0, 0, ""};
    set_common(s_curl, &x, j, hdr);
    int t0 = ms_now();
    s_cert_date_only = 0;
    pending_clear();
    CURLcode rc = curl_easy_perform(s_curl);
    char suite[64] = "";
    if (rc == CURLE_OK || x.file || x.too_big) {
        struct curl_tlssessioninfo *ti = NULL;
        if (curl_easy_getinfo(s_curl, CURLINFO_TLS_SSL_PTR, &ti) == CURLE_OK && ti && ti->backend == CURLSSLBACKEND_MBEDTLS &&
            ti->internals) {
            const char *n = mbedtls_ssl_get_ciphersuite((mbedtls_ssl_context *)ti->internals);
            if (n) snprintf(suite, sizeof(suite), "%s", n);
        }
        int before = s_trust_added;
        keep_intermediates();
        if (s_trust_added != before)
            wlog("  kept %d intermediate certificate(s) as trusted", s_trust_added - before);
    } else {
        pending_clear();
    }
    curl_slist_free_all(hdr);
    int ms = ms_now() - t0;
    long code = 0;
    char *eff = NULL, *ct = NULL;
    double cl = -1;
    curl_easy_getinfo(s_curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(s_curl, CURLINFO_EFFECTIVE_URL, &eff);
    curl_easy_getinfo(s_curl, CURLINFO_CONTENT_TYPE, &ct);
    curl_easy_getinfo(s_curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl);
    s_phase = WF_WORKING;
    if (stale(j)) {
        free(x.buf);
        goto dropped;
    }
    if (j->kind == J_IMG && (rc != CURLE_OK || code >= 400))
        wlog("  picture %d: %s -> %ld %s", j->img, j->url, code, rc != CURLE_OK ? curl_easy_strerror(rc) : "");
    if (j->kind == J_PAGE) {
        double tls = 0, conn = 0;
        curl_easy_getinfo(s_curl, CURLINFO_APPCONNECT_TIME, &tls);
        curl_easy_getinfo(s_curl, CURLINFO_CONNECT_TIME, &conn);
        wlog("GET %s -> %ld, %d bytes, %d ms (connect %d, tls %d%s%s)%s%s", eff ? eff : j->url, code, x.len, ms,
             (int)(conn * 1000), (int)(tls * 1000), suite[0] ? ", " : "", suite, rc != CURLE_OK && !x.file && !x.too_big ? ": " : "",
             rc != CURLE_OK && !x.file && !x.too_big ? curl_easy_strerror(rc) : "");
    }
    if (rc != CURLE_OK && !x.file && !x.too_big) {
        free(x.buf);
        int lost = rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_CONNECT || rc == CURLE_OPERATION_TIMEDOUT ||
                   rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR || rc == CURLE_GOT_NOTHING;
        if (lost && j->kind == J_PAGE && attempt == 0 && !stale(j)) {
            /* The link may be gone (a USBNet cable pulled and put back, Wi-Fi out of range)
             * while the PSP still says it is connected: start over and try once more. */
            wlog("  network lost? (%s): joining again", curl_easy_strerror(rc));
            curl_easy_cleanup(s_curl); /* its connections are dead */
            s_curl = NULL;
            net_disconnect();
            attempt = 1;
            if (ensure_net(err, sizeof(err)) == 0 && curl_ready())
                goto retry;
            if (!err[0]) snprintf(err, sizeof(err), "The network was lost");
            goto failed;
        }
        say_error(rc, j->url, err, sizeof(err));
        goto failed;
    }
    /* hand it to the worker */
    Job *w = job_new(j->kind == J_PAGE ? J_PARSE : J_DECODE, j->gen);
    if (!w) {
        free(x.buf);
        snprintf(err, sizeof(err), "Out of memory");
        goto failed;
    }
    w->img = j->img;
    w->max_w = j->max_w;
    w->data = x.buf;
    w->len = x.len;
    w->status = (int)code;
    w->ms_fetch = ms;
    w->is_file = x.file;
    w->announced = cl > 0 ? (int64_t)cl : x.len;
    snprintf(w->url, sizeof(w->url), "%s", eff ? eff : j->url);
    snprintf(w->ctype, sizeof(w->ctype), "%s", ct ? ct : "");
    snprintf(w->disposition, sizeof(w->disposition), "%s", x.disposition);
    if (x.too_big && j->kind == J_PAGE)
        snprintf(w->err, sizeof(w->err), "truncated");
    q_put(&s_cpu, w);
    return;
failed:
    if (j->kind == J_PAGE) {
        WebPage *p = calloc(1, sizeof(WebPage));
        if (p) {
            p->kind = WP_ERROR;
            snprintf(p->url, sizeof(p->url), "%s", j->url);
            snprintf(p->err, sizeof(p->err), "%s", err);
        }
        res.kind = WR_PAGE;
        res.page = p;
    } else {
        res.kind = WR_IMG;
        res.img = j->img;
    }
    post_result(&res);
    return;
dropped:
    res.kind = j->kind == J_PAGE ? WR_PAGE : WR_IMG;
    res.img = j->img;
    res.gen = -1; /* the browser ignores it */
    post_result(&res);
}

/* ------------------------------------------------------------ downloads */

static volatile int s_dl_active, s_dl_stop, s_dl_kbps;
static volatile int64_t s_dl_got, s_dl_total;

typedef struct {
    SceUID fd;
    char buf[64 * 1024]; /* the Memory Stick likes big writes */
    int len;
    int failed;
} DlFile;

static int dl_flush(DlFile *f)
{
    if (f->len > 0 && sceIoWrite(f->fd, f->buf, f->len) != f->len) f->failed = 1;
    f->len = 0;
    return !f->failed;
}

static size_t on_file(char *p, size_t size, size_t n, void *user)
{
    DlFile *f = user;
    size_t bytes = size * n, done = 0;
    while (done < bytes) {
        size_t room = sizeof(f->buf) - f->len, k = bytes - done < room ? bytes - done : room;
        memcpy(f->buf + f->len, p + done, k);
        f->len += (int)k;
        done += k;
        if (f->len == (int)sizeof(f->buf) && !dl_flush(f)) return 0;
    }
    s_dl_got += (int64_t)bytes;
    return bytes;
}

static int on_dl_progress(void *user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ut, curl_off_t un)
{
    (void)user; (void)dlnow; (void)ut; (void)un;
    if (dltotal > 0) s_dl_total = dltotal;
    return s_dl_stop ? 1 : 0;
}

static void do_download(Job *j)
{
    WebResult res;
    memset(&res, 0, sizeof(res));
    res.kind = WR_DOWNLOAD;
    char err[128] = "";
    s_dl_got = s_dl_total = 0;
    s_dl_kbps = 0;
    const char *name = strrchr(j->path, '/');
    name = name ? name + 1 : j->path;
    if (ensure_net(err, sizeof(err)) < 0 || !curl_ready()) {
        snprintf(res.msg, sizeof(res.msg), "%s", err[0] ? err : "The network couldn't start");
        post_result(&res);
        return;
    }
    static DlFile f;
    f.len = 0;
    f.failed = 0;
    f.fd = sceIoOpen(j->path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f.fd < 0) {
        snprintf(res.msg, sizeof(res.msg), "Couldn't create %s", name);
        post_result(&res);
        return;
    }
    Xfer x = {j, NULL, 0, 0, 0, 0, 0, ""};
    struct curl_slist *hdr = curl_slist_append(NULL, "Accept: */*");
    set_common(s_curl, &x, j, hdr);
    curl_easy_setopt(s_curl, CURLOPT_WRITEFUNCTION, on_file);
    curl_easy_setopt(s_curl, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(s_curl, CURLOPT_XFERINFOFUNCTION, on_dl_progress);
    curl_easy_setopt(s_curl, CURLOPT_ACCEPT_ENCODING, NULL); /* the file as it is */
    s_phase = WF_RECEIVING;
    int t0 = ms_now();
    CURLcode rc = curl_easy_perform(s_curl);
    curl_slist_free_all(hdr);
    long code = 0;
    curl_easy_getinfo(s_curl, CURLINFO_RESPONSE_CODE, &code);
    int ok = rc == CURLE_OK && code < 400 && dl_flush(&f);
    sceIoClose(f.fd);
    int ms = ms_now() - t0;
    if (!ok) {
        sceIoRemove(j->path);
        if (s_dl_stop) snprintf(res.msg, sizeof(res.msg), "Download stopped");
        else if (f.failed) snprintf(res.msg, sizeof(res.msg), "Couldn't write %s: is the Memory Stick full?", name);
        else if (code >= 400) snprintf(res.msg, sizeof(res.msg), "The server refused the file (%ld)", code);
        else say_error(rc, j->url, res.msg, sizeof(res.msg));
    } else {
        res.ok = 1;
        snprintf(res.msg, sizeof(res.msg), "%s", j->path);
    }
    wlog("download %s -> %s: %s, %lld bytes in %d ms", j->url, j->path, ok ? "saved" : res.msg, (long long)s_dl_got, ms);
    post_result(&res);
}

static int net_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        Job *j = q_take(&s_net);
        if (!j) continue;
        if (j->kind == J_DOWNLOAD) {
            do_download(j);
            s_dl_active = 0;
        } else if (j->kind == J_DISCONNECT) {
            if (s_curl) { curl_easy_cleanup(s_curl); s_curl = NULL; } /* its connections are dead now */
            net_disconnect();
        } else {
            do_fetch(j);
        }
        s_phase = WF_IDLE;
        job_free(j);
    }
    return 0;
}

/* -------------------------------------------------------------- worker */

static int charset_enc(const char *cs)
{
    if (!cs[0]) return ENC_AUTO;
    if (strstr(cs, "utf-8") || strstr(cs, "utf8")) return ENC_UTF8;
    if (strstr(cs, "1253") || strstr(cs, "8859-7") || strstr(cs, "greek")) return ENC_1253;
    if (strstr(cs, "1252") || strstr(cs, "8859-1") || strstr(cs, "latin1") || strstr(cs, "ascii") ||
        strstr(cs, "8859-15"))
        return ENC_1252;
    return ENC_AUTO;
}

static void header_charset(const char *ct, char *out, int n)
{
    out[0] = 0;
    const char *c = strcasestr(ct, "charset=");
    if (!c) return;
    c += 8;
    while (*c == '"' || *c == ' ') c++;
    int k = 0;
    while (*c && *c != ';' && *c != '"' && *c != ' ' && k < n - 1)
        out[k++] = (char)tolower((unsigned char)*c++);
    out[k] = 0;
}

/* The page's bytes as UTF-8 (a new buffer, or the same one). */
static char *to_utf8(Job *j, int *len)
{
    char cs[32];
    header_charset(j->ctype, cs, sizeof(cs));
    if (!cs[0]) html_sniff_charset(j->data, j->len, cs, sizeof(cs));
    int enc = charset_enc(cs);
    if (enc == ENC_AUTO)
        enc = utf8_valid((const uint8_t *)j->data, j->len) ? ENC_UTF8 : ENC_1252;
    if (enc == ENC_UTF8) {
        *len = j->len;
        char *d = j->data;
        j->data = NULL;
        return d;
    }
    int used;
    char *u = text_to_utf8((const uint8_t *)j->data, j->len, enc, len, &used);
    return u;
}

/* Text and pictures are shown through small made-up pages. */
static char *wrap_plain(const char *t, int n, int *out_len)
{
    size_t cap = (size_t)n * 2 + 64;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t k = 0;
    k += sprintf(o, "<pre>");
    for (int i = 0; i < n; i++) {
        const char *rep = t[i] == '<' ? "&lt;" : t[i] == '&' ? "&amp;" : NULL;
        size_t need = rep ? strlen(rep) : 1;
        if (k + need + 16 >= cap) {
            cap *= 2;
            char *no = realloc(o, cap);
            if (!no) { free(o); return NULL; }
            o = no;
        }
        if (rep) { memcpy(o + k, rep, need); k += need; }
        else o[k++] = t[i];
    }
    k += sprintf(o + k, "</pre>");
    *out_len = (int)k;
    return o;
}

static void do_parse(Job *j)
{
    WebResult res;
    memset(&res, 0, sizeof(res));
    res.kind = WR_PAGE;
    res.gen = j->gen;
    WebPage *p = calloc(1, sizeof(WebPage));
    if (!p) {
        post_result(&res);
        return;
    }
    res.page = p;
    snprintf(p->url, sizeof(p->url), "%s", j->url);
    snprintf(p->ctype, sizeof(p->ctype), "%s", j->ctype);
    p->status = j->status;
    p->size = j->announced;
    p->ms_fetch = j->ms_fetch;
    int t0 = ms_now();
    const char *ct = j->ctype;
    char *html = NULL;
    int hlen = 0;
    if (j->is_file) {
        p->kind = WP_FILE;
        char sz[32] = "";
        if (j->announced > 0) {
            if (j->announced >= 1 << 20) snprintf(sz, sizeof(sz), ", %.1f MB", j->announced / 1048576.0);
            else snprintf(sz, sizeof(sz), ", %d KB", (int)((j->announced + 1023) / 1024));
        }
        snprintf(p->err, sizeof(p->err), "This is a file (%s%s), not a page", ct[0] ? ct : "unknown type", sz);
        url_file_name(j->url, j->disposition[0] ? j->disposition : NULL, p->filename, sizeof(p->filename));
    } else if (!strncasecmp(ct, "image/", 6)) {
        p->kind = WP_IMAGE;
        char tmp[URL_MAX + 64];
        hlen = snprintf(tmp, sizeof(tmp), "<img src=\"%s\">", j->url);
        html = strdup(tmp);
    } else {
        int len;
        char *u = to_utf8(j, &len);
        if (!u) {
            p->kind = WP_ERROR;
            snprintf(p->err, sizeof(p->err), "Out of memory");
        } else if (!strncasecmp(ct, "text/plain", 10) || strcasestr(ct, "json")) {
            p->kind = WP_TEXT;
            html = wrap_plain(u, len, &hlen);
            free(u);
        } else {
            p->kind = WP_HTML;
            html = u;
            hlen = len;
        }
    }
    if (html) {
        p->doc = html_parse(html, hlen);
        free(html);
        if (p->doc) {
            if (j->err[0]) p->doc->truncated = 1;
            p->imgs = calloc(p->doc->nimgs > 0 ? p->doc->nimgs : 1, sizeof(WebImgState));
            WebLayCfg c = s_cfg;
            c.imgs = p->imgs;
            p->lay = p->imgs ? weblay_run(p->doc, &c) : NULL;
        }
        if (!p->doc || !p->lay) {
            html_free(p->doc);
            p->doc = NULL;
            weblay_free(p->lay);
            p->lay = NULL;
            p->kind = WP_ERROR;
            snprintf(p->err, sizeof(p->err), "The page is too big for the PSP's memory");
        }
    }
    p->ms_parse = ms_now() - t0;
    if (p->doc)
        wlog("  parsed: %d items, %d links, %d pictures, %d px tall in %d ms (%d KB free)", p->doc->nitems,
             p->doc->nlinks, p->doc->nimgs, p->lay->height, p->ms_parse, (int)(sceKernelTotalFreeMemSize() >> 10));
    if (stale(j)) {
        wf_free_page(p);
        res.page = NULL;
        res.gen = -1;
    }
    post_result(&res);
}

static uint8_t *s_rgba; /* decode buffer: IMG_MAX_W x IMG_MAX_H RGBA */

/* Decodes into a texture: 5650 if opaque, 4444 if it has transparency. */
static void do_decode(Job *j)
{
    WebResult res;
    memset(&res, 0, sizeof(res));
    res.kind = WR_IMG;
    res.gen = stale(j) ? -1 : j->gen;
    res.img = j->img;
    if (stale(j))
        goto out;
    if (!j->data || j->len < 8) {
        wlog("  picture %d: nothing received", j->img);
        goto out;
    }
    if (!s_rgba) s_rgba = malloc(IMG_MAX_W * IMG_MAX_H * 4);
    if (!s_rgba) goto out;
    int mw = j->max_w > 0 && j->max_w < IMG_MAX_W ? j->max_w : IMG_MAX_W;
    int w, h;
    if (img_decode_mem((const uint8_t *)j->data, j->len, IMG_FIT, mw, IMG_MAX_H, s_rgba, &w, &h) < 0 || w <= 0 || h <= 0) {
        const unsigned char *b = (const unsigned char *)j->data;
        wlog("  picture %d: can't decode %d bytes (starts %02X %02X %02X %02X)", j->img, j->len, b[0], b[1], b[2], b[3]);
        goto out;
    }
    int stride = mw * 4;
    int alpha = 0;
    for (int y = 0; y < h && !alpha; y++) {
        const uint8_t *r = s_rgba + (size_t)y * stride;
        for (int x = 0; x < w; x++)
            if (r[x * 4 + 3] < 250) { alpha = 1; break; }
    }
    int tbw = (w + 7) & ~7, rows = h + 1; /* one spare row and column for the filter */
    if (tbw == w) tbw += 8;
    uint16_t *t = memalign(16, (size_t)tbw * rows * 2);
    if (!t) goto out;
    for (int y = 0; y < rows; y++) {
        const uint8_t *r = s_rgba + (size_t)(y < h ? y : h - 1) * stride;
        uint16_t *o = t + (size_t)y * tbw;
        for (int x = 0; x < tbw; x++) {
            const uint8_t *px = r + (x < w ? x : w - 1) * 4;
            if (alpha)
                o[x] = (uint16_t)((px[3] >> 4) << 12 | (px[2] >> 4) << 8 | (px[1] >> 4) << 4 | (px[0] >> 4));
            else
                o[x] = (uint16_t)((px[2] >> 3) << 11 | (px[1] >> 2) << 5 | (px[0] >> 3));
        }
    }
    sceKernelDcacheWritebackRange(t, tbw * rows * 2);
    int tw = 1, th = 1;
    while (tw < w) tw <<= 1;
    while (th < h) th <<= 1;
    res.ok = 1;
    res.tex = t;
    res.psm = alpha ? 2 /* GU_PSM_4444 */ : 0 /* GU_PSM_5650 */;
    res.tw = tw;
    res.th = th;
    res.tbw = tbw;
    res.w = w;
    res.h = h;
out:
    post_result(&res);
}

/* data: URLs (base64 pictures inside the page) need no download. */
static int data_url(const char *u, char **out, int *len)
{
    if (strncasecmp(u, "data:", 5)) return 0;
    const char *comma = strchr(u, ',');
    if (!comma) return -1;
    int b64 = 0;
    for (const char *s = u; s < comma; s++)
        if (!strncasecmp(s, ";base64", 7)) b64 = 1;
    const char *s = comma + 1;
    int n = (int)strlen(s);
    char *o = malloc(n + 1);
    if (!o) return -1;
    int k = 0;
    if (b64) {
        unsigned v = 0;
        int bits = 0;
        for (; *s; s++) {
            int c = *s;
            int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26
                  : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
            if (d < 0) continue;
            v = v << 6 | d;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                o[k++] = (char)(v >> bits);
            }
        }
    } else {
        for (; *s; s++) {
            if (*s == '%' && s[1] && s[2]) {
                char h[3] = {s[1], s[2], 0};
                o[k++] = (char)strtol(h, NULL, 16);
                s += 2;
            } else {
                o[k++] = *s;
            }
        }
    }
    *out = o;
    *len = k;
    return 1;
}

static int cpu_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        Job *j = q_take(&s_cpu);
        if (!j) continue;
        switch (j->kind) {
        case J_PARSE: do_parse(j); break;
        case J_DECODE: do_decode(j); break;
        case J_LAYOUT: {
            WebResult r;
            memset(&r, 0, sizeof(r));
            r.kind = WR_LAYOUT;
            r.gen = stale(j) ? -1 : j->gen;
            if (!stale(j)) {
                WebLayCfg c = s_cfg;
                c.imgs = j->imgs;
                r.lay = weblay_run(j->doc, &c);
            }
            post_result(&r);
            break;
        }
        case J_FREE:
            if (j->page) {
                html_free(j->page->doc);
                weblay_free(j->page->lay);
                free(j->page->imgs);
                free(j->page);
            }
            break;
        }
        job_free(j);
    }
    return 0;
}

/* ------------------------------------------------------------------ API */

static int s_started;

void wf_init(const char *app_dir)
{
    static int done;
    if (done) return; /* main.c may have called it already (cache/netboot) */
    done = 1;
    snprintf(s_log, sizeof(s_log), "%.200scache/web.log", app_dir); /* app_dir ends with '/' */
    snprintf(s_trust_path, sizeof(s_trust_path), "%.200scache/tls_trust.bin", app_dir);
    log_create();
    wlog("XLT started, %d KB free", (int)(sceKernelTotalFreeMemSize() >> 10));
    net_set_log(wlog);
}

static void start(void)
{
    if (s_started)
        return;
    s_started = 1;
    q_init(&s_net, "xl_wnet");
    q_init(&s_cpu, "xl_wcpu");
    s_res_lock = sceKernelCreateSema("xl_wres", 0, 1, 1, NULL);
    /* below the UI (0x20); the network thread is mostly waiting on sockets */
    SceUID t = sceKernelCreateThread("xl_webnet", net_thread, 0x2A, 0x40000, PSP_THREAD_ATTR_USER, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
    t = sceKernelCreateThread("xl_webcpu", cpu_thread, 0x31, 0x20000, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

void wf_set_layout(const WebLayCfg *cfg) { s_cfg = *cfg; s_cfg.imgs = NULL; }

void wf_navigate(int gen, const char *url, const char *post, const char *referer)
{
    start();
    Job *j = job_new(J_PAGE, gen);
    if (!j) return;
    snprintf(j->url, sizeof(j->url), "%s", url);
    snprintf(j->referer, sizeof(j->referer), "%s", referer ? referer : "");
    j->post = post ? strdup(post) : NULL;
    __sync_fetch_and_add(&s_pending, 1);
    q_put(&s_net, j);
}

void wf_image(int gen, int img, const char *url, int max_w, const char *referer)
{
    start();
    char *data;
    int len;
    int du = data_url(url, &data, &len);
    Job *j = job_new(du > 0 ? J_DECODE : J_IMG, gen);
    if (!j) {
        if (du > 0) free(data);
        return;
    }
    j->img = img;
    j->max_w = max_w;
    __sync_fetch_and_add(&s_pending, 1);
    if (du > 0) {
        j->data = data;
        j->len = len;
        q_put(&s_cpu, j);
        return;
    }
    if (du < 0 || !url_is_web(url)) { /* nothing to fetch: answer at once */
        WebResult r;
        memset(&r, 0, sizeof(r));
        r.kind = WR_IMG;
        r.gen = gen;
        r.img = img;
        post_result(&r);
        job_free(j);
        return;
    }
    snprintf(j->url, sizeof(j->url), "%s", url);
    snprintf(j->referer, sizeof(j->referer), "%s", referer ? referer : "");
    q_put(&s_net, j);
}

void wf_relayout(int gen, const HtmlDoc *doc, const WebImgState *imgs, int nimgs)
{
    start();
    Job *j = job_new(J_LAYOUT, gen);
    if (!j) return;
    j->doc = doc;
    j->imgs = malloc((nimgs > 0 ? nimgs : 1) * sizeof(WebImgState));
    if (!j->imgs) { free(j); return; }
    memcpy(j->imgs, imgs, nimgs * sizeof(WebImgState));
    q_put(&s_cpu, j);
}

void wf_cancel(int gen)
{
    if (gen > s_cancel_gen)
        s_cancel_gen = gen;
}

void wf_free_page(WebPage *p)
{
    if (!p) return;
    start();
    Job *j = job_new(J_FREE, 0);
    if (!j) return;
    j->page = p;
    q_put(&s_cpu, j);
}

int wf_busy(void) { return s_pending > 0; }

void wf_progress(int *phase, int64_t *got, int64_t *total)
{
    *phase = s_phase;
    *got = s_got;
    *total = s_total;
}

void wf_use_profile(int id) { s_profile = id; }

void wf_download(const char *url, const char *referer, const char *path)
{
    start();
    if (s_dl_active) return;
    Job *j = job_new(J_DOWNLOAD, 0);
    if (!j) return;
    snprintf(j->url, sizeof(j->url), "%s", url);
    snprintf(j->referer, sizeof(j->referer), "%s", referer ? referer : "");
    snprintf(j->path, sizeof(j->path), "%s", path);
    s_dl_stop = 0;
    s_dl_got = s_dl_total = 0;
    s_dl_active = 1;
    q_put(&s_net, j);
}

void wf_download_stop(void) { if (s_dl_active) s_dl_stop = 1; }

int wf_download_progress(int64_t *got, int64_t *total, int *kbps)
{
    static int64_t last_got;
    static unsigned last_ms;
    static int rate;
    unsigned now = (unsigned)ms_now();
    if (now - last_ms >= 1000) { /* a rate over the last second or so */
        int64_t g = s_dl_got;
        rate = last_ms && g >= last_got ? (int)((g - last_got) * 1000 / (now - last_ms) / 1024) : 0;
        last_got = g;
        last_ms = now;
    }
    *got = s_dl_got;
    *total = s_dl_total;
    *kbps = rate;
    return s_dl_active;
}

void wf_disconnect(void)
{
    if (!s_started) return;
    Job *j = job_new(J_DISCONNECT, 0);
    if (j) q_put(&s_net, j);
}

void wf_shutdown(void)
{
    if (!s_started) return;
    wf_cancel(0x7fffffff);
    net_shutdown();
}
