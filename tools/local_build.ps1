Param(
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$FullClean,
    [string]$Port = 'COM27'
)

$ErrorActionPreference = 'Stop'

function Resolve-IdfPython() {
    $candidates = @()
    if ($env:IDF_PYTHON_ENV_PATH) {
        $candidates += (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe')
    }
    # Known absolute install (observed earlier in logs)
    $candidates += 'C:\Essence_SC\ESP\ESP-IDF_compiler\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
    # Relative fallback from project/tools
    $candidates += (Join-Path $PSScriptRoot '..\..\..\..\..\ESP\ESP-IDF_compiler\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe')
    foreach ($c in $candidates) { if (Test-Path $c) { return (Resolve-Path $c).Path } }
    throw "Unable to locate python for idf.py. Checked: `n$($candidates -join "`n")" 
}

function Resolve-IdfPy() {
    $candidates = @()
    if ($env:IDF_PATH) { $candidates += (Join-Path $env:IDF_PATH 'tools/idf.py') }
    # Canonical v5.5 path (prior successful builds)
    $candidates += 'C:\Essence_SC\ESP\ESP-IDF_compiler\v5.5\esp-idf\tools\idf.py'
    # Previously discovered alt idf.py (other distribution)
    $candidates += 'C:\Essence_SC\ESP\ESP-EYE\esp-who\V1\esp-idf\tools\idf.py'
    # Relative search
    $candidates += (Join-Path $PSScriptRoot '..\..\..\..\..\ESP\ESP-IDF_compiler\v5.5\esp-idf\tools\idf.py')
    foreach ($c in $candidates) { if (Test-Path $c) { return (Resolve-Path $c).Path } }
    throw "Unable to locate idf.py. Checked: `n$($candidates -join "`n")"
}

$py = Resolve-IdfPython
$idfpy = Resolve-IdfPy
Write-Host "Using python: $py"
Write-Host "Using idf.py: $idfpy"

# Force IDF_PATH to match v5.5 toolchain (override any stale environment value)
$desiredIdf = 'C:\Essence_SC\ESP\ESP-IDF_compiler\v5.5\esp-idf'
if (-not (Test-Path $desiredIdf)) { throw "Desired IDF path $desiredIdf not found" }
$env:IDF_PATH = $desiredIdf
Write-Host "Set IDF_PATH=$env:IDF_PATH" -ForegroundColor Yellow

# Ensure cmake is on PATH (idf.py requires external cmake executable). Search known IDF download cache.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $cmakeRoots = @(
        (Join-Path $env:USERPROFILE ".espressif\tools\cmake"),
        'C:\Essence_SC\ESP\ESP-IDF_compiler\.espressif\tools\cmake',
        'C:\Essence_SC\ESP\ESP-IDF_compiler\.espressif\dist'  # zipped dist fallback
    )
    $cmakeBin = $null
    foreach ($r in $cmakeRoots) {
        if (Test-Path $r) {
            # Find newest cmake*/bin/cmake.exe
            $candidates = Get-ChildItem -Path $r -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
            if ($candidates -and $candidates.Count -gt 0) {
                $cmakeBin = $candidates[0].DirectoryName
                break
            }
        }
    }
    if ($cmakeBin) {
        Write-Host "Adding CMake to PATH: $cmakeBin" -ForegroundColor Yellow
        $env:PATH = "$cmakeBin;$env:PATH"
    } else {
        Write-Warning "CMake not found in expected locations; build may fail again. Consider running: idf.py install"
    }
}

# Ensure ninja is on PATH (IDF default generator). Search typical .espressif/tools locations.
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $ninjaRoots = @(
        (Join-Path $env:USERPROFILE ".espressif\tools\ninja"),
        'C:\Essence_SC\ESP\ESP-IDF_compiler\.espressif\tools\ninja'
    )
    $ninjaBin = $null
    foreach ($r in $ninjaRoots) {
        if (Test-Path $r) {
            $candidates = Get-ChildItem -Path $r -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending
            if ($candidates -and $candidates.Count -gt 0) {
                $ninjaBin = $candidates[0].DirectoryName
                break
            }
        }
    }
    if ($ninjaBin) {
        Write-Host "Adding Ninja to PATH: $ninjaBin" -ForegroundColor Yellow
        $env:PATH = "$ninjaBin;$env:PATH"
    } else {
        Write-Warning "Ninja not found; consider running: idf.py install"
    }
}

# Ensure RISC-V toolchain (riscv32-esp-elf) is on PATH for fresh configure after fullclean.
if (-not (Get-Command riscv32-esp-elf-gcc -ErrorAction SilentlyContinue)) {
    $tcRoots = @(
        'C:\Essence_SC\ESP\ESP-IDF_compiler',
        (Join-Path $env:USERPROFILE '.espressif\tools'),
        (Join-Path $env:USERPROFILE '.espressif')
    )
    $candidatesAll = @()
    foreach ($r in $tcRoots) {
        if (-not (Test-Path $r)) { continue }
        try {
            $candidatesAll += Get-ChildItem -Path $r -Recurse -Filter 'riscv32-esp-elf-gcc.exe' -ErrorAction SilentlyContinue
        } catch { }
    }
    if ($candidatesAll.Count -gt 0) {
        # Prefer esp-14.* toolchain, then highest lexical path.
        $preferred = $candidatesAll | Where-Object { $_.FullName -match 'esp-14' } | Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $preferred) { $preferred = $candidatesAll | Sort-Object FullName -Descending | Select-Object -First 1 }
        if ($preferred) {
            $tcBin = $preferred.DirectoryName
            Write-Host "Adding RISC-V toolchain to PATH: $tcBin" -ForegroundColor Yellow
            $env:PATH = "$tcBin;$env:PATH"
        }
    }
    if (-not (Get-Command riscv32-esp-elf-gcc -ErrorAction SilentlyContinue)) {
        Write-Warning "RISC-V toolchain not found; run 'idf.py install'"
    }
}

# If doing a FullClean, also purge unwanted managed_components (esp_hosted & related) and stale lock file
if ($FullClean) {
    $purge = @(
        'managed_components/espressif__esp_hosted',
        'managed_components/espressif__esp_wifi_remote',
        'managed_components/espressif__wifi_remote_over_eppp',
        'managed_components/espressif__eppp_link',
        'managed_components/espressif__esp_serial_slave_link'
    )
    foreach ($p in $purge) {
        if (Test-Path $p) {
            Write-Host "Removing $p" -ForegroundColor DarkYellow
            Remove-Item -Recurse -Force $p
        }
    }
    if (Test-Path 'dependencies.lock') {
        Write-Host "Removing stale dependencies.lock (will regenerate minimal)" -ForegroundColor DarkYellow
        Remove-Item -Force 'dependencies.lock'
    }
}

$argsList = @()
if ($FullClean) { $argsList += 'fullclean' }
if ($Flash -and $Monitor) { $argsList += 'flash'; $argsList += 'monitor' }
elseif ($Flash) { $argsList += 'flash' }
elseif ($Monitor) { $argsList += 'monitor' }
else { $argsList += 'build' }

if ($argsList -contains 'flash' -or $argsList -contains 'monitor') {
    $argsList += @('-p', $Port)
}

Write-Host "Invoking: $py $idfpy $($argsList -join ' ')" -ForegroundColor Cyan
& $py $idfpy @argsList
if ($LASTEXITCODE -ne 0) { throw "idf.py failed with exit code $LASTEXITCODE" }