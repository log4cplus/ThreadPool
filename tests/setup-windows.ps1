param(
    [Parameter(Mandatory)]
    [ValidateSet('v142', 'v143', 'v145')]
    [string]$Toolset,
    [Parameter(Mandatory)]
    [ValidateSet('MSVC', 'Clang')]
    [string]$Compiler
)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer" 'vswhere.exe'
$versionRange = if ($Toolset -eq 'v145') { '[18.0,19.0)' } else { '[17.0,18.0)' }
$installation = & $vswhere -latest -products '*' -version $versionRange -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
    throw "Visual Studio $versionRange was not found"
}

$component = switch ($Toolset) {
    'v142' { 'Microsoft.VisualStudio.ComponentGroup.VC.Tools.142.x86.x64' }
    default { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
}
$installed = & $vswhere -latest -products '*' -version $versionRange `
    -requires $component -property installationPath
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio component discovery failed' }
if ($installed -ne $installation) {
    throw "The requested $Toolset component is not preinstalled"
}

$pattern = switch ($Toolset) {
    'v142' { '^14\.2[0-9]\.' }
    'v143' { '^14\.(3[0-9]|4[0-9])\.' }
    'v145' { '^14\.5[0-9]\.' }
}
$tools = Get-ChildItem (Join-Path $installation 'VC\Tools\MSVC') -Directory |
    Where-Object { $_.Name -match $pattern } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $tools) { throw "No preinstalled compiler version matches $Toolset" }
$toolsVersion = $tools.Name

$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvarsall.bat'
Write-Output "Visual Studio: $installation; toolset: $Toolset ($toolsVersion); target: x64"
$originalEnvironment = [Environment]::GetEnvironmentVariables('Process')
$environment = & $env:ComSpec /d /s /c "`"$vcvars`" x64 -vcvars_ver=$toolsVersion >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment setup failed' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        # Persist the same environment for the subsequent workflow step.
        if ($env:GITHUB_ENV -and $Matches[1] -notmatch '^(GITHUB_|RUNNER_|NODE_OPTIONS$)' `
            -and $originalEnvironment[$Matches[1]] -ne $Matches[2]) {
            "$($Matches[1])=$($Matches[2])" | Out-File $env:GITHUB_ENV -Encoding utf8 -Append
        }
    }
}

if ($Compiler -eq 'Clang') {
    $cxx = Join-Path $installation 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
    if (-not (Test-Path $cxx)) { throw 'The bundled Visual Studio clang-cl was not found' }
    & $cxx --version
    if ($LASTEXITCODE -ne 0) { throw 'clang-cl version discovery failed' }
} else {
    $cxx = (Get-Command cl.exe -ErrorAction Stop).Source
    # cl prints its version with no input; toolsets return either 0 or 2.
    & $cxx
    if ($LASTEXITCODE -notin @(0, 2)) { throw 'MSVC compiler discovery failed' }
    $global:LASTEXITCODE = 0
}
$env:CXX = $cxx
if ($env:GITHUB_ENV) {
    "CXX=$cxx" | Out-File $env:GITHUB_ENV -Encoding utf8 -Append
}
