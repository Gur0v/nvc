# Implementation contracts

See [README.md](README.md) for configuration and commands.

## Sources

By default, GitHub tags and Forgejo/Gitea releases and tags use API order.
GitLab releases request `released_at` descending; tags request `updated`
descending. Paginated sources request 100 items per page. A full page, a
`Link` next relation, a positive `X-Next-Page`, or a total count greater than
the number of items seen indicates another page. The latest valid `X-Total-Count`
(or GitLab's `X-Total`) remains in effect until replaced. Only final response
headers count; informational, proxy CONNECT, and trailer headers do not.
Requests increment the page number at the configured origin; URLs in headers are
never followed. An empty page with more indicated, or a request that would exceed
100 pages, fails. A full page 100 fails even if its total count is exhausted.

Only tag sources accept `sort=numeric`. Include and exclude filters run on raw
identifiers, before one literal prefix is removed. Numeric identifiers contain
ASCII digits separated by dots; a single component is valid. An eligible
nonnumeric tag fails the check, even if an earlier tag was valid. Components
compare by significant digit count, then by their bytes. No integer conversion
is needed. Leading zeros and missing trailing zero components compare equal.
The first tag wins a tie, and nvc keeps its spelling after prefix removal.
Numeric selection checks every page. A request, response, or pagination error
suppresses the result. Without numeric selection, nvc stops at the first
eligible candidate.

Each project has at most one active request. Output follows configuration
order, regardless of which request finishes first. Redirects are rejected.
GitHub's default token is not sent to other providers.

Branch-head sources request the provider's single-branch endpoint. A generic
commit endpoint could resolve a tag instead. GitHub reads `commit.sha`; GitLab,
Forgejo, and Gitea read `commit.id`. IDs must contain 40 or 64 hexadecimal
characters and are kept verbatim. Filters can reject the branch head, but
`prefix` is not permitted. Release sources do not fall back to tags or commits.

## State transactions

State is version-1 JSON with an identity, accepted version, pending version,
and eligibility for each project. The identity stores source settings, the token
variable name, and the full public URL if present. It never stores the token
value. A default identity has nine fields: source, repo, host, url, pattern,
include, exclude, prefix, and token variable. A commit identity adds the branch
as field ten. A numeric tag identity adds a null branch field and `"numeric"` as
field eleven. State rejects unknown sources and identities with the wrong
fields for their source. Changing branch or sort invalidates an observation.
Older binaries may reject commit or numeric identities. nvc does not overwrite
malformed or unsupported state.

Checks fetch before taking the state lock. Under the lock, nvc reads the latest
state and merges the selected results. Concurrent checks of different projects
keep each other's updates; for the same project, the last writer wins. A changed
configuration resets that project's observations. A failed check keeps the
previous pending version if the identity matches, but clears eligibility.
Compare and accept require matching identities. Accept writes nothing if any
selected project is ineligible. Removed and unselected entries stay in state.

State and lock files are opened without following final symlinks and must be
regular files. Nonblocking opens allow FIFO rejection; lock acquisition still
blocks. Writes use a private same-directory temporary file, file sync, atomic
rename, and directory sync. An interruption before rename preserves old state but
may leave an orphan temporary file. Directory-sync failure after rename reports
an error even though the replacement is already visible. Locks require
cooperating processes and trusted local parent directories.

## API references

- [GitHub tags](https://docs.github.com/en/rest/repos/repos#list-repository-tags)
- [GitLab releases](https://docs.gitlab.com/api/releases/)
- [Forgejo pagination](https://forgejo.org/docs/latest/user/api/usage/)
- [Gitea releases](https://docs.gitea.com/api/1.24/operations/repo-list-releases/)
- [Forgejo OpenAPI](https://codeberg.org/swagger.v1.json)
- [GitHub branches](https://docs.github.com/en/rest/branches/branches#get-a-branch)
- [GitLab tags](https://docs.gitlab.com/api/tags/)
- [GitLab branches](https://docs.gitlab.com/api/branches/)
- [Gitea tags](https://docs.gitea.com/api/1.24/operations/repo-list-tags/)
- [Gitea branches](https://docs.gitea.com/api/1.24/operations/repo-get-branch/)
