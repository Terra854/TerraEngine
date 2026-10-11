<#
Shared installed workload selection. Later optimizations cannot block an earlier
story gate, while every fault applicable to that stage remains required. This
selects existing frozen definitions and never rewrites the manifest or counts.
#>
function GetRendererEfficiencyStageSteps($Workload, [string]$Stage) {
	if ($Stage -notin @('P1','P2','P3','P4','P5')) { throw 'A concrete renderer stage is required' }
	if ($Workload.stage -ne 'All') {
		if ($Workload.stage -ne $Stage) { throw 'Workload belongs to another renderer stage' }
		return @($Workload.steps)
	}
	if ($Workload.id -eq 'multiview-lights') { return @($Workload.steps) }
	if ($Workload.id -ne 'recovery-capacity') { throw 'Unknown shared renderer workload' }
	$actions = @('recorded-submit-abort','incomplete-point-faces','resource-recreation','device-recovery','transaction-conflict-rollback')
	if ($Stage -in @('P2','P3','P4','P5')) { $actions += 'buffer-overflow' }
	if ($Stage -in @('P4','P5')) { $actions += @('group-overflow','unsupported-grouped-route') }
	if ($Stage -eq 'P5') { $actions += @('unsupported-depth-transfer','zero-budget','too-small-budget','whole-cube-admission','lower-budget-in-flight','retirement-view-churn') }
	$steps = @($Workload.steps | Where-Object { $_.action -in $actions })
	if ($steps.Count -ne $actions.Count) { throw 'Frozen shared workload is missing required stage actions' }
	return $steps
}
