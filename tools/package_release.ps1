# package_release.ps1 - the portable release zip for GitHub. It copies an explicit list of files (the three
# binaries from build\, the docs and the licences), so settings, logs, saves, debug files and anything from the
# disc never end up in it, and writes SHA256SUMS.txt next to it.
# Usage: tools\package_release.ps1 -Version 0.2.0 -SourceUrl https://github.com/<owner>/<repo>
param(
    [Parameter(Mandatory)] [ValidatePattern('^\d+\.\d+\.\d+([-+][0-9A-Za-z.]+)?$')] [string]$Version,
    [Parameter(Mandatory)] [ValidatePattern('^https://\S+$')] [string]$SourceUrl  # GPL: where the source is
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$build = Join-Path $root "build"
$dist = Join-Path $root "dist"
$name = "TeamBuddies-$Version-win"
$stage = Join-Path $dist $name

# the binaries must exist and be newer than every runtime and launcher source
$bins = "TeamBuddies.exe", "TeamBuddiesLauncher.exe", "SDL3.dll" | ForEach-Object { Join-Path $build $_ }
foreach ($b in $bins) { if (-not (Test-Path $b)) { throw "missing $b - run build.bat first" } }
$newest = Get-ChildItem (Join-Path $root "runtime"), (Join-Path $root "launcher") -File -Recurse |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$oldest = Get-Item $bins[0], $bins[1] | Sort-Object LastWriteTime | Select-Object -First 1
if ($newest.LastWriteTime -gt $oldest.LastWriteTime) {
    throw "$($newest.Name) is newer than $($oldest.Name) - run build.bat runtime first"
}
if (-not (Select-String -Path (Join-Path $root "CHANGELOG.md") -Pattern "^## $([regex]::Escape($Version))\b" -Quiet)) {
    Write-Warning "CHANGELOG.md has no '## $Version' section (rename '## Unreleased', then build.bat launcher, so the launcher's patch notes show it)"
}

# stage
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage "licenses") | Out-Null
Copy-Item $bins $stage
Copy-Item (Join-Path $root "CHANGELOG.md"), (Join-Path $root "LICENSE"), (Join-Path $root "docs\PLAYING.md") $stage
Copy-Item (Join-Path $root "licenses\*") (Join-Path $stage "licenses")
$readme = (Get-Content (Join-Path $PSScriptRoot "release\README.txt") -Raw).Replace("{VERSION}", $Version).Replace("{SOURCE}", $SourceUrl)
[IO.File]::WriteAllText((Join-Path $stage "README.txt"), ($readme -replace "\r?\n", "`r`n"), (New-Object Text.UTF8Encoding $false))

# exactly these files, nothing else
$expected = @("TeamBuddies.exe", "TeamBuddiesLauncher.exe", "SDL3.dll", "CHANGELOG.md", "LICENSE", "PLAYING.md", "README.txt") +
    (Get-ChildItem (Join-Path $root "licenses") -File | ForEach-Object { "licenses\" + $_.Name })
$actual = Get-ChildItem $stage -File -Recurse | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
$diff = Compare-Object ($expected | Sort-Object) ($actual | Sort-Object)
if ($diff) { throw "unexpected package contents: $($diff | Out-String)" }

# zip into a temp file, then swap it in (a failed run never leaves a half-written zip); entry names use '/'
# as the format requires (Compress-Archive in Windows PowerShell writes '\')
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = Join-Path $dist "$name.zip"
$tmp = Join-Path $dist "$name.partial.zip"
if (Test-Path $tmp) { Remove-Item $tmp -Force }
$archive = [IO.Compression.ZipFile]::Open($tmp, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($rel in $actual | Sort-Object) {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $stage $rel), $rel.Replace("\", "/"),
            [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
Move-Item $tmp $zip -Force
Remove-Item $stage -Recurse -Force

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
$sums = Join-Path $dist "SHA256SUMS.txt"
[IO.File]::WriteAllText("$sums.tmp", "$hash  $name.zip`n", (New-Object Text.UTF8Encoding $false))
Move-Item "$sums.tmp" $sums -Force
"{0}  {1:N1} MB" -f $zip, ((Get-Item $zip).Length / 1MB)
"$sums  $hash"
