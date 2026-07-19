#ifndef CBM_GIT_URL_H
#define CBM_GIT_URL_H

#include <stdbool.h>

/* Normalized view of a remote git URL, so index_repository / search_code can
 * take a URL in place of a local repo_path / project.
 *
 * Produced by cbm_git_url_normalize; release with cbm_git_url_free.
 *
 *   raw           the input URL verbatim
 *   clone_url     the URL without the trailing @ref (what `git clone` receives)
 *   host          lowercased, scheme and user@ stripped, port kept
 *                 ("github.com", "gitea.example.com:3000")
 *   path          repo path under the host with slashes preserved and a
 *                 trailing ".git" removed ("DeusData/codebase-memory-mcp")
 *   ref           requested branch / tag / commit, or NULL for the remote's
 *                 default branch
 *   project_name  validator-safe token used as the <cache>/<name>.db stem.
 *                 Slashes map to "__" (not "-", as cbm_project_name_from_path
 *                 would) so "o/r" and "o-r" cannot collapse to one DB.
 *   clone_dir     absolute working-tree path under the cache:
 *                 <cache>/git-cache/<host>/<path>/[@ref/]
 *
 * Recognized forms all normalize to the same host/path:
 *   https://github.com/o/r[.git][@ref]
 *   git@github.com:o/r[.git][@ref]      (scp-like)
 *   ssh://[git@]github.com[:22]/o/r[.git][@ref]
 * A trailing "@ref" is split off only when it is not the user@ of an scp-like
 * or ssh:// URL. */
typedef struct {
    char *raw;
    char *clone_url;
    char *host;
    char *path;
    char *ref;
    char *project_name;
    char *clone_dir;
} cbm_git_url_t;

/* True iff `s` looks like a remote git URL — a scheme:// prefix or the
 * scp-like user@host: shape — rather than a local path. The wire points in
 * index_repository / search_code use this to decide whether to clone. */
bool cbm_git_url_is_url(const char *s);

/* Parse `url` into *out. Returns 0 on success (free *out with
 * cbm_git_url_free), CBM_NOT_FOUND on malformed or non-URL input. */
int cbm_git_url_normalize(const char *url, cbm_git_url_t *out);

void cbm_git_url_free(cbm_git_url_t *u);

/* Validate only the local checkout. Never contacts the remote. */
bool cbm_git_url_cache_is_valid(const cbm_git_url_t *u);

/* Idempotently place `ref` (or the remote default when ref is NULL) at
 * clone_dir. A first clone is staged in a temporary sibling and installed only
 * after git succeeds; an incomplete cache is removed and retried. Existing
 * caches are fetched on explicit index and fetch failures are retried, then
 * treated as stale-but-usable when the local repository is valid.
 * Returns 0 on success, CBM_NOT_FOUND when no usable checkout is available. */
int cbm_git_url_ensure_cloned(const cbm_git_url_t *u);

#endif /* CBM_GIT_URL_H */
