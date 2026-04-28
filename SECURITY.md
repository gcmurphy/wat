# Security Audit — wat

Date: 2026-04-17
Scope: main.c, capture.c, audio.c, wav.c, vad.c, util.c

## Threat model

`wat` is a local CLI tool. Primary attack surfaces:

1. **WAV files** — may be untrusted (downloaded, received over a channel)
2. **Environment variables** — `WAT_MODEL`, `XDG_DATA_HOME`, `HOME`
3. **Command-line arguments** — numeric parsing (-t, -T, -S)
4. **Model file** — treated as trusted (loaded by whisper.cpp)
5. **Audio data from mic** — attacker can place sounds near the mic

Not in scope: kernel audio driver bugs, whisper.cpp internals, ggml bugs.

## Findings

### HIGH — (none after fixes)

### MEDIUM

**M1 — `strtof` result not checked (main.c `-T`)**
`strtof(optarg, NULL)` returns 0 for malformed input. A flag like
`-T garbage` silently becomes `-T 0` which disables VAD entirely.
*Fix*: use `errno` + `endptr`, reject non-numeric input.

**M2 — `atoi` silently accepts garbage**
`atoi("abc")` returns 0. Same problem as above for `-t` and `-S`.
*Fix*: use `strtol` with `endptr` validation.

**M3 — No path length limit on `-m`/`WAT_MODEL`**
A very long path passed to `-m` is handed directly to whisper.cpp
which eventually calls `fopen`. Not exploitable but worth noting.
`default_model()` uses a 4KB buffer with `snprintf` which is safe
(truncates gracefully).

### LOW

**L1 — `whisper_log_cb` may be called from other threads**
`g_verbose` is read without a memory barrier. Benign: it's only
written once at startup before any whisper call.

**L2 — `signal()` instead of `sigaction()`**
`signal()` has historically-implementation-defined behavior around
SA_RESTART and handler persistence. Modern glibc defaults to BSD
semantics (persist) so this is safe on Linux/macOS.
*Mitigation*: could switch to `sigaction` for portability guarantee.

**L3 — `fread` return values in skip-read fallback**
Handled correctly — function returns 0 on short read.

### INFORMATIONAL

**I1 — No ASLR hardening flags in build**
Default Linux binaries are PIE with ASLR. We pass `-O2` which is fine.
Could add `-fstack-protector-strong -D_FORTIFY_SOURCE=2` for defense in depth.

**I2 — Ring buffer uses mutex, not lock-free**
Classic trade-off. Mutex contention at 16 kHz is trivial. Safe.

**I3 — `capture_vad` timed-wait doesn't check `quit_flag` inside lock**
The 100ms poll means Ctrl+C latency is bounded. Fine.

## WAV parser — detailed review

The parser is the primary external attack surface. Reviewed
`wav.c:wav_load` against common WAV/RIFF bugs:

- ✅ Chunk size truncation: reads little-endian explicitly via `read_u16`/`read_u32`
- ✅ Negative fseek: chunk sizes are `uint32_t`, never signed
- ✅ Division by zero: guards `bits != 16` and `channels > 0` before `frame_sz`
- ✅ Integer overflow in `bps * channels`: `bits` is 16, `channels` ≤ 8 → max 16
- ✅ Huge allocation: `nframes` capped at `WAV_MAX_SAMPLES` (28.8M, ~115MB)
- ✅ `bits=0` / `channels=0`: rejected before use
- ✅ `data` before `fmt`: rejected with `have_fmt` guard
- ✅ Non-PCM format (e.g. `fmt=3` IEEE float, `fmt=7` µ-law): rejected
- ✅ Missing chunks: loop breaks on short read, returns NULL
- ✅ Odd chunk size pad byte: handled
- ✅ Endianness: uses byte-level reads, host-independent

## Recommendations applied

1. Validated `-t`, `-T`, `-S` numeric args (M1, M2)
2. Added `-fstack-protector-strong -D_FORTIFY_SOURCE=2` to CFLAGS (I1)
3. Switched to `sigaction` (L2)

## Open items

- Consider seccomp sandbox for file-mode (no mic needed → no device access)
- Consider reducing privileges after model load (no mic mode)
