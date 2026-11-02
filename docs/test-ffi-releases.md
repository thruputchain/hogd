# FFI releases

FFI versions are independent of full hogd releases and packaged bundles. Update
`TEST_FFI_VERSION_MAJOR`, `TEST_FFI_VERSION_MINOR`, and `TEST_FFI_VERSION_PATCH`
in `hogd/test_ffi/test_bindings.h`, land the change, and push the matching
`test-ffi-vX.Y.Z` tag through the normal release process.

`Release test_ffi Library` builds Linux x86_64, Linux ARM64, and macOS ARM64
archives and publishes a GitHub release. Its tag check requires the header
version to match. The full hogd `Release` workflow responds to `v*` tags and is
not involved.

After that tagged build succeeds, `Publish test_ffi to public S3` starts
automatically. It runs publisher code from the default branch, downloads the
six authenticated GitHub release assets, and verifies all checksums, archive
layouts, binary formats, architectures, and shared-library types before
assuming the dedicated AWS role. Archive inspection is streamed, with a 512 MiB
library limit, 1 MiB per binding/header, and a total decompressed limit of
514 MiB plus 64 KiB for tar overhead. The total bound also covers extended
metadata and trailing data. It publishes unchanged files under:

```text
test-ffi/test-ffi-vX.Y.Z/libtest_ffi-linux-x86_64.tar.gz
test-ffi/test-ffi-vX.Y.Z/libtest_ffi-linux-x86_64.tar.gz.sha256
test-ffi/test-ffi-vX.Y.Z/libtest_ffi-linux-arm64.tar.gz
test-ffi/test-ffi-vX.Y.Z/libtest_ffi-linux-arm64.tar.gz.sha256
test-ffi/test-ffi-vX.Y.Z/libtest_ffi-macos-arm64.tar.gz
test-ffi/test-ffi-vX.Y.Z/libtest_ffi-macos-arm64.tar.gz.sha256
```

Each object is created conditionally and downloaded anonymously over HTTPS to
verify its exact size and SHA-256. Retries accept an existing object only if its
bytes match. No packaging request or bundle release is needed.

The macOS dylib retains its upstream signing status. The publisher does not add
Developer ID signing or notarization. Header checks establish target identity,
not runtime compatibility or loadability on every supported host.

## One-time activation

Before the first automatic publication:

1. Create the `ffi-public-artifacts` Environment in `thruputchain/hogd` and
   restrict its deployment branches to `main`. The follow-on workflow executes
   on main even though the preceding build runs on an FFI tag. Protect main and
   release tags through the repository's normal review and release controls.
2. Apply the reviewed `ops/tofu/envs/release-artifacts/us-west-2` change that
   adds public `test-ffi/` access and its dedicated publisher role. It trusts
   only `repo:thruputchain/hogd:environment:ffi-public-artifacts` and cannot
   write devnode artifacts.
3. Configure the Environment from the ops outputs:

   | Setting | Type | Value |
   |---|---|---|
   | `AWS_FFI_ARTIFACT_ROLE_ARN` | Secret | `ffi_publisher_role_arn` output |
   | `PUBLIC_ARTIFACT_BUCKET` | Variable | `bucket_name` output |
   | `PUBLIC_ARTIFACT_BASE_URL` | Variable | `base_url` output |
   | `PUBLIC_ARTIFACT_REGION` | Variable | `us-west-2` |

4. Land the publisher on default-branch main before cutting a new FFI tag.
   Run the manual dry run against an existing version, inspect its artifact,
   then publish that version to verify OIDC and anonymous downloads.

## Retry or publish an existing FFI release

Dispatch the publisher on main with an existing numeric FFI tag. The default is
`dry_run=true`, which downloads and validates the release and uploads a candidate
workflow artifact without assuming the AWS role or writing to S3:

```bash
gh workflow run publish_test_ffi.yml --repo thruputchain/hogd --ref main \
  -f tag=test-ffi-vX.Y.Z -f dry_run=true
```

Use `dry_run=false` to publish after verification or retry a partial publication.
The retry downloads the original GitHub release assets; it does not rebuild
them. Drafts, prereleases, malformed tags, missing platforms, invalid libraries,
and checksum mismatches are rejected. Follow the publisher run separately from
the build run: a successful GitHub release is not proof of S3 delivery.

For local validation of already-downloaded assets:

```bash
python3 scripts/publish_test_ffi.py --tag test-ffi-vX.Y.Z --assets-dir /path/to/assets
python3 -m unittest discover -s scripts -p test_publish_test_ffi.py
```

## SDK migration and old mirror

The SDK downloads test_ffi from public S3. The old `sync_release_mirror.yml`,
which copied release assets to `groundhog.dev` from a self-hosted runner, is
removed; the files it already mirrored stay there for older SDK versions.
`release_test_ffi.yml` remains the FFI build and GitHub release producer, with
every platform on Namespace runners.
