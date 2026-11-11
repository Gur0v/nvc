# nvc

nvc checks upstream versions using an INI file. It fetches releases, tags, and
branch-head commits from GitHub, GitLab, Forgejo, and Gitea. It can also extract
a version from an HTTPS page with a regex. Checks run concurrently.

## Build

Build with a C17 compiler, GNU make, pkg-config, libcurl 7.85 or later with
HTTPS and the header API, json-c 0.15 or later, POSIX interfaces, `getopt_long`,
and a CA store. Use `gmake` on systems that name GNU make that way.

```sh
make
make test
```

Tests use loopback HTTP without upstream access or real credentials and require
`python3` 3.7 or later. Set `PYTHON` to use another interpreter. Run `make clean`
before changing build flags.

## Usage

```text
nvc -c file [-j jobs] [project ...]
nvc check -c file -s state [-j jobs] [project ...]
nvc compare -c file -s state [project ...]
nvc accept -c file -s state [project ...]
```

Pass the configuration path with `-c`; there is no default path. State commands
also need `-s`. Project names select INI sections; without names, nvc checks all
projects. `-j` sets concurrency from 1 to 64 (default 8). `--help` and
`--version` print program information.

Checks print the project name and version, separated by a tab, in configuration
order. Project errors go to stderr without stopping other checks. Any error
returns nonzero. A changed version is not an error.

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
```

Set `source` in each section. API sources need `repo`; GitLab, Forgejo, and
Gitea also need `host`. The sources select versions as follows:

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

Hosts must be HTTPS origins without paths, queries, fragments, or userinfo.
Regex URLs must use HTTPS and cannot have userinfo or fragments. Never put
credentials in a URL: nvc records the URL in state. GitHub does not support a
custom host.

`include_regex` and `exclude_regex` filter candidates before `prefix` removes
one literal leading occurrence. Without `sort = numeric`, tag and release
lists use the first eligible candidate. A filter that rejects a single release,
commit, or regex match does not start another search. Commit sources filter the
full ID and reject `prefix`; other sources reject `branch`. Empty identifiers,
control characters, invalid UTF-8, and NUL regex bodies fail.

Tag sources accept `sort = numeric`. nvc scans all pages and selects the
greatest eligible dot-separated numeric tag. A nonnumeric eligible tag fails
the check. `1.10` beats `1.9`; leading zeros and missing trailing zero components
do not affect the comparison. Ties keep the first tag's spelling. Changing
`sort` invalidates saved observations. Other sources do not accept `sort`.

INI names and keys are case-sensitive. Duplicate sections or keys, unknown keys,
unsupported sources, and missing values fail before requests begin. Files must
use UTF-8. Full-line `#` and `;` comments are allowed. nvc trims surrounding
spaces and tabs; values are literal, with no inline comments, quoting, escapes,
or expansion.

## State

With the configuration above saved as `versions.ini`:

```sh
nvc -c versions.ini
nvc check -c versions.ini -s versions.json
nvc compare -c versions.ini -s versions.json
nvc accept -c versions.ini -s versions.json neovim
```

The first command checks without saving state. `check` saves the observed
version as pending. `compare` prints four tab-separated fields: project, status,
accepted version, and pending version. Status is `unchecked`, `failed`, `new`,
`changed`, or `unchanged`. Missing versions print as empty fields. `unchecked`
and `failed` return nonzero; `changed` does not.

`accept` acknowledges the selected projects without fetching again. It prints
nothing on success. Each selected project needs an eligible pending observation
from the same configuration; if one is ineligible, none is accepted. A failed
check preserves a pending version from the same configuration but makes it
ineligible. State retains unselected and removed projects.

[Gur0v/ports](https://github.com/Gur0v/ports) keeps a `.nvc.ini` next to each
`Pkgfile` and reports upstream versions that differ from the recipes.

## Security and limits

`GITHUB_TOKEN` applies only to GitHub. An API project can use `token_env` to
name another environment variable. GitLab sends the value as `PRIVATE-TOKEN`;
other APIs use Bearer authorization. Regex sources cannot use tokens. An unset
or empty variable means anonymous access. nvc does not save or print token
values. Use trusted configuration when supplying tokens: `token_env` can select
any environment variable, and the project selects the API host that receives it.

nvc verifies TLS and rejects redirects. libcurl proxy environment variables
apply. A request has a 10-second connection timeout, a 30-second total timeout,
a 1 MiB body limit, and an 8 KiB per-header limit. DNS lookup can exceed the
timeouts if libcurl uses a synchronous resolver. Pagination stops at 100 pages;
numeric selection fails rather than report an incomplete result if it needs
page 101. nvc does not retry or cache requests, and regex matching has no
separate CPU deadline.

Configuration and state files are each limited to 1 MiB. Keep state in a trusted
local directory with POSIX locks, atomic rename, and directory sync. Existing
state and `.lock` paths must be regular files, not symlinks. Do not remove the
lock file while nvc runs. An interrupted write can leave a `.tmp.*` file;
remove it only when nvc is not running. If directory sync fails after rename,
nvc reports an error although the new state is already visible.

See [DESIGN.md](DESIGN.md) for state format and API references.

## License

nvc is distributed under the BSD 3-Clause License.
See [LICENSE](LICENSE) for the full terms.
