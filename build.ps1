param(
  [ValidateSet('debug','release','asan')][string]$Preset = 'debug',
  [switch]$Web,
  [switch]$Serve,
  [ValidateRange(1024,65535)][int]$WebPort = 8080,
  [switch]$ConfigureOnly,
  [switch]$RunTests,
  [switch]$Package
)
$ErrorActionPreference = 'Stop'
$buildPreset = $Preset
function Invoke-BuildTool([string]$Tool, [string[]]$ToolArguments) {
  # Native tools routinely write progress and warnings to stderr in Windows PowerShell.
  $ErrorActionPreference = 'Continue'
  & $Tool @ToolArguments
  if ($LASTEXITCODE) { throw "$Tool failed: $LASTEXITCODE" }
}
if ($Web -and $Preset -eq 'asan') { throw 'asan is Windows-only.' }
if ($Serve -and -not $Web) { throw 'Use -Web with -Serve.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio CMake and Ninja tools were not found by vswhere.' }
if ($Web) {
  $emsdkVersion = '6.0.6'
  $emsdk = Join-Path $PSScriptRoot 'out/emsdk'
  if (-not (Test-Path (Join-Path $emsdk 'emsdk.bat'))) {
    Invoke-BuildTool git @('clone','--depth','1','https://github.com/emscripten-core/emsdk.git',$emsdk)
  }
  $stamp = Join-Path $emsdk '.sacrifice-sdk-version'
  if (-not (Test-Path $stamp) -or (Get-Content $stamp -Raw).Trim() -ne $emsdkVersion -or
      -not (Test-Path (Join-Path $emsdk 'upstream/emscripten/emcc.py'))) {
    Invoke-BuildTool (Join-Path $emsdk 'emsdk.bat') @('install',$emsdkVersion)
    Invoke-BuildTool (Join-Path $emsdk 'emsdk.bat') @('activate',$emsdkVersion)
    Set-Content -Path $stamp -Value $emsdkVersion
  }
  $env:EMSDK_QUIET = '1'
  $cmdText = '"' + (Join-Path $emsdk 'emsdk_env.bat') + '" >nul && set'
  & $env:ComSpec /d /s /c $cmdText | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
  }
  if ($LASTEXITCODE) { throw 'Cannot load emsdk environment.' }
  $buildPreset = "web-$Preset"
} else {
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$cmdText = '"' + $devcmd + '" -no_logo -arch=x64 -host_arch=x64 && set'
& $env:ComSpec /d /s /c $cmdText | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
}
$env:VCPKG_ROOT = Join-Path $vs 'VC\vcpkg'
}
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$ninjaDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = $ninjaDir + ';' + $env:PATH
@('out/downloads','out/registries','out/binary-cache') | ForEach-Object {
  New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot $_) | Out-Null
}
Push-Location $PSScriptRoot
try {
  Invoke-BuildTool $cmake @('--preset',$buildPreset)
  if (-not $ConfigureOnly) {
    Invoke-BuildTool $cmake @('--build','--preset',$buildPreset)
    if ($RunTests) {
      Invoke-BuildTool $ctest @('--preset',$buildPreset)
    }
    if ($Package) {
      if ($Preset -ne 'release') { throw 'Use -Preset release for packaging.' }
      $packageDir = Join-Path $PSScriptRoot $(if ($Web) { 'out/package-web' } else { 'out/package' })
      Invoke-BuildTool $cmake @('--install',(Join-Path $PSScriptRoot "out/$buildPreset"),'--prefix',$packageDir)
    }
    if ($Serve) {
      $webRoot = if ($Package) { $packageDir } else { Join-Path $PSScriptRoot "out/$buildPreset" }
      # Keep the origin stable so IndexedDB survives successive builds and server restarts.
      $serverInfo = Join-Path $PSScriptRoot 'out/web-server.json'
      $server = $null
      if (Test-Path $serverInfo) {
        try {
          $previous = Get-Content $serverInfo -Raw | ConvertFrom-Json
          $owned = Get-Process -Id $previous.pid -ErrorAction Stop
          if ("$($owned.StartTime.ToFileTimeUtc())" -eq $previous.started -and $previous.port -eq $WebPort) {
            if ($previous.root -eq $webRoot) { $server = $owned }
            else { Stop-Process -Id $owned.Id -ErrorAction Stop }
          }
        } catch { } # A prior preview server may already have been closed.
      }
      if (-not $server) {
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $WebPort)
        try { $listener.Start() } catch { throw "Port $WebPort is occupied. Use -WebPort <port> for this preview." }
        finally { $listener.Stop() }
        $server = Start-Process -FilePath $env:EMSDK_PYTHON -WindowStyle Hidden -PassThru -ArgumentList @(
          '-m','http.server',"$WebPort",'--bind','127.0.0.1','--directory',('"' + $webRoot + '"')) `
          -RedirectStandardOutput (Join-Path $PSScriptRoot "out/web-server-$WebPort.log") `
          -RedirectStandardError (Join-Path $PSScriptRoot "out/web-server-$WebPort-errors.log")
        @{pid=$server.Id; started="$($server.StartTime.ToFileTimeUtc())"; root=$webRoot; port=$WebPort} |
          ConvertTo-Json | Set-Content $serverInfo
      }
      $serverReady = $false
      for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        $probe = [System.Net.Sockets.TcpClient]::new()
        try { $probe.Connect('127.0.0.1', $WebPort); $serverReady = $true; break }
        catch { Start-Sleep -Milliseconds 100 }
        finally { $probe.Dispose() }
      }
      if (-not $serverReady) { throw "Preview server did not start; see out/web-server-$WebPort-errors.log" }
      Write-Host "Web preview: http://localhost:$WebPort/ (server PID $($server.Id))"
      Start-Process "http://localhost:$WebPort/"
    }
  }
} finally { Pop-Location }
