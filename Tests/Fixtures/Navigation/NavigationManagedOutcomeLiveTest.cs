// NavigationManagedOutcomeLiveTest.cs - caller-visible managed navigation outcomes.
//
// Attach this script to the existing Navigation Agent live fixture object. It
// uses only the public managed facades and never edits transforms or terrain.
// The explicitly authored stale-agent marker opts into component removal through
// the normal deferred API. Cleanup cancels active direct requests before releasing
// their results; release alone intentionally rejects a still-active request.
//
// Optional acceptance targets are discovered by name:
// Navigation Managed Missing Coverage Target (exact -> UnavailableCoverage/
// MissingNeighbor), Navigation Managed Unreachable Target (exact ->
// Unreachable/MissingSupport), Navigation Managed Partial Target (policy 1 ->
// Partial/DomainLimit), and Navigation Managed Budget Target (exact ->
// BudgetExhausted/OutputLimit), and Navigation Managed Provider Failure Target
// (exact -> Failed/ProviderFailure). The budget target requires the project to
// be configured with maxRoutePoints=1 for test startup; no managed API changes
// that setting.
// The provider-failure target uses the normal reachable placement but requests
// a valid profile whose tiny voxel size makes Recast reject its grid dimensions
// during deferred preparation.
// Two further opt-in marker objects enable facade lifecycle probes:
// Navigation Managed Stale Agent owns a NavigationAgentComponent that this
// script removes through Object.RemoveComponent, while Navigation Managed
// Capacity Probe enables 8193 Stop submissions against the bounded queue.

using System;
using TerraEngine;

public sealed class NavigationManagedOutcomeLiveTest : Script
{
	private NavigationAgent? _agent;
	private TerraEngine.Object? _target;
	private TerraEngine.Object? _missingCoverageTarget;
	private TerraEngine.Object? _unreachableTarget;
	private TerraEngine.Object? _partialTarget;
	private TerraEngine.Object? _budgetTarget;
	private TerraEngine.Object? _providerFailureTarget;
	private TerraEngine.Object? _staleAgentObject;
	private TerraEngine.Object? _capacityProbeMarker;
	private NavigationAgent? _staleAgent;
	private NavigationRequestHandle _request;
	private NavigationRequestHandle _optionalRequest;
	private bool _probed;
	private bool _cancelIssued;
	private bool _optionalComplete;
	private bool _finished;
	private bool _allPass = true;
	private bool _providerPreparationObserved;
	private int _optionalStage;
	private int _agentProbeStage;
	private float _elapsed;
	private float _optionalElapsed;

	public override void OnStart()
	{
		_agent = Object.GetComponent<NavigationAgent>();
		_target = TerraEngine.Object.Find("Navigation Agent Target");
		// These names are deliberately optional. The ordinary live fixture keeps
		// its existing scene unchanged; an acceptance scene opts into one or more
		// caller-visible terminal outcome cases by authoring the named target.
		_missingCoverageTarget = TerraEngine.Object.Find("Navigation Managed Missing Coverage Target");
		_unreachableTarget = TerraEngine.Object.Find("Navigation Managed Unreachable Target");
		_partialTarget = TerraEngine.Object.Find("Navigation Managed Partial Target");
		_budgetTarget = TerraEngine.Object.Find("Navigation Managed Budget Target");
		_providerFailureTarget = TerraEngine.Object.Find("Navigation Managed Provider Failure Target");
		_staleAgentObject = TerraEngine.Object.Find("Navigation Managed Stale Agent");
		_capacityProbeMarker = TerraEngine.Object.Find("Navigation Managed Capacity Probe");
		if(_agent == null || _target == null)
		{
			Log.Info("NAVMANAGED FAIL missing NavigationAgent or Navigation Agent Target");
			_finished = true;
		}
	}

	public override void OnUpdate()
	{
		if(_finished) return;
		_elapsed += Time.DeltaTime;
		if(_elapsed > 15.0F)
		{
			Log.Info("NAVMANAGED FAIL timeout");
			_allPass = false;
			ReleaseOwnedRequest();
			_finished = true;
			return;
		}
		if(_agent == null || _target == null) return;

		if(!_probed)
		{
			// TryBind is intentionally reached through the public property. Before
			// protected publication the wrapper may not have a valid identity yet.
			if(!_agent.Identity.IsValid) return;
			NavigationProfile invalidProfile = new();
			NavigationRouteResult invalidProfileResult = Navigation.RequestRoute(Object, _target, invalidProfile, correlationId: 91001);
			Log.Info($"NAVMANAGED INVALID_PROFILE status={invalidProfileResult.Status} diagnostic={invalidProfileResult.Diagnostic}");

			NavigationRouteResult invalidEndpointResult = Navigation.RequestRoute(Object, new NavigationLocation(),
				DefaultProfile(), correlationId: 91002);
			Log.Info($"NAVMANAGED INVALID_ENDPOINT status={invalidEndpointResult.Status} diagnostic={invalidEndpointResult.Diagnostic}");

			NavigationAgentReceipt unknownReceipt = _agent.ReadReceipt(ulong.MaxValue);
			Log.Info($"NAVMANAGED UNKNOWN_RECEIPT state={unknownReceipt.State} outcome={unknownReceipt.Outcome}");
			bool invalidProfilePass = invalidProfileResult.Status == NavigationStatus.InvalidArgument
				&& invalidProfileResult.Diagnostic.Contains("InvalidProfile", StringComparison.Ordinal);
			bool invalidEndpointPass = invalidEndpointResult.Status == NavigationStatus.InvalidArgument
				&& invalidEndpointResult.Diagnostic.Contains("InvalidDestination", StringComparison.Ordinal);
			bool unknownReceiptPass = unknownReceipt.State == NavigationAgentReceiptState.Expired
				&& unknownReceipt.Outcome == NavigationOutcome.Expired;
			_allPass &= invalidProfilePass && invalidEndpointPass && unknownReceiptPass;
			Log.Info($"NAVMANAGED {(invalidProfilePass && invalidEndpointPass && unknownReceiptPass ? "PASS" : "FAIL")} initial outcomes");
			_probed = true;
		}

		if(!RunOptionalAgentProbes()) return;

		if(!_optionalComplete)
		{
			if(!RunOptionalOutcomeScenario()) return;
		}

		if(!_cancelIssued)
		{
			NavigationRouteResult submitted = Navigation.RequestRoute(Object, _target, DefaultProfile(), correlationId: 91003);
			if(!submitted.Request.IsValid) return;
			_request = submitted.Request;
			NavigationStatus cancelStatus = Navigation.CancelRoute(_request);
			Log.Info($"NAVMANAGED CANCEL status={cancelStatus}");
			if(cancelStatus != NavigationStatus.Success)
			{
				Log.Info("NAVMANAGED FAIL cancellation was not accepted");
				_allPass = false;
				ReleaseOwnedRequest();
				_finished = true;
				return;
			}
			_cancelIssued = true;
			return;
		}

		// Read on the following frame, rather than immediately after CancelRoute,
		// to prove the deferred managed visibility contract.
		NavigationRouteResult cancelled = Navigation.ReadRequest(_request);
		bool pass = cancelled.Outcome == NavigationOutcome.Cancelled
			&& cancelled.Reason == NavigationReason.Cancelled;
		_allPass &= pass;
		Log.Info($"NAVMANAGED CANCEL_READ status={cancelled.Status} outcome={cancelled.Outcome} reason={cancelled.Reason}");
		Log.Info($"NAVMANAGED {(_allPass ? "PASS" : "FAIL")} managed navigation outcomes");
		ReleaseOwnedRequest();
		_finished = true;
	}

	private bool RunOptionalAgentProbes()
	{
		if(_agentProbeStage == 0)
		{
			if(_capacityProbeMarker != null)
			{
				int rejectedCapacity = 0;
				for(int index = 0; index < 8193; ++index)
				{
					NavigationAgentReceipt receipt = _agent!.Stop();
					if(receipt.State == NavigationAgentReceiptState.Rejected
						&& receipt.Outcome == NavigationOutcome.RejectedCapacity
						&& receipt.Reason == NavigationReason.OutputLimit)
						++rejectedCapacity;
				}
				bool pass = rejectedCapacity > 0;
				_allPass &= pass;
				Log.Info($"NAVMANAGED CAPACITY rejected={rejectedCapacity} expected=RejectedCapacity/OutputLimit");
				Log.Info($"NAVMANAGED {(pass ? "PASS" : "FAIL")} capacity outcome");
			}
			_agentProbeStage = 1;
		}

		if(_agentProbeStage == 1)
		{
			if(_staleAgentObject == null)
			{
				_agentProbeStage = 3;
				return true;
			}
			_staleAgent = _staleAgentObject.GetComponent<NavigationAgent>();
			if(_staleAgent == null || !_staleAgent.Identity.IsValid)
			{
				Log.Info("NAVMANAGED FAIL stale-agent marker has no bound NavigationAgent");
				_allPass = false;
				_agentProbeStage = 3;
				return true;
			}
			if(!_staleAgentObject.RemoveComponent<NavigationAgent>())
			{
				Log.Info("NAVMANAGED FAIL stale-agent component removal was rejected");
				_allPass = false;
				_agentProbeStage = 3;
				return true;
			}
			_agentProbeStage = 2;
			return false;
		}

		if(_agentProbeStage == 2)
		{
			NavigationAgentStatus stale = _staleAgent!.Status;
			bool pass = stale.Outcome == NavigationOutcome.Stale
				&& stale.Reason == NavigationReason.SceneReplaced;
			_allPass &= pass;
			Log.Info($"NAVMANAGED STALE status={stale.State} outcome={stale.Outcome} reason={stale.Reason}");
			Log.Info($"NAVMANAGED {(pass ? "PASS" : "FAIL")} stale-agent outcome");
			_agentProbeStage = 3;
		}
		return true;
	}

	private bool RunOptionalOutcomeScenario()
	{
		if(_optionalRequest.IsValid)
		{
			// Keep one deadline per optional stage, including admission retries. A
			// transient stale root must not receive a fresh timeout on every resubmit.
			if(AdvanceOptionalDeadline()) return false;
			NavigationRouteResult result = Navigation.ReadRequest(_optionalRequest);
			if(_optionalStage == 4 && result.Outcome == NavigationOutcome.PendingPreparation
				&& result.Reason == NavigationReason.DirtyCoverage)
			{
				_providerPreparationObserved = true;
			}
			if(IsTransientOptionalRetry(result))
			{
				Log.Info($"NAVMANAGED OPTIONAL retry stage={_optionalStage} result={result.Outcome}/{result.Reason}");
				ReleaseOptionalRequest();
				return false;
			}
			if(IsOptionalReadinessWait(result))
			{
				Log.Info($"NAVMANAGED OPTIONAL readiness-wait stage={_optionalStage} result={result.Outcome}/{result.Reason}");
				ReleaseOptionalRequest();
				return false;
			}
			if(result.Lifecycle != NavigationRequestLifecycle.Terminal) return false;
			NavigationOutcome expectedOutcome;
			NavigationReason expectedReason;
			ExpectedOptionalOutcome(out expectedOutcome, out expectedReason);
			bool providerPreparationPass = _optionalStage != 4 || _providerPreparationObserved;
			bool partialRoutePass = true;
			if(_optionalStage == 2)
			{
				// A Partial/NearestReachable result is useful only when the public
				// facade retained the reachable corridor. Compare the final copied
				// point with the authored target instead of assuming fixture geometry.
				partialRoutePass = result.Points.Length != 0;
				if(partialRoutePass && _partialTarget != null)
				{
					NavigationLocation finalPoint = result.Points[result.Points.Length - 1];
					WorldPosition targetPosition = _partialTarget.Transform.Position;
					const double worldCellEdgeMetres = 65536.0;
					double finalX = finalPoint.CellX * worldCellEdgeMetres + finalPoint.OffsetX;
					double finalZ = finalPoint.CellZ * worldCellEdgeMetres + finalPoint.OffsetZ;
					double targetX = targetPosition.Cell.X * worldCellEdgeMetres + targetPosition.OffsetX;
					double targetZ = targetPosition.Cell.Z * worldCellEdgeMetres + targetPosition.OffsetZ;
					double dx = finalX - targetX;
					double dz = finalZ - targetZ;
					partialRoutePass = (dx * dx + dz * dz) > 0.25;
				}
			}
			bool pass = result.Outcome == expectedOutcome && result.Reason == expectedReason
				&& providerPreparationPass && partialRoutePass;
			_allPass &= pass;
			Log.Info($"NAVMANAGED OPTIONAL stage={_optionalStage} status={result.Status} outcome={result.Outcome} reason={result.Reason}");
			Log.Info($"NAVMANAGED {(pass ? "PASS" : "FAIL")} optional outcome expected={expectedOutcome}/{expectedReason} routePoints={result.Points.Length} partialRoute={partialRoutePass}");
			ReleaseOptionalRequest();
			_optionalElapsed = 0.0F;
			_optionalStage++;
			return false;
		}

		while(_optionalStage < 5 && OptionalTarget(_optionalStage) == null) ++_optionalStage;
		if(_optionalStage >= 5)
		{
			_optionalComplete = true;
			return true;
		}
		TerraEngine.Object? target = OptionalTarget(_optionalStage);
		if(target == null) return false;
		if(AdvanceOptionalDeadline()) return false;
		// Stages 0/1/3/4 use exact destination; stage 2 opts into
		// NearestReachable for an explicit Partial result.
		uint routePolicy = _optionalStage == 2 ? 1U : 0U;
		NavigationProfile profile = _optionalStage == 4 ? ProviderFailureProfile() : DefaultProfile();
		NavigationRouteResult submitted = Navigation.RequestRoute(Object, target, profile, routePolicy,
			replacementKey: 0, correlationId: (ulong)(92000 + _optionalStage));
		if(_optionalStage == 4 && submitted.Request.IsValid)
		{
			NavigationRouteResult pending = Navigation.ReadRequest(submitted.Request);
			bool pendingPass = pending.Outcome == NavigationOutcome.PendingPreparation
				&& pending.Reason == NavigationReason.DirtyCoverage;
			_providerPreparationObserved |= pendingPass;
			if(!pendingPass && !IsTransientOptionalRetry(pending)
				&& !IsOptionalReadinessWait(pending) && pending.Outcome != NavigationOutcome.Queued) _allPass = false;
			Log.Info($"NAVMANAGED PROVIDER_PREPARATION status={pending.Status} lifecycle={pending.Lifecycle} outcome={pending.Outcome} reason={pending.Reason}");
		}
		if(!submitted.Request.IsValid)
		{
			if(IsTransientOptionalRetry(submitted))
			{
				Log.Info($"NAVMANAGED OPTIONAL retry stage={_optionalStage} admission={submitted.Outcome}/{submitted.Reason}");
				return false;
			}
			if(IsOptionalReadinessWait(submitted))
			{
				Log.Info($"NAVMANAGED OPTIONAL readiness-wait stage={_optionalStage} admission={submitted.Outcome}/{submitted.Reason}");
				return false;
			}
			Log.Info($"NAVMANAGED FAIL optional stage={_optionalStage} submit status={submitted.Status} diagnostic={submitted.Diagnostic}");
			_allPass = false;
			_optionalElapsed = 0.0F;
			_optionalStage++;
			return false;
		}
		_optionalRequest = submitted.Request;
		return false;
	}

	private TerraEngine.Object? OptionalTarget(int stage)
	{
		return stage == 0 ? _missingCoverageTarget : stage == 1 ? _unreachableTarget
			: stage == 2 ? _partialTarget : stage == 3 ? _budgetTarget : _providerFailureTarget;
	}

	private void ExpectedOptionalOutcome(out NavigationOutcome outcome, out NavigationReason reason)
	{
		if(_optionalStage == 0)
		{
			outcome = NavigationOutcome.UnavailableCoverage;
			reason = NavigationReason.MissingNeighbor;
		}
		else if(_optionalStage == 1)
		{
			outcome = NavigationOutcome.Unreachable;
			reason = NavigationReason.MissingSupport;
		}
		else if(_optionalStage == 2)
		{
			outcome = NavigationOutcome.Partial;
			reason = NavigationReason.DomainLimit;
		}
		else if(_optionalStage == 3)
		{
			outcome = NavigationOutcome.BudgetExhausted;
			reason = NavigationReason.OutputLimit;
		}
		else
		{
			outcome = NavigationOutcome.Failed;
			reason = NavigationReason.ProviderFailure;
		}
	}

	private static bool IsTransientOptionalRetry(NavigationRouteResult result)
	{
		// Coverage may still be replacing a stale root when an optional target is
		// first queried. This is retryable probe setup, not evidence for a terminal
		// Stale outcome; retain the stage and its existing bounded timeout.
		return result.Outcome == NavigationOutcome.Stale
			&& result.Reason == NavigationReason.DirtyCoverage;
	}

	private static bool IsOptionalReadinessWait(NavigationRouteResult result)
	{
		// A newly launched fixture can briefly report missing terrain while its
		// authored terrain publication is still crossing the protected boundary.
		// This is setup retry only; the expected MissingNeighbor/MissingSupport
		// terminal reasons remain exact once the scene is ready.
		return result.Outcome == NavigationOutcome.UnavailableCoverage
			&& result.Reason == NavigationReason.MissingTerrain;
	}

	private bool AdvanceOptionalDeadline()
	{
		_optionalElapsed += Time.DeltaTime;
		if(_optionalElapsed <= 5.0F) return false;
		Log.Info($"NAVMANAGED FAIL optional stage={_optionalStage} timeout");
		_allPass = false;
		ReleaseOptionalRequest();
		_optionalElapsed = 0.0F;
		_optionalStage++;
		return true;
	}

	private void ReleaseOptionalRequest()
	{
		if(!_optionalRequest.IsValid) return;
		Navigation.CancelRoute(_optionalRequest);
		Navigation.ReleaseResult(_optionalRequest);
		_optionalRequest = default;
	}

	private static NavigationProfile ProviderFailureProfile()
	{
		NavigationProfile profile = DefaultProfile();
		profile.HorizontalResolutionMetres = .001;
		return profile;
	}

	private static NavigationProfile DefaultProfile()
	{
		return new NavigationProfile
		{
			RadiusMetres = .4, HeightMetres = 1.8, StepHeightMetres = .35,
			MaxSlopeDegrees = 45, ClearanceMarginMetres = .01,
			HorizontalResolutionMetres = .1, VerticalResolutionMetres = .05
		};
	}

	private void ReleaseOwnedRequest()
	{
		if(_request.IsValid)
		{
			// Terminal cancellation may be rejected harmlessly; release still owns
			// reclamation. An active request first becomes terminal through CancelRoute.
			Navigation.CancelRoute(_request);
			Navigation.ReleaseResult(_request);
			_request = default;
		}
		if(_optionalRequest.IsValid)
		{
			Navigation.CancelRoute(_optionalRequest);
			Navigation.ReleaseResult(_optionalRequest);
			_optionalRequest = default;
		}
	}

	public override void OnDisable() => ReleaseOwnedRequest();
	public override void OnDestroy() => ReleaseOwnedRequest();
}
