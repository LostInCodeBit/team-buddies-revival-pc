# sign_release.ps1 - signs dist\SHA256SUMS.txt with the release key (tools/release_key.ps1) into
# dist\SHA256SUMS.txt.sig: base64 of the 64-byte ECDSA P-256 signature (r then s) of the file's bytes. The
# launcher installs a release only with this signature, so upload the .sig with the other assets.
# It then checks the signature with the public key compiled into the launcher (launcher/release_key.inc): a key
# that does not match it would make every update fail. Run it after the last package script
# (package_release.ps1 runs it at its end; package_linux.sh deletes a signature it makes stale).
# Usage: tools\sign_release.ps1 [-Dist <folder>] [-KeyFile <path>] [-Test]   (-Test: any key, no release_key.inc check)
param(
    [string]$Dist = (Join-Path (Split-Path $PSScriptRoot) "dist"),
    [string]$KeyFile = $(if ($env:TB_RELEASE_KEY) { $env:TB_RELEASE_KEY } else { Join-Path $env:USERPROFILE ".team-buddies\release-signing.key" }),
    [switch]$Test
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Core
$sums = Join-Path $Dist "SHA256SUMS.txt"
if (-not (Test-Path $sums)) { throw "missing $sums" }
if (-not (Test-Path $KeyFile)) { throw "no signing key at $KeyFile (tools\release_key.ps1 makes one)" }
$fmt = [System.Security.Cryptography.CngKeyBlobFormat]
$key = [System.Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($KeyFile), $fmt::EccPrivateBlob)
$data = [IO.File]::ReadAllBytes($sums)
$signer = New-Object System.Security.Cryptography.ECDsaCng($key)
$signer.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
$sig = $signer.SignData($data)
if ($sig.Length -ne 64) { throw "unexpected signature size $($sig.Length)" }

# check it the way the launcher will
if ($Test) {
    $pub = $key.Export($fmt::EccPublicBlob)
} else {
    $inc = Join-Path (Split-Path $PSScriptRoot) "launcher\release_key.inc"
    $pub = [byte[]]([regex]::Matches((Get-Content $inc -Raw), "0x([0-9A-Fa-f]{2})") | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) })
    if ($pub.Length -ne 72) { throw "launcher\release_key.inc does not hold a 72-byte public key" }
}
$checker = New-Object System.Security.Cryptography.ECDsaCng([System.Security.Cryptography.CngKey]::Import($pub, $fmt::EccPublicBlob))
$checker.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
if (-not $checker.VerifyData($data, $sig)) { throw "the key $KeyFile is not the one in launcher\release_key.inc: launchers would refuse this release" }

$out = Join-Path $Dist "SHA256SUMS.txt.sig"
[IO.File]::WriteAllText("$out.tmp", [Convert]::ToBase64String($sig) + "`n", (New-Object Text.UTF8Encoding $false))
Move-Item "$out.tmp" $out -Force
"$out  (signed $($data.Length) bytes of SHA256SUMS.txt)"
