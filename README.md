# nvc

A minimal nv checker in C.

nvc checks upstream versions from INI files using concurrent HTTP requests.
It supports GitHub, GitLab, and Forgejo/Gitea releases, tags, and branch-head
commits, plus regex extraction from public HTTPS pages. Versions are opaque
strings by default; tag sources can opt into numeric selection.

## Building

Requires a C17 compiler, Make, pkg-config, libcurl 7.85.0+, json-c 0.15+,
POSIX interfaces with `getopt_long`, and a CA certificate store.
Tests also require Python 3.

```sh
make
make test
```

Run `./nvc` after building. Run `make clean` before changing build flags.

## Usage

```text
nvc -c file [-j jobs] [project ...]
nvc check -c file -s state [-j jobs] [project ...]
nvc compare -c file -s state [project ...]
nvc accept -c file -s state [project ...]
```

`-c` and state-command `-s` paths are explicit. Project arguments select section
names; omitting them selects all projects. Concurrency defaults to 8, with `-j`
accepting 1–64. Use `--help` or `--version` for program information.

Stateless checks and `check` print project and version separated by a tab, in
configuration order. Errors go to stderr without cancelling other projects.
`compare` prints project, status, accepted version, and pending version as four
tab-separated columns. Status is `unchecked`, `failed`, `new`, `changed`, or
`unchanged`; absent versions are empty.

```sh
./nvc check -c versions.ini -s versions.json
./nvc compare -c versions.ini -s versions.json
./nvc accept -c versions.ini -s versions.json neovim
```

Checks never accept automatically. Acceptance requires a successful observation
with matching configuration and is all-or-nothing for the selection. Failed
checks retain the last successful value but make it ineligible for acceptance.
Changed configuration invalidates old observations. Unselected and removed
projects remain in state.

Exit status is 0 for successful operations, including comparisons showing
changes. Errors, failed or unchecked observations, and ineligible acceptance
return nonzero.

## Configuration

```ini
[curl]
source = github
repo = curl/curl

[neovim]
source = github-tags
repo = neovim/neovim
include_regex = ^v[0-9]
exclude_regex = nightly
prefix = v

[forgejo]
source = forgejo
host = https://codeberg.org
repo = forgejo/forgejo
```

Sources and required settings:

- `github`: `repo=owner/name`; latest published stable release, no tag fallback.
- `github-tags`: `repo=owner/name`; first eligible tag in API order.
- `gitlab`: `host` and `repo=group/project`, including nested groups; releases
  ordered by `released_at` descending, skipping upcoming entries. Use filters
  for prerelease naming conventions.
- `forgejo` or `gitea`: `host` and `repo=owner/name`; first published stable
  release in API order.
- `gitlab-tags`: `host` and `repo`; tags ordered by update time descending.
- `forgejo-tags` or `gitea-tags`: `host` and `repo`; first eligible tag in API order.
- `github-commit`, `gitlab-commit`, `forgejo-commit`, or `gitea-commit`: `repo`
  and explicit `branch`, plus `host` for non-GitHub sources. Returns the branch
  head's full commit ID, never a shortened ID. No default branch or tag lookup.
- `regex`: `url` and `pattern`; first capture group of the first POSIX ERE match.
  For example, `pattern=version=([0-9]+[.][0-9]+)` extracts from text such as
  `version=1.2`. Matching does not use `REG_NEWLINE`.

Hosts are HTTPS origins without paths, queries, fragments, or userinfo.
Regex URLs must be public HTTPS URLs without userinfo or fragments; never put
credentials in their query strings, which are included in saved state identity.
GitHub does not support a custom host.

Optional `include_regex` and `exclude_regex` filter raw candidates before
`prefix` removes one literal leading occurrence. By default, lists use the first eligible
candidate; rejecting a single-result source does not trigger another search.
Commit sources apply filters to the full ID and reject `prefix` to preserve it.
`branch` is rejected on other sources. Empty identifiers, control characters,
invalid UTF-8, and NUL regex bodies fail.

Tag sources accept `sort = numeric` to select the greatest eligible tag across
all pages. After filtering and prefix removal, each candidate must contain only
dot-separated ASCII integers, or the check fails. Components compare left to
right: `1.10` beats `1.9`. Leading zeros and missing trailing zero components
compare equal, so `01.2`, `1.2.0`, and `1.2` tie. The first encountered spelling
wins ties. Changing `sort` invalidates saved observations.

INI names and keys are case-sensitive. Duplicate sections or keys, unknown keys,
missing values, and unsupported sources fail before requests begin. Full-line
`#` and `;` comments and LF/CRLF are accepted. Surrounding spaces and tabs are
trimmed; values have no quoting, escapes, inline comments, or expansion.

## Authentication and limits

`GITHUB_TOKEN` applies only to GitHub. API sources can select another environment
variable with `token_env`; regex sources cannot use tokens. GitLab uses
`PRIVATE-TOKEN`, other APIs Bearer authorization. Empty or unset tokens mean
anonymous access. Token values are never saved or printed. Use only trusted
configuration with credentials: `token_env` can select any environment variable,
and non-GitHub API entries choose where it is sent.

TLS verification is enabled and redirects are rejected. Requests have a 10-second
connection timeout, a 30-second total timeout, and a 1 MiB response limit.
Pagination searches at most 100 pages. Numeric selection scans every page and
fails if another page is indicated at the cap, without printing an incomplete maximum.
There are no retries, caches, or separate regex CPU deadlines. Standard libcurl
proxy environment variables apply.

Configuration and state are each limited to 1 MiB. Keep state in a trusted local
directory with working POSIX locks and atomic rename. Do not remove its `.lock`
file while nvc runs. Interrupted writes can leave `.tmp.*` files; remove them
only when no nvc process is running.

Tests use local fixtures without upstream access or real credentials.
See [DESIGN.md](DESIGN.md) for persistence details and API references.

## Real-world example

[Gur0v/ports](https://github.com/Gur0v/ports) shows how to keep a `.nvc.ini`
next to each `Pkgfile` and report upstream versions that differ from the recipes.

## License

nvc is distributed under the BSD 3-Clause License.
See [LICENSE](LICENSE) for the full terms.
