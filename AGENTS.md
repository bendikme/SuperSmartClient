# Repository instructions

Read and follow [agent.md](agent.md) before working in this repository. That file
is the single source of project-specific instructions.

## Issues: tracked in DYNATEC Project

This repository's issues — bugs, features, improvements, tasks and questions — are kept in **DYNATEC Project's issue tracker**, not in
GitHub issues, TODO files or final reports: **SuperSmartClient (VNC for Unified panels)** (product key `SSC`). People see them at `https://portal.dynatec.no/project#/products/SSC`; an issue's key
looks like `SSC-12`.

**How an agent reaches it.** Through the DYNATEC Portal's MCP server: `call_application_tool` with application `DynatecProject` (read a tool's
definition first with `application_tools`). DYNATEC Project's tools run in the person's open DYNATEC Project page: if a tool answers that it
needs the page, ask the person to open https://portal.dynatec.no/project and try again. Tools:

| Tool | Use |
| --- | --- |
| `find_product_issues` | `{ product: "SSC", open: true, q }` — before starting work, look for the issue it belongs to |
| `create_product_issue` | `{ product: "SSC", title, kind: Bug \| Feature \| Improvement \| Task \| Question, severity, description, area?, foundInVersion?, targetVersion? }` |
| `get_item`, `update_item` | read an issue (`SSC-12`) in full; change its state, assignee, priority, or add a comment |
| `add_git_link` | link a commit, pull request or branch to an issue |
| `list_products`, `add_product_area`, `plan_product_release`, `product_release_notes` | the product's areas and releases; release notes are made from the Done issues |

**All agent work is logged in the tracker — across the whole DYNATEC suite** (the user, 2026-11-15: *"from now ALL our agent work shall be logged through the dynatec project issue tracker"*, *"This applies to the whole suite of products"*). The same rule holds in every DYNATEC repository and product, this one included. Every piece of work an agent does — a feature, a fix however small, a refactor, a dependency or security update, a test or tooling change, a release, a deploy, an investigation — has an issue: found first, or created before the work starts, in the product it changes; work that spans several products has an issue in each of them, linked to one another in their comments. No commit, release or deploy without one. The issue's comments are the work's log: what was done, where (commits, release, deploy) and how it was verified (tests, checks, what was looked at), written as the work moves, not only at the end.

**Rules.**
1. **Look first, then work.** Before ANY change, find its issue (`find_product_issues`); when there is none, create one — every piece of work has one, however small.
2. **Report what you find and do not fix** — bugs, gaps, follow-ups, "left" items of a report — as issues (one issue per problem, a clear title,
   what happens, what should happen, how to reproduce, the version or commit). Do not leave them only in a chat or a final report.
3. **Name the key** in commit messages, branch names and pull request titles (`SSC-12: …`): the tracker's GitHub webhook links them by itself.
4. **Move the issue** as the work moves: InProgress when you start, InReview when it waits for review or release, a comment saying what was done
   and where (commit, release). Done is set when the work is in a release; nothing is ever deleted (WontFix or Duplicate instead).
5. **Never** put secrets, tokens, passwords, customer personal data or production connection strings in an issue or a comment.
6. Severity: Critical (data loss, security, production down), Major (a main function broken, no workaround), Minor (default), Trivial (cosmetic).
   Write in English; the person may write in Norwegian.
