# Interview notes

## 30-second pitch
"I built a chaos-engineering tool that emulates bad networks. Instead of just wrapping `tc netem`, I wrote the
impairment engine myself — burst loss with a Gilbert-Elliott Markov model, a token-bucket shaper, jitter
distributions — and verified it against the closed-form maths. The same profile drives a userspace UDP proxy that
needs no root, or the kernel's netem. It measures its own effect with a probe, supports timeline scenarios, and has
safety features like an auto-revert timer and an SSH lock-out guard."

## 2-minute live demo
1. `./chaos profiles` — show presets.
2. `./chaos sim --set ge_p=2 --set ge_r=30` vs `--set loss=6.25` — same loss %, different burstiness.
3. Three terminals: `echo`, `proxy --profile lossy-wifi`, `probe --timeline`.
4. `./chaos netem apply --dev eth0 --profile 3g --dry-run` — show the exact kernel command.
5. `make check` — tests green.  (If a VM is available: `sudo scripts/lab.sh demo`.)

## Questions you should expect
**Why Gilbert-Elliott instead of just "loss 5%"?** Real loss is bursty (congested buffers, radio fades). Independent
loss under-represents it; the same average loss as bursts hurts TCP, video and VoIP much more. Mean burst = 1/r.

**What is a token bucket?** Tokens accrue at the rate R up to depth B; each packet spends tokens equal to its size.
No tokens → wait (shaping) or drop (policing). Mine lets tokens go negative to compute the wait, and tail-drops
once the wait exceeds the queue limit.

**Why a userspace proxy when netem exists?** It needs no root, can't lock you out, is deterministic with a seed,
can change profiles on a timeline, and lets me measure per-direction counters. netem is better for impairing
arbitrary traffic at line rate — which is why both backends exist.

**How do you avoid command injection with root privileges?** Whitelist interface names, `fork/execvp` with an argv
vector instead of `system()` (no shell), numeric parameters parsed and range-checked.

**What does jitter cause?** Variable delay; if it exceeds the inter-packet gap, packets reorder — my tests show this
explicitly, as real networks do.

**How do you know your simulation is right?** Seeded runs compared to theory: steady-state loss p/(p+r), mean burst
1/r, shaper drain time = bytes/rate, normal-distribution mean; plus end-to-end measurement over real sockets.

**What are the limits?** UDP only in userspace mode; netem egress only; ~1 ms timer granularity. Roadmap: TUN/NFQUEUE,
IFB ingress, per-flow selectors.

**What bug did testing find?** AddressSanitizer found a use-after-scope in my argument parser (`strtod`'s end
pointer into a destroyed temporary). Fixed, and the sanitizer build now runs in CI.

## Be upfront about
- The kernel backend was validated as generated command strings and dry-run; run `scripts/lab.sh demo` on your own
  Ubuntu VM before the interview so you can say you saw real `tc` output.
