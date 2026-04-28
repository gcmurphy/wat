# wat

A small voice-to-text CLI. Listen to the microphone, transcribe locally with
[whisper.cpp](https://github.com/ggml-org/whisper.cpp), print plain text to
stdout. No network, no daemon, no telemetry.

```sh
$ wat
hello world
```

`wat` does one thing — it turns speech into text on stdout — so it composes
with anything else through pipes:

```sh
wat | xclip -selection clipboard          # dictate into the clipboard
wat | rg -i "todo"                        # speak, grep your stream of consciousness
wat -s | tee dictation.log                # streaming, with a transcript on disk
```

## Build

Requires a C11 compiler, `cmake` (for whisper.cpp), `pthread`, and `libgomp`
on Linux.

```sh
git clone --recurse-submodules <repo-url> wat
cd wat
make            # builds build/wat
make model      # downloads ggml-tiny.en.bin into ~/.local/share/wat/
./build/wat
```

`make model MODEL=base.en` (or `small.en`, `medium.en`, etc.) for a different
model. Smaller is faster but less accurate; `tiny.en` transcribes faster than
real-time on a modern laptop CPU.

## Usage

```
wat [options] [file.wav]

  -m PATH   model (default: $WAT_MODEL or ~/.local/share/wat/...)
  -l LANG   language (default: en)
  -t N      threads (default: 4, range 1-64)
  -s        streaming mode (continuous)
  -p        push-to-talk (press key to start/stop)
  -T FLOAT  VAD threshold (default: 0.01)
  -S MS     silence before stop (default: 1500, range 100-60000)
  -v        verbose (timing + VU meter on stderr)
  -h        help
```

Three capture modes:

| Mode | Trigger | Stops when |
|---|---|---|
| **VAD** (default) | Run `wat`, start talking | After `-S` ms of silence |
| **Streaming** (`-s`) | Run `wat -s`, talk freely | Ctrl+C |
| **Push-to-talk** (`-p`) | Run `wat -p`, press any key | Press any key again |

Pass a `.wav` file as the last argument to transcribe a file instead of the
mic. Files must be 16-bit PCM (any sample rate; mismatches print a warning).

## How it works

```
mic ──► miniaudio ──► f32 PCM @ 16kHz ──► [ring buffer] ──► whisper_full() ──► text ──► stdout
```

1. **Capture** (`src/audio.c`): `miniaudio` (single-header, public domain)
   delivers audio on a callback thread. A mutex+condvar ring buffer hands
   samples to the main thread. The callback is the only writer; main is the
   only reader. The condvar's `pthread_cond_timedwait` wakes every 100 ms so
   Ctrl+C is never blocked.
2. **Frame & VAD** (`src/capture.c`, `src/vad.c`): we slice the stream into
   30 ms frames (480 samples), compute RMS energy, and stop when energy
   stays below `-T` for `-S` milliseconds.
3. **Transcribe** (`src/main.c`): hand the float32 buffer to
   `whisper_full()`. whisper builds an 80-band mel spectrogram, runs the
   transformer encoder over it, and autoregressively decodes text tokens.
   We just print the segments.

Whisper requires 16 kHz mono float32 — that's why every step in the pipeline
speaks that one format.

## Project layout

```
src/        capture, audio, wav, vad, util, tty, main
include/    public-ish headers; constants live here
vendor/     miniaudio.h, whisper.cpp/ (submodule, pinned to v1.8.4)
tests/      run_tests.c — 24 unit tests, no framework
fuzz/       AFL harness, structured corpus generator, dictionary
build/      out-of-tree artifacts; gitignored
```

## Development

| Command | What it does |
|---|---|
| `make` | Build `build/wat` |
| `make test` | 24 unit tests for the WAV parser and VAD |
| `make asan` | Tests under AddressSanitizer + UBSan |
| `make ubsan` | Tests under UBSan only |
| `make valgrind` | Tests under valgrind, leak-check=full |
| `make lint` | `cppcheck --enable=all` |
| `make fuzz-corpus` | (Re)generate the 16-seed AFL corpus |
| `make fuzz` | Build the AFL harness; prints the `afl-fuzz` invocation |
| `make clean` | Wipe `build/` and `fuzz/findings/` |

### Fuzzing

The WAV parser is the primary external attack surface, so it gets the most
attention. `fuzz/make_corpus.c` programmatically builds 16 seeds — every
supported sample rate × {mono, stereo}, plus near-valid hostile inputs that
each land on a different error branch (bad format code, 8-bit, 24-bit,
9-channels, odd-size pad-byte chunk, truncated header, empty file).
`fuzz/wav.dict` gives AFL the RIFF/WAVE/fmt /data/LIST/INFO/etc tokens so
its mutations reach valid chunk-handling code paths quickly.

```sh
make fuzz
afl-fuzz -i fuzz/seeds -o fuzz/findings -x fuzz/wav.dict -- build/fuzz/fuzz_wav @@
```

## Security

See [SECURITY.md](SECURITY.md) for the threat model and audit notes. Short
version: WAV files and CLI args are treated as untrusted; the model and
microphone audio are trusted; whisper.cpp/ggml are out of scope.

Hardening in the production build:
`-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wall -Wextra -Wshadow
-Wpointer-arith -Wcast-align -Wstrict-prototypes -Wmissing-prototypes`.

## License

BSD 2-Clause — see [LICENSE](LICENSE). Vendored: whisper.cpp is MIT,
miniaudio is public domain.
