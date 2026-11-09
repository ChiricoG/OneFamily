param (
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Version,

    [Parameter(Position = 1)]
    [string]$Title
)

$PROJECT = $PSScriptRoot
$PYTHON = "C:/Users/Gianmarco/AppData/Local/Python/bin/python.exe"
$PROPS_PATH = "$PROJECT/version.properties"
$README_PATH = "$PROJECT/README.md"

# Clean leading 'v' or 'V'
$cleanVer = $Version.Trim()
if ($cleanVer.StartsWith("v", [System.StringComparison]::OrdinalIgnoreCase)) {
    $cleanVer = $cleanVer.Substring(1)
}

# Match Major.Minor.Patch[-Prerelease]
if ($cleanVer -notmatch '^(\d+)\.(\d+)\.(\d+)(?:-(.+))?$') {
    Write-Error "Invalid version format '$Version'. Expected format: X.Y.Z or X.Y.Z-prerelease (e.g. 1.2.2 or 1.2.2-beta17)"
    exit 1
}

$major = [int]$Matches[1]
$minor = [int]$Matches[2]
$patch = [int]$Matches[3]
$prerelease = if ($Matches[4]) { $Matches[4].Trim() } else { "" }

# Read existing title if none provided
$currentTitle = "Multi-Controller Engine"
if (Test-Path $PROPS_PATH) {
    Get-Content $PROPS_PATH | ForEach-Object {
        $line = $_.Trim()
        if ($line.StartsWith("VERSION_TITLE=")) {
            $currentTitle = $line.Substring("VERSION_TITLE=".Length).Trim()
        }
    }
}

if (-not [string]::IsNullOrWhiteSpace($Title)) {
    $currentTitle = $Title.Trim()
}

# 1. Update version.properties
$propsContent = @"
# OneFamily Project Version Configuration
# Single Source of Truth for Daemon, Companion App, SFO, and Updater

VERSION_MAJOR=$major
VERSION_MINOR=$minor
VERSION_PATCH=$patch

# Suffix for pre-release (e.g. beta17, rc1). Leave empty for official stable release.
VERSION_PRERELEASE=$prerelease

# Human-readable title description displayed in Companion App UI
VERSION_TITLE=$currentTitle
"@

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($PROPS_PATH, $propsContent, $utf8NoBom)

# 2. Run generate_version.py to synchronize include/onefamily_version.h
& "$PYTHON" "$PROJECT/scripts/generate_version.py"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to generate version header!"
    exit 1
}

# 3. Regenerate param.sfo with make_sfo_app.py
& "$PYTHON" "$PROJECT/app/make_sfo_app.py"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to update param.sfo!"
    exit 1
}

# 4. Synchronize README.md shields.io badge
if (Test-Path $README_PATH) {
    $rawVer = if ($prerelease) { "$major.$minor.$patch-$prerelease" } else { "$major.$minor.$patch" }
    $badgeVer = if ($prerelease) { "v$major.$minor.$patch--$prerelease" } else { "v$major.$minor.$patch" }
    $readmeContent = Get-Content -Raw -Encoding UTF8 $README_PATH
    $newReadme = [System.Text.RegularExpressions.Regex]::Replace(
        $readmeContent,
        'https://img\.shields\.io/badge/Version-[^"]+',
        "https://img.shields.io/badge/Version-$badgeVer-blueviolet?style=for-the-badge"
    )
    if ($newReadme -ne $readmeContent) {
        [System.IO.File]::WriteAllText($README_PATH, $newReadme, $utf8NoBom)
        Write-Host "[README] Synchronized shields.io version badge -> $badgeVer"
    }
}

$rawVer = if ($prerelease) { "$major.$minor.$patch-$prerelease" } else { "$major.$minor.$patch" }
$fullVer = "v$rawVer ($currentTitle)"

Write-Host ""
Write-Host "=== OneFamily Version Set Successfully ==="
Write-Host "  Raw Version   : $rawVer"
Write-Host "  Version Tag   : v$rawVer"
Write-Host "  Display Title : $fullVer"
Write-Host "  Properties    : version.properties"
Write-Host "  C/C++ Header  : include/onefamily_version.h"
Write-Host "  App SFO       : app/sce_sys/param.sfo"
Write-Host "=========================================="
