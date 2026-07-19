#include "git/git_url.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "foundation/subprocess.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define cbm_git_pid _getpid
#else
#include <unistd.h>
#define cbm_git_pid getpid
#endif

static char *git_url_strdup(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n);
    return out;
}

static char *git_url_strndup(const char *s, size_t n) {
    char *out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static bool is_safe_name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

static bool is_scheme_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '+' || c == '-' || c == '.';
}

/* Length of `s` after the project-name / clone-dir character mapping: '/' →
 * slash_repl, non-ASCII bytes → two hex digits, everything else unsafe → '_'.
 * Mirrors map_seg byte-for-byte so the two never disagree on a buffer bound. */
static size_t seg_mapped_len(const char *s, const char *slash_repl) {
    if (!s) {
        return 0;
    }
    size_t repl_len = strlen(slash_repl);
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '/') {
            n += repl_len;
        } else if (is_safe_name_char(c)) {
            n++;
        } else if (c >= 0x80) {
            n += 2;
        } else {
            n++;
        }
    }
    return n;
}

static size_t map_seg(char *out, size_t off, const char *s, const char *slash_repl) {
    static const char hex_digits[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '/') {
            for (const char *r = slash_repl; *r; r++) {
                out[off++] = *r;
            }
        } else if (is_safe_name_char(c)) {
            out[off++] = (char)c;
        } else if (c >= 0x80) {
            out[off++] = hex_digits[(c >> 4) & 0xF];
            out[off++] = hex_digits[c & 0xF];
        } else {
            out[off++] = '_';
        }
    }
    return off;
}

static size_t map_lit(char *out, size_t off, const char *lit) {
    for (const char *p = lit; *p; p++) {
        out[off++] = *p;
    }
    return off;
}

/* project_name = host__path[__ref], with '/' → "__" so "o/r" and "o-r" stay
 * distinct (cbm_project_name_from_path maps both to "o-r"). A host ':' (port)
 * and any other unsafe char become '_'. */
static char *build_project_name(const char *host, const char *path, const char *ref) {
    const char *sep = "__";
    size_t cap = seg_mapped_len(host, "") + seg_mapped_len(sep, "") +
                 seg_mapped_len(path, sep) +
                 (ref ? seg_mapped_len(sep, "") + seg_mapped_len(ref, "_") : 0) + 1;
    char *out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t off = 0;
    off = map_seg(out, off, host, "");
    off = map_seg(out, off, sep, "");
    off = map_seg(out, off, path, sep);
    if (ref) {
        off = map_seg(out, off, sep, "");
        off = map_seg(out, off, ref, "_");
    }
    out[off] = '\0';
    return out;
}

/* clone_dir = <cache>/git-cache/<host>/<path>/[<ref>/], keeping '/' in the
 * path (real directories), mapping host ':' → '_' (Windows forbids ':' in
 * names) and ref '/' → '_'. */
static char *build_clone_dir(const char *host, const char *path, const char *ref) {
    const char *cache = cbm_resolve_cache_dir();
    if (!cache || !cache[0]) {
        return NULL;
    }
    size_t cap = strlen(cache) + strlen("/git-cache/") + seg_mapped_len(host, "") + 1 +
                 seg_mapped_len(path, "/") + 1 +
                 (ref ? seg_mapped_len(ref, "_") + 1 : 0) + 1;
    char *out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t off = 0;
    off = map_lit(out, off, cache);
    off = map_lit(out, off, "/git-cache/");
    off = map_seg(out, off, host, "");
    out[off++] = '/';
    off = map_seg(out, off, path, "/");
    out[off++] = '/';
    if (ref) {
        off = map_seg(out, off, ref, "_");
        out[off++] = '/';
    }
    out[off] = '\0';
    cbm_normalize_path_sep(out);
    return out;
}

bool cbm_git_url_is_url(const char *s) {
    if (!s || !s[0]) {
        return false;
    }
    const char *sep = strstr(s, "://");
    if (sep && sep > s) {
        for (const char *p = s; p < sep; p++) {
            if (!is_scheme_char((unsigned char)*p)) {
                return false;
            }
        }
        return true;
    }
    /* scp-like: user@host:path — a ':' before any '/' after the '@'. This
     * excludes Windows drive paths ("C:/..."), which have no '@'. */
    const char *at = strchr(s, '@');
    if (at && at > s) {
        const char *co = strchr(at + 1, ':');
        const char *sl = strchr(at + 1, '/');
        if (co && (!sl || co < sl)) {
            return true;
        }
    }
    return false;
}

void cbm_git_url_free(cbm_git_url_t *u) {
    if (!u) {
        return;
    }
    free(u->raw);
    free(u->clone_url);
    free(u->host);
    free(u->path);
    free(u->ref);
    free(u->project_name);
    free(u->clone_dir);
    memset(u, 0, sizeof(*u));
}

int cbm_git_url_normalize(const char *url, cbm_git_url_t *out) {
    if (!out) {
        return CBM_NOT_FOUND;
    }
    memset(out, 0, sizeof(*out));
    if (!url || !url[0] || !cbm_git_url_is_url(url)) {
        return CBM_NOT_FOUND;
    }

    char *s = git_url_strdup(url);
    out->raw = git_url_strdup(url);
    if (!s || !out->raw) {
        free(s);
        cbm_git_url_free(out);
        return CBM_NOT_FOUND;
    }

    char *rest = s;

    /* 1. Strip "<scheme>://" (validated scheme chars only). The scheme is then
     *    gated against an allowlist: file:// would let a caller point clone at
     *    an arbitrary local path (SSRF / disk read), and any non-transport
     *    scheme is rejected. ext:: can't reach here — it has no "://". */
    char *scheme_sep = strstr(rest, "://");
    bool had_scheme = false;
    if (scheme_sep && scheme_sep > rest) {
        bool ok = true;
        for (char *p = rest; p < scheme_sep; p++) {
            if (!is_scheme_char((unsigned char)*p)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            for (char *p = rest; p < scheme_sep; p++) {
                *p = (char)tolower((unsigned char)*p);
            }
            *scheme_sep = '\0';
            static const char *const allowed[] = {"http", "https", "ssh", "git"};
            bool allowed_scheme = false;
            for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
                if (strcmp(rest, allowed[i]) == 0) {
                    allowed_scheme = true;
                    break;
                }
            }
            if (!allowed_scheme) {
                free(s);
                cbm_git_url_free(out);
                return CBM_NOT_FOUND;
            }
            rest = scheme_sep + 3;
            had_scheme = true;
        }
    }

    /* 2. Strip a leading "user@" that sits before the first '/' — covers
     *    ssh://user@host/... and the scp-like user@host:path form. */
    {
        char *first_slash = strchr(rest, '/');
        char *at = strchr(rest, '@');
        if (at && (!first_slash || at < first_slash)) {
            rest = at + 1;
        }
    }

    /* 3. Split a trailing "@ref": the last '@' in what remains. After the
     *    user@ strip, any '@' left is our ref separator (repo paths do not
     *    normally contain '@'). */
    char *ref = NULL;
    size_t clone_url_len = strlen(url);
    {
        char *ref_at = strrchr(rest, '@');
        if (ref_at) {
            clone_url_len = (size_t)(ref_at - s);
            *ref_at = '\0';
            ref = ref_at + 1;
            if (!ref[0]) {
                ref = NULL;
                clone_url_len = strlen(url);
            }
        }
    }

    out->clone_url = git_url_strndup(url, clone_url_len);
    if (!out->clone_url) {
        free(s);
        cbm_git_url_free(out);
        return CBM_NOT_FOUND;
    }

    /* 4. Split host / path. Scheme form: host ends at the first '/' (may
     *    hold ":port"). scp-like: host ends at the first ':'. */
    char *host;
    char *path;
    if (had_scheme) {
        char *sl = strchr(rest, '/');
        if (sl) {
            *sl = '\0';
            host = rest;
            path = sl + 1;
        } else {
            host = rest;
            path = "";
        }
    } else {
        char *co = strchr(rest, ':');
        if (!co) {
            free(s);
            cbm_git_url_free(out);
            return CBM_NOT_FOUND;
        }
        *co = '\0';
        host = rest;
        path = co + 1;
    }

    /* 5. Trim trailing slashes and a trailing ".git" from the path. */
    {
        size_t plen = strlen(path);
        while (plen > 0 && path[plen - 1] == '/') {
            path[--plen] = '\0';
        }
        const char *git_suffix = ".git";
        size_t suf_len = strlen(git_suffix);
        if (plen >= suf_len && strcmp(path + plen - suf_len, git_suffix) == 0) {
            path[plen - suf_len] = '\0';
            plen -= suf_len;
        }
        while (plen > 0 && path[plen - 1] == '/') {
            path[--plen] = '\0';
        }
    }

    if (!host[0] || !path[0]) {
        free(s);
        cbm_git_url_free(out);
        return CBM_NOT_FOUND;
    }

    /* 6. Lowercase host (DNS is case-insensitive); keep path case-sensitive. */
    for (char *p = host; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    out->host = git_url_strdup(host);
    out->path = git_url_strdup(path);
    out->ref = ref ? git_url_strdup(ref) : NULL;
    free(s);

    if (!out->host || !out->path || (ref && !out->ref)) {
        cbm_git_url_free(out);
        return CBM_NOT_FOUND;
    }

    out->project_name = build_project_name(out->host, out->path, out->ref);
    out->clone_dir = build_clone_dir(out->host, out->path, out->ref);
    if (!out->project_name || !out->clone_dir ||
        !cbm_validate_project_name(out->project_name)) {
        cbm_git_url_free(out);
        return CBM_NOT_FOUND;
    }
    return 0;
}

enum {
    GIT_URL_DEFAULT_RETRIES = 3,
    GIT_URL_MAX_RETRIES = 8,
    GIT_URL_RETRY_DELAY_US = 250000,
    GIT_URL_INT_BUFFERS = 4,
};

static const char *git_url_int(int value) {
    static CBM_TLS char buffers[GIT_URL_INT_BUFFERS][CBM_SZ_16];
    static CBM_TLS unsigned next;
    char *out = buffers[next++ % GIT_URL_INT_BUFFERS];
    (void)snprintf(out, CBM_SZ_16, "%d", value);
    return out;
}

static int run_git(const char *const *argv) {
    cbm_proc_opts_t opts = {0};
    opts.bin = "git";
    opts.argv = argv;
    opts.log_file = NULL; /* discard output — only the exit code matters */
    /* No quiet-timeout: with log_file=NULL there is nothing to tail, so a
     * timeout would only mis-kill a long clone. git finishes or fails on its
     * own; a genuine hang is the caller's to interrupt. */
    opts.quiet_timeout_ms = 0;
    cbm_proc_result_t r;
    int rc = cbm_subprocess_run(&opts, &r);
    if (rc != 0) {
        cbm_log_warn("git_url.spawn_failed", "bin", "git");
        return CBM_NOT_FOUND;
    }
    if (r.outcome != CBM_PROC_CLEAN) {
        cbm_log_warn("git_url.git_failed", "outcome", cbm_proc_outcome_str(r.outcome));
        cbm_log_int(CBM_LOG_WARN, "git_url.git_exit", "code", (int64_t)r.exit_code);
        return CBM_NOT_FOUND;
    }
    return 0;
}

static int git_retry_limit(void) {
    const char *value = getenv("CBM_GIT_RETRIES");
    if (!value || !value[0]) {
        return GIT_URL_DEFAULT_RETRIES;
    }
    int retries = atoi(value);
    if (retries < 1) {
        return 1;
    }
    return retries > GIT_URL_MAX_RETRIES ? GIT_URL_MAX_RETRIES : retries;
}

static int run_git_retry(const char *const *argv, const char *operation) {
    int attempts = git_retry_limit();
    for (int attempt = 1; attempt <= attempts; attempt++) {
        if (run_git(argv) == 0) {
            return 0;
        }
        if (attempt < attempts) {
            cbm_log_warn("git_url.retry", "operation", operation, "attempt",
                         git_url_int(attempt), "max", git_url_int(attempts));
            cbm_usleep((unsigned int)(GIT_URL_RETRY_DELAY_US * attempt));
        }
    }
    return CBM_NOT_FOUND;
}

static bool git_trim_path(const char *path, char *out, size_t out_size);

static bool git_checkout_is_valid(const char *path) {
    /* Cheap local probe: an interrupted clone normally has no resolvable HEAD.
     * Avoid `git fsck` here because search_code calls this path without fetch and
     * fsck would turn every search into a full object-database scan. */
    const char *argv[] = {"git", "-C", path, "rev-parse", "--verify", "HEAD^{commit}", NULL};
    return run_git(argv) == 0;
}

bool cbm_git_url_cache_is_valid(const cbm_git_url_t *u) {
    if (!u || !u->clone_dir || !u->clone_dir[0]) {
        return false;
    }
    char path[CBM_SZ_4K];
    return git_trim_path(u->clone_dir, path, sizeof(path)) && git_checkout_is_valid(path);
}

static bool git_remove_cache(const char *path) {
    if (cbm_remove_tree(path)) {
        return true;
    }
    cbm_log_warn("git_url.cache_remove_failed", "path", path);
    return false;
}

static bool git_remove_stale_partials(const char *clone_path) {
    char parent[CBM_SZ_4K];
    int written = snprintf(parent, sizeof(parent), "%s", clone_path);
    if (written < 0 || (size_t)written >= sizeof(parent)) {
        return false;
    }
    /* Trim a trailing slash so the basename (base) is never empty — otherwise
     * the partial-dir prefix would never match. Self-sufficient: no caller
     * contract required. */
    while (written > 1 && parent[written - 1] == '/') {
        parent[--written] = '\0';
    }
    char *slash = strrchr(parent, '/');
    if (!slash) {
        return true;
    }
    *slash = '\0';
    const char *base = slash + 1;
    char prefix[CBM_SZ_256];
    int prefix_len = snprintf(prefix, sizeof(prefix), "%s.partial.", base);
    if (prefix_len < 0 || (size_t)prefix_len >= sizeof(prefix)) {
        return false;
    }

    cbm_dir_t *dir = cbm_opendir(parent);
    if (!dir) {
        return false;
    }
    bool ok = true;
    while (ok) {
        cbm_dirent_t *entry = cbm_readdir(dir);
        if (!entry) {
            break;
        }
        if (strncmp(entry->name, prefix, (size_t)prefix_len) != 0) {
            continue;
        }
        char partial[CBM_SZ_4K];
        int partial_len = snprintf(partial, sizeof(partial), "%s/%s", parent, entry->name);
        if (partial_len < 0 || (size_t)partial_len >= sizeof(partial) ||
            !git_remove_cache(partial)) {
            ok = false;
        }
    }
    cbm_closedir(dir);
    return ok;
}

static bool git_trim_path(const char *path, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s", path ? path : "");
    if (written < 0 || (size_t)written >= out_size || written == 0) {
        return false;
    }
    while (written > 1 && out[written - 1] == '/') {
        out[--written] = '\0';
    }
    return true;
}

static int git_clone_staged(const cbm_git_url_t *u, const char *temp_dir, const char *ref) {
    const char *clone[] = {"git", "clone", "--quiet", u->clone_url, temp_dir, NULL};
    /* The caller owns the retry loop for clone: every failed attempt must
     * remove temp_dir before git sees it again, otherwise git refuses to reuse
     * its own partial destination. */
    if (run_git(clone) != 0) {
        return CBM_NOT_FOUND;
    }

    if (ref) {
        const char *fetch[] = {"git", "-C", temp_dir, "fetch", "--quiet", "origin", NULL};
        if (run_git_retry(fetch, "fetch") != 0) {
            return CBM_NOT_FOUND;
        }
        const char *checkout[] = {"git", "-C", temp_dir, "checkout", "--quiet", ref, NULL};
        if (run_git_retry(checkout, "checkout") != 0) {
            return CBM_NOT_FOUND;
        }
    }
    return git_checkout_is_valid(temp_dir) ? 0 : CBM_NOT_FOUND;
}

int cbm_git_url_ensure_cloned(const cbm_git_url_t *u) {
    if (!u || !u->clone_url || !u->clone_dir || !u->clone_dir[0]) {
        return CBM_NOT_FOUND;
    }

    char clone_path[CBM_SZ_4K];
    if (!git_trim_path(u->clone_dir, clone_path, sizeof(clone_path))) {
        return CBM_NOT_FOUND;
    }

    char parent[CBM_SZ_4K];
    int parent_len = snprintf(parent, sizeof(parent), "%s", clone_path);
    if (parent_len < 0 || (size_t)parent_len >= sizeof(parent)) {
        return CBM_NOT_FOUND;
    }
    char *slash = strrchr(parent, '/');
    if (!slash) {
        return CBM_NOT_FOUND;
    }
    *slash = '\0';
    if (!cbm_mkdir_p(parent, 0755)) {
        cbm_log_warn("git_url.mkdir_failed", "parent", parent);
        return CBM_NOT_FOUND;
    }
    if (!git_remove_stale_partials(clone_path)) {
        cbm_log_warn("git_url.partial_cleanup_failed", "path", clone_path);
    }
    const char *ref = (u->ref && u->ref[0]) ? u->ref : NULL;
    bool cache_exists = cbm_file_exists(clone_path);
    if (cache_exists && !git_checkout_is_valid(clone_path)) {
        cbm_log_warn("git_url.invalid_cache", "path", clone_path);
        if (!git_remove_cache(clone_path)) {
            return CBM_NOT_FOUND;
        }
        cache_exists = false;
    }

    if (cache_exists) {
        const char *fetch[] = {"git", "-C", clone_path, "fetch", "--quiet", "origin", NULL};
        if (run_git_retry(fetch, "fetch") == 0) {
            if (!ref) {
                return git_checkout_is_valid(clone_path) ? 0 : CBM_NOT_FOUND;
            }
            const char *checkout[] = {"git", "-C", clone_path, "checkout", "--quiet", ref, NULL};
            if (run_git_retry(checkout, "checkout") == 0 && git_checkout_is_valid(clone_path)) {
                return 0;
            }
            return CBM_NOT_FOUND;
        }

        /* A valid cached checkout is still useful when the refresh is offline.
         * A pinned ref must already be locally available; never index a wrong
         * ref just because refresh failed. */
        if (!ref) {
            cbm_log_warn("git_url.stale_cache", "path", clone_path);
            return 0;
        }
        const char *checkout[] = {"git", "-C", clone_path, "checkout", "--quiet", ref, NULL};
        if (run_git_retry(checkout, "checkout_stale") == 0 && git_checkout_is_valid(clone_path)) {
            cbm_log_warn("git_url.stale_cache", "path", clone_path, "ref", ref);
            return 0;
        }
        return CBM_NOT_FOUND;
    }

    for (int attempt = 1; attempt <= git_retry_limit(); attempt++) {
        char temp_dir[CBM_SZ_4K];
        int temp_len = snprintf(temp_dir, sizeof(temp_dir), "%s.partial.%ld.%d", clone_path,
                                (long)cbm_git_pid(), attempt);
        if (temp_len < 0 || (size_t)temp_len >= sizeof(temp_dir)) {
            return CBM_NOT_FOUND;
        }
        if (!git_remove_cache(temp_dir)) {
            return CBM_NOT_FOUND;
        }
        cbm_log_info("git_url.clone", "host", u->host, "ref", ref ? ref : "default",
                     "attempt", git_url_int(attempt));
        if (git_clone_staged(u, temp_dir, ref) == 0) {
            if (cbm_rename_replace(temp_dir, clone_path) == 0) {
                return 0;
            }
            cbm_log_warn("git_url.cache_install_failed", "path", clone_path);
        }
        if (!git_remove_cache(temp_dir)) {
            return CBM_NOT_FOUND;
        }
        if (attempt < git_retry_limit()) {
            cbm_usleep((unsigned int)(GIT_URL_RETRY_DELAY_US * attempt));
        }
    }
    return CBM_NOT_FOUND;
}
