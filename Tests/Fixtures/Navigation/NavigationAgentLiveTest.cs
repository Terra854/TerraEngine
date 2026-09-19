// NavigationAgentLiveTest.cs - managed agent guidance and movement-ownership fixture.

using System;
using TerraEngine;

public sealed class NavigationAgentLiveTest : Script
{
	private NavigationAgent? _agent;
	private CharacterController? _controller;
	private TerraEngine.Object? _target;
	private ColliderSet? _ownerColliderSet;
	private bool _stopped;
	private ulong _movementCorrelation;
	private ulong _agentCorrelation;
	private bool _destinationPending;
	private bool _fixtureValid;

	public override void OnStart()
	{
		_agent = Object.GetComponent<NavigationAgent>();
		_controller = Object.GetComponent<CharacterController>();
		_target = TerraEngine.Object.Find("Navigation Agent Target");
		_ownerColliderSet = Object.GetComponent<ColliderSet>();
		if(_agent == null || _controller == null || _target == null) {
			Log.Info("NAVAGENT FAIL missing NavigationAgent, CharacterController, or target");
			return;
		}
		// The sample deliberately accepts only the conservative authored controller
		// geometry that matches the default agent profile.
		NavigationProfile defaultProfile = new() { RadiusMetres = .4, HeightMetres = 1.8, StepHeightMetres = .35, MaxSlopeDegrees = 45, ClearanceMarginMetres = .01 };
		if(_ownerColliderSet != null && _ownerColliderSet.Enabled || !NavigationRouteFollower.IsProfileCompatibleWithController(_controller, defaultProfile)) {
			Log.Info("NAVAGENT FAIL controller geometry is incompatible with the default profile");
			return;
		}
		_fixtureValid = true;
		// The fixture uses the authored defaults (.4 radius, 1.8 height, .35 step,
		// 45 degree slope, .01 clearance). The agent supplies guidance only; the
		// CharacterController remains the sole movement and transform authority.
		_destinationPending = true;
		TrySubmitDestination();
	}

	public override void OnUpdate()
	{
		if(!_fixtureValid || _agent == null || _controller == null) return;
		TrySubmitDestination();
		NavigationAgentGuidance guidance = _agent.Guidance;
		if(!_stopped && guidance.Usable && guidance.State == NavigationAgentState.Following) {
			_controller.SetDesiredVelocity(guidance.SuggestedDirection, correlationId: ++_movementCorrelation);
			return;
		}
		// No route, stopped, arrived, failed, or invalidated guidance always clears
		// this controller's persistent intent before the next protected physics edge.
		_controller.SetDesiredVelocity(default, correlationId: ++_movementCorrelation);
		if(!_stopped && _agent.Status.State == NavigationAgentState.Arrived) {
			_agent.Stop();
			_stopped = true;
			Log.Info($"NAVAGENT ARRIVED receipt={_agent.ReadReceipt(_agentCorrelation).State}");
		}
	}

	private void TrySubmitDestination()
	{
		if(!_destinationPending || _agent == null || _target == null || _stopped) return;
		NavigationAgentReceipt receipt = _agent.SetDestination(_target);
		if(receipt.CorrelationId != 0) _agentCorrelation = receipt.CorrelationId;
		_destinationPending = !receipt.Accepted;
		if(!receipt.Accepted) return;
		Log.Info($"NAVAGENT DESTINATION receipt={receipt.State}");
	}

	public override void OnDisable()
	{
		_fixtureValid = false;
		_controller?.SetDesiredVelocity(default, correlationId: ++_movementCorrelation);
		_agent?.Stop();
	}

	public override void OnDestroy()
	{
		_fixtureValid = false;
		_controller?.SetDesiredVelocity(default, correlationId: ++_movementCorrelation);
		_agent?.Stop();
	}
}
