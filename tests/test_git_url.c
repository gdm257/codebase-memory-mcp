/*
 * test_git_url.c — Tests for the git URL normalizer (cbm_git_url_normalize).
 *
 * Pure string in/out — no network, no disk, no git. cbm_git_url_ensure_cloned
 * spawns git and is intentionally not covered here (it is exercised by hand
 * via the index_repository / search_code wire points).
 */
#include "test_framework.h"

#include "foundation/compat.h" /* cbm_mkdtemp, cbm_setenv / cbm_unsetenv */
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "git/git_url.h"

#include <stdio.h>
#include <stdlib.h> /* getenv */
#include <string.h>

TEST(git_url_is_url) {
    ASSERT_TRUE(cbm_git_url_is_url("https://github.com/o/r"));
    ASSERT_TRUE(cbm_git_url_is_url("https://github.com/o/r.git"));
    ASSERT_TRUE(cbm_git_url_is_url("git@github.com:o/r.git"));
    ASSERT_TRUE(cbm_git_url_is_url("ssh://git@github.com:22/o/r"));
    ASSERT_TRUE(cbm_git_url_is_url("http://example.com/o/r"));
    ASSERT_FALSE(cbm_git_url_is_url("/home/user/repo"));
    ASSERT_FALSE(cbm_git_url_is_url("./foo"));
    ASSERT_FALSE(cbm_git_url_is_url("C:/Users/repo"));
    ASSERT_FALSE(cbm_git_url_is_url("relative/path"));
    ASSERT_FALSE(cbm_git_url_is_url(""));
    ASSERT_FALSE(cbm_git_url_is_url(NULL));
    PASS();
}

TEST(git_url_normalize_https) {
    cbm_git_url_t u;
    ASSERT_EQ(cbm_git_url_normalize("https://github.com/DeusData/codebase-memory-mcp.git", &u), 0);
    ASSERT_STR_EQ(u.host, "github.com");
    ASSERT_STR_EQ(u.path, "DeusData/codebase-memory-mcp");
    ASSERT_NULL(u.ref);
    ASSERT_STR_EQ(u.project_name, "github.com__DeusData__codebase-memory-mcp");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_normalize_no_git_suffix) {
    cbm_git_url_t u;
    ASSERT_EQ(cbm_git_url_normalize("https://github.com/o/r", &u), 0);
    ASSERT_STR_EQ(u.path, "o/r");
    ASSERT_STR_EQ(u.project_name, "github.com__o__r");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_normalize_scp_like) {
    cbm_git_url_t u;
    ASSERT_EQ(cbm_git_url_normalize("git@github.com:o/r.git", &u), 0);
    ASSERT_STR_EQ(u.host, "github.com");
    ASSERT_STR_EQ(u.path, "o/r");
    ASSERT_NULL(u.ref);
    ASSERT_STR_EQ(u.project_name, "github.com__o__r");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_normalize_ref) {
    cbm_git_url_t u;
    ASSERT_EQ(cbm_git_url_normalize("https://github.com/o/r@v1.2.0", &u), 0);
    ASSERT_STR_EQ(u.ref, "v1.2.0");
    ASSERT_STR_EQ(u.clone_url, "https://github.com/o/r");
    ASSERT_STR_EQ(u.project_name, "github.com__o__r__v1.2.0");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_normalize_scp_like_ref_not_user) {
    cbm_git_url_t u;
    /* The user@ of scp-like must not be mistaken for the @ref separator. */
    ASSERT_EQ(cbm_git_url_normalize("git@github.com:o/r@main", &u), 0);
    ASSERT_STR_EQ(u.host, "github.com");
    ASSERT_STR_EQ(u.path, "o/r");
    ASSERT_STR_EQ(u.ref, "main");
    ASSERT_STR_EQ(u.clone_url, "git@github.com:o/r");
    ASSERT_STR_EQ(u.project_name, "github.com__o__r__main");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_normalize_ssh_port) {
    cbm_git_url_t u;
    ASSERT_EQ(cbm_git_url_normalize("ssh://git@github.com:22/o/r.git", &u), 0);
    ASSERT_STR_EQ(u.host, "github.com:22");
    ASSERT_STR_EQ(u.path, "o/r");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_normalize_nested_path) {
    cbm_git_url_t u;
    ASSERT_EQ(cbm_git_url_normalize("https://gitlab.com/g/s/r.git", &u), 0);
    ASSERT_STR_EQ(u.host, "gitlab.com");
    ASSERT_STR_EQ(u.path, "g/s/r");
    ASSERT_STR_EQ(u.project_name, "gitlab.com__g__s__r");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_normalize_host_lowercased) {
    cbm_git_url_t u;
    ASSERT_EQ(cbm_git_url_normalize("https://GitHub.COM/O/R", &u), 0);
    ASSERT_STR_EQ(u.host, "github.com");
    ASSERT_STR_EQ(u.path, "O/R");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_normalize_no_collision) {
    /* "o/r" and "o-r" must map to distinct project names — the reason '/' →
     * "__" rather than the "-" that cbm_project_name_from_path uses. */
    cbm_git_url_t a;
    cbm_git_url_t b;
    ASSERT_EQ(cbm_git_url_normalize("https://github.com/o/r", &a), 0);
    ASSERT_EQ(cbm_git_url_normalize("https://github.com/o-r", &b), 0);
    ASSERT_STR_EQ(a.project_name, "github.com__o__r");
    ASSERT_STR_EQ(b.project_name, "github.com__o-r");
    ASSERT_STR_NEQ(a.project_name, b.project_name);
    cbm_git_url_free(&a);
    cbm_git_url_free(&b);
    PASS();
}

TEST(git_url_normalize_rejects_local_path) {
    cbm_git_url_t u;
    ASSERT_NEQ(cbm_git_url_normalize("/home/user/repo", &u), 0);
    ASSERT_NEQ(cbm_git_url_normalize("C:/Users/repo", &u), 0);
    ASSERT_NEQ(cbm_git_url_normalize("", &u), 0);
    ASSERT_NEQ(cbm_git_url_normalize(NULL, &u), 0);
    PASS();
}

TEST(git_url_normalize_rejects_file_scheme) {
    /* file:// would let a caller point clone at any local path (SSRF / disk
     * read). Only the network transports {http,https,ssh,git} are allowed. */
    cbm_git_url_t u;
    ASSERT_NEQ(cbm_git_url_normalize("file:///etc", &u), 0);
    ASSERT_NEQ(cbm_git_url_normalize("file:///C:/Windows/System32", &u), 0);
    ASSERT_NEQ(cbm_git_url_normalize("ftp://example.com/o/r", &u), 0);
    /* Scheme is matched case-insensitively; HTTPS still works. */
    ASSERT_EQ(cbm_git_url_normalize("HTTPS://github.com/o/r", &u), 0);
    ASSERT_STR_EQ(u.host, "github.com");
    cbm_git_url_free(&u);
    PASS();
}

TEST(git_url_clone_dir) {
    /* Pin the cache root so clone_dir is deterministic and platform-neutral
     * (cbm_normalize_path_sep leaves forward slashes alone). */
    const char *saved = getenv("CBM_CACHE_DIR");
    cbm_setenv("CBM_CACHE_DIR", "/tmp/cbm-git-url-test", 1);
    cbm_git_url_t u;
    ASSERT_EQ(cbm_git_url_normalize("https://github.com/o/r@main", &u), 0);
    ASSERT_STR_EQ(u.clone_dir, "/tmp/cbm-git-url-test/git-cache/github.com/o/r/main/");
    cbm_git_url_free(&u);
    if (saved) {
        cbm_setenv("CBM_CACHE_DIR", saved, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    PASS();
}

TEST(git_url_remove_tree) {
    char root[CBM_SZ_256] = "/tmp/cbm-git-url-tree-XXXXXX";
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    char nested[CBM_SZ_512];
    int n = snprintf(nested, sizeof(nested), "%s/nested", root);
    ASSERT_TRUE(n > 0 && (size_t)n < sizeof(nested));
    ASSERT_TRUE(cbm_mkdir_p(nested, 0755));
    char file[CBM_SZ_512];
    n = snprintf(file, sizeof(file), "%s/file.txt", nested);
    ASSERT_TRUE(n > 0 && (size_t)n < sizeof(file));
    FILE *fp = cbm_fopen(file, "wb");
    ASSERT_NOT_NULL(fp);
    ASSERT_EQ(fwrite("partial", 1, 7, fp), 7);
    ASSERT_EQ(fclose(fp), 0);
    ASSERT_TRUE(cbm_remove_tree(root));
    ASSERT_FALSE(cbm_file_exists(root));
    PASS();
}

/* Mirrors the inline format built by build_unindexed_url_error in mcp.c — kept
 * in sync by hand so a format drift fails the test even though the helper
 * itself is static (not linkable from this TU). */
static char *format_unindexed_url_error(const char *raw_url) {
    cbm_git_url_t u;
    char *project = NULL;
    if (cbm_git_url_normalize(raw_url, &u) == 0) {
        project = strdup(u.project_name);
        cbm_git_url_free(&u);
    }
    char url_escaped[4096];
    cbm_json_escape(url_escaped, sizeof(url_escaped), raw_url);
    char buf[5120];
    if (project) {
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"project for URL has not been indexed\","
                 " \"url\":\"%s\", \"project\":\"%s\","
                 " \"hint\":\"project \\\"%s\\\" has not been indexed — call "
                 "{\\\"tool\\\":\\\"index_repository\\\",\\\"arguments\\\":"
                 "{\\\"repo_path\\\":\\\"%s\\\"}} first, then retry this tool.\"}",
                 url_escaped, project, project, url_escaped);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"project for URL has not been indexed\","
                 " \"url\":\"%s\","
                 " \"hint\":\"URL \\\"%s\\\" has not been indexed — call "
                 "{\\\"tool\\\":\\\"index_repository\\\",\\\"arguments\\\":"
                 "{\\\"repo_path\\\":\\\"%s\\\"}} first, then retry this tool.\"}",
                 url_escaped, url_escaped, url_escaped);
    }
    free(project);
    return strdup(buf);
}

TEST(unindexed_url_error_format) {
    /* normalized project name is emitted verbatim, and the raw url (JSON-
     * escaped) is spliced into three places: url=, project=, and the hint. */
    char *err = format_unindexed_url_error("https://github.com/o/r@main");
    ASSERT_NOT_NULL(err);
    ASSERT_STR_EQ(err,
                  "{\"error\":\"project for URL has not been indexed\","
                  " \"url\":\"https://github.com/o/r@main\","
                  " \"project\":\"github.com__o__r__main\","
                  " \"hint\":\"project \\\"github.com__o__r__main\\\" has not been "
                  "indexed — call {\\\"tool\\\":\\\"index_repository\\\","
                  "\\\"arguments\\\":{\\\"repo_path\\\":\\\"https://github.com/o/r@main\\\"}} "
                  "first, then retry this tool.\"}");
    free(err);
    PASS();
}

TEST(unindexed_url_error_json_escapes_quotes) {
    /* A url with embedded characters that JSON cares about (\, ") is escaped
     * before being spliced into the hint. */
    char *err = format_unindexed_url_error("https://github.com/o/r");
    ASSERT_NOT_NULL(err);
    ASSERT_STR_NEQ(strstr(err, "\"url\":\"https://github.com/o/r\""), NULL);
    free(err);
    PASS();
}

TEST(unindexed_url_error_falls_back_when_malformed) {
    /* The helper must still produce the "error/url/hint" skeleton even when the
     * url fails to normalize — just without the project= field. */
    char *err = format_unindexed_url_error("https://");
    ASSERT_NOT_NULL(err);
    ASSERT_STR_NEQ(strstr(err, "\"error\":\"project for URL has not been indexed\""), NULL);
    ASSERT_STR_NEQ(strstr(err, "\"url\":\"https://\""), NULL);
    /* No project= field when the url failed to normalize to a project name. */
    ASSERT_STR_EQ(strstr(err, "\"project\":"), NULL);
    free(err);
    PASS();
}

SUITE(git_url) {
    RUN_TEST(git_url_is_url);
    RUN_TEST(git_url_normalize_https);
    RUN_TEST(git_url_normalize_no_git_suffix);
    RUN_TEST(git_url_normalize_scp_like);
    RUN_TEST(git_url_normalize_ref);
    RUN_TEST(git_url_normalize_scp_like_ref_not_user);
    RUN_TEST(git_url_normalize_ssh_port);
    RUN_TEST(git_url_normalize_nested_path);
    RUN_TEST(git_url_normalize_host_lowercased);
    RUN_TEST(git_url_normalize_no_collision);
    RUN_TEST(git_url_normalize_rejects_local_path);
    RUN_TEST(git_url_normalize_rejects_file_scheme);
    RUN_TEST(git_url_clone_dir);
    RUN_TEST(git_url_remove_tree);
    RUN_TEST(unindexed_url_error_format);
    RUN_TEST(unindexed_url_error_json_escapes_quotes);
    RUN_TEST(unindexed_url_error_falls_back_when_malformed);
}
