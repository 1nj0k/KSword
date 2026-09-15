# Submission notes — verified 2026-09-15

This file is an internal authoring note, not part of the anonymous manuscript.

## Applicable format

EuroSys requires a PDF with at most 12 pages of technical content and additional
reference pages. Use two columns within its 178 × 229 mm block, at least 8 mm
between columns, at least 10 pt text and 12 pt leading, numbered pages, and
grayscale-readable figures. Optional supplements cannot carry essential
arguments. The fall title/abstract deadline is **2026-09-17 AoE**; the full-paper
deadline is **2026-09-24 AoE**. These are submission deadlines, not acceptance
or publication dates. [EuroSys 2027 CFP](https://2027.eurosys.org/cfp.html)

The supplied ACM page is headed “Submitting Articles to ACM Journals” and
describes a general single-column review workflow. For this conference, apply
EuroSys's explicit two-column rules and its recommended SIGPLAN format.
Use ACM's current `acmart` package, retain its fonts and geometry, and provide
figure descriptions as well as captions. The live ACM page lists version 2.20
(August 18, 2026); recheck before assembling the final source package.
[ACM submissions](https://www.acm.org/publications/authors/submissions)

Candidate class options for the full draft are
`\documentclass[sigplan,10pt,review,anonymous]{acmart}`. SIGPLAN documents the
`sigplan` variant and the `review` option. The actual rendered PDF must still be
checked against the CFP; these options alone are not a compliance certificate.
[SIGPLAN author instructions](https://sigplan-www.sigplan.hosting.acm.org/Resources/Author/)

## Identity and disclosure

Review is double-blind. The CFP also asks for a substantially different title
and, when applicable, a different system/tool name for related work already
disseminated outside peer review. The abstract and figure use a descriptive
monitor label; public repository links, author names, local paths, machine
names and identifying metadata must be reviewed before submission. Maintain a
private mapping from anonymized evidence to the originals.
[EuroSys 2027 CFP](https://2027.eurosys.org/cfp.html)

The current ACM authorship policy distinguishes AI used in research, which
must be described in detail in Methods, from assistance with writing. EuroSys's
CFP explicitly requests disclosure of any AI use. This project includes
AI-assisted implementation, experiment tooling/execution, analysis, archiving,
and drafting, so a writing-only acknowledgment would omit material work.
[ACM authorship policy](https://www.acm.org/publications/policies/new-acm-policy-on-authorship),
[EuroSys 2027 CFP](https://2027.eurosys.org/cfp.html)

### Draft methods disclosure — author review required

OpenAI Codex assisted with implementing and debugging the prototype, writing
experiment and analysis scripts, executing tests in the designated laboratory,
assembling the evidence archive, and drafting text and figures. Reported
measurements derive from retained machine-readable execution logs; absent
measurements are marked as unmeasured. Before submission, the human authors
will review the source changes, experimental procedures, raw logs, statistical
analysis, citations, and claims and will document their checks. The tool/model
versions and the scope of assistance will be finalized in the submitted Methods
section. This draft does not assert that the human review has already occurred.

## Figure and artifact handling

- The current submission-field deliverable is `submission-abstract.md`: Markdown
  text and a fenced topology tree, with no embedded image. `abstract.md` explains
  its scope in Chinese. The vector exports below belong to the earlier draft and are not
  embedded in the current review document.
- The architecture figure is original vector artwork generated from the local
  mechanism and evidence. It contains no third-party logo or stock illustration.
- Native dimensions: 178 mm wide; labels are at least 10 pt. Use the exported
  PDF without reducing its physical width. Recheck the final figure and caption
  together when paginating the paper.
- `figure.tex` includes a complementary `\Description` for accessibility.
- The abstract and figure are review artifacts, not a finished ACM/TAPS source
  package. Author/ORCID, CCS metadata, bibliography, rights text and final layout
  belong to the subsequent full-manuscript and publication stages.
- No DOI, acceptance status, ACM rights block, or author certification has been
  invented. Nothing has been submitted or uploaded to a publication system.

Retrieval note: the web fetcher returned HTTP 403 for ACM's pages. Their live
contents were read successfully through the browser. An earlier search-index
copy still listed template 2.19; the live 2.20 listing above takes precedence.
