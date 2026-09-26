// NavigationRouteFollower.cs - collision-safe, component-free route following for scripts.

using System;

namespace TerraEngine
{
	/// <summary>
	/// The observable state of a <see cref="NavigationRouteFollower"/>.
	/// Waiting states are normal while navigation coverage is rebuilt or a physical
	/// body temporarily blocks an otherwise valid route.
	/// </summary>
	public enum NavigationRouteFollowerState : uint
	{
		Idle,
		WaitingForRoute,
		Moving,
		WaitingToReplan,
		Arrived,
		Failed
	}

	/// <summary>
	/// Drives a copied navigation route through an existing character controller.
	/// The helper deliberately owns no scene component and never writes transforms:
	/// the physics controller is the sole authority that can move the object.
	/// </summary>
	public sealed class NavigationRouteFollower : IDisposable
	{
		// This mirrors Scene::kWorldCellEdgeMetres. Keeping the conversion local makes
		// movement decisions work across cells without changing the navigation ABI.
		private const double WorldCellEdgeMetres = 65_536.0;
		private const float MinimumReplanIntervalSeconds = 0.10F;
		private const float MinimumBlockedDurationSeconds = 0.10F;
		private const float ControllerLinearToleranceMetres = 0.001F;
		private const float ControllerSlopeToleranceDegrees = 0.001F;
		private const float ControllerUnitVectorTolerance = 0.001F;
		private const float ControllerScaleTolerance = 0.001F;

		private readonly Object _source;
		private readonly Transform _transform;
		private readonly CharacterController _controller;
		private readonly NavigationProfile _profile;
		private float _speedMetresPerSecond;
		private readonly float _waypointToleranceMetres;
		private readonly float _blockedDurationSeconds;
		private readonly float _replanIntervalSeconds;
		private readonly ulong _replacementKey;
		private readonly ulong _correlationId;

		private NavigationLocation _destination;
		private Object? _target;
		private NavigationRequestHandle _request;
		private NavigationRouteHandle _route;
		private NavigationLocation[] _points = Array.Empty<NavigationLocation>();
		private int _nextWaypoint;
		private float _replanRemainingSeconds;
		private float _unmovedSeconds;
		private float _progressSinceCheckMetres;
		private WorldPosition _lastPosition;
		private bool _hasLastPosition;
		private bool _hasDestination;
		private NavigationRouteFollowerState _state;
		private NavigationStatus _lastNavigationStatus;
		private NavigationOutcome _lastOutcome;
		private NavigationReason _lastReason;
		private PhysicsStatus _lastMovementStatus;
		private string _lastMovementDiagnostic = string.Empty;

		/// <summary>
		/// Creates a follower for one ordinary object and its already configured
		/// character controller. The profile is copied once, so fixed steps do not
		/// reflect profile data or allocate route-policy state each frame.
		/// </summary>
		public NavigationRouteFollower(Object source, CharacterController controller,
			NavigationProfile profile, float speedMetresPerSecond,
			float waypointToleranceMetres = 0.10F, float blockedDurationSeconds = 0.50F,
			float replanIntervalSeconds = 0.50F, ulong replacementKey = 0,
			ulong correlationId = 0)
		{
			_source = source ?? throw new ArgumentNullException(nameof(source));
			_controller = controller ?? throw new ArgumentNullException(nameof(controller));
			if (controller.Object.ID != source.ID)
				throw new ArgumentException("The character controller must belong to the route source.", nameof(controller));
			if (!IsFinitePositive(speedMetresPerSecond))
				throw new ArgumentOutOfRangeException(nameof(speedMetresPerSecond));
			if (!IsFinitePositive(waypointToleranceMetres))
				throw new ArgumentOutOfRangeException(nameof(waypointToleranceMetres));

			_transform = source.Transform;
			_profile = profile;
			_speedMetresPerSecond = speedMetresPerSecond;
			_waypointToleranceMetres = waypointToleranceMetres;
			_blockedDurationSeconds = Math.Max(MinimumBlockedDurationSeconds, blockedDurationSeconds);
			_replanIntervalSeconds = Math.Max(MinimumReplanIntervalSeconds, replanIntervalSeconds);
			_replacementKey = replacementKey;
			_correlationId = correlationId;
			_state = NavigationRouteFollowerState.Idle;
		}

		/// <summary>
		/// Creates a follower from the controller's owning object. This is the usual
		/// script-facing overload and avoids duplicating an object reference in code.
		/// </summary>
		public NavigationRouteFollower(CharacterController controller, NavigationProfile profile)
			: this(controller?.Object ?? throw new ArgumentNullException(nameof(controller)), controller, profile, 1.0F)
		{
		}

		public NavigationRouteFollowerState State => _state;
		public NavigationRequestHandle Request => _request;
		public NavigationRouteHandle Route => _route;
		public NavigationStatus LastNavigationStatus => _lastNavigationStatus;
		public NavigationOutcome LastOutcome => _lastOutcome;
		public NavigationReason LastReason => _lastReason;
		public PhysicsStatus LastMovementStatus => _lastMovementStatus;
		/// <summary>Copied native reason for the most recent controller command.</summary>
		public string LastMovementDiagnostic => _lastMovementDiagnostic;

		/// <summary>Requested horizontal controller speed in metres per second.</summary>
		public float Speed
		{
			get => _speedMetresPerSecond;
			set
			{
				if (!IsFinitePositive(value)) throw new ArgumentOutOfRangeException(nameof(value));
				_speedMetresPerSecond = value;
			}
		}

		/// <summary>
		/// Starts (or replaces) a route to the copied destination. A false result is
		/// terminal for invalid controller/profile input; accepted requests continue
		/// through <see cref="FixedStep"/> until a route is ready.
		/// </summary>
		public bool Begin(NavigationLocation destination)
		{
			StopMovement();
			ReleaseCurrentRequest();
			_destination = destination;
			_target = null;
			_hasDestination = true;
			_replanRemainingSeconds = 0.0F;
			_unmovedSeconds = 0.0F;
			_progressSinceCheckMetres = 0.0F;
			_hasLastPosition = false;
			_points = Array.Empty<NavigationLocation>();
			_route = default;
			_nextWaypoint = 0;

			if (!IsProfileCompatibleWithController(_controller, _profile) || !HasDisabledOwnerColliderSet())
			{
				_lastNavigationStatus = NavigationStatus.InvalidArgument;
				_lastOutcome = NavigationOutcome.InvalidRequest;
				_lastReason = NavigationReason.InsufficientClearance;
				_state = NavigationRouteFollowerState.Failed;
				return false;
			}

			return RequestReplacement();
		}

		/// <summary>
		/// Starts a route to an object. Each bounded retry captures the target's
		/// current pose again, which lets a moving target be followed without a
		/// persistent navigation component.
		/// </summary>
		public bool Begin(Object target)
		{
			if (target == null) throw new ArgumentNullException(nameof(target));
			StopMovement();
			ReleaseCurrentRequest();
			_target = target;
			_hasDestination = true;
			_replanRemainingSeconds = 0.0F;
			_unmovedSeconds = 0.0F;
			_progressSinceCheckMetres = 0.0F;
			_hasLastPosition = false;
			_points = Array.Empty<NavigationLocation>();
			_route = default;
			_nextWaypoint = 0;

			if (!IsProfileCompatibleWithController(_controller, _profile) || !HasDisabledOwnerColliderSet())
			{
				_lastNavigationStatus = NavigationStatus.InvalidArgument;
				_lastOutcome = NavigationOutcome.InvalidRequest;
				_lastReason = NavigationReason.InsufficientClearance;
				_state = NavigationRouteFollowerState.Failed;
				return false;
			}

			return RequestReplacement();
		}

		/// <summary>
		/// Advances the follower from a script's fixed callback (or its explicit
		/// update loop). This emits only a desired velocity; collision resolution,
		/// stepping, and transform publication remain in the physics system.
		/// </summary>
		public NavigationRouteFollowerState FixedStep(float deltaSeconds)
		{
			if (!IsFiniteNonNegative(deltaSeconds))
				return Fail(NavigationStatus.InvalidArgument, NavigationOutcome.InvalidRequest, NavigationReason.ProviderFailure);
			if (_state == NavigationRouteFollowerState.Idle || _state == NavigationRouteFollowerState.Arrived || _state == NavigationRouteFollowerState.Failed)
				return _state;
			// Recheck shape, effective scale and owner collision on each step. A live
			// authoring edit must not keep following a route built for old geometry.
			if (!_controller.Enabled || !_controller.PhysicsEnabled)
				return Fail(NavigationStatus.Unavailable, NavigationOutcome.Failed, NavigationReason.ProviderFailure);
			if (!IsProfileCompatibleWithController(_controller, _profile) || !HasDisabledOwnerColliderSet())
				return Fail(NavigationStatus.InvalidArgument, NavigationOutcome.InvalidRequest, NavigationReason.InsufficientClearance);

			if (_replanRemainingSeconds > 0.0F)
			{
				_replanRemainingSeconds = Math.Max(0.0F, _replanRemainingSeconds - deltaSeconds);
				if (_replanRemainingSeconds > 0.0F)
					return _state;
				// A replacement can be accepted, deferred for unavailable coverage, or
				// terminally rejected. In all cases wait until the next fixed step before
				// reading it, so a deferred submission is never queried as a default handle.
				RequestReplacement();
				return _state;
			}

			if (!_route.IsValid)
				return ObserveRequest();

			NavigationRouteValidation validation = Navigation.ValidateRoute(_route);
			_lastNavigationStatus = validation.Status;
			_lastOutcome = validation.Outcome;
			_lastReason = validation.Reason;
			if (validation.Status != NavigationStatus.Success || !validation.Usable)
				return ScheduleReplan(validation.Status, validation.Outcome, validation.Reason);

			WorldPosition currentPosition;
			try
			{
				// Transform is read after physics publication. It is progress evidence only,
				// never a movement command, so the helper cannot phase through blockers.
				currentPosition = _transform.Position;
			}
			catch (InvalidOperationException)
			{
				return Fail(NavigationStatus.StaleTarget, NavigationOutcome.Stale, NavigationReason.SceneReplaced);
			}

			while (_nextWaypoint < _points.Length
				&& IsGroundWaypointReached(currentPosition, _points[_nextWaypoint]))
			{
				++_nextWaypoint;
			}
			if (_nextWaypoint >= _points.Length)
			{
				StopMovement();
				ReleaseCurrentRequest();
				_route = default;
				_points = Array.Empty<NavigationLocation>();
				_state = NavigationRouteFollowerState.Arrived;
				return _state;
			}

			if (!TryPlanarDirection(currentPosition, _points[_nextWaypoint], out Vector3 direction, out float waypointDistance))
				return Fail(NavigationStatus.LimitExceeded, NavigationOutcome.Failed, NavigationReason.DomainLimit);

			// Do not command farther than this fixed step can reach. This avoids skipping
			// a narrow waypoint at a large host delta while retaining physics collision.
			float safeSpeed = deltaSeconds > 0.0F
				? Math.Min(_speedMetresPerSecond, waypointDistance / deltaSeconds) : 0.0F;
			SubmitMovement(new Vector3(direction.X * safeSpeed, 0.0F, direction.Z * safeSpeed));
			if (_lastMovementStatus != PhysicsStatus.Accepted && _lastMovementStatus != PhysicsStatus.Success)
			{
				if (_lastMovementStatus == PhysicsStatus.StaleTarget)
					return Fail(NavigationStatus.StaleTarget, NavigationOutcome.Stale, NavigationReason.Expired);
				return ScheduleReplan(NavigationStatus.Unavailable, NavigationOutcome.Failed, NavigationReason.ProviderFailure);
			}

			if (_hasLastPosition && PlanarDistance(currentPosition, _lastPosition, out float progressDistance))
			{
				// Aggregate an interval of physical progress rather than judging one frame:
				// a high-rate fixed step can legitimately move less than waypoint tolerance.
				_progressSinceCheckMetres += progressDistance;
				_unmovedSeconds += deltaSeconds;
			}
			_lastPosition = currentPosition;
			_hasLastPosition = true;
			if (_unmovedSeconds >= _blockedDurationSeconds)
			{
				if (_progressSinceCheckMetres < _waypointToleranceMetres * 0.25F)
					return ScheduleReplan(NavigationStatus.Accepted, NavigationOutcome.PendingPreparation, NavigationReason.DirtyCoverage);
				_unmovedSeconds = 0.0F;
				_progressSinceCheckMetres = 0.0F;
			}

			_state = NavigationRouteFollowerState.Moving;
			return _state;
		}

		/// <summary>
		/// Alias for scripts whose host uses Update instead of an OnFixedUpdate
		/// callback. Pass that host's stable simulation delta explicitly.
		/// </summary>
		public NavigationRouteFollowerState Update(float deltaSeconds) => FixedStep(deltaSeconds);

		/// <summary>
		/// Stops safe controller motion and releases the copied request/result. Call it
		/// when the owning script disables or replaces this follower.
		/// </summary>
		public void Stop()
		{
			StopMovement();
			ReleaseCurrentRequest();
			_points = Array.Empty<NavigationLocation>();
			_route = default;
			_request = default;
			_hasDestination = false;
			_target = null;
			_unmovedSeconds = 0.0F;
			_progressSinceCheckMetres = 0.0F;
			_hasLastPosition = false;
			_state = NavigationRouteFollowerState.Idle;
		}

		/// <summary>Releases the route exactly as <see cref="Stop"/> does.</summary>
		public void Dispose() => Stop();

		/// <summary>
		/// Checks the conservative contract between a route profile and controller.
		/// A route must reserve the controller capsule, and its clearance margin must
		/// cover contact padding, otherwise a physics-safe follower could receive an
		/// unsafe corridor.
		/// </summary>
		public static bool IsProfileCompatibleWithController(CharacterController controller, NavigationProfile profile)
		{
			if (controller == null || !IsFinitePositive(profile.RadiusMetres) || !IsFinitePositive(profile.HeightMetres)
				|| !IsFiniteNonNegative(profile.ClearanceMarginMetres) || !IsFiniteNonNegative(profile.StepHeightMetres)
				|| !IsFinitePositive(profile.MaxSlopeDegrees))
				return false;
			float controllerRadius = controller.Radius;
			float controllerHeight = controller.Height;
			float controllerPadding = controller.ContactPadding;
			float controllerStepHeight = controller.StepHeight;
			float controllerSlope = controller.MaximumSlopeAngleDegrees;
			Vector3 controllerUp = controller.UpDirection;
			Vector3 controllerCenter = controller.LocalCenter;
			WorldPose ownerPose = controller.Object.Transform.Pose;
			Vector3 effectiveScale = ownerPose.Scale;
			Quaternion rotation = ownerPose.Rotation;
			double rotationLengthSquared = (double)rotation.X * rotation.X + (double)rotation.Y * rotation.Y
				+ (double)rotation.Z * rotation.Z + (double)rotation.W * rotation.W;
			bool uprightWorldRotation = rotationLengthSquared > 1.0e-12
				&& !double.IsNaN(rotationLengthSquared) && !double.IsInfinity(rotationLengthSquared)
				&& Math.Abs(2.0 * ((double)rotation.X * rotation.Y - (double)rotation.W * rotation.Z)
					/ rotationLengthSquared) <= ControllerUnitVectorTolerance
				&& Math.Abs(2.0 * ((double)rotation.Y * rotation.Z + (double)rotation.W * rotation.X)
					/ rotationLengthSquared) <= ControllerUnitVectorTolerance
				&& 2.0 * ((double)rotation.X * rotation.X + (double)rotation.Z * rotation.Z)
					/ rotationLengthSquared <= ControllerUnitVectorTolerance;
			return controller.Enabled && controller.PhysicsEnabled
				&& controller.Shape == PhysicsCharacterShape.Capsule
				&& uprightWorldRotation
				&& IsFinitePositive(controllerRadius) && IsFinitePositive(controllerHeight)
				&& IsFiniteNonNegative(controllerPadding) && IsFiniteNonNegative(controllerStepHeight)
				&& IsFinitePositive(controllerSlope)
				// Physics currently constructs the virtual character from unscaled
				// authored radius/height. Reject other effective world scales rather
				// than claiming its route profile reserves unsupported geometry.
				&& Math.Abs(effectiveScale.X - 1.0F) <= ControllerScaleTolerance
				&& Math.Abs(effectiveScale.Y - 1.0F) <= ControllerScaleTolerance
				&& Math.Abs(effectiveScale.Z - 1.0F) <= ControllerScaleTolerance
				// Ground navigation assumes an upright capsule centred halfway up its body.
				// Rejecting another convention is safer than steering a different shape
				// through a corridor that was built for the object transform's ground point.
				&& Math.Abs(controllerUp.X) <= ControllerUnitVectorTolerance
				&& Math.Abs(controllerUp.Y - 1.0F) <= ControllerUnitVectorTolerance
				&& Math.Abs(controllerUp.Z) <= ControllerUnitVectorTolerance
				&& Math.Abs(controllerCenter.X) <= ControllerLinearToleranceMetres
				&& Math.Abs(controllerCenter.Y - controllerHeight * 0.5F) <= ControllerLinearToleranceMetres
				&& Math.Abs(controllerCenter.Z) <= ControllerLinearToleranceMetres
				// Reflected float values are not bit-identical to the copied double profile,
				// so this small one-millimetre tolerance avoids false unsafe-profile rejects.
				&& profile.RadiusMetres + ControllerLinearToleranceMetres >= controllerRadius
				&& profile.ClearanceMarginMetres + ControllerLinearToleranceMetres >= controllerPadding
				&& profile.HeightMetres + ControllerLinearToleranceMetres >= controllerHeight
				&& profile.StepHeightMetres <= controllerStepHeight + ControllerLinearToleranceMetres
				&& profile.MaxSlopeDegrees <= controllerSlope + ControllerSlopeToleranceDegrees;
		}

		private NavigationRouteFollowerState ObserveRequest()
		{
			NavigationRouteResult result = Navigation.ReadRequest(_request);
			_lastNavigationStatus = result.Status;
			_lastOutcome = result.Outcome;
			_lastReason = result.Reason;
			if (result.Complete && result.Route.IsValid && result.Points.Length > 0)
			{
				// ReadRequest allocates route points only once at completion; later fixed
				// steps validate the retained immutable route without copying it again.
				_route = result.Route;
				_points = result.Points;
				_nextWaypoint = 0;
				_unmovedSeconds = 0.0F;
				_progressSinceCheckMetres = 0.0F;
				_hasLastPosition = false;
				_state = NavigationRouteFollowerState.Moving;
				return _state;
			}
			if (result.Status == NavigationStatus.Success && result.Lifecycle != NavigationRequestLifecycle.Terminal)
			{
				_state = NavigationRouteFollowerState.WaitingForRoute;
				return _state;
			}
			return ScheduleReplan(result.Status, result.Outcome, result.Reason);
		}

		private bool RequestReplacement()
		{
			if (!_hasDestination)
			{
				_state = NavigationRouteFollowerState.Idle;
				return false;
			}
			ReleaseCurrentRequest();
			NavigationRouteResult result = _target != null
				? Navigation.RequestRoute(_source, _target, _profile, replacementKey: _replacementKey, correlationId: _correlationId)
				: Navigation.RequestRoute(_source, _destination, _profile, replacementKey: _replacementKey, correlationId: _correlationId);
			_lastNavigationStatus = result.Status;
			_lastOutcome = result.Outcome;
			_lastReason = result.Reason;
			_request = result.Request;
			if ((result.Status == NavigationStatus.Accepted || result.Status == NavigationStatus.Success) && _request.IsValid)
			{
				_state = NavigationRouteFollowerState.WaitingForRoute;
				return true;
			}
			// Coverage can be briefly unavailable during streaming/rebuild. Keep trying
			// at the bounded interval rather than leaving a deferred request forever.
			return ScheduleReplan(result.Status, result.Outcome, result.Reason) != NavigationRouteFollowerState.Failed;
		}

		private NavigationRouteFollowerState ScheduleReplan(NavigationStatus status, NavigationOutcome outcome, NavigationReason reason)
		{
			_lastNavigationStatus = status;
			_lastOutcome = outcome;
			_lastReason = reason;
			StopMovement();
			ReleaseCurrentRequest();
			_route = default;
			_points = Array.Empty<NavigationLocation>();
			_nextWaypoint = 0;
			_unmovedSeconds = 0.0F;
			_progressSinceCheckMetres = 0.0F;
			_hasLastPosition = false;
			if (!_hasDestination || status == NavigationStatus.StaleTarget || reason == NavigationReason.SceneReplaced || !CanRetry(status, outcome))
				return Fail(status, outcome, reason);
			_replanRemainingSeconds = _replanIntervalSeconds;
			_state = NavigationRouteFollowerState.WaitingToReplan;
			return _state;
		}

		private static bool CanRetry(NavigationStatus status, NavigationOutcome outcome)
		{
			return status == NavigationStatus.Accepted || status == NavigationStatus.Unavailable
				|| status == NavigationStatus.Expired || outcome == NavigationOutcome.PendingPreparation
				|| outcome == NavigationOutcome.UnavailableCoverage || outcome == NavigationOutcome.Stale;
		}

		private NavigationRouteFollowerState Fail(NavigationStatus status, NavigationOutcome outcome, NavigationReason reason)
		{
			_lastNavigationStatus = status;
			_lastOutcome = outcome;
			_lastReason = reason;
			StopMovement();
			ReleaseCurrentRequest();
			_state = NavigationRouteFollowerState.Failed;
			return _state;
		}

		private void ReleaseCurrentRequest()
		{
			if (!_request.IsValid) return;
			Navigation.CancelRoute(_request);
			Navigation.ReleaseResult(_request);
			_request = default;
		}

		private void StopMovement()
		{
			SubmitMovement(default);
		}

		private void SubmitMovement(Vector3 desiredVelocity)
		{
			// Preserve the copied receipt diagnostic so gameplay can distinguish a
			// rejected command from a collision that correctly stopped the controller.
			PhysicsCommandReceipt receipt = _controller.SetDesiredVelocity(desiredVelocity, correlationId: _correlationId);
			_lastMovementStatus = receipt.Status;
			_lastMovementDiagnostic = receipt.Diagnostic;
		}

		private bool HasDisabledOwnerColliderSet()
		{
			// A controller and a simultaneously enabled collider set on one object can
			// create self-contact. Ground following rejects that ambiguous ownership
			// instead of guessing which physical shape navigation should represent.
			ColliderSet? colliderSet = _source.GetComponent<ColliderSet>();
			return colliderSet == null || !colliderSet.Enabled;
		}

		private static bool TryPlanarDirection(WorldPosition from, NavigationLocation to, out Vector3 direction, out float distance)
		{
			direction = default;
			distance = 0.0F;
			if (to.World.IsValid == false) return false;
			double x = ((double)to.CellX - (double)from.Cell.X) * WorldCellEdgeMetres + to.OffsetX - from.OffsetX;
			double z = ((double)to.CellZ - (double)from.Cell.Z) * WorldCellEdgeMetres + to.OffsetZ - from.OffsetZ;
			if (double.IsNaN(x) || double.IsInfinity(x) || double.IsNaN(z) || double.IsInfinity(z)) return false;
			double length = Math.Sqrt(x * x + z * z);
			if (double.IsNaN(length) || double.IsInfinity(length) || length > float.MaxValue) return false;
			distance = (float)length;
			if (length <= double.Epsilon) return true;
			direction = new Vector3((float)(x / length), 0.0F, (float)(z / length));
			return true;
		}

		private bool IsGroundWaypointReached(WorldPosition current, NavigationLocation waypoint)
		{
			if (!TryPlanarDirection(current, waypoint, out _, out float horizontalDistance)
				|| horizontalDistance > _waypointToleranceMetres)
				return false;
			double verticalDistance = ((double)waypoint.CellY - (double)current.Cell.Y) * WorldCellEdgeMetres
				+ waypoint.OffsetY - current.OffsetY;
			return !double.IsNaN(verticalDistance) && !double.IsInfinity(verticalDistance)
				&& Math.Abs(verticalDistance) <= _waypointToleranceMetres;
		}

		private static bool PlanarDistance(WorldPosition first, WorldPosition second, out float distance)
		{
			distance = 0.0F;
			double x = ((double)second.Cell.X - (double)first.Cell.X) * WorldCellEdgeMetres
				+ second.OffsetX - first.OffsetX;
			double z = ((double)second.Cell.Z - (double)first.Cell.Z) * WorldCellEdgeMetres
				+ second.OffsetZ - first.OffsetZ;
			if (double.IsNaN(x) || double.IsInfinity(x) || double.IsNaN(z) || double.IsInfinity(z)) return false;
			double length = Math.Sqrt(x * x + z * z);
			if (double.IsNaN(length) || double.IsInfinity(length) || length > float.MaxValue) return false;
			distance = (float)length;
			return true;
		}

		private static bool IsFinitePositive(float value) => value > 0.0F && !float.IsNaN(value) && !float.IsInfinity(value);
		private static bool IsFinitePositive(double value) => value > 0.0 && !double.IsNaN(value) && !double.IsInfinity(value);
		private static bool IsFiniteNonNegative(float value) => value >= 0.0F && !float.IsNaN(value) && !float.IsInfinity(value);
		private static bool IsFiniteNonNegative(double value) => value >= 0.0 && !double.IsNaN(value) && !double.IsInfinity(value);
	}
}
