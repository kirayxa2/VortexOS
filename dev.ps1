# VortexOS - Build, Watch & Run
# Запуск: powershell -ExecutionPolicy Bypass -File D:\VOS\dev.ps1
# Параметры: .\dev.ps1 [build|run|watch|clean]

param(
    [string]$Action = "build"
)

$ProjectDir = "D:\VOS"
$Make       = "C:\msys64\usr\bin\make.exe"
$QemuLog    = "$ProjectDir\build\qemu.log"

Set-Location $ProjectDir

function Write-Header($text) {
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor Cyan
}

function Write-Step($n, $total, $text) {
    Write-Host "[$n/$total] " -ForegroundColor Yellow -NoNewline
    Write-Host $text -ForegroundColor White
}

function Write-OK($text) {
    Write-Host "      [OK] $text" -ForegroundColor Green
}

function Write-Fail($text) {
    Write-Host "      [FAIL] $text" -ForegroundColor Red
}

function Invoke-Make($target) {
    $output = & $Make $target 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        return $false
    }
    return $true
}

function Build-All {
    Write-Header "VortexOS Build"

    Write-Step 1 4 "Cleaning..."
    & $Make clean | Out-Null
    Write-OK "Clean done"

    Write-Step 2 4 "Building kernel..."
    $result = & $Make 2>&1
    if ($LASTEXITCODE -ne 0) {
        $result | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        Write-Fail "Kernel build failed!"
        return $false
    }
    Write-OK "Kernel built"

    Write-Step 3 4 "Building userspace..."
    $result = & $Make userspace 2>&1
    if ($LASTEXITCODE -ne 0) {
        $result | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        Write-Fail "Userspace build failed!"
        return $false
    }
    Write-OK "Userspace built"

    Write-Step 4 4 "Creating disk image..."
    $result = & $Make "disk-with-apps" 2>&1
    if ($LASTEXITCODE -ne 0) {
        $result | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        Write-Fail "Disk creation failed!"
        return $false
    }
    Write-OK "Disk image ready"

    Write-Host ""
    Write-Host "  Build successful!" -ForegroundColor Green
    return $true
}

function Start-QEMU {
    Write-Header "Launching VortexOS in QEMU"
    Write-Host "  Press Ctrl+C or close QEMU to stop" -ForegroundColor Gray
    Write-Host ""
    & $Make run
}

function Start-Watch {
    Write-Header "Watch Mode - Auto-rebuild on file changes"
    Write-Host "  Watching: kernel\**\*.c, kernel\**\*.asm, userspace\*.c" -ForegroundColor Gray
    Write-Host "  Press Ctrl+C to stop" -ForegroundColor Gray
    Write-Host ""

    $watchDirs = @(
        "$ProjectDir\kernel",
        "$ProjectDir\userspace"
    )

    $watchers = @()
    $queue = [System.Collections.Concurrent.ConcurrentQueue[string]]::new()

    foreach ($dir in $watchDirs) {
        $w = New-Object System.IO.FileSystemWatcher
        $w.Path = $dir
        $w.Filter = "*.*"
        $w.IncludeSubdirectories = $true
        $w.NotifyFilter = [System.IO.NotifyFilters]::LastWrite

        $action = {
            $path = $Event.SourceEventArgs.FullPath
            if ($path -match '\.(c|h|asm|ld)$') {
                $global:queue.Enqueue($path)
            }
        }

        Register-ObjectEvent $w Changed -Action $action | Out-Null
        $w.EnableRaisingEvents = $true
        $watchers += $w
    }

    $lastBuild = [DateTime]::MinValue

    try {
        while ($true) {
            $path = $null
            if ($queue.TryDequeue([ref]$path)) {
                # Debounce: ждём 500мс после последнего изменения
                Start-Sleep -Milliseconds 500
                $now = [DateTime]::Now
                if (($now - $lastBuild).TotalSeconds -gt 1) {
                    $lastBuild = $now
                    $filename = Split-Path $path -Leaf
                    Write-Host ""
                    Write-Host "  Changed: $filename" -ForegroundColor Yellow
                    Write-Host "  $(Get-Date -Format 'HH:mm:ss') - Rebuilding..." -ForegroundColor Gray

                    $ok = Build-All
                    if ($ok) {
                        Write-Host ""
                        Write-Host "  Ready. Waiting for changes..." -ForegroundColor Cyan
                    }
                }
            }
            Start-Sleep -Milliseconds 200
        }
    }
    finally {
        foreach ($w in $watchers) {
            $w.EnableRaisingEvents = $false
            $w.Dispose()
        }
    }
}

# ============================================================
# Main
# ============================================================

switch ($Action.ToLower()) {
    "build" {
        $ok = Build-All
        if (-not $ok) { exit 1 }
    }
    "run" {
        $ok = Build-All
        if ($ok) { Start-QEMU }
        else { exit 1 }
    }
    "watch" {
        Start-Watch
    }
    "clean" {
        Write-Header "Cleaning"
        & $Make clean
        Write-OK "Done"
    }
    "qemu" {
        # Только запуск QEMU без пересборки
        Start-QEMU
    }
    default {
        Write-Host "Usage: .\dev.ps1 [build|run|watch|clean|qemu]" -ForegroundColor Yellow
        Write-Host "  build  - Clean + build all + create disk" -ForegroundColor Gray
        Write-Host "  run    - Build + launch QEMU" -ForegroundColor Gray
        Write-Host "  watch  - Auto-rebuild on .c/.h/.asm changes" -ForegroundColor Gray
        Write-Host "  clean  - Clean build artifacts" -ForegroundColor Gray
        Write-Host "  qemu   - Launch QEMU without rebuilding" -ForegroundColor Gray
    }
}
