# Run make in the pinned devkitPPC container from the repo root. Args go to make.
$ErrorActionPreference = 'Stop'
$image = if ($env:WIIPEOUT_IMAGE) { $env:WIIPEOUT_IMAGE } else { 'devkitpro/devkitppc:20261102' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$toolPath = '/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin'
# PowerShell 5.1 does not quote native args that contain spaces, so quote them here.
$makeArgs = @($args | ForEach-Object { if ($_ -match '\s') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } })
docker run --rm -e "PATH=$toolPath" -v "${root}:/src" -w /src $image make @makeArgs
exit $LASTEXITCODE
