# Nero

An experimental terminal application and network research project written in C.
For isolated or explicitly authorized laboratory environments only.

## Start here

| Task | Entry point |
| --- | --- |
| Find your way around the code | [Code map](docs/code-map.md) |
| Build and test the project | [Development guide](docs/building.md) |
| Understand component boundaries | [Architecture](docs/architecture.md) |
| Contribute changes | [Contribution guidelines](CONTRIBUTING.md) |
| Browse all documentation | [Documentation index](docs/README.md) |

## Quick start — macOS

From the repository root:

```sh
make             # Build objects in .build/ and the nero executable at the root
./nero           # Open the terminal interface
make test        # Run offline regression tests
make help        # List development commands
```

A C compiler and SDK are required; tests also require Python 3.
For CMake development, see the [setup and preset commands](docs/building.md).

## Source layout

```text
nero/
├── src/
│   ├── main.c                  Application entry point
│   ├── cli/                    Terminal session, input, rendering and commands
│   ├── core/                   Platform information and shared foundation
│   ├── modules/                Platform report and existing Linux modules
│   └── platform/               Operating system adapters
├── include/nero/               Public project headers
├── third_party/cjson/          Third-party code
├── tests/                     Regression, unit, integration and mock tests
├── cmake/                     Shared CMake target configuration
├── docs/                      Code map and development documentation
├── data/                      Data files
└── .build/                    Generated files; excluded from Git
```

`src/cli/cli_internal.h` defines the private contract between CLI modules.
External components use the headers in `include/nero/`.

## Platform scope

| Platform | Make | CMake |
| --- | --- | --- |
| macOS | Terminal and local interface adapter; network operations disabled | Offline CLI and platform tests |
| Linux | Existing Linux modules | Offline CLI and platform tests |
| Windows | Unsupported | Platform report and foundation tests; no CLI |

Local validation covers macOS. Linux and Windows CI definitions are not evidence
of successful execution on those platforms. See [platform support](docs/platform-support.md).

[License](LICENSE.txt) · [Security](SECURITY.md) · [Laboratory use](docs/laboratory-safety.md)
