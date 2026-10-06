param(
    [Parameter(Mandatory)]
    [ValidateSet('v140', 'v141', 'v142', 'v143', 'v145')]
    [string]$Toolset,
    [Parameter(Mandatory)]
    [ValidateSet('MSVC', 'Clang')]
    [string]$Compiler
)

$ErrorActionPreference = 'Stop'
$installerDirectory = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere = Join-Path $installerDirectory 'vswhere.exe'
$versionRange = if ($Toolset -eq 'v145') { '[18.0,19.0)' } else { '[17.0,18.0)' }
$installation = & $vswhere -latest -products '*' -version $versionRange -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
    throw "Visual Studio $versionRange was not found"
}

$component = switch ($Toolset) {
    'v140' { 'Microsoft.VisualStudio.Component.VC.140' }
    'v141' { 'Microsoft.VisualStudio.Component.VC.v141.x86.x64' }
    'v142' { 'Microsoft.VisualStudio.ComponentGroup.VC.Tools.142.x86.x64' }
    default { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
}
$installed = & $vswhere -latest -products '*' -version $versionRange `
    -requires $component -property installationPath
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio component discovery failed' }
if ($installed -ne $installation) {
    Write-Output "Installing $component in $installation"
    $setup = Join-Path $installerDirectory 'setup.exe'
    $process = Start-Process $setup -Wait -PassThru -ArgumentList @(
        'modify', '--installPath', "`"$installation`"", '--add', $component,
        '--quiet', '--norestart'
    )
    if ($process.ExitCode -notin @(0, 3010)) {
        throw "Visual Studio component installation failed: $($process.ExitCode)"
    }
    $installed = & $vswhere -latest -products '*' -version $versionRange `
        -requires $component -property installationPath
    if ($LASTEXITCODE -ne 0 -or $installed -ne $installation) {
        throw "The requested $Toolset component is still missing"
    }
}

if ($Toolset -eq 'v140') {
    $toolsVersion = '14.0'
} else {
    $pattern = switch ($Toolset) {
        'v141' { '^14\.1[0-9]\.' }
        'v142' { '^14\.2[0-9]\.' }
        'v143' { '^14\.(3[0-9]|4[0-9])\.' }
        'v145' { '^14\.5[0-9]\.' }
    }
    $tools = Get-ChildItem (Join-Path $installation 'VC\Tools\MSVC') -Directory |
        Where-Object { $_.Name -match $pattern } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
    if (-not $tools) { throw "No compiler version matching $Toolset was installed" }
    $toolsVersion = $tools.Name
}

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
    # cl prints its version with no input and returns 2 for the missing source.
    & $cxx
    if ($LASTEXITCODE -ne 2) { throw 'MSVC compiler discovery failed' }
    $global:LASTEXITCODE = 0
}
$env:CXX = $cxx
if ($env:GITHUB_ENV) {
    "CXX=$cxx" | Out-File $env:GITHUB_ENV -Encoding utf8 -Append
}
