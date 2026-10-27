$TOOLCHAIN = "C:/Users/Gianmarco/Projects/video-grab/ps4-store/tools/openorbis/OpenOrbis/PS4Toolchain"
$PROJECT = $PSScriptRoot
$CREATE_FSELF = "C:/tmp/create-fself-new.exe"
$DAEMON_DIR = "$PROJECT/daemon"

if (-not (Test-Path "$DAEMON_DIR/obj")) { New-Item -ItemType Directory -Path "$DAEMON_DIR/obj" | Out-Null }
if (-not (Test-Path "$PROJECT/bin")) { New-Item -ItemType Directory -Path "$PROJECT/bin" | Out-Null }

$CFLAGS = @(
    "--target=x86_64-pc-freebsd12-elf",
    "-fPIC",
    "-funwind-tables",
    "-fno-delete-null-pointer-checks",
    "-c",
    "-Wall",
    "-isysroot", $TOOLCHAIN,
    "-isystem", "$TOOLCHAIN/include",
    "-isystem", "$TOOLCHAIN/include/orbis/_types",
    "-I$DAEMON_DIR/include",
    "-D__PS4__",
    "-D__ORBIS__"
)

Write-Host "=== Compiling OneFamily Daemon Sources ==="

$srcFiles = @(
    "$DAEMON_DIR/src/daemon_syscalls.c",
    "$DAEMON_DIR/src/main.c",
    "$DAEMON_DIR/src/vpad.c",
    "$DAEMON_DIR/src/usb_driver.c",
    "$DAEMON_DIR/src/config.c",
    "$DAEMON_DIR/src/log.c",
    "$DAEMON_DIR/src/controller_detect.c",
    "$DAEMON_DIR/src/translator.c"
)

$objFiles = @()
foreach ($f in $srcFiles) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($f)
    $obj = "$DAEMON_DIR/obj/$name.o"
    $objFiles += $obj
    Write-Host "  Compiling $name.c -> daemon/obj/$name.o"
    & clang @CFLAGS -o $obj $f
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to compile $f"
        exit 1
    }
}
Write-Host "All daemon sources compiled OK"

Write-Host "=== Linking OneFamily Daemon ELF ==="
$LDFLAGS = @(
    "-m", "elf_x86_64",
    "-pie",
    "--script", "$TOOLCHAIN/link.x",
    "--eh-frame-hdr",
    "-L$TOOLCHAIN/lib"
)

$LIBS = @(
    "-lc",
    "-lkernel"
)

$ELF = "$PROJECT/bin/onefamily.elf"
$BIN = "$PROJECT/bin/onefamily.bin"

& ld.lld @LDFLAGS -o $ELF @objFiles @LIBS
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to link daemon ELF!"
    exit 1
}
Write-Host "Daemon ELF linked OK"

Write-Host "=== Preparing Payload Files ==="
if (Test-Path $CREATE_FSELF) {
    Copy-Item $ELF $BIN -Force
} else {
    Copy-Item $ELF $BIN -Force
}

Write-Host "=== Build Complete! Generated bin/onefamily.elf and bin/onefamily.bin ==="
