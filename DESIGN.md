# Implementation contracts

User-facing configuration and commands are documented in [README.md](README.md).

## Sources

By default, GitHub tags and Forgejo/Gitea releases and tags use API order.
GitLab releases request `released_at` descending; tags request `updated`
descending. Paginated sources request 100 items per page and continue on a full page or a next-page header, stopping
at the first eligible candidate. Pagination stays at the configured origin;
URLs supplied in response headers are never followed. Searches exceeding 100
pages fail rather than return an incomplete result.

Only tag sources accept `sort=numeric`. Raw include/exclude filters precede
literal prefix removal; remaining identifiers must be ASCII dot-separated
nonnegative integers, including single components. Eligible nonnumeric values
fail even after a maximum has been found. Compare components by significant
digit count and then digit bytes, without integer conversion. Leading zeros
and missing trailing zero components compare equal; retain the first encountered
spelling on ties. The saved result preserves that spelling after prefix removal.
This is an explicit numeric tuple order, not semantic or universal version order.
Keep the best owned candidate across pages and scan to exhaustion. A full page
or next marker at page 100 fails rather than return an incomplete maximum;
transport, malformed-response, and validation failures also suppress the result.
Default selection still stops at the first eligible candidate.

Each project has at most one active request. Global concurrency is bounded,
output follows configuration order, and redirects are rejected. Credentials
belong to the configured API origin; GitHub's default token is not shared with
other sources.

Branch-head sources use the provider's single-branch endpoint, not a generic
commit endpoint that can resolve a tag. GitHub reads `commit.sha`; GitLab and
Forgejo/Gitea read `commit.id`. IDs must be full 40- or 64-character hexadecimal
strings and are preserved verbatim. Filters can reject the single result;
`prefix` is not permitted. There are no git subprocesses or new dependencies.
Release sources never fall back to tags or commits.

## State transactions

State is strict version-1 JSON containing per-project identity, accepted and
pending versions, and acceptance eligibility. Identity includes all source
settings and the token variable name, never its value. Public source URLs are
stored verbatim. Existing sources retain the original nine-element identity
(source, repo, host, url, pattern, include, exclude, prefix, token variable).
Commit sources append the explicit branch as a tenth element. Numeric tag
identities append a null branch slot and `"numeric"` as element eleven. Defaults
keep the original nine elements; commits keep ten. All three lengths are readable
in format 1. Eleven-element identities require a tag source, null branch, and
exact numeric option. Changing branch or sort invalidates observations. Older
binaries may reject ten- or eleven-element identities.
Malformed, duplicate-key, or unsupported state is not overwritten.

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
- [GitHub branches](https://docs.github.com/en/rest/branches/branches#get-a-branch)
- [GitLab tags](https://docs.gitlab.com/api/tags/)
- [GitLab branches](https://docs.gitlab.com/api/branches/)
- [Gitea tags](https://docs.gitea.com/api/1.24/operations/repo-list-tags/)
- [Gitea branches](https://docs.gitea.com/api/1.24/operations/repo-get-branch/)
