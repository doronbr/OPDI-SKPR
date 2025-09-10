#!/usr/bin/env pwsh
<#
PowerShell documentation validation & packaging script.
Mirrors bash version but avoids PATH issues for Windows users.
Steps:
 1. Check for doxygen, dot.
 2. Optionally disable graphs if dot missing.
 3. Run doxygen, fail on warnings.
 4. Package html + log into docs/docx/artifacts (+ zip).
 5. Run markdownlint, codespell, lychee if available; otherwise warn.
#>

$ErrorActionPreference = 'Stop'
Write-Host 'validate_docs.ps1 :: Starting' -ForegroundColor Cyan

function Test-CommandExists {
  param([string]$Name)
  return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}
function Add-ToPathIfExists {
  param([string]$Dir)
  if($Dir -and (Test-Path $Dir) -and -not ($env:Path -split ';' | Where-Object { $_ -eq $Dir })){
    $env:Path = "$Dir;" + $env:Path
  }
}

# Attempt auto-path for common installs if commands absent
if(-not (Test-CommandExists -Name doxygen)){
  Add-ToPathIfExists 'C:\Program Files\doxygen\bin'
  Add-ToPathIfExists 'C:\Program Files (x86)\doxygen\bin'
}
if(-not (Test-CommandExists -Name dot)){
  Add-ToPathIfExists 'C:\Program Files\Graphviz\bin'
  Add-ToPathIfExists 'C:\Program Files (x86)\Graphviz\bin'
}

if(-not (Test-CommandExists -Name doxygen)){
  Write-Error "doxygen not found after PATH augmentation. Install via winget install --id Doxygen.Doxygen -e or choco install doxygen.install -y"
}
$haveDot = Test-CommandExists -Name dot
if(-not $haveDot){
  Write-Warning 'Graphviz dot not found - proceeding without graphs (HAVE_DOT=NO)'
}

# Prepare temp Doxyfile if needed
$doxyfile = 'Doxyfile'
$tempDoxy = $null
if(-not $haveDot){
  $tempDoxy = 'Doxyfile.nodot'
  (Get-Content $doxyfile) -replace '^(HAVE_DOT\s*=).*','$1 NO' | Set-Content $tempDoxy
  $doxyfile = $tempDoxy
}

Write-Host "validate_docs.ps1 :: Running doxygen ($doxyfile)" -ForegroundColor Cyan
$doxygenLogDir = 'docs/api'
New-Item -ItemType Directory -Force -Path $doxygenLogDir | Out-Null
$doxygenLog = Join-Path $doxygenLogDir 'doxygen.log'
& doxygen $doxyfile 2>&1 | Tee-Object -FilePath $doxygenLog
if($LASTEXITCODE -ne 0){ throw "Doxygen exited with code $LASTEXITCODE" }

# Fail on any 'warning' substring
if(Select-String -Path $doxygenLog -Pattern 'warning' -SimpleMatch -Quiet){
  throw "Doxygen warnings found (treated as error)"
}

if($tempDoxy){ Remove-Item $tempDoxy -ErrorAction SilentlyContinue }

# Package artifacts
$artDir = 'docs/docx/artifacts'
if(Test-Path $artDir){ Remove-Item -Recurse -Force $artDir }
New-Item -ItemType Directory -Path $artDir | Out-Null
Copy-Item docs/api/html -Destination (Join-Path $artDir 'html') -Recurse
Copy-Item $doxygenLog -Destination $artDir
$manifest = @(
  "Generated: $((Get-Date).ToUniversalTime().ToString('u'))",
  "HAVE_DOT: $haveDot",
  "Source Commit: ${env:GITHUB_SHA}" ,
  "Files documented: $((Get-ChildItem -Recurse docs/api/html -Filter *.html).Count)"
) -join [Environment]::NewLine
$manifest | Out-File (Join-Path $artDir 'manifest.txt') -Encoding UTF8

# Create zip
$zip = 'docs/docx/artifacts.zip'
if(Test-Path $zip){ Remove-Item $zip -Force }
Compress-Archive -Path $artDir -DestinationPath $zip -Force

function Invoke-IfToolAvailable {
    param(
        [string]$Tool,
        [string[]]$ToolArgs,
        [string]$Friendly
    )
  if(Test-CommandExists -Name $Tool){
    Write-Host "validate_docs.ps1 :: Running $Friendly" -ForegroundColor Cyan
    & $Tool @ToolArgs
    if($LASTEXITCODE -ne 0){ throw "$Friendly failed ($Tool)" }
  } else {
    Write-Warning "$Friendly skipped - '$Tool' not in PATH"
  }
}

# Use single quotes to avoid PowerShell treating ** as expandable
Invoke-IfToolAvailable -Tool markdownlint -ToolArgs @('**/*.md','--ignore','build','--ignore','docs/api') -Friendly 'markdownlint'
Invoke-IfToolAvailable -Tool codespell -ToolArgs @('-q','3') -Friendly 'codespell'
Invoke-IfToolAvailable -Tool lychee -ToolArgs @('--no-progress','--accept','200,429','.') -Friendly 'lychee link check'

Write-Host 'validate_docs.ps1 :: SUCCESS' -ForegroundColor Green

