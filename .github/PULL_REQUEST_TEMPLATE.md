<!--
Thanks for sending a PR. A few things that make review faster:
- One change per PR. A bug fix that also reformats a file is two PRs.
- If this touches hardware behavior, say what board you ran it on and what
  you actually saw -- "should work" isn't a test result.
- See CONTRIBUTING.md, especially the AI/LLM guidelines if a tool helped
  write this.
-->

## What does this PR do, and why?

<!-- One change, described plainly. Link an issue if there is one. -->

## How was this tested?

<!--
Pick what actually applies -- delete the rest. Be specific: board, NFC
frontend, wallet/credential type, build variant (plain / Matter).
-->

- [ ] Ran `python3 tools/check_consistency.py` — no new errors
- [ ] Built for at least one target (`idf.py build`) — which: ______
- [ ] Flashed and booted on real hardware — board: ______, NFC: ______
- [ ] Exercised the actual behavior changed (a tap, a config change, an OTA,
      a Matter command) and confirmed the result, not just that it compiled
- [ ] Not hardware-testable from here — explain what a reviewer with a board
      would need to check: ______

## Checklist

- [ ] This PR is scoped to one thing — no unrelated file changes or reformatting
- [ ] New code follows the existing seams (`nfc_transport`, `aliro_reader`,
      `access_control`, `app_config`, `net_manager`, `web_server`, …) rather
      than spreading one concern across several
- [ ] No new dependency was added without a one-sentence reason in this PR
- [ ] Comments (if any) explain *why*, not what the code already says
- [ ] If this adapts code from another project, the license/attribution is
      noted here and in [`NOTICE.md`](../NOTICE.md)

## AI assistance disclosure

<!-- Required -- see CONTRIBUTING.md's AI/LLM guidelines. -->

- [ ] No AI assistance was used
- [ ] AI assistance was used, and every claim in this PR (behavior, test
      results, API/register names) was verified by a human before opening it
