# Security policy

## Report a problem

Do not put a credential or exploit detail in a public issue. Use GitHub private
vulnerability reporting when it is available. If it is not available, open an
issue that contains only a request for a private security channel. Wait for a
maintainer response before you send technical details.

## Secret rules

- Do not commit API keys, Wi-Fi credentials, Apple signing data, personal
  paths, or stable device identifiers.
- Use `.secrets/device.env` only for local development. Git ignores this path.
- Store iOS secrets in Keychain, not `UserDefaults`.
- Provision secrets only over authenticated, encrypted BLE.
- Keep device secrets in encrypted NVS. Do not print them.
- Run `uv run --locked opendle-secrets check` before each commit.

If a secret enters Git history, revoke it first. Removing the text in a later
commit is not sufficient.
