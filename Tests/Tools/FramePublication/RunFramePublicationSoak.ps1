[CmdletBinding()]
param([string]$Project,[string]$Scene,[string]$Manifest,[string]$OutputDirectory,[string]$CameraHash,[string]$EditorProcessName='TerraEngine-Editor',[string]$LiveAdapter=(Join-Path $PSScriptRoot '../VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1'),[int]$WarmupSeconds=30,[int]$DurationSeconds=300,[int]$SampleIntervalSeconds=1,[string]$Executable,[double[]]$ValidationCameraPosition=@(0.0,0.5,1.0),[double[]]$ValidationCameraTarget=@(0.0,0.5,0.0),[double]$ValidationCameraVerticalFovDegrees=60.0)
$ErrorActionPreference='Stop'; if($DurationSeconds -lt 300 -or $SampleIntervalSeconds -ne 1){throw 'Soak requires 300 seconds and one-second samples.'}; if([string]::IsNullOrWhiteSpace($CameraHash)){throw 'CameraHash is required; camera identity is never inferred.'}; if(@(Get-Process -Name $EditorProcessName -ErrorAction SilentlyContinue).Count -gt 1){throw 'At most one editor instance may be used for soak.'}; if(-not(Test-Path $Manifest)){throw "Manifest is missing: $Manifest"}; if(-not(Test-Path $Project)){throw "Project is missing: $Project"}; if(-not(Test-Path $Scene)){throw "Scene is missing: $Scene"}; New-Item -ItemType Directory -Force $OutputDirectory|Out-Null
if(-not(Test-Path $LiveAdapter)){throw "Live MCP adapter is missing: $LiveAdapter"}; if([string]::IsNullOrWhiteSpace($Executable)){throw 'Installed editor executable is required.'}; if(-not(Test-Path $Executable)){throw "Editor executable is missing: $Executable"}
$existingEditors = @(Get-Process -Name $EditorProcessName -ErrorAction SilentlyContinue)
if($existingEditors.Count -ne 0){throw 'Soak starts and owns exactly one installed editor.'}
$projectRoot = [IO.Path]::GetFullPath($Project)
$scenePath = [IO.Path]::GetFullPath($Scene)
if(-not $scenePath.StartsWith($projectRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,
	[StringComparison]::OrdinalIgnoreCase)){throw 'Soak scene must be inside the selected project.'}
$relativeScene = $scenePath.Substring($projectRoot.TrimEnd('\','/').Length + 1).Replace('\','/')
& $LiveAdapter -InstalledEditor $Executable -Project $Project -ScenePath $relativeScene `
	-PrefabInstanceCount 0 -OutputDirectory $OutputDirectory -WarmupSeconds $WarmupSeconds `
	-SoakDurationSeconds $DurationSeconds -SoakSampleIntervalSeconds $SampleIntervalSeconds `
	-ValidationCameraPosition $ValidationCameraPosition -ValidationCameraTarget $ValidationCameraTarget `
	-ValidationCameraVerticalFovDegrees $ValidationCameraVerticalFovDegrees `
	-McpPort 43155 -EnterPlayMode -ExpectAnimatedChange
# The adapter is another PowerShell script, not a native executable. A successful
# script invocation does not assign LASTEXITCODE, so use PowerShell's immediate
# pipeline status rather than turning a completed five-minute soak into a false
# failure after its editor has already exited cleanly.
$adapterSucceeded = $?
if(-not $adapterSucceeded){throw 'Live soak adapter failed.'}
