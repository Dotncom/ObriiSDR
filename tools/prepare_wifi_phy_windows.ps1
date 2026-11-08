param([switch]$Install)
$ErrorActionPreference='Stop'
$Workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Prefix=Join-Path $Workspace 'build/runtime/wifi-phy'
$Root=Join-Path $Workspace 'build/deps/mamba-root'
$Mamba=Join-Path $Workspace 'build/deps/micromamba/Library/bin/micromamba.exe'
if ($Install -or -not (Test-Path -LiteralPath (Join-Path $Prefix 'python.exe'))) {
    if (-not (Test-Path -LiteralPath $Mamba)) { throw 'Prepare official micromamba 2.9.0 in build/deps/micromamba first (https://mamba.readthedocs.io/).' }
    & $Mamba --no-rc create --yes -p $Prefix -r $Root -c conda-forge -c ryanvolz --strict-channel-priority 'python=3.12' 'gnuradio-ieee802_11=0.0.0.20260519.dev+gad0598e' 'libblas=*=*openblas' 'liblapack=*=*openblas'
    if($LASTEXITCODE){throw 'PHY environment preparation failed'}
}
$Sources=@()
$Licenses=Join-Path $Prefix 'source-notices'
foreach($Metadata in Get-ChildItem -LiteralPath (Join-Path $Prefix 'conda-meta') -Filter '*.json') {
    $Package=Get-Content -LiteralPath $Metadata.FullName -Raw|ConvertFrom-Json
    $Sources+=@{name=$Package.name;version=$Package.version;build=$Package.build;license=$Package.license;url=$Package.url;sha256=$Package.sha256}
    $Original=Join-Path $Package.extracted_package_dir 'info/licenses'
    if(Test-Path -LiteralPath $Original){
        $Destination=Join-Path $Licenses $Package.name
        New-Item -ItemType Directory -Path $Destination -Force|Out-Null
        Copy-Item -LiteralPath $Original -Destination $Destination -Recurse -Force
    }
}
$Sources|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $Prefix 'PACKAGE_SOURCES.json') -Encoding utf8
# Remove only build/test artifacts created by this helper's isolated environment.
$AbsolutePrefix=[IO.Path]::GetFullPath($Prefix)
foreach($File in Get-ChildItem -LiteralPath $Prefix -Recurse -File) {
    $Relative=$File.FullName.Substring($AbsolutePrefix.Length+1).Replace('\','/')
    if($Relative -match '(^|/)__pycache__/|^Lib/test/|^Library/include/|^Library/lib/.*\.(lib|a)$' -or $File.Extension -in @('.pyc','.pdb')) {
        $Target=[IO.Path]::GetFullPath($File.FullName)
        if(!$Target.StartsWith($AbsolutePrefix+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe artifact path'}
        Remove-Item -LiteralPath $Target -Force
    }
}
$ScriptDirectory=Join-Path $Workspace 'build/runtime/wireless'
New-Item -ItemType Directory -Path $ScriptDirectory -Force|Out-Null
foreach($Script in @('wifi_phy_decoder.py', 'wifi_packet_bus.py', 'wifi_extcap.py')) {
    Copy-Item -LiteralPath (Join-Path $Workspace "tools/$Script") -Destination $ScriptDirectory -Force
}
$Files=Get-ChildItem -LiteralPath $Prefix -Recurse -File|Where-Object { $_.Name -ne 'runtime-manifest.json' -and $_.FullName -notmatch '\\conda-meta\\' }|ForEach-Object {
    @{path=$_.FullName.Substring($AbsolutePrefix.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
}
@{version=1;source='https://github.com/bastibl/gr-ieee802-11/tree/ad0598e4a874f4b8e1f391a1e0323e80df2b34ff';files=@($Files)}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $Prefix 'runtime-manifest.json') -Encoding utf8
& (Join-Path $Prefix 'python.exe') -I -B (Join-Path $ScriptDirectory 'wifi_phy_decoder.py') --check
if($LASTEXITCODE){throw 'PHY runtime import check failed'}
Write-Output 'Offline OFDM runtime and package notices prepared; no device opened.'
