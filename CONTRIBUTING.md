# Contributing

Thanks for your interest in contributing to Casio AI Machine.

## Development Setup

1. Install PlatformIO.
2. Clone this repository.
3. Create local env file:
   - copy `.env.example` to `.env.local`
   - fill your own values
4. Build and flash using `platformio.ini` environments.

## Contribution Scope

We welcome contributions in:

- firmware reliability
- OLED rendering quality
- camera stability
- button/UX behavior
- backend integration docs
- testing and diagnostics

## Pull Request Guidelines

1. Keep PRs focused and small.
2. Explain what changed and why.
3. Include test steps and observed results.
4. Never commit secrets, private keys, or production URLs.
5. Update docs when behavior or setup changes.

## Coding Notes

- Use PlatformIO workflow.
- Follow repository token-hygiene rules in `AGENTS.md`.
- Keep hardware pin mappings explicit and documented.

## Security

If you find a security issue, do not open a public issue first.
Please follow [SECURITY.md](SECURITY.md).
