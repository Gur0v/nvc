# Implementation contracts

User-facing configuration and commands are documented in [README.md](README.md).

## Sources

GitHub tags and Forgejo/Gitea releases use API order, not version sorting.
GitLab explicitly requests `released_at` descending. Paginated sources request
100 items per page and continue on a full page or a next-page header, stopping
at the first eligible candidate. Pagination stays at the configured origin;
URLs supplied in response headers are never followed. Searches exceeding 100
pages fail rather than return an incomplete result.

Each project has at most one active request. Global concurrency is bounded,
output follows configuration order, and redirects are rejected. Credentials
belong to the configured API origin; GitHub's default token is not shared with
other sources.

## State transactions

State is strict version-1 JSON containing per-project identity, accepted and
pending versions, and acceptance eligibility. Identity includes all source
settings and the token variable name, never its value. Public source URLs are
stored verbatim. Malformed, duplicate-key, or unsupported state is not overwritten.

Checks fetch outside the lock, then merge selected results into the newest state
under a persistent advisory lock. Disjoint selections preserve each other's
updates; the last completed writer wins for the same project. Configuration
changes reset the selected entry's observations. Failures retain its previous
successful pending value but invalidate eligibility. Offline operations require
matching identity, and acceptance writes nothing if any selected entry is
ineligible. Removed and unselected entries remain untouched.

Writes use a private same-directory temporary file, file sync, atomic rename,
and directory sync. An interruption before rename preserves old state but may
leave an orphan temporary file. Directory-sync failure after rename reports an
error even though the replacement may already be visible. Locks require
cooperating processes and a trusted local filesystem.

## API references

- [GitHub tags](https://docs.github.com/en/rest/repos/repos#list-repository-tags)
- [GitLab releases](https://docs.gitlab.com/api/releases/)
- [Forgejo pagination](https://forgejo.org/docs/latest/user/api/usage/)
- [Gitea releases](https://docs.gitea.com/api/1.24/operations/repo-list-releases/)
- [Forgejo OpenAPI](https://codeberg.org/swagger.v1.json)
