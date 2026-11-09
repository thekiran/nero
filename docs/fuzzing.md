# Offline input fuzzing

Harnesses compile the real tokenizer (`parser.c`), private history reader
(`read_entries`) and log importer (`import_recent`, severity recovery, filtering,
record splitting and JSON escaping). History/log harnesses include their source
file to access private functions without adding public APIs. Their link graphs
contain no CLI dispatcher, scanner, scan session or packet engine. Only temporary
stdio streams are used for input/output; supplied bytes never become a path.
Log import was split from file opening without changing its byte processing.

Ubuntu Clang with compiler-rt/libFuzzer is the supported coverage-guided runner:

```sh
cmake -S . -B .build/fuzz -DCMAKE_C_COMPILER=clang -DBUILD_TESTING=OFF -DNERO_FUZZING=ON
cmake --build .build/fuzz --target fuzz-parser fuzz-history fuzz-log --parallel 2
python3 tests/fuzz/run.py .build/fuzz
python3 tests/fuzz/run.py .build/fuzz --campaign
```

Each campaign uses seed 1905, at most 10,000 runs or 30 seconds per target,
2 seconds per input, 512 MiB RSS, 128 MiB allocation limit and 128 KiB inputs.
The parser bounds itself to 64 KiB; history covers the 64 KiB tail boundary.
ASan/UBSan and strict warnings are always enabled. Reviewed seeds include valid,
truncated, malformed, binary, Unicode, empty and oversized inputs. Campaigns copy
seeds into temporary storage so mutations never dirty the source tree.

Missing Clang/runtime support is an error, never a successful skip. Apple Clang
may omit libFuzzer. For deterministic sanitizer replay there, replace
`-DNERO_FUZZING=ON` with `-DNERO_CORPUS_REPLAY=ON` and run the first run.py command.
Standalone replay is explicitly not a coverage-guided campaign. Windows harness
execution is unsupported; Windows production parser/history/log regression tests
remain in native CI.

CI runs reviewed seeds and bounded campaigns on every PR. This job must be added
to server-required checks by the administrator before it can be called a mandatory
merge gate; a workflow definition alone does not enforce branch protection.

For a crash, the runner preserves emitted inputs under the build directory
`failures/<target>` and CI uploads them for seven days. Only synthetic reviewed
seeds/mutations enter these targets. For interactive debugging, use an explicit
artifact directory when running a binary directly,
reproduce with the same binary/input, and minimize with `-minimize_crash=1`
`-exact_artifact_path=reproducer`. Commit the smallest non-sensitive reproducer
and a targeted regression after confirming a production defect. Do not upload
arbitrary user history/logs. Bounded fuzz success is evidence for those inputs,
not a proof that all malformed inputs are safe.
