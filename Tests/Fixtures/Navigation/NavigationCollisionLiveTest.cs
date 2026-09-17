using System;
using TerraEngine;

// Live regression: attach to Navigation Duck, configured with a .4m-radius,
// 1.2m-height CharacterController centered .6m above its feet. Disable the duck's
// separate ColliderSet. The nonstatic Navigation Obstacle has a kinematic box
// body at (22,1,40), scale (4,2,10). Navigation Target is at (35,0,20).
// Add Navigation Blocker Duck at (28,0,20) with the same controller and no
// separate collider. Terrain spans this cell-zero fixture. No actor Transform
// writes are allowed; the second duck also moves only through its controller.
public class NavigationLiveTest : Script
{
	private NavigationRouteFollower follower;
	private Rigidbody blocker;
	private CharacterController characterBlocker;
	private float characterBlockedSeconds;
	private bool characterCleared, characterStopped, characterBlockedLogged;
	private float elapsed;
	private float nextDiagnostic = 5;
	private float blockedSeconds;
	private bool begun, entered, cleared, finished, blockedLogged;
	private bool blockerHeld, blockerStopped;
	private NavigationRouteFollowerState lastState;

	public override void OnStart()
	{
		var controller = Object.GetComponent<CharacterController>();
		var obstacle = TerraEngine.Object.Find("Navigation Obstacle");
		blocker = obstacle?.GetComponent<Rigidbody>();
		characterBlocker = TerraEngine.Object.Find("Navigation Blocker Duck")?.GetComponent<CharacterController>();
		if (controller == null || blocker == null || characterBlocker == null)
		{
			Log.Info("NAVCOLL FAIL missing controller or kinematic blocker");
			finished = true;
			return;
		}
		follower = new NavigationRouteFollower(controller, new NavigationProfile
		{
			RadiusMetres = .4, HeightMetres = 1.2, StepHeightMetres = .3,
			MaxSlopeDegrees = 45, ClearanceMarginMetres = .05,
			HorizontalResolutionMetres = .25, VerticalResolutionMetres = .1
		}) { Speed = 3f };
		Log.Info("NAVCOLL START collision-resolved route follower");
	}

	private void Fail(string reason)
	{
		Log.Info("NAVCOLL FAIL " + reason);
		follower?.Stop();
		finished = true;
	}

	public override void OnUpdate()
	{
		if (finished) return;
		// Correct physics positions are insufficient if publication silently resizes
		// the imported meshes. Both authored duck objects use a uniform .01 scale.
		var meshScale = Transform.Scale;
		var blockerScale = characterBlocker.Object.Transform.Scale;
		if (Math.Abs(meshScale.X - .01f) > .00001f || Math.Abs(meshScale.Y - .01f) > .00001f || Math.Abs(meshScale.Z - .01f) > .00001f
			|| Math.Abs(blockerScale.X - .01f) > .00001f || Math.Abs(blockerScale.Y - .01f) > .00001f || Math.Abs(blockerScale.Z - .01f) > .00001f)
		{ Fail("physics publication changed authored duck scale"); return; }
		elapsed += Time.DeltaTime;
		if (elapsed > 45) { Fail("timeout"); return; }
		if (!begun && elapsed >= 3)
		{
			var target = TerraEngine.Object.Find("Navigation Target");
			if (target == null || !follower.Begin(target))
			{ Fail("route admission " + follower.LastNavigationStatus); return; }
			begun = true;
		}
		// Move a real kinematic body into a route that has already been planned.
		// Its native movement is deferred to the protected physics command boundary.
		// Both body commands omit correlation IDs to exercise automatic sequencing.
		if (!entered && elapsed >= 4)
		{
			var receipt = blocker.MoveKinematic(new PhysicsPose
			{
				Position = new Vector3(22, 1, 20),
				Rotation = new Quaternion(0, 0, 0, 1)
			}, 1f);
			if (!receipt.Accepted) { Fail("blocker entry " + receipt.Status); return; }
			entered = true;
			Log.Info("NAVCOLL BLOCKER entering planned route");
		}
		if (!cleared && elapsed >= 12)
		{
			if (blockedSeconds < .8f) { Fail("no sustained blocked observation"); return; }
			var receipt = blocker.MoveKinematic(new PhysicsPose
			{
				Position = new Vector3(22, 1, 40),
				Rotation = new Quaternion(0, 0, 0, 1)
			}, 1f);
			if (!receipt.Accepted) { Fail("blocker exit " + receipt.Status); return; }
			cleared = true;
			Log.Info("NAVCOLL BLOCKER clearing route");
		}
		if (!characterCleared && elapsed >= 22)
		{
			if (characterBlockedSeconds < .8f) { Fail("no character blocking observation"); return; }
			var receipt = characterBlocker.SetDesiredVelocity(new Vector3(0, 0, 3), correlationId: 103);
			if (!receipt.Accepted) { Fail("character blocker exit " + receipt.Status); return; }
			characterCleared = true;
			Log.Info("NAVCOLL CHARACTER clearing route");
		}
		if (characterCleared && !characterStopped && elapsed >= 24)
		{
			characterBlocker.SetDesiredVelocity(default, correlationId: 104);
			characterStopped = true;
		}
		// MoveKinematic supplies the velocity needed for its duration; it is not a
		// timed motion controller. Explicitly hold the box after each one-second move.
		if (entered && !blockerHeld && elapsed >= 5)
		{
			var receipt = blocker.SetLinearVelocity(default);
			if (!receipt.Accepted) { Fail("blocker hold " + receipt.Status); return; }
			blockerHeld = true;
		}
		if (cleared && !blockerStopped && elapsed >= 13)
		{
			var receipt = blocker.SetLinearVelocity(default);
			if (!receipt.Accepted) { Fail("blocker stop " + receipt.Status); return; }
			blockerStopped = true;
		}
		if (!begun) return;
		var state = follower.Update(Time.DeltaTime);
		if (state != lastState)
		{
			Log.Info($"NAVCOLL STATE {state} navigation={follower.LastNavigationStatus} movement={follower.LastMovementStatus} diagnostic={follower.LastMovementDiagnostic}");
			lastState = state;
		}
		var p = Transform.Position;
		// Low-frequency fixture evidence compares physics and published scene poses.
		// This allocation is diagnostics only, never part of the reusable follower.
		if (elapsed >= nextDiagnostic)
		{
			nextDiagnostic += 5;
			Log.Info($"NAVCOLL POSE time={elapsed} scene=({p.OffsetX},{p.OffsetY},{p.OffsetZ})");
			foreach (var body in Physics.BodyStates())
				Log.Info($"NAVCOLL BODY slot={body.Body.SlotIndex} generation={body.Identity.ComponentGeneration} position=({body.Pose.Position.X},{body.Pose.Position.Y},{body.Pose.Position.Z}) velocity=({body.LinearVelocity.X},{body.LinearVelocity.Y},{body.LinearVelocity.Z})");
			foreach (var character in Physics.CharacterStates())
				Log.Info($"NAVCOLL PHYS slot={character.Character.SlotIndex} tick={character.Tick} position=({character.Pose.Position.X},{character.Pose.Position.Y},{character.Pose.Position.Z}) velocity=({character.LinearVelocity.X},{character.LinearVelocity.Y},{character.LinearVelocity.Z}) contacts={character.ContactCount}");
		}
		// The settled box spans x20..24,z15..25. The actor must stay on its
		// near side while blocked; the .1m allowance covers contact tolerances.
		if (elapsed >= 7 && elapsed < 12)
		{
			if (p.OffsetX > 19.7) { Fail($"phased through blocker x={p.OffsetX}"); return; }
			if (p.OffsetX >= 18.8 && Math.Abs(p.OffsetZ - 20) < .2)
			{
				blockedSeconds += Time.DeltaTime;
				if (!blockedLogged && blockedSeconds >= .8f)
				{
					blockedLogged = true;
					Log.Info($"NAVCOLL PASS held before moving blocker x={p.OffsetX} z={p.OffsetZ}");
				}
			}
		}
		// The second controller stays at x28,z20 until 22 seconds. Two .4m
		// capsules must keep their centers apart; no rigid body backs this actor.
		if (elapsed >= 16 && elapsed < 22)
		{
			if (p.OffsetX > 27.3) { Fail($"phased through character x={p.OffsetX}"); return; }
			if (p.OffsetX >= 26.5 && Math.Abs(p.OffsetZ - 20) < .2)
			{
				characterBlockedSeconds += Time.DeltaTime;
				if (!characterBlockedLogged && characterBlockedSeconds >= .8f)
				{
					characterBlockedLogged = true;
					Log.Info($"NAVCOLL PASS held before character x={p.OffsetX} z={p.OffsetZ}");
				}
			}
		}
		if (state == NavigationRouteFollowerState.Failed)
		{ Fail("follower failed " + follower.LastReason); return; }
		if (state == NavigationRouteFollowerState.Arrived)
		{
			if (!cleared || !blockedLogged || !characterCleared || !characterBlockedLogged) { Fail("arrival before blocker test completed"); return; }
			Log.Info($"NAVCOLL PASS resumed and arrived x={p.OffsetX} z={p.OffsetZ}");
			follower.Stop();
			finished = true;
		}
	}
	public override void OnDisable()
	{
		follower?.Dispose();
		characterBlocker?.SetDesiredVelocity(default);
	}
}
