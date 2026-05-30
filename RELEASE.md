# Release Flow

Use this checklist when cutting a new Melody release.

## 1. Prepare versioned files

Pick the next version, then update:

- `CHANGELOG.md`: move relevant `Unreleased` entries under the new version/date.
- `PKGBUILD`: set `pkgver` to the new version without the leading `v`.
- `android/app/build.gradle.kts`: increment `versionCode` and set `versionName`.

Run the normal checks before tagging:

```sh
go test ./...
./build
cd android
./gradlew :app:assembleRelease
cd ..
```

The Android release build signs from `~/.local/android/release-keys/melody.properties` by default, or from the file pointed to by `MELODY_ANDROID_SIGNING_PROPERTIES`.

Expected properties:

```properties
MELODY_ANDROID_STORE_FILE=/home/carnager/.local/android/release-keys/<keystore-file>
MELODY_ANDROID_STORE_PASSWORD=<password>
MELODY_ANDROID_KEY_ALIAS=<alias>
MELODY_ANDROID_KEY_PASSWORD=<password>
```

Do not commit the keystore or properties file.

## 2. Build release artifacts

Create a clean release directory:

```sh
rm -rf release
mkdir -p release/amd64 release/arm64
```

Build Linux amd64:

```sh
GOOS=linux GOARCH=amd64 CGO_ENABLED=1 ./build
cp bin/melodyd bin/melody-agent bin/melody-tui bin/melody-cli bin/melody-musiclist bin/melody-lrcmatch release/amd64/
```

Build Linux arm64. The arm build needs the aarch64 ALSA headers:

```sh
GOOS=linux GOARCH=arm64 CGO_ENABLED=1 \
  CC=aarch64-linux-gnu-gcc \
  CGO_CFLAGS="-I/usr/aarch64-linux-gnu/include" \
  CGO_LDFLAGS="-L/usr/aarch64-linux-gnu/lib" \
  ./build
cp bin/melodyd bin/melody-agent bin/melody-tui bin/melody-cli bin/melody-musiclist bin/melody-lrcmatch release/arm64/
```

Copy the signed APK:

```sh
cp android/app/build/outputs/apk/release/app-release.apk release/melody-<version>-android.apk
```

Package the binary assets:

```sh
tar -C release/amd64 -czf release/melody-<version>-linux-amd64.tar.gz .
tar -C release/arm64 -czf release/melody-<version>-linux-arm64.tar.gz .
sha256sum release/melody-<version>-linux-amd64.tar.gz \
          release/melody-<version>-linux-arm64.tar.gz \
          release/melody-<version>-android.apk \
  > release/SHA256SUMS
```

## 3. Commit, tag, and push

```sh
git status --short
git add CHANGELOG.md PKGBUILD android/app/build.gradle.kts
git commit -m "Release <version>"
git tag -a v<version> -m "Release <version>"
git push
git push origin v<version>
```

If there are functional fixes before the release commit, commit those separately first.

## 4. Create or update the GitHub release

```sh
gh release create v<version> \
  --title "Melody <version>" \
  --notes-file <release-notes-file> \
  release/melody-<version>-linux-amd64.tar.gz \
  release/melody-<version>-linux-arm64.tar.gz \
  release/melody-<version>-android.apk \
  release/SHA256SUMS
```

If the release already exists:

```sh
gh release upload v<version> \
  release/melody-<version>-linux-amd64.tar.gz \
  release/melody-<version>-linux-arm64.tar.gz \
  release/melody-<version>-android.apk \
  release/SHA256SUMS \
  --clobber
```

Upload individual binaries too if needed by the user:

```sh
gh release upload v<version> release/amd64/* release/arm64/* --clobber
```

## 5. Smoke checks

Check the release binary contents before upload:

```sh
tar -tzf release/melody-<version>-linux-amd64.tar.gz
tar -tzf release/melody-<version>-linux-arm64.tar.gz
```

Check Android package metadata:

```sh
aapt dump badging release/melody-<version>-android.apk | head
```

If installing on the test phone fails with `INSTALL_FAILED_UPDATE_INCOMPATIBLE`, the installed app was signed with another key. Use a debug build for same-key in-place testing, or uninstall the old package first if preserving app data is not required:

```sh
cd android
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

