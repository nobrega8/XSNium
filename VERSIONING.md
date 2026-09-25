# Versioning

XSNium uses [Semantic Versioning](https://semver.org): `MAJOR.MINOR.PATCH`, with an optional pre-release label.

| Part | Meaning |
| --- | --- |
| `MAJOR` | A change that breaks how you use XSNium: the CLI, the library API or the saved-data behaviour. |
| `MINOR` | New ability: a control, a rule or expression feature, a fidelity stage, a new command. |
| `PATCH` | Fixes only: bugs, security fixes, documentation, no new behaviour. |
| `-alpha.N`, `-beta.N`, `-rc.N` | Pre-releases, in that order. `1.0.0-rc.1` comes before `1.0.0`. |

## The road to 1.0

While the major version is `0`, the project is in early development. A `MINOR` bump may include breaking changes, and the release notes say so. `1.0.0` is released when:

- the MVP in [plan.md](plan.md) is complete and every form in the test set opens, fills in and saves correctly;
- rendering fidelity stages F1 to F4 are done and measured against InfoPath;
- the library API and the CLI are documented and stable.

After `1.0.0`, breaking changes wait for `2.0.0`.

## Where the number lives

- `package.json` holds the version. It is the only place to edit.
- `xsnium --version` prints it.
- A release is a git tag `v<version>` (for example `v0.2.0`). The release workflow refuses a tag that does not match `package.json`.
- The Windows installer is named `xsnium-<version>-setup.exe`. Its four-part file version is `<version>.<build>`, where `build` is the CI run number, so every build is distinguishable even when the version has not changed.

## Making a release

1. Update `version` in `package.json`, commit it as `Release x.y.z`.
2. Tag it: `git tag vX.Y.Z` and push the tag.
3. The [release workflow](.github/workflows/release.yml) builds the single-file program for Windows, macOS and Linux and the Windows installer, then publishes them with SHA-256 checksums as a GitHub release (marked pre-release while the version has a label or is `0.x`).

## Data compatibility

The format XSNium writes is InfoPath's own form XML, so a saved form does not depend on the XSNium version that saved it. What may change between versions is the behaviour of the runtime and the look of rendering, and the release notes list those changes.
