# Installer builds and public verification

## Run a verification

Open **Actions → Build setup → Run workflow**. Enter an existing release tag,
such as `v1.1`, in `release_tag`. The workflow resolves that tag to a full commit
ID, rebuilds its installer twice on separate clean Windows runners, downloads
the release's exact `Bleach.Soul.Resurreccion.-.Setup.exe` asset and compares all
three complete files using SHA-256. It never runs the downloaded installer.

A green run with `verified` means all three hashes match. A red run retains
`verification-evidence`, with hashes, source and recipe commits, asset identity,
file differences and a human-readable summary. It does not issue a success
attestation. The `build-a` and `build-b` artifacts contain build logs, installers
and manifests even when a later comparison fails. Artifacts are retained for
90 days; download them for longer retention. GitHub run logs are public.

Leave `release_tag` blank to test the selected branch/commit without comparing
a release. Its result is explicitly `reproducible-build`, not `verified`.
Manual runs never create or replace a release.

## What changed from v1.1

The original workflow's RPCS3 download did not match its expected hash. The local
folder was named `rpcs3-v0.0.30-16061-85f4c38b_win64`, but its update history records
an automatic update to **0.0.39-18794-b30a44c1**. The latter's official archive
contains the same `rpcs3.exe` as v1.1:
`4a878bd38d19931d8e6c046be1552bc9e818503fc8ad57deb7229a83a4718105`.

v1.1 was created with the machine's legacy C# compiler, random assembly IDs,
build timestamps and filesystem timestamps inside its ZIP. Its local package
also differs from the clean public packaging recipe. Its published SHA-256 is
`8fe76a5e24d59234d563374fb700762b10d1d5eb569cf0bd8ec3a76f82d7ca59`.
**Do not expect the new recipe to reproduce v1.1 byte for byte.** Comparing it
must fail honestly. This workflow cannot retroactively prove that old build.

Future releases must be built with this deterministic recipe. The legacy
`montar-instalador.py` is a development tool that uses private local game inputs;
it is not the release build entry point.

## Fixed inputs and deterministic packaging

`launcher/build-inputs.json` fixes download URLs and SHA-256 for the full archives:
Python 3.11.9 (including zlib 1.3.1), Roslyn 4.8.0, .NET Framework 4.8 reference
assemblies 1.0.3, 7-Zip reduced extractor 24.09, RPCS3 0.0.39-18794 and the FFmpeg
essentials build dated 2026-09-02. All archives are verified before execution or
extraction. A changed archive causes a failure, not an automatic lock update.

Sources are exported with `git archive` from an exact commit, excluding local
files and avoiding checkout line-ending conversions. The compiler uses explicit
reference assemblies, `/noconfig`, `/nostdlib+`, `/deterministic+`, no debug files
and mapped paths. ZIP members have sorted names, fixed timestamps and permissions,
and fixed compression settings. No game dump or firmware is required or uploaded.
The produced launcher/setup targets .NET Framework 4.8 (included in current
Windows 10/11 installations; older installations may need the Microsoft runtime).

The hosted `windows-2022` image still receives updates. Its version, OS, compiler,
references, Python and zlib are recorded in each build manifest. Two equal builds
demonstrate reproducibility for those inputs/environments; they do not prove
universal reproducibility across every future runner image. Both copies must pass.
RPCS3 and FFmpeg are pinned official prebuilt inputs; this workflow does not
recompile or audit their entire upstream source trees.

## Publish a new release

After reviewing and merging these changes, push a new `v*` tag at the desired
commit. Both clean builds must match before an attestation is issued or a release
is created. The release includes the exact attested installer, `SHA256SUMS.txt`,
the verification report, manifests and signed attestation bundle. An existing
release is never overwritten. Run the workflow again with that release tag to
independently rebuild and compare the actual published download.

Verify a downloaded installer using GitHub CLI:

```powershell
gh attestation verify "Bleach.Soul.Resurreccion.-.Setup.exe" --repo xPXavierx/bleach-soul-resurreccion-recomp --signer-workflow xPXavierx/bleach-soul-resurreccion-recomp/.github/workflows/release.yml
```

Review the attestation's source commit, workflow ref and invocation URL. An
attestation from a branch test is not a claim that the file is an official release.
The signed `verification.json` binds the comparison result to the immutable source,
recipe and downloaded asset identities. When checking an old release, the build
recipe comes from the selected workflow commit and is recorded separately from
the release's source commit; it is not silently treated as the historical recipe.

Neither equal hashes nor provenance attestations certify absence of malware.

## Rebuild locally (PowerShell 7, Windows)

From a clean checkout of this implementation, use fresh directories:

```powershell
$commit = git rev-parse HEAD
git archive HEAD -o source.tar
New-Item -ItemType Directory snapshot | Out-Null
tar -xf source.tar -C snapshot
./launcher/prepare-build.ps1 -Destination build-tools
./build-tools/python/python.exe launcher/montar-instalador-ci.py --source snapshot --source-commit $commit --recipe-commit $commit --tools build-tools --work build-first --saida dist-first
./build-tools/python/python.exe launcher/montar-instalador-ci.py --source snapshot --source-commit $commit --recipe-commit $commit --tools build-tools --work build-second --saida dist-second
Get-FileHash 'dist-first/Bleach Soul Resurrección - Setup.exe', 'dist-second/Bleach Soul Resurrección - Setup.exe'
```

Use the same recipe and source commit as the workflow. Local uncommitted edits
are intentionally not included in the `git archive` snapshot.

References: [GitHub artifact attestations](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations),
[C# deterministic compilation](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/compiler-options/code-generation#deterministic).
