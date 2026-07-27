# NewAnimationCleanSnapshot.ps1 - Materialize an isolated Spec 043 source snapshot.
#
# The implementation branch can be intentionally dirty while a long Spec Kit run
# is in progress. This tool captures the selected feature tree through a temporary
# Git index, creates an unreachable provenance commit plus a portable bundle, and
# optionally checks it out as a clean detached worktree. It never changes the
# caller's index, current branch, or working files.
#
# From the repository root:
#   powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Animation/NewAnimationCleanSnapshot.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Animation/NewAnimationCleanSnapshot.ps1 -MaterializeWorktree -InitializeSubmodulesFromLocalObjects
# The second form requires every recorded submodule checkout and object to
# already exist locally. It adds detached submodule worktrees directly from
# those checkouts, so Git has no opportunity to fall back to a network clone.

[CmdletBinding()]
param(
	[ValidateNotNullOrEmpty()][string]$OutputRoot = 'out/validation/animation/final/clean-checkout/source-snapshot',
	[ValidatePattern('^[A-Za-z0-9._-]+$')][string]$RunId = (Get-Date -Format 'yyyyMMddTHHmmssfff'),
	[switch]$MaterializeWorktree,
	[switch]$InitializeSubmodulesFromLocalObjects
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Get-AbsolutePath([string]$Path, [string]$Base) {
	if([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
	return [System.IO.Path]::GetFullPath((Join-Path $Base $Path))
}

function Assert-AtOrBelow([string]$Parent, [string]$Child, [string]$Description) {
	$root = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
	$value = [System.IO.Path]::GetFullPath($Child).TrimEnd('\', '/')
	if($value.Equals($root, [System.StringComparison]::OrdinalIgnoreCase)) { return }
	if(-not $value.StartsWith($root + [System.IO.Path]::DirectorySeparatorChar,
		[System.StringComparison]::OrdinalIgnoreCase)) {
		throw "$Description must remain below '$root'."
	}
}

function Get-RepoRelativePath([string]$Root, [string]$Path) {
	$normalizedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
	$normalizedPath = [System.IO.Path]::GetFullPath($Path)
	Assert-AtOrBelow $normalizedRoot $normalizedPath 'Evidence path'
	if($normalizedPath.Equals($normalizedRoot, [System.StringComparison]::OrdinalIgnoreCase)) { return '.' }
	return $normalizedPath.Substring($normalizedRoot.Length + 1).Replace('\', '/')
}

function Invoke-Git([string[]]$Arguments, [string]$WorkingDirectory) {
	# Windows PowerShell promotes any native stderr text to an ErrorRecord when
	# ErrorActionPreference is Stop. Git legitimately writes warnings (such as
	# line-ending notices) to stderr while still succeeding, so capture the
	# complete stream and decide success exclusively from Git's exit code.
	$previousErrorActionPreference = $ErrorActionPreference
	try {
		$ErrorActionPreference = 'Continue'
		$lines = @(& git -C $WorkingDirectory @Arguments 2>&1)
		$exitCode = $LASTEXITCODE
	}
	finally {
		$ErrorActionPreference = $previousErrorActionPreference
	}
	if($exitCode -ne 0) {
		throw "git $($Arguments -join ' ') failed:`n$($lines -join [Environment]::NewLine)"
	}
	# Preserve meaningful leading characters such as the exact-state space in
	# `git submodule status`; only trailing record separators are disposable.
	return ($lines -join "`n").TrimEnd()
}

function Write-Json([string]$Path, $Value) {
	[System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
	[System.IO.File]::WriteAllText($Path,
		(($Value | ConvertTo-Json -Depth 32) + "`n"), $script:Utf8NoBom)
}

function Add-LocalSubmoduleWorktrees([string]$SourceRoot, [string]$DestinationRoot) {
	# The source checkout is the repository-contained dependency cache. Requiring
	# a clean, exact submodule status ensures each destination receives the commit
	# recorded by the superproject instead of an arbitrary local branch tip.
	$statusText = Invoke-Git @('submodule', 'status', '--recursive') $SourceRoot
	$statusLines = @($statusText -split "`n" |
		Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
	foreach($line in $statusLines) {
		if($line -notmatch '^([ +\-U])([0-9a-fA-F]{40}) (.+?)(?: \(.+\))?$') {
			throw "Cannot parse local submodule status line '$line'."
		}
		$state = $Matches[1]
		$commit = $Matches[2].ToLowerInvariant()
		$relativePath = $Matches[3]
		if($state -ne ' ') {
			throw "Submodule '$relativePath' is not initialized at its recorded commit (state '$state')."
		}

		$sourcePath = Get-AbsolutePath $relativePath $SourceRoot
		$destinationPath = Get-AbsolutePath $relativePath $DestinationRoot
		Assert-AtOrBelow $SourceRoot $sourcePath 'Source submodule'
		Assert-AtOrBelow $DestinationRoot $destinationPath 'Destination submodule'
		if(-not (Test-Path -LiteralPath $sourcePath -PathType Container)) {
			throw "Local submodule checkout '$sourcePath' is unavailable."
		}
		[void](Invoke-Git @('cat-file', '-e', "$commit`^{commit}") $sourcePath)

		[System.IO.Directory]::CreateDirectory((Split-Path -Parent $destinationPath)) | Out-Null
		if(Test-Path -LiteralPath $destinationPath) {
			$existingEntries = @(Get-ChildItem -LiteralPath $destinationPath -Force)
			if($existingEntries.Count -ne 0) {
				throw "Destination submodule path '$destinationPath' is not empty."
			}
			# Git worktree add requires the destination itself not to exist. This
			# removes only the verified-empty gitlink placeholder in the new,
			# isolated validation worktree.
			Remove-Item -LiteralPath $destinationPath -Force
		}
		[void](Invoke-Git @('worktree', 'add', '--detach', $destinationPath, $commit) $sourcePath)
	}
}

$repoRoot = (Invoke-Git @('rev-parse', '--show-toplevel') (Get-Location).Path).Trim()
$repoRoot = [System.IO.Path]::GetFullPath($repoRoot)
$validationRoot = Join-Path $repoRoot 'out/validation/animation'
$snapshotRoot = Get-AbsolutePath $OutputRoot $repoRoot
Assert-AtOrBelow $validationRoot $snapshotRoot 'Snapshot output'

$bundlePath = Join-Path $snapshotRoot "spec043-$RunId.bundle"
$manifestPath = Join-Path $snapshotRoot 'snapshot-manifest.json'
$worktreePath = Join-Path $snapshotRoot 'worktree'
if((Test-Path -LiteralPath $bundlePath) -or (Test-Path -LiteralPath $manifestPath) -or ($MaterializeWorktree -and (Test-Path -LiteralPath $worktreePath))) {
	throw "Snapshot run '$RunId' would overwrite existing evidence beneath '$snapshotRoot'."
}
[System.IO.Directory]::CreateDirectory($snapshotRoot) | Out-Null

# read-tree supplies every unchanged tracked file. The selected roots below add
# feature files that are still untracked without accidentally capturing local
# agent packages, dumps, screenshots, credentials, or build outputs.
$includedUntrackedRoots = @(
	'.gitattributes',
	'AGENTS.md',
	'CMakeLists.txt',
	'Docs',
	'Editor',
	'Engine',
	# The managed package archives are existing build dependencies copied into a
	# repository feed for offline restores. Admit only the ignored .nuget cache
	# rather than every unrelated or untracked third-party working file.
	'.nuget',
	'Launcher',
	'NuGet.Config',
	'Tests',
	'Tools',
	'cmake',
	'global.json',
	'specs',
	'SPEC_043_PROGRESS.md'
)
$excludedLocalRoots = @('.agents', '.gemini', '.specify', 'dump', 'out',
	'Screenshot 2026-07-18 021850.png', 'crash_analysis_report.md')

$temporaryIndex = Join-Path -Path ([System.IO.Path]::GetTempPath()) `
	-ChildPath ("terraengine-spec043-$RunId-$([Guid]::NewGuid().ToString('N')).index")
$temporaryRef = "refs/codex-validation/spec043-$RunId-$([Guid]::NewGuid().ToString('N'))"
$previousIndex = [Environment]::GetEnvironmentVariable('GIT_INDEX_FILE', 'Process')
$refCreated = $false
try {
	[Environment]::SetEnvironmentVariable('GIT_INDEX_FILE', $temporaryIndex, 'Process')
	$baseCommit = Invoke-Git @('rev-parse', 'HEAD') $repoRoot
	[void](Invoke-Git @('read-tree', $baseCommit) $repoRoot)
	# Stage every tracked edit/deletion regardless of directory, then admit only
	# explicitly selected untracked roots. The user's real index is never opened.
	[void](Invoke-Git @('add', '-u', '--', '.') $repoRoot)
	foreach($relative in $includedUntrackedRoots) {
		if(Test-Path -LiteralPath (Join-Path $repoRoot $relative)) {
			[void](Invoke-Git @('add', '-A', '--', $relative) $repoRoot)
		}
	}

	$capturedPathsText = Invoke-Git @('diff', '--cached', '--name-only', '--diff-filter=ACDMRTUXB') $repoRoot
	$capturedPaths = @($capturedPathsText -split "`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
	foreach($path in $capturedPaths) {
		foreach($excluded in $excludedLocalRoots) {
			if($path -eq $excluded -or $path.StartsWith($excluded + '/',
				[System.StringComparison]::OrdinalIgnoreCase)) {
				throw "Temporary snapshot unexpectedly captured excluded local path '$path'."
			}
		}
	}

	$tree = Invoke-Git @('write-tree') $repoRoot
	# Pass every commit-tree argument through the checked Git helper. Building the
	# argument vector explicitly also keeps Windows PowerShell from treating a
	# continuation-line "-c" token as a new command.
	$commit = Invoke-Git @(
		'-c', 'user.name=TerraEngine Validation',
		'-c', 'user.email=validation@terraengine.invalid',
		'commit-tree', $tree,
		'-p', $baseCommit,
		'-m', "Spec 043 isolated validation snapshot $RunId"
	) $repoRoot
	[void](Invoke-Git @('update-ref', $temporaryRef, $commit) $repoRoot)
	$refCreated = $true
	[void](Invoke-Git @('bundle', 'create', $bundlePath, $temporaryRef) $repoRoot)

	# The temporary index is only for constructing the snapshot commit. Worktree
	# creation must let Git select each new worktree's own index; otherwise the
	# materialized files appear deleted/untracked after this script removes the
	# temporary index. The caller's prior environment value is restored in finally.
	[Environment]::SetEnvironmentVariable('GIT_INDEX_FILE', $null, 'Process')
	if($MaterializeWorktree) {
		[void](Invoke-Git @('worktree', 'add', '--detach', $worktreePath, $commit) $repoRoot)
		if($InitializeSubmodulesFromLocalObjects) {
			Add-LocalSubmoduleWorktrees $repoRoot $worktreePath
		}
		$status = Invoke-Git @('status', '--porcelain=v1', '--untracked-files=all') $worktreePath
		if(-not [string]::IsNullOrWhiteSpace($status)) {
			throw "Materialized worktree is not clean:`n$status"
		}
	}

	$submoduleStatus = Invoke-Git @('submodule', 'status', '--recursive') $repoRoot
	$manifest = [ordered]@{
		schemaVersion = 1
		feature = '043-complete-animation-system'
		runId = $RunId
		createdAt = (Get-Date).ToString('o')
		baseCommit = $baseCommit
		snapshotCommit = $commit
		snapshotTree = $tree
		branchAndIndexChanged = $false
		bundle = Get-RepoRelativePath $repoRoot $bundlePath
		bundleSha256 = (Get-FileHash -LiteralPath $bundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
		materializedWorktree = if($MaterializeWorktree) {
			Get-RepoRelativePath $repoRoot $worktreePath
		} else { $null }
		localSubmodulesInitialized = [bool]($MaterializeWorktree -and $InitializeSubmodulesFromLocalObjects)
		includedUntrackedRoots = $includedUntrackedRoots
		excludedLocalRoots = $excludedLocalRoots
		capturedChangeCount = $capturedPaths.Count
		capturedChanges = $capturedPaths
		submoduleStatus = @($submoduleStatus -split "`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
	}
	Write-Json $manifestPath $manifest
	Write-Output $manifest
}
finally {
	if($refCreated) {
		try { [void](Invoke-Git @('update-ref', '-d', $temporaryRef) $repoRoot) } catch { Write-Warning $_.Exception.Message }
	}
	[Environment]::SetEnvironmentVariable('GIT_INDEX_FILE', $previousIndex, 'Process')
	if(Test-Path -LiteralPath $temporaryIndex) { Remove-Item -LiteralPath $temporaryIndex -Force }
}
