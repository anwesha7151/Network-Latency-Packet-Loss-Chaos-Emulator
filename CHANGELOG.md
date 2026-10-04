# Changes in this revision (review + fixes)

Verified working before any change: clean build (0 warnings), 69/69 unit checks, 4/4 end-to-end tests, ASan/UBSan clean,
all CLI output matches the README, scenarios, Ctrl+C shutdown, netem command generation / injection resistance.

## Bugs fixed
1. **proxy: 100% CPU when the target is down.** The ICMP "port unreachable" error on the upstream socket was never read, so
   `poll()` returned immediately forever. Errors are now consumed and counted ("socket errors" in the totals).
2. **proxy: per-client sockets were never released.** After ~fd-limit distinct client ports (1024 by default) the proxy kept
   running but silently ignored every new client. Added `--idle SEC` expiry (default 60) and LRU eviction on EMFILE/ENFILE.
3. **Mistyped options were silently ignored** (`sim --profle 3g` ran with *no* impairment; `--pakets` used the default).
   Unknown options/flags/stray arguments are now errors, with a "did you mean" hint. `--flag=value` on flags is rejected,
   and `<command> --help` prints usage instead of running the command.
4. **Numeric options were not validated**: `--packets -5` crashed with a cryptic allocation error, `--pps 0` printed `-nan`,
   `--size 0`, `--seed -1`, `--wait -5` were accepted, `probe --size 70000` silently lost 100% (exceeds UDP max).
   All numbers are range-checked with clear messages; `HOST:PORT` rejects trailing junk (`:80abc`).
5. **netem backend**: `netem scenario` returned exit 0 even if `tc` failed; `waitpid` ignored EINTR; a missing `tc` gave a
   bare "execvp: No such file"; scenario/TTL timing used accumulated `usleep` (drifts) instead of a clock.
6. **scripts/lab.sh demo** reported success (and showed an un-impaired "impaired" ping) when netem failed to apply.
7. **Makefile** re-used stale objects when `CXXFLAGS` changed (e.g. sanitizer build after a normal build).

## Added
- `tests/cli_regress.sh` (`make cli`, part of `make check` and CI): 31 checks covering every item above.
