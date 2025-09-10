param(
  [string] $Port = $env:IDF_PORT,
  [switch] $AutoPort,
  [switch] $SkipBuild,
  [switch] $SkipFlash,
  [switch] $SkipMonitor,
  [switch] $FullClean,
  [switch] $DirectFlash, # use esptool with pre-generated flash_args (avoids invoking idf.py flash)
  [string] $IdfPath,
  [string] $Python,
  [switch] $Offline # when set, disable component manager network resolution
)

function Remove-BuildTree {
  param([string] $Path = 'build')
  if (-not (Test-Path $Path)) { return }
  Write-Host "Removing build directory '$Path' (FullClean)" -ForegroundColor Yellow
  try {
    Remove-Item -Recurse -Force $Path -ErrorAction Stop
    return
  } catch {
    Write-Warning "Standard Remove-Item failed: $($_.Exception.Message)"
  }
  try {
    $full = (Resolve-Path $Path).Path
    $long = "\\\\?\\$full"
    Write-Host "Retrying with long path prefix: $long" -ForegroundColor DarkYellow
    Remove-Item -Recurse -Force $long -ErrorAction Stop
    return
  } catch {
    Write-Warning "Long path removal attempt failed: $($_.Exception.Message)"
  }
  # Fallback: rename then robocopy mirror empty folder to truncate
  $fallbackName = "${Path}_delete_$(Get-Date -Format yyyyMMddHHmmss)"
  try {
    Rename-Item -Path $Path -NewName $fallbackName -ErrorAction Stop
    $emptyDir = "_empty_dir_for_delete"
    if (-not (Test-Path $emptyDir)) { New-Item -ItemType Directory -Path $emptyDir | Out-Null }
    Write-Host "Using robocopy mirror trick to purge: $fallbackName" -ForegroundColor DarkYellow
    robocopy $emptyDir $fallbackName /MIR | Out-Null
    Remove-Item -Recurse -Force $fallbackName -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $emptyDir -ErrorAction SilentlyContinue
  } catch {
    Write-Warning "Fallback deletion approach failed: $($_.Exception.Message). You may need to close any open file handles and remove '$Path' manually."
  }
}

function Select-AutoPort {
  Write-Host "Attempting automatic serial port detection..." -ForegroundColor Cyan
  try {
    $ports = Get-CimInstance Win32_SerialPort -ErrorAction Stop
  } catch {
    try { $ports = Get-WmiObject Win32_SerialPort -ErrorAction SilentlyContinue } catch { $ports = @() }
  }
  if (-not $ports) { Write-Warning "No serial ports discovered"; return $null }
  # Prefer Espressif VID (303A) if present
  $esp = $ports | Where-Object { $_.PNPDeviceID -match 'VID_303A' }
  if ($esp) { $candidates = $esp } else { $candidates = $ports }
  # Pick highest COM number (often the most recently enumerated board)
  $pick = $candidates | Sort-Object { [int]($_.DeviceID -replace '[^0-9]','') } | Select-Object -Last 1
  if ($pick) {
    Write-Host (" Auto-selected {0} : {1}" -f $pick.DeviceID, $pick.Description)
    return $pick.DeviceID
  }
  Write-Warning "AutoPort failed to choose a port"
  return $null
}

if ($AutoPort) {
  $auto = Select-AutoPort
  if ($auto) { $Port = $auto }
}

if (-not $Port) { $Port = 'COM27' }
Write-Host "Using port: $Port"

if ($FullClean) { Remove-BuildTree -Path 'build' }

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
  # Autodetect ESP-IDF path (Option B) if -IdfPath not given.
  $preferredVersionPattern = 'v5\.5'
  $tried = @()
  function Add-Tried($p) { if ($p -and ($tried -notcontains $p)) { $script:tried += $p } }

  if (-not $IdfPath) {
    # Only trust existing IDF_PATH if it looks like desired version and has export.ps1
    if ($env:IDF_PATH -and (Test-Path (Join-Path $env:IDF_PATH 'export.ps1')) -and ($env:IDF_PATH -match $preferredVersionPattern)) {
      $IdfPath = $env:IDF_PATH; Add-Tried $IdfPath
    } else {
      if ($env:IDF_PATH) { Add-Tried $env:IDF_PATH }
      $searchRoots = @(
        "C:/Essence_SC/ESP/ESP-IDF_compiler",
        (Join-Path $PSScriptRoot '..'),
        (Join-Path $PSScriptRoot '../../../../../..')
      ) | Sort-Object -Unique
      $candidateList = @()
      foreach ($root in $searchRoots) {
        if (Test-Path $root) {
          try {
            # Harvest esp-idf directories containing preferred version marker
            $candidateList += Get-ChildItem -Path $root -Directory -Recurse -ErrorAction SilentlyContinue |
              Where-Object { $_.FullName -match $preferredVersionPattern -and $_.Name -eq 'esp-idf' } |
              ForEach-Object { $_.FullName }
          } catch { }
        }
      }
      $candidateList = $candidateList | Sort-Object -Unique
      foreach ($c in $candidateList) {
        Add-Tried $c
        if (Test-Path (Join-Path $c 'export.ps1')) { $IdfPath = (Resolve-Path $c).Path; break }
      }
      if (-not $IdfPath) {
        # Final hard-coded fallback
        $fallback = "C:/Essence_SC/ESP/ESP-IDF_compiler/v5.5/esp-idf"
        Add-Tried $fallback
        if (Test-Path (Join-Path $fallback 'export.ps1')) { $IdfPath = $fallback }
      }
    }
  } else { Add-Tried $IdfPath }

  if ($IdfPath -and (Test-Path (Join-Path $IdfPath 'export.ps1'))) {
    Write-Host "Bootstrapping ESP-IDF from $IdfPath" -ForegroundColor Cyan
    . (Join-Path $IdfPath 'export.ps1') | Out-Null
  } else {
    Write-Warning ("Unable to locate export.ps1 for preferred ESP-IDF (pattern {0}). Tried paths: `n - {1}" -f $preferredVersionPattern, ($tried -join "`n - "))
  }
}

# Component manager handling: allow by default so managed_components resolve.
if ($Offline) {
  Write-Host "Offline mode: disabling component manager" -ForegroundColor DarkYellow
  $env:IDF_COMPONENT_MANAGER = "0"
} else {
  # Ensure not explicitly disabled so that 'lvgl__lvgl' and others resolve.
  if ($env:IDF_COMPONENT_MANAGER) { Write-Host "Ensuring component manager active (unset override)" -ForegroundColor DarkGray }
  Remove-Item Env:IDF_COMPONENT_MANAGER -ErrorAction SilentlyContinue | Out-Null
}

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
  throw "idf.py still not found in PATH after bootstrap"
}

if (-not $SkipBuild) {
  idf.py build
  if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
} else { Write-Host 'Build skipped' }

if (-not $SkipFlash) {
  if ($DirectFlash) {
    # Project root is parent of tools directory
    $projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
    $buildDir = Join-Path $projectRoot 'build'
    $flashArgsFile = Join-Path $buildDir 'flash_args'
    $flasherJson = Join-Path $buildDir 'flasher_args.json'
    if (Test-Path $flashArgsFile) {
      Push-Location $buildDir
      Write-Host "Direct flash via esptool using flash_args (port $Port)" -ForegroundColor Cyan
      if (-not $Python) { $Python = (Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1).Path }
      if (-not $Python -and $env:PYTHONHOME) { $Python = (Join-Path $env:PYTHONHOME 'python.exe') }
      if (-not $Python) { throw "Python executable not found for DirectFlash" }
      & $Python -m esptool -p $Port @flash_args
      $rc = $LASTEXITCODE
      Pop-Location
      if ($rc -ne 0) { throw "esptool direct flash failed ($rc)" }
    } elseif (Test-Path $flasherJson) {
  Write-Host "Direct flash via esptool using flasher_args.json (port $Port)" -ForegroundColor Cyan
  Write-Host " Using build dir: $buildDir" -ForegroundColor DarkGray
      $json = Get-Content $flasherJson -Raw | ConvertFrom-Json
      $chip = $json.extra_esptool_args.chip
      $before = $json.extra_esptool_args.before
      $after = $json.extra_esptool_args.after
      $flashArgs = @()
      $flashArgs += '--chip'; $flashArgs += $chip
      $flashArgs += '-p'; $flashArgs += $Port
      $flashArgs += '-b'; $flashArgs += '460800'
      $flashArgs += '--before'; $flashArgs += $before
      $flashArgs += '--after';  $flashArgs += $after
      $flashArgs += 'write_flash'
      $flashArgs += $json.write_flash_args
      foreach ($kv in $json.flash_files.PSObject.Properties) { $flashArgs += $kv.Name; $flashArgs += (Join-Path $buildDir $kv.Value) }
      if (-not $Python) { $Python = (Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1).Path }
      if (-not $Python -and $env:PYTHONHOME) { $Python = (Join-Path $env:PYTHONHOME 'python.exe') }
      if (-not $Python) { throw "Python executable not found for DirectFlash" }
      Push-Location $buildDir
      & $Python -m esptool @flashArgs
      $rc = $LASTEXITCODE
      Pop-Location
      if ($rc -ne 0) { throw "esptool direct flash (json) failed ($rc)" }
    } else {
      throw "DirectFlash requested but neither flash_args nor flasher_args.json found"
    }
  } else {
    idf.py -p $Port flash
    if ($LASTEXITCODE -ne 0) { throw "Flash failed ($LASTEXITCODE)" }
  }
} else { Write-Host 'Flash skipped' }

if (-not $SkipMonitor) {
  idf.py -p $Port monitor
} else { Write-Host 'Monitor skipped' }
