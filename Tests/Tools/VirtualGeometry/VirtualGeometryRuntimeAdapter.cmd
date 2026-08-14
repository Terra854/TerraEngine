@echo off
rem Production virtual-geometry runtime adapter entry point.
rem
rem The native benchmark invokes this file with --output, --workload,
rem --backend, and --sample-index. PowerShell owns the typed validation and
rem evidence writing; this command wrapper only makes the adapter directly
rem executable through cmd.exe from both source and installed trees.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0CaptureVirtualGeometryRuntimeSample.ps1" %*
exit /b %ERRORLEVEL%
