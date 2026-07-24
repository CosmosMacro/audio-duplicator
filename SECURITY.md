# Security policy

## Scope

Audio Duplicator is intended to run locally on Windows. The application should not require administrator privileges, install drivers, create services, modify startup settings, access the network, or write user data unless a future feature explicitly documents that behavior.

## Trusted builds

For development builds, prefer artifacts produced by this repository's GitHub Actions workflow or compile the source locally. CI artifacts are unsigned and include a `SHA256SUMS.txt` file so the downloaded executable can be checked for accidental corruption.

A source review does not prove that an unrelated prebuilt executable was produced from the reviewed source. Do not treat binaries from other repositories or file-sharing services as equivalent to this project's source.

## Reporting a vulnerability

Please open a GitHub security advisory for vulnerabilities that should not be public immediately. For ordinary robustness bugs, open an issue and include:

- Windows version;
- affected audio devices;
- exact reproduction steps;
- whether Bluetooth hands-free mode was active;
- relevant error messages.

Do not attach recordings or logs containing private audio content.
