# Releasing

How to ship:

1. Wait for master's full run (a push to master runs every platform and every test, and publishes nothing) to go green.
2. Run `tools/ship.sh` (`--dry-run` first to see the number): it checks that run and pushes the tag `build-<N>`, N one above the latest release.
3. The tag's run builds, signs, notarizes and publishes the release "Build N", marked latest, which the self-updaters and Sparkle's appcast find.

A run started by hand (Actions, Build, Run workflow) with a version does the same; with `dry_run`, or from any branch but master, it publishes nothing and keeps the would-be release as an artifact. A failing master run opens an issue labelled `ci-broken`, and the next green run closes it.

## From our own machines

`tools/release_local.py` makes the same release without GitHub Actions. GitHub only hosts the files. Each build runs `tools/ci_build.py`, as in the workflow. The tests are `tools/ci_plan.py`'s two lists, plus the scorecard and the merge checks. Packaging, notes, the expected-file check, the appcast and the signature are the workflow's own scripts.

1. `python3 tools/release_local.py build --number N` (from any checkout with gh signed in) builds every platform. Linux x86-64, Windows (cross-compiled against `~/goat-rig/xwin`), the DLSS add-on and the tests run on omarchy. macOS, iOS, Android and Linux ARM64 (in an arm64 build of `.github/ci-image`) run on the release Mac. The release folder is assembled in omarchy's `~/goat-release/build-N`. A platform whose build fails is carried over from the latest release, and the notes say so. Failed tests stop the release at `sign`. `status --number N` shows where it stands.
2. On the release Mac, at its keyboard: `security unlock-keychain`, then `python3 ~/goat-release/build-N/src-macos/tools/release_local.py sign --number N`. This re-signs the APKs with the release keystore, signs and notarizes the macOS app, signs its disk image for Sparkle, writes appcast.xml, checks every expected file, then writes SHA256SUMS and signs it with the release key. The keys stay on that Mac.
3. `python3 tools/release_local.py publish --number N --dry-run`, then without `--dry-run`: one `gh release create --latest`, and the last five releases kept.

The machines are `omarchy` and `release-mac` in `~/.config/goat-rig/hosts` (`<name> <user@host>`), or `OPENH1_RELEASE_LINUX` and `OPENH1_RELEASE_MAC`. To replace a machine, give the new one the same tools: the CI image's file system and xwin on the Linux machine (`tools/ci-runner/install.sh`, `tools/rig/install.sh omarchy --windows`); Xcode, Homebrew's ninja, ccache and OpenSSL 3, the Android SDK with Android Studio's JDK, and OrbStack or Docker on the Mac.

The signing Mac holds:
- the release key's seed, `~/.config/openh1/release-signing-key`;
- the Android keystore, `~/.openh1-android/openh1-release.jks`, with its password in the login keychain as "OpenH1 Android keystore";
- the Developer ID Application identity and Sparkle's key (account `openh1`), both in the login keychain;
- the App Store Connect API key, `~/.appstoreconnect/private_keys/AuthKey_<id>.p8`, with its issuer ID in `~/.config/openh1/asc-issuer-id` or `OPENH1_ASC_ISSUER_ID`.
