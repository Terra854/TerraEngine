# RunLiveSmoke.Tests.ps1 - CPU-only regression entry point for live-smoke safety gates.
#
# This script deliberately does not launch an editor or touch the GPU. It verifies
# the event-query seam rejects pre-existing System events and stops at operation N.

[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "RunLiveSmoke.ps1") -RunHarnessTests
