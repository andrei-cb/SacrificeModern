$ErrorActionPreference = 'Stop'

$serverInfo = Join-Path $PSScriptRoot 'out/web-server.json'
if (-not (Test-Path -LiteralPath $serverInfo)) {
  Write-Host 'The web server is already stopped.'
  return
}

$server = Get-Content -LiteralPath $serverInfo -Raw | ConvertFrom-Json
$process = Get-Process -Id $server.pid -ErrorAction SilentlyContinue
if (-not $process -or "$($process.StartTime.ToFileTimeUtc())" -ne [string]$server.started) {
  Remove-Item -LiteralPath $serverInfo
  Write-Host 'The web server is already stopped.'
  return
}

Stop-Process -Id $process.Id -ErrorAction Stop
Remove-Item -LiteralPath $serverInfo
Write-Host "Stopped the web server on port $($server.port). You can close its browser tab."
