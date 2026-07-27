#!/usr/bin/env zsh

# Validate the observable structure of the zredis CodeQL workflow.

emulate -LR zsh
setopt errexit nounset pipefail

readonly script_dir="${0:A:h}"
readonly workflow="${1:-${script_dir:h}/workflows/codeql-analysis.yml}"
readonly yq_bin="${YQ_BIN:-yq}"

fail() {
  print -u2 -r -- "FAIL: $1"
  exit 1
}

assert_yq() {
  local description="$1"
  local expression="$2"

  if ! "$yq_bin" eval --exit-status "$expression" "$workflow" >/dev/null; then
    fail "$description"
  fi
}

[[ -r "$workflow" ]] || fail "workflow is not readable: $workflow"
if [[ "$yq_bin" == */* ]]; then
  [[ -x "$yq_bin" ]] || fail "YQ_BIN is not executable: $yq_bin"
else
  command -v "$yq_bin" >/dev/null || fail "yq is not available: $yq_bin"
fi

assert_yq \
  "expected exactly the Actions/none and C/C++/manual matrix entries" \
  '(.jobs.codeql.strategy."fail-fast" == false) and
   ((.jobs.codeql.strategy.matrix.include | length) == 2) and
   (([.jobs.codeql.strategy.matrix.include[] |
      select(.language == "actions" and ."build-mode" == "none")] |
      length) == 1) and
   (([.jobs.codeql.strategy.matrix.include[] |
      select(.language == "c-cpp" and ."build-mode" == "manual")] |
      length) == 1)'

assert_yq \
  "expected plain workflow and job names" \
  '.name == "CodeQL" and
   .jobs.codeql.name == "CodeQL (${{ matrix.language }})"'

assert_yq \
  "expected push, pull request, weekly schedule, and manual triggers" \
  '(.on.push.branches | length) == 1 and
   .on.push.branches[0] == "main" and
   (.on.pull_request.branches | length) == 1 and
   .on.pull_request.branches[0] == "main" and
   (.on.schedule | length) == 1 and
   .on.schedule[0].cron == "15 21 * * 6" and
   .on.workflow_dispatch.inputs."upload-results".type == "boolean" and
   .on.workflow_dispatch.inputs."upload-results".default == false'

assert_yq \
  "expected workflow/ref concurrency with cancellation" \
  '.concurrency.group == "codeql-${{ github.workflow }}-${{ github.ref }}" and
   .concurrency."cancel-in-progress" == true'

assert_yq \
  "expected least-privilege workflow and job permissions" \
  '(.permissions | length) == 1 and
   .permissions.contents == "read" and
   (.jobs.codeql.permissions | length) == 2 and
   .jobs.codeql.permissions.contents == "read" and
   .jobs.codeql.permissions."security-events" == "write"'

assert_yq \
  "expected a 30-minute CodeQL timeout" \
  '.jobs.codeql."timeout-minutes" == 30'

assert_yq \
  "expected immutable checkout and CodeQL initialization" \
  '(([.jobs.codeql.steps[] |
      select(.uses ==
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1")] |
      length) == 1) and
   (([.jobs.codeql.steps[] |
      select(.uses ==
        "github/codeql-action/init@e4fba868fa4b1b91e1fdab776edc8cfbe6e9fb81" and
        .with.languages == "${{ matrix.language }}" and
        .with."build-mode" == "${{ matrix.build-mode }}" and
        .with.queries == "security-extended")] |
      length) == 1)'

assert_yq \
  "expected the exact C/C++ dependency installation" \
  '([.jobs.codeql.steps[] |
     select(.name == "Install C/C++ dependencies" and
       .if == "matrix.build-mode == '\''manual'\''" and
       (.run | contains("build-essential")) and
       (.run | contains("zsh")) and
       (.run | contains("yodl")) and
       (.run | contains("libhiredis-dev")) and
       (.run | contains("libgdbm-dev")) and
       (.run | contains("libncurses-dev")))] |
     length) == 1'

assert_yq \
  "expected the explicit module configure, capability, and output checks" \
  '([.jobs.codeql.steps[] |
     select(.name == "Build C/C++ modules" and
       .if == "matrix.build-mode == '\''manual'\''" and
       ."working-directory" == "module" and
       (.run | contains("./configure --enable-gdbm --without-tcsetpgrp")) and
       (.run | contains("HAVE_HIREDIS_HIREDIS_H")) and
       (.run | contains("HAVE_REDISCONNECT")) and
       (.run | contains("HAVE_GDBM_H")) and
       (.run | contains("HAVE_GDBM_OPEN")) and
       (.run | contains("\nmake\n")) and
       (.run | contains("name=zshell/db")) and
       (.run | contains("name=zshell/zredis")) and
       (.run | contains("name=zshell/zgdbm")) and
       (.run | contains("Src/zshell/db.so")) and
       (.run | contains("Src/zshell/zredis.so")) and
       (.run | contains("Src/zshell/zgdbm.so")))] |
     length) == 1'

assert_yq \
  "expected mutually exclusive upload and non-upload analysis steps" \
  '(([.jobs.codeql.steps[] |
      select(.name == "Analyze without upload" and
        .uses ==
          "github/codeql-action/analyze@e4fba868fa4b1b91e1fdab776edc8cfbe6e9fb81" and
        .if ==
          "github.event_name == '\''pull_request'\'' || (github.event_name == '\''workflow_dispatch'\'' && inputs.upload-results == false)" and
        .with.upload == "never" and
        .with.category == "/language:${{ matrix.language }}")] |
      length) == 1) and
   (([.jobs.codeql.steps[] |
      select(.name == "Analyze and upload" and
        .uses ==
          "github/codeql-action/analyze@e4fba868fa4b1b91e1fdab776edc8cfbe6e9fb81" and
        .if ==
          "github.event_name == '\''push'\'' || github.event_name == '\''schedule'\'' || (github.event_name == '\''workflow_dispatch'\'' && inputs.upload-results == true)" and
        .with.category == "/language:${{ matrix.language }}" and
        (.with | has("upload") | not))] |
      length) == 1)'

assert_yq \
  "expected no CodeQL autobuild action" \
  '([.jobs.codeql.steps[].uses // "" |
     select(test("github/codeql-action/autobuild@"))] |
     length) == 0'

assert_yq \
  "expected every action reference to use a full immutable SHA" \
  '([.jobs.codeql.steps[].uses // "" |
     select(. != "" and (test("@[0-9a-f]{40}$") | not))] |
     length) == 0'

print -r -- "PASS: CodeQL workflow contract"
