# Signing and provenance

What the files on [Releases](https://github.com/karimalsalah/guarana/releases) carry, how to check a download before you run it, and how the owner turns Authenticode signing on.

## What is signed, and when

Every published build gets the same: a `vX.Y.Z` or `vX.Y.Z-rcN` release, and the [`nightly`](https://github.com/karimalsalah/guarana/releases/tag/nightly) pre-release. Workflow artifacts (test builds of branches and pull requests) get neither signatures nor attestations.

| | Covers | Made by |
|---|---|---|
| `SHA256SUMS.txt` | Every file of the release | `release-binaries.yml`; `launcher.yml` adds the installer's line |
| Build provenance | Every file of the release, `SHA256SUMS.txt` included | [`actions/attest-build-provenance`](https://github.com/actions/attest-build-provenance), in the same two workflows |
| Authenticode signature | `guarana.exe` and `guarana-cli.exe` in both Windows zips, and `Guarana-Setup-<version>.exe` | [Azure Artifact Signing](https://learn.microsoft.com/azure/artifact-signing/) (formerly Trusted Signing), once the owner has [turned it on](#turn-signing-on) |

The order is fixed, so the checksums and the provenance describe the files as signed: the two programs are signed before they are zipped, the installer as soon as it is built, and only then are the zips, the installer and the rest listed in `SHA256SUMS.txt`, attested and uploaded.

- **Build provenance** is a [SLSA](https://slsa.dev/spec/v1.0/provenance) statement signed with a short-lived [Sigstore](https://www.sigstore.dev/) certificate issued to the workflow run: this file, with this SHA-256, came from this run of this workflow of karimalsalah/guarana on a GitHub-hosted runner, with the run's commit and ref. The repository is public, so each attestation is also written to Sigstore's public transparency log. The repository lists them under [Attestations](https://github.com/karimalsalah/guarana/attestations).
- **Releases up to v20.2.0** have `SHA256SUMS.txt`, but no attestations and no signatures.
- **Not covered**: the Linux files have no signature of their own, only the checksums and the provenance. The macOS app is signed ad hoc and not notarized. The libraries next to `guarana.exe` (Qt and the others) are not signed by this project. The programs the installer puts on your PC, the launcher and its uninstaller, are not signed on their own: only the installer is.

## Check a download

Check the file exactly as you downloaded it, the zip or the installer: the attestations and the checksums are for the downloaded files, not for the programs inside a zip.

**Build provenance**, with the [GitHub CLI](https://cli.github.com/) signed in (`gh auth login`):

```sh
gh attestation verify guarana-X.Y.Z-windows-x86_64-pgo.zip --repo karimalsalah/guarana
```

It passes only when a workflow of this repository attested a file with exactly this SHA-256, and prints which workflow that was. Any change to the file, one byte or a different build, fails it.

**Checksums**: put `SHA256SUMS.txt` from the same release next to the file.

```sh
sha256sum -c --ignore-missing SHA256SUMS.txt        # Linux: "OK" for each file you have
shasum -a 256 -c --ignore-missing SHA256SUMS.txt    # macOS
```

```powershell
# PowerShell: True when the file matches its line in SHA256SUMS.txt
$file = 'Guarana-Setup-X.Y.Z.exe'
$want = (Select-String -Path SHA256SUMS.txt -SimpleMatch "  $file" | Select-Object -First 1).Line.Split(' ')[0]
(Get-FileHash -Algorithm SHA256 $file).Hash -eq $want
```

`SHA256SUMS.txt` is attested as well (`gh attestation verify SHA256SUMS.txt --repo karimalsalah/guarana`), so a checksum that matches a verified `SHA256SUMS.txt` is as good as the file's own attestation.

**Authenticode** (Windows), on the installer and on the two programs from a zip:

```powershell
Get-AuthenticodeSignature .\Guarana-Setup-X.Y.Z.exe, .\guarana.exe, .\guarana-cli.exe |
  Format-List Path, Status, StatusMessage, SignerCertificate, TimeStamperCertificate
```

`Status` must be `Valid`, and `SignerCertificate` names the publisher that Microsoft validated for this project, the same on every signed release. Until the owner turns signing on, and for every release made before that, `Status` is `NotSigned`: check those files with the attestation and the checksums. The signing certificates are short-lived; the timestamp in each signature (`TimeStamperCertificate`) keeps the signature valid after its certificate expires. Explorer shows the same under the file's Properties > Digital Signatures.

## Turn signing on

For the owner. Signing is wired into `release-binaries.yml` (each Windows build, before its upload) and `launcher.yml` (the installer of a release) and waits for six repository secrets. With none set, each of those builds logs a notice and skips signing; with only some set, it logs a warning naming the missing ones and skips signing. Microsoft's [quickstart](https://learn.microsoft.com/azure/artifact-signing/quickstart) has the details and screenshots for steps 1 to 3, and its prerequisites say where Public Trust certificates are offered.

1. **Account.** In the [Azure portal](https://portal.azure.com/), in a subscription that can be billed, register the `Microsoft.CodeSigning` resource provider (Subscriptions > the subscription > Resource providers). Then create an **Artifact Signing account** in a region that offers it. Note the account name and the region's endpoint URI, for example `https://eus.codesigning.azure.net` for East US.
2. **Identity.** On the account, give yourself the **Artifact Signing Identity Verifier** role (Access control (IAM) > Add role assignment). Then under Identity validations create a **Public** validation, as an organization or as an individual. The name it validates is the publisher Windows shows. Microsoft's review takes days to weeks.
3. **Certificate profile.** Once the validation is Completed, create a **Public Trust** certificate profile on it and note its name.
4. **App registration.** In Microsoft Entra ID > App registrations, register an app for the workflows, for example `guarana-release-signing`. Note its Application (client) ID and Directory (tenant) ID. Under Certificates & secrets, create a client secret and copy its Value (not its Secret ID).
5. **Role.** On the certificate profile, or on the whole account: Access control (IAM) > Add role assignment > **Artifact Signing Certificate Profile Signer**, for the app.
6. **Secrets.** In the repository, Settings > Secrets and variables > Actions > New repository secret, one per row:

   | Secret | Value |
   |---|---|
   | `AZURE_TENANT_ID` | The Directory (tenant) ID, step 4 |
   | `AZURE_CLIENT_ID` | The Application (client) ID, step 4 |
   | `AZURE_CLIENT_SECRET` | The client secret's Value, step 4 |
   | `ARTIFACT_SIGNING_ENDPOINT` | The endpoint URI, step 1 |
   | `ARTIFACT_SIGNING_ACCOUNT` | The account name, step 1 |
   | `ARTIFACT_SIGNING_PROFILE` | The certificate profile name, step 3 |

   From a terminal, `gh secret set AZURE_TENANT_ID --repo karimalsalah/guarana` asks for the value; repeat it for each name.
7. **Check.** Run Actions > nightly > Run workflow with "force". In the run, each Windows binary job must show "Sign the Windows programs" done and "Check the signatures" printing `Valid` for both programs, and the installer's "Build the installer" job the same with "Sign the installer" and "Check the signature". Then download the new nightly and check it as above.

From then on every release and nightly is signed. A release published before then stays unsigned unless you rebuild its tag (Actions > release-binaries > Run workflow, [LANES.md](LANES.md)), which replaces its files.

- The secrets reach only the signing step, which runs [`Azure/artifact-signing-action`](https://github.com/Azure/artifact-signing-action) (it was `azure/trusted-signing-action` before Microsoft renamed the service) pinned to a commit. Only builds that get published are signed: pushed `v*` tags, runs by hand that publish to a release, and the nightly. Whoever can push `v*` tags or run these workflows can therefore have files signed, so keep both to maintainers ([LANES.md](LANES.md), "Repository settings these need").
- The client secret expires on the date chosen in step 4. Before then, create a new one and update `AZURE_CLIENT_SECRET`: an expired secret fails the signing step, and with it the release.
- To turn signing off, delete the six secrets. Deleting only some turns it off as well, with a warning in each build.

## Alternative: SignPath Foundation

[SignPath Foundation](https://signpath.org/) signs open-source projects for free. The certificate is the foundation's, which vouches that the files were built from the project's repository, so Windows shows SignPath Foundation as the publisher rather than the owner. A project [applies](https://signpath.org/apply) and has to meet its [terms](https://signpath.org/terms). In the workflows, the Azure step would be replaced: the Windows job uploads the unsigned programs as a workflow artifact, [`SignPath/github-action-submit-signing-request`](https://github.com/SignPath/github-action-submit-signing-request) submits it and returns the signed files, and the zip, the checksums and the provenance follow as they do now.
