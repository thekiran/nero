# Remaining release readiness — 9 November 2026

Scope: unchecked items of #11 only, with a review of existing distribution
verification. No version/tag was created and published assets were not replaced.

| Remaining item | State | Evidence / next requirement |
| --- | --- | --- |
| macOS Developer ID signing / notarization | Blocked | package_macos.py truthfully records signed=false, notarized=false; no authorized signing integration supplied |
| Windows Authenticode | Blocked | Windows packaging metadata says unsigned; no owner certificate/service authorization supplied |
| Optional signed APT hosting | Deferred, optional | Local .deb distribution exists; hosting/key ownership/rotation/domain and publication policy not supplied |
| Independent LAN-device observations | Blocked | No accessible owner-designated physical device/network matrix; see independent checklist below |

## Existing installer verification audit

The Release packages workflow builds native macOS ARM64/Intel `.pkg` and portable
archives, Linux amd64/arm64 `.deb` and archives, Windows x86_64 `.msi` and ZIP.
It verifies payloads and SHA-256 sidecars before aggregate Release checks, and
only trusted publication paths receive contents:write. No signing is fabricated.

The 2026-11-09 PR #65 run [42644280091](https://github.com/thekiran/nero/actions/runs/42644280091)
verified macOS ARM64/Intel packages/global command in normal and sudo shells and
removal, Linux x86_64 packaging/global command/removal, and Windows MSI real
installation/upgrade/PATH/removal. The initial Linux ARM64 job timed out in the
existing portscanner-reliability sanitizer fixture, before packaging; it is not
counted as a full-platform pass. A failed-job rerun was requested after reading
the failure; never weaken or remove this test to obtain a release pass.

Distribution run [42644280145](https://github.com/thekiran/nero/actions/runs/42644280145)
succeeded: Debian first-install/version-upgrade/removal on both architectures
and published Homebrew formula install/reinstall/removal on Apple Silicon/Intel.
These are separate evidence from unexecuted physical-device tests. Source/archive
lifecycle regression also passed locally on macOS arm64. The native macOS `.pkg`
release path tests install/removal, but does not independently demonstrate a
two-version `.pkg` upgrade. Do not infer that case from Homebrew reinstallation.
No machine-wide installer was run on the owner's development computer.

## Signing handoff requirements

For macOS, the owner must authorize an Apple Developer team, Developer ID
Application identity for the Mach-O binary, Developer ID Installer identity for
`.pkg`, and notarization access through a supported keychain/API-key profile.
Use hardened runtime and secure timestamps; sign the binary before packaging,
sign the installer, submit with `notarytool`, inspect the accepted result and
staple/validate the package ticket. Verify with `codesign --verify --strict`,
`pkgutil --check-signature`, `stapler validate` and Gatekeeper assessment on a
clean machine before changing release metadata to signed/notarized. Do not
export identities, dump keychains, put passwords in arguments or bypass warnings.

For Windows, authorize an owner-controlled Authenticode certificate with its
required hardware/key protection or supported signing service. Sign the executable
before MSI packaging and sign the final MSI, using SHA-256 and a supported RFC3161
timestamp. Verify the signature chain/timestamp with `signtool verify /pa /v`
and test on a clean supported Windows host. Never substitute a self-signed
certificate or claim that a SHA-256 checksum is an Authenticode signature.

All final hashes and metadata must be generated after signing/stapling; signed
bytes differ from unsigned bytes. Credential/service setup requires owner action.
No secret value is needed in an issue or PR. Optional APT hosting and Homebrew Core
are separate from these essential integrity/trust disclosures.

Official references: [Apple Developer ID](https://developer.apple.com/developer-id/),
[Apple notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow),
[Microsoft SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool).

## Independent LAN test checklist (not executed)

An independent tester should use only owner-controlled devices on an isolated
network, with a written target/port/time-window approval and an immediate stop
procedure. Keep third-party devices outside the lab. Do not run `-Syn` or any
flood engine for this checklist; no raw-engine certification is implied.

Before testing, record NERO commit/version, exact OS/architecture, installation
method, interface type, authorized device pseudonyms and expected listener/firewall
states. Keep raw addresses/MACs/usernames/tokens out of shared evidence. Record
awake/sleep state at the moment of each observation, not from memory.

| Case | Setup and expected evidence | Result |
| --- | --- | --- |
| Listening service | One known TCP listener on an awake owned device; selected port classified open | Not run |
| Refused service | Known closed TCP port that returns refusal; classified closed, not timeout | Not run |
| Filtered traffic | Explicit lab firewall drop; filtered/inconclusive outcome and timeout documented honestly | Not run |
| Awake versus sleeping phone | Repeat controlled listener test in each known state; inconclusive result never described as proof device absent | Not run |
| Wrong route / unavailable interface | Controlled local route/interface failure; bounded exit with clear error, no fabricated closed ports | Not run |
| Partial selection | After one open port appears, Enter; menu uses discovered ports and reports partial coverage | Not run |
| Cancellation | Esc/Ctrl+C at scan and menu; workers stop and terminal echo/cursor restore | Not run |
| Repeated use | Repeat after cancellation; clean state and no stale selection | Not run |
| Plain/TTY output | Redirected and interactive runs; readable report and restored terminal | Not run |

For each case preserve sanitized expected/observed output, duration, exit status,
pass/fail rationale, tester/date and controlled service/firewall setup. Never
label mock sockets, loopback fixtures or CI runners as independent LAN validation.
Keep #11 open until signing and independent observations satisfy its acceptance
criteria; optional repository hosting may remain separately deferred.

## Final source validation evidence

The latest completed code rollout is PR #68, main
`1438243803afa6d853838ea156ec029fe9501706`, including the separately reviewed
readable-menu PR #70. Its [Release run 42652892574](https://github.com/thekiran/nero/actions/runs/42652892574)
passed all five native installer/archive targets and aggregate verification;
[Distribution run 42652892728](https://github.com/thekiran/nero/actions/runs/42652892728)
passed both Debian upgrade/removal and both Homebrew install/reinstall/removal
legs. The initial ARM64 fixture timeout described above is retained as historical
risk evidence; later green runs do not establish its root cause or certify LAN
behavior. No scanner or raw-engine code was changed to obtain green checks.

Local combined-source evidence (macOS ARM64, AppleClang 21):

```sh
cmake --preset macos
cmake --build --preset macos -j 8
ctest --preset macos -E '^scanner-runtime$' --output-on-failure
cmake --preset macos-sanitize
cmake --build --preset macos-sanitize -j 8
ctest --preset macos-sanitize -E '^scanner-runtime$' --output-on-failure
python3 -m unittest tests/test_wiki_sync.py tests/test_workflow_security.py
make -j 8 BUILD_DIR=.build-command-groups TARGET=.build-command-groups/nero
make analyze BUILD_DIR=.build-command-groups
```

Results: 16/16 native CTests, 16/16 ASan/UBSan CTests and 12/12 Wiki/workflow
Python tests passed. The excluded local scanner-runtime fixture attempted a real
loopback bind denied by the execution sandbox; it remained enabled and passed in
native CI. Existing .build/obj writes were restricted locally, so an isolated
Make output directory was used successfully instead of weakening permissions.

Ubuntu coverage campaigns used the documented `NERO_FUZZING=ON` build and
`python3 tests/fuzz/run.py .build/fuzz --campaign`, with all three targets passing
at the integrated PR head. Native CI also executed Make tests, ASan/UBSan suites,
macOS TSan fixtures and terminal/storage/scanner regressions. These are
mock/loopback observations; no live packet-flood operation or independent physical
LAN-device test was run. TCP scanner, scan session, packet engine, hardware prompt
and worker-limit header Git blob hashes remained identical to baseline `1dceac5`.

The [security rollout audit](security-controls-2026-11-09.md) records actual
CodeQL/Scorecard evidence and the GitHub reauthentication blocker for adding four
new required checks. Signing identities, a private physical LAN matrix and actual
Wiki credential scope/expiry remain owner verification steps. No public release,
new version tag, signing certificate or notarization ticket was created here.
