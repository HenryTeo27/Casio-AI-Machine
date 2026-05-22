# Security Policy

## Supported Scope

This project includes:

- device firmware
- backend reference templates
- prompt/harness integration notes

## Reporting a Vulnerability

Please report security issues privately first.

Recommended report content:

1. Affected component/file
2. Reproduction steps
3. Potential impact
4. Suggested mitigation (optional)

Do not publish working exploit details in a public issue before maintainers have time to patch.

## Sensitive Data Rules

- Never commit API keys, DB credentials, or device secrets.
- Keep `.env.local` private.
- Rotate any credential immediately if exposed.
- Use placeholders in examples and docs.

## Firmware-Specific Notes

- Do not hardcode production provider keys in ESP firmware.
- Device auth keys in firmware should be treated as revocable credentials.
- Prefer server-side signing/auth upgrades for production deployments.
