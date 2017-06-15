# Valgrind automatic tests suite (VATS) for Zsh

## Fundamental test-configuration

Main configuration file is `vtest.conf`. It defines two fields:

```zsh
test_bin="local-zsh"        # Binary that runs any test (local-zsh: ../Src/zsh)
zsh_control_bin="zsh"       # Binary used when scheduling tests & interpreting Valgrind output
```

Field `zsh_control_bin` is used to implement special `#!`, shebang behavior: `runtests.zsh`
starts with `#!/bin/sh`, reads `vtest.conf`, and restarts with `zsh_control_bin`. This way
user can define shebang interpreter via separate configuration file (`vtest.conf`).

The second script that uses `zsh_control_bin` is `zsh-valgrind-parse.cmd`. It also restarts
via `#!/bin/sh` and `exec /usr/bin/env "$zsh_control_bin"`.

The field `test_bin` specifies Zsh binary used to run tests. **This binary is examined by Valgrind**.
Special value `local-zsh` means: *Zsh binary from current build directory*, normally `../Src/zsh`. This
is the default.

## Remaining test-configuration

With `vtest.conf` field `tkind`, user sets a **test-kind**. These are modes of Valgrind operation.
Allowed values are: `error` (**only detect read/write errors**), `leak` (**also detect memory leaks**),
`nopossiblylost` (**detect memory leaks, but not _possibly_ lost blocks**).

```zsh
tkind="leak"                # Test kind: error, leak, nopossiblylost
```
