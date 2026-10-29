$TOOLCHAIN = "C:/Users/Gianmarco/Projects/video-grab/ps4-store/tools/openorbis/OpenOrbis/PS4Toolchain"
$PROJECT = $PSScriptRoot
$APP_DIR = "$PROJECT/app"
$PYTHON = "C:/Users/Gianmarco/AppData/Local/Python/bin/python.exe"
$CREATE_FSELF = "C:/tmp/create-fself-new.exe"
$PKGTOOL = "$TOOLCHAIN/bin/windows/PkgTool.Core.dll"

$TITLE_ID = "XBOX00001"
$CONTENT_ID = "UP9000-XBOX00001_00-XBOXCONFIGURATOR"

if (-not (Test-Path "$APP_DIR/obj")) { New-Item -ItemType Directory -Path "$APP_DIR/obj" | Out-Null }
if (-not (Test-Path "$APP_DIR/bin")) { New-Item -ItemType Directory -Path "$APP_DIR/bin" | Out-Null }
if (-not (Test-Path "$PROJECT/bin")) { New-Item -ItemType Directory -Path "$PROJECT/bin" | Out-Null }
if (-not (Test-Path "$APP_DIR/assets/payload")) { New-Item -ItemType Directory -Path "$APP_DIR/assets/payload" | Out-Null }

Write-Host "=== Verifying / Building OneFamily Daemon Payload ==="
if (-not (Test-Path "$PROJECT/bin/onefamily.bin")) {
    Write-Host "onefamily.bin not found, running build_daemon.ps1..."
    & powershell -File "$PROJECT/build_daemon.ps1"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to build daemon payload!"
        exit 1
    }
}
Copy-Item "$PROJECT/bin/onefamily.bin" "$APP_DIR/assets/payload/onefamily.bin" -Force
Write-Host "Copied onefamily.bin to app/assets/payload/onefamily.bin"

Write-Host "=== Generating SFO with make_sfo_app.py ==="
& "$PYTHON" "$APP_DIR/make_sfo_app.py"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to generate param.sfo"
    exit 1
}

$CXXFLAGS = @(
    "--target=x86_64-pc-freebsd12-elf",
    "-fPIC",
    "-funwind-tables",
    "-fno-exceptions",
    "-fno-rtti",
    "-c",
    "-Wall",
    "-DGRAPHICS_USES_FONT",
    "-isysroot", $TOOLCHAIN,
    "-isystem", "$TOOLCHAIN/include",
    "-isystem", "$TOOLCHAIN/include/c++/v1",
    "-I$APP_DIR/src",
    "-I$APP_DIR/src/common"
)

Write-Host "=== Compiling App Sources (no-exceptions, no-rtti) ==="

$srcFiles = @(
    "$APP_DIR/src/main.cpp",
    "$APP_DIR/src/config_manager.cpp",
    "$APP_DIR/src/installer.cpp",
    "$APP_DIR/src/hid_profile.cpp",
    "$APP_DIR/src/ui.cpp",
    "$APP_DIR/src/common/graphics.cpp",
    "$APP_DIR/src/common/controller.cpp",
    "$APP_DIR/src/common/png.cpp"
)

$objFiles = @()
foreach ($src in $srcFiles) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = "$APP_DIR/obj/$name.o"
    $objFiles += $obj
    Write-Host "  Compiling $name.cpp -> $name.o"
    & clang++ @CXXFLAGS -o $obj $src
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to compile $src"
        exit 1
    }
}

Write-Host "=== Linking ELF ==="
$LDFLAGS = @(
    "-m", "elf_x86_64",
    "-pie",
    "--script", "$TOOLCHAIN/link.x",
    "--eh-frame-hdr",
    "-L$TOOLCHAIN/lib",
    "$TOOLCHAIN/lib/crt1.o"
)

$LIBS = @(
    "-lc",
    "-lkernel",
    "-lc++",
    "-lSceVideoOut",
    "-lSceSysmodule",
    "-lSceFreeType",
    "-lScePad",
    "-lSceUserService",
    "-lSceSystemService"
)

$ELF = "$APP_DIR/bin/app.elf"
$OELF = "$APP_DIR/bin/app.oelf"
$EBOOT = "$APP_DIR/eboot.bin"

& ld.lld @LDFLAGS @objFiles -o $ELF @LIBS
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to link ELF"
    exit 1
}

Write-Host "=== Creating eboot.bin via create-fself ==="
& $CREATE_FSELF -in "$ELF" -out "$OELF" -eboot "$EBOOT" -paid 0x3800000000000011
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to create eboot.bin"
    exit 1
}
Write-Host "eboot.bin created successfully!"

Write-Host "=== Building PKG via PkgTool.Core ==="
Push-Location "$APP_DIR"
& dotnet --roll-forward LatestMajor "$PKGTOOL" pkg_build pkg.gp4 .
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to build PKG"
    Pop-Location
    exit 1
}

Write-Host "=== Validating PKG via PkgTool.Core pkg_validate ==="
& dotnet --roll-forward LatestMajor "$PKGTOOL" pkg_validate "$CONTENT_ID.pkg"
if ($LASTEXITCODE -ne 0) {
    Write-Error "PKG validation failed!"
    Pop-Location
    exit 1
}
Pop-Location

Copy-Item "$APP_DIR/$CONTENT_ID.pkg" "$PROJECT/bin/onefamily_companion.pkg" -Force
Write-Host "=== Build Complete & Validated! Package ready at bin/onefamily_companion.pkg ==="
