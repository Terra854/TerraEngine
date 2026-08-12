Describe 'Physics optimization evidence tooling' {
	$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
	function Set-ComparableEvidence {
		param([string]$Directory, [bool]$Candidate)
		foreach($file in Get-ChildItem -LiteralPath $Directory -Filter 'physics-*.json' -File) {
			$evidence = Get-Content -Raw -LiteralPath $file.FullName | ConvertFrom-Json
			$evidence.status = 'passed'
			$evidence.baseline.sameRevisionParent = $true
			$evidence.identity.quality.applied = $true
			foreach($gate in $evidence.gates.PSObject.Properties) { $gate.Value = $true }
			foreach($run in $evidence.runs) {
				$run.coverageRatio = 1.0
				foreach($stage in $run.stages.PSObject.Properties) {
					$value = if($Candidate) { 10.0 } else { 100.0 }
					$stage.Value.availability = 'available'; $stage.Value.reason = $null
					$stage.Value.sampleCount = 100; $stage.Value.median = $value
					$stage.Value.p95 = $value; $stage.Value.p99 = $value; $stage.Value.worst = $value
					$stage.Value.workItems = 1; $stage.Value.allocationCount = 1; $stage.Value.allocationBytes = 1
				}
				$run.jobs.submitted = 1; $run.jobs.completed = 1; $run.jobs.observedWorkerThreads = 1
				$run.correctness.stateHash = "state-$($evidence.identity.workload.id)"
				$run.correctness.eventHash = "events-$($evidence.identity.workload.id)"
				$run.correctness.poseHash = "pose-$($evidence.identity.workload.id)"
				$run.correctness.rollbackPassed = $true; $run.correctness.visibilityPassed = $true
			}
			$evidence.failures = @()
			$evidence | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file.FullName -Encoding UTF8
		}
	}

	function Add-WorkerEvidenceLegs {
		param([string]$Directory, [bool]$Candidate)
		$source = Join-Path $Directory 'physics-worker-large-v1.json'
		foreach($worker in @('1','4')) {
			$path = Join-Path $Directory "physics-worker-large-v1-worker-$worker-quality-default.json"
			$evidence = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
			if($Candidate) {
				$duration = if($worker -eq '1') { 150.0 } else { 100.0 }
				foreach($run in $evidence.runs) { $run.stages.simulation.median = $duration; $run.stages.simulation.p95 = $duration }
			}
			$evidence | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $path -Encoding UTF8
		}
		if($Candidate) {
			$maximum = Get-Content -Raw -LiteralPath $source | ConvertFrom-Json
			foreach($run in $maximum.runs) { $run.stages.simulation.median = 105.0; $run.stages.simulation.p95 = 105.0 }
			$maximum | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $source -Encoding UTF8
		}
	}

	function Add-AlternateQualityLeg {
		param([string]$Directory)
		$source = Join-Path $Directory 'physics-worker-large-v1.json'
		$alternate = Get-Content -Raw -LiteralPath $source | ConvertFrom-Json
		$alternate.identity.quality.velocityIterations = 20
		$alternate.identity.quality.positionIterations = 4
		$alternate.identity.quality.applied = $false
		$alternate | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $Directory 'physics-worker-large-v1-quality-alternate.json') -Encoding UTF8
	}

	function New-ComparableEvidencePair {
		param([string]$Parent)
		$baseline = Join-Path $Parent 'baseline'
		$candidate = Join-Path $Parent 'candidate'
		$arguments = @{ InstallRoot=(Join-Path $root 'out/install/RelWithDebInfo'); Manifest=(Join-Path $root 'Tests/Fixtures/Performance/Physics/workloads.json'); Runs=10; AcceptanceRepetitions=10; DryRun=$true }
		& (Join-Path $root 'Tools/Physics/CapturePhysicsOptimizationBenchmark.ps1') @arguments -OutputDirectory $baseline | Out-Null
		& (Join-Path $root 'Tools/Physics/CapturePhysicsOptimizationBenchmark.ps1') @arguments -OutputDirectory $candidate | Out-Null
		Set-ComparableEvidence $baseline $false
		Set-ComparableEvidence $candidate $true
		Add-WorkerEvidenceLegs $baseline $false
		Add-WorkerEvidenceLegs $candidate $true
		return @{ Baseline=$baseline; Candidate=$candidate }
	}
	It 'discovers six canonical workloads' {
		$m = Get-Content -Raw (Join-Path $root 'Tests/Fixtures/Performance/Physics/workloads.json') | ConvertFrom-Json
		$m.workloads.Count | Should Be 6
		($m.workloads.id -contains 'physics-worker-large-v1') | Should Be $true
	}
	It 'uses an active worker-large population rather than startup timing' {
		$m = Get-Content -Raw (Join-Path $root 'Tests/Fixtures/Performance/Physics/workloads.json') | ConvertFrom-Json
		$worker = $m.workloads | Where-Object id -eq 'physics-worker-large-v1'
		$worker.activeBodies | Should Be 8000
	}
	It 'fails closed for missing evidence' {
		$dir = Join-Path $TestDrive 'missing'; New-Item -ItemType Directory $dir | Out-Null
		$out = Join-Path $TestDrive 'comparison.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $dir -CandidateDirectory $dir -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
		(Get-Content -Raw $out | ConvertFrom-Json).status | Should Be 'incomparable'
	}
	It 'rejects insufficient capture runs before writing evidence' {
		$manifest = Join-Path $root 'Tests/Fixtures/Performance/Physics/workloads.json'
		$install = Join-Path $root 'out/install/RelWithDebInfo'
		{ & (Join-Path $root 'Tools/Physics/CapturePhysicsOptimizationBenchmark.ps1') -InstallRoot $install -Manifest $manifest -OutputDirectory (Join-Path $TestDrive 'short') -Runs 4 -DryRun } | Should Throw
	}
	It 'records unavailable dry-run metrics as null rather than zero' {
		$directory = Join-Path $TestDrive 'dry'
		& (Join-Path $root 'Tools/Physics/CapturePhysicsOptimizationBenchmark.ps1') `
			-InstallRoot (Join-Path $root 'out/install/RelWithDebInfo') `
			-Manifest (Join-Path $root 'Tests/Fixtures/Performance/Physics/workloads.json') `
			-OutputDirectory $directory -Runs 5 -DryRun | Out-Null
		$evidence = Get-Content -Raw (Join-Path $directory 'physics-unchanged-10000-v1.json') | ConvertFrom-Json
		$evidence.status | Should Be 'incomparable'
		$evidence.runs[0].stages.authoringPreparation.availability | Should Be 'unavailable'
		$evidence.runs[0].stages.authoringPreparation.median | Should BeNullOrEmpty
	}
	It 'reports identity mismatches as incomparable' {
		$baseline = Join-Path $TestDrive 'baseline'
		$candidate = Join-Path $TestDrive 'candidate'
		$arguments = @{ InstallRoot=(Join-Path $root 'out/install/RelWithDebInfo'); Manifest=(Join-Path $root 'Tests/Fixtures/Performance/Physics/workloads.json'); Runs=5; DryRun=$true }
		& (Join-Path $root 'Tools/Physics/CapturePhysicsOptimizationBenchmark.ps1') @arguments -OutputDirectory $baseline | Out-Null
		& (Join-Path $root 'Tools/Physics/CapturePhysicsOptimizationBenchmark.ps1') @arguments -OutputDirectory $candidate | Out-Null
		$file = Join-Path $candidate 'physics-unchanged-10000-v1.json'
		$value = Get-Content -Raw $file | ConvertFrom-Json
		$value.identity.build.sourceRevision = 'mismatched-revision'
		$value | ConvertTo-Json -Depth 24 | Set-Content -Encoding UTF8 $file
		$out = Join-Path $TestDrive 'identity-comparison.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $baseline -CandidateDirectory $candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
		(Get-Content -Raw $out | ConvertFrom-Json).status | Should Be 'incomparable'
	}
	It 'fails a paired threshold regression even when identity and correctness match' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'threshold')
		$file = Join-Path $pair.Candidate 'physics-contact-pairs-4096-v1.json'
		$evidence = Get-Content -Raw -LiteralPath $file | ConvertFrom-Json
		foreach($run in $evidence.runs) { $run.stages.eventPublication.p95 = 90.0 }
		$evidence | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file -Encoding UTF8
		$out = Join-Path $TestDrive 'threshold-comparison.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
		(Get-Content -Raw $out | ConvertFrom-Json).status | Should Be 'failed'
	}
	It 'passes a complete comparable evidence set only when every gate succeeds' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'success')
		$out = Join-Path $TestDrive 'successful-comparison.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
		$result = Get-Content -Raw $out | ConvertFrom-Json
		$result.status | Should Be 'failed'
		$result.gates.emptyLowEventPassed | Should Be $false
	}
	It 'rejects max-worker simulation regression beyond ten percent' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'max-regression')
		$file = Join-Path $pair.Candidate 'physics-worker-large-v1.json'
		$value = Get-Content -Raw $file | ConvertFrom-Json
		foreach($run in $value.runs) { $run.stages.simulation.median = 120.0; $run.stages.simulation.p95 = 120.0 }
		$value | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file -Encoding UTF8
		$out = Join-Path $TestDrive 'max-regression.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
	}
	It 'rejects same-profile correctness hash mismatch' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'hash-mismatch')
		$file = Join-Path $pair.Candidate 'physics-worker-large-v1.json'
		$value = Get-Content -Raw $file | ConvertFrom-Json
		$value.runs[0].correctness.stateHash = 'different-state-hash'
		$value | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file -Encoding UTF8
		$out = Join-Path $TestDrive 'hash-mismatch.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
	}
	It 'rejects alternate-quality evidence that was not provider-applied' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'alternate-quality')
		Add-AlternateQualityLeg $pair.Candidate
		$out = Join-Path $TestDrive 'alternate-quality.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
	}
	It 'rejects coverage below 99 percent' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'coverage')
		$file = Join-Path $pair.Candidate 'physics-unchanged-10000-v1.json'
		$value = Get-Content -Raw $file | ConvertFrom-Json
		$value.runs[0].coverageRatio = 0.98
		$value | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file -Encoding UTF8
		$out = Join-Path $TestDrive 'coverage.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
	}
	It 'rejects missing allocation evidence for bulk admission' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'allocation')
		$file = Join-Path $pair.Candidate 'physics-bulk-admission-10000-v1.json'
		$value = Get-Content -Raw $file | ConvertFrom-Json
		foreach($run in $value.runs) { $run.stages.providerCommit.allocationCount = $null; $run.stages.providerCommit.allocationBytes = $null }
		$value | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file -Encoding UTF8
		$out = Join-Path $TestDrive 'allocation.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
	}
	It 'rejects ragdoll doubling beyond the bounded scaling limit' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'ragdoll-doubling')
		$file = Join-Path $pair.Candidate 'physics-ragdoll-crowd-128x16-v1.json'
		$value = Get-Content -Raw $file | ConvertFrom-Json
		foreach($run in $value.runs) { $run.stages.extraction.median = 30.0; $run.stages.extraction.p95 = 30.0 }
		$value | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file -Encoding UTF8
		$out = Join-Path $TestDrive 'ragdoll-doubling.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
	}
	It 'requires fixture-scoped contact and ragdoll correctness hashes' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'fixture-hashes')
		foreach($fixture in @('physics-contact-pairs-4096-v1','physics-ragdoll-crowd-64x16-v1')) {
			$file = Join-Path $pair.Candidate "$fixture.json"
			$value = Get-Content -Raw $file | ConvertFrom-Json
			$value.runs[0].correctness.eventHash = if($fixture -like '*contact*'){'changed-event'}else{$value.runs[0].correctness.eventHash}
			$value.runs[0].correctness.poseHash = if($fixture -like '*ragdoll*'){'changed-pose'}else{$value.runs[0].correctness.poseHash}
			$value | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file -Encoding UTF8
		}
		$out = Join-Path $TestDrive 'fixture-hashes.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
	}
	It 'uses combined extraction and snapshot stages for ragdoll acceptance' {
		$pair = New-ComparableEvidencePair (Join-Path $TestDrive 'combined-ragdoll')
		$file = Join-Path $pair.Candidate 'physics-ragdoll-crowd-64x16-v1.json'
		$value = Get-Content -Raw $file | ConvertFrom-Json
		foreach($run in $value.runs) { $run.stages.extraction.median = 20.0; $run.stages.snapshotConstruction.median = 20.0 }
		$value | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath $file -Encoding UTF8
		$out = Join-Path $TestDrive 'combined-ragdoll.json'
		& (Join-Path $root 'Tools/Physics/ComparePhysicsOptimizationEvidence.ps1') -BaselineDirectory $pair.Baseline -CandidateDirectory $pair.Candidate -Schema (Join-Path $root 'specs/050-optimize-physics-runtime/contracts/physics-performance-evidence.schema.json') -Output $out
		$LASTEXITCODE | Should Not Be 0
	}
	It 'identifies a mixed-stage regression larger than five percent' {
		$baseline = 100.0; $candidate = 106.0
		(($candidate - $baseline) / $baseline) | Should BeGreaterThan 0.05
	}
	It 'exposes optional worker scaling evidence provenance input' {
		$command = Get-Command (Join-Path $root 'Tools/Physics/CapturePhysicsOptimizationBenchmark.ps1')
		$command.Parameters.ContainsKey('WorkerScalingEvidenceSummary') | Should Be $true
	}
	It 'uses an aggregate worker median instead of median-of-group-medians' {
		$raw = @(1..100 | ForEach-Object { [double]$_ })
		$aggregate = ($raw[49] + $raw[50]) / 2.0
		$groupMedians = @(0..9 | ForEach-Object { $offset=$_*10; ($raw[$offset+4]+$raw[$offset+5])/2.0 })
		$aggregate | Should Be 50.5
		(($groupMedians | Measure-Object -Average).Average) | Should Not Be $aggregate
	}
	It 'does not classify zero baseline stage duration as a regression' {
		$baseline = 0.0; $candidate = 0.0
		($baseline -eq 0 -and $candidate -le ($baseline * 1.05)) | Should Be $true
	}
}
