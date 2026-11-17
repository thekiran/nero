# macOS Wi-Fi identifier access

NERO's C CLI delegates active discovery to `NEROWiFi.app`, a small Cocoa app
using CoreLocation for permission and CoreWLAN for scanning. The app does not
request coordinates, join/disassociate networks, change channels, or enable
monitor mode. Active scans transmit normal probe requests and can briefly share
radio time with other traffic.

## Build and authorize

On macOS 11+ with Xcode command-line tools and a current SDK:

```sh
make -j4
make wifi-helper
open ./NEROWiFi.app
```

Click **Allow Wi-Fi Identifier Access** in the app, then approve the macOS prompt.
Opening the app alone does not request permission. Close its window afterward.
Run NERO **as your logged-in desktop user, without sudo**, and enter:

```text
wifi scan
wifi scan en0 --band 5
wifi aps
```

The helper is found beside the resolved CLI executable, then at
`/Applications/NEROWiFi.app`. For an installed CLI, copy the signed app to
`/Applications` using Finder (or put it beside that CLI). `make install` continues
to install only the CLI. Custom `TARGET` locations need the app beside that
binary too. Do not place the helper inside an app-translocated download folder.

`wifi scan` always requests a fresh active scan. On macOS, `wifi aps` does so
only when there are no captured packets and no running passive session; it labels
that source explicitly. Otherwise `wifi aps` retains its passive snapshot and
filtering semantics, even when a channel filter has no matches. Active results
are not inserted into the passive tracker, exported as captured frames, or used
to invent clients. Linux/Windows and offline CLI fixtures keep their previous
behavior. Existing explicit monitor commands remain separate; neither permission
setup nor these two commands starts a monitor test.

## Signing and persistence

The default build applies and verifies an **ad-hoc signature**, suitable for local
testing. For a stable identity across rebuilds, use a certificate present in your
Keychain. The Make helper target re-signs and verifies on every invocation:

```sh
make wifi-helper WIFI_SIGN_IDENTITY='Developer ID Application: YOUR NAME (TEAMID)'
codesign --verify --strict NEROWiFi.app
```

Keep bundle ID `com.thekiran.nero.wifi`, signing identity, and installation path
stable. Ad-hoc rebuilds can lose consent continuity; grant access again through
the app/System Settings if needed. Distribution also requires your usual Apple
notarization workflow; this target does not notarize or create a certificate.
The hardened-runtime signature includes `com.apple.security.personal-information.location`,
required for macOS to show the Location prompt. No privileged service, App Sandbox,
or TCC database modification is used.
Both `NSLocationWhenInUseUsageDescription` (requested compatibility key) and
`NSLocationUsageDescription` (Apple's macOS key) are present.

CMake provides an optional, signed helper target; its normal CLI targets remain
offline test fixtures:

```sh
cmake -S . -B .build-location/cmake -DBUILD_TESTING=ON
cmake --build .build-location/cmake --target NEROWiFi
# Optional configure flag: -DNERO_WIFI_SIGN_IDENTITY='Developer ID Application: ...'
open .build-location/cmake/NEROWiFi.app
```

## Permission and scan outcomes

- **not_determined:** open the app and click the permission button. CLI scans
  never trigger a permission prompt or wait for the user to answer one.
- **denied:** enable NERO Wi-Fi in System Settings → Privacy & Security →
  Location Services; repeated scans do not re-prompt.
- **restricted:** system policy prevents access; contact the administrator.
- **disabled:** turn on Location Services globally, then authorize the app.
- **no_interface / powered_off / scan_failed:** the helper reports the actual
  failure; it does not substitute another interface for an explicitly named one.
- **authorized, empty:** a successful scan heard no networks. This differs from
  permission failure and from zero captured monitor frames.
- **authorized, redacted:** nil identifiers remain unavailable. A known empty
  SSID is treated as hidden; a withheld SSID is not claimed to be a hidden network.

The app's status updates when CoreLocation reports an authorization change.
Neither helper nor CLI claims that a granted permission guarantees every network
will disclose an SSID/BSSID. Monitor-mode zero-frame warnings and client counts
remain independent of active discovery.

## IPC and privacy

CLI uses `posix_spawn` with an argument array to run `/usr/bin/open -n -W -g`;
LaunchServices starts a separate app instance with the signed bundle identity.
It does not invoke a shell. Each request has a randomly named mode-0700 directory
and pre-created mode-0600 response file. The helper opens the directory/file with
no-follow checks and verifies ownership, permissions, regular-file type, link
count and initial emptiness. No shared socket or predictable cache is used.
Concurrent scans have distinct requests. Scans refuse elevated execution.

The version-1 JSON envelope is `{version:1,status:"authorized",aps:[...]}`.
Rows contain `bssid` (string/null), `ssid_hex` (hex bytes/null), `channel`, `band`,
`rssi`, and `security` (NERO enums). Error envelopes have an empty `aps` array.
The CLI limits responses to 128 KiB / 256 APs, validates the version, shape, types,
ranges and identifiers, and rejects malformed/truncated responses atomically.
SSID control/non-ASCII bytes are replaced with `?` for the existing ASCII table
renderer. Unknown error strings are never printed as terminal text. Raw network
names are not passed as arguments or logged by the helper. Temporary responses
are removed after success/failure; abrupt process termination may leave a private
`/tmp/nero-wifi.*` directory for the user to remove. The helper terminates a stalled
scan after 25 seconds; the CLI stops waiting after about 30 seconds.

## Tests and manual acceptance

```sh
make test-wifi-helper
make test-wifi-helper-bundle  # verifies the signed Location entitlement, no radio activity
make test
cmake --build .build-location/cmake -j4
ctest --test-dir .build-location/cmake --output-on-failure
make BUILD_DIR=.build-location-sanitize test-wifi-helper SANITIZERS='-fsanitize=address,undefined -fno-omit-frame-pointer'
```

After granting consent, an explicitly opt-in test checks real identifiers and
repeated fresh helper launches (requires audible networks; never logs names):

```sh
python3 tests/test_macos_wifi_live.py ./nero ./NEROWiFi.app
```

This live test is deliberately excluded from `make test` and CTest.
The protocol tests are offline: granted/empty/redacted results, permission errors,
invalid version/shape/ranges/hex, trailing data, duplicate envelope keys, terminal
control bytes and zero fabricated frame/client counts. Existing pcap/table tests
exercise passive AP/client data. These tests do not authorize Location Services
or run a live monitor capture.

Manual acceptance on a Mac (requires the user's permission decision):

1. Before authorization, run `wifi scan` and `wifi aps`: both should give the
   permission instruction without a prompt or fabricated rows.
2. Open the app, click its button and deny access. Retry: expect the denied
   diagnostic. Enable NERO Wi-Fi in Location Services and retry.
3. With access granted, check real SSIDs/BSSIDs against visible nearby networks.
   Verify both commands and a channel/band filter. Verify internet connectivity
   continues. Do not disconnect Wi-Fi or enable monitor mode for this test.
4. Revoke access and retry. No cached identifier results should appear. If an
   existing passive capture is loaded, `wifi aps` continues to show that capture.
5. A managed/restricted Mac and globally disabled Location Services should report
   their respective states. These states are fixture-tested without changing
   the user's system policy.

Local build/test evidence is recorded separately from manual permission approval;
passing offline tests is not evidence of a granted live scan, Linux/Windows runs,
or GitHub CI success.

Apple references: [authorization requests](https://developer.apple.com/documentation/corelocation/requesting-authorization-to-use-location-services),
[macOS usage-description key](https://developer.apple.com/documentation/bundleresources/nslocationusagedescription),
[authorization callbacks](https://developer.apple.com/documentation/corelocation/cllocationmanagerdelegate/locationmanagerdidchangeauthorization(_:)).

### Local verification — 2026-11-15

Live validation found and fixed two issues missed by the original offline tests:

- The hardened-runtime signature lacked the Location entitlement, so macOS
  silently suppressed the permission prompt. Both signing paths now embed it;
  `test_macos_wifi_bundle.py` checks the entitlement in the actual signed app.
- Scanning immediately after constructing CLLocationManager saw a transient
  `not_determined` state. Scan launches now wait for the initial authorization
  delegate callback before reading permission or using CoreWLAN.

The actual macOS permission dialog appeared and access was granted with the
user's approval. With the GUI closed, the opt-in live test passed: the helper
returned **4 real APs with SSIDs and BSSIDs**, and fresh launches for `wifi scan`,
`wifi aps`, then `wifi scan` each displayed **4 BSSIDs**. An earlier 5 GHz filter
returned one AP; an invalid interface produced the expected explicit error.
Network visibility can change between scans. Post-scan HTTPS connectivity
returned HTTP 200. No monitor capture, disassociation or channel change was run.

Make and CMake signed-bundle checks passed; CTest passed **26/26**, and the passive
pcap/table regression passed against the production CLI. The earlier full
`make test` and ASan/UBSan protocol checks also passed. The root `./nero` executable
was updated; its previous version is backed up at
`.build-location/nero-before-location`. The build uses `.build-location` because
older `.build` outputs were not writable.

Actual restricted/denied UI states remain fixture-tested only. Linux, Windows,
Developer ID signing/notarization, and remote CI were not run or verified.
Close the helper before rebuilding it. Ad-hoc recompilation can require renewed
consent; use a stable signing identity and one installation path for daily use.
