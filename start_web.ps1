param(
  [ValidateRange(1024,65535)][int]$WebPort = 8080
)
$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'build.ps1') -Web -Preset release -Package -Serve -WebPort $WebPort
