// NavigationAgent.cs - a cached, generation-safe managed command facade.
using System;
using System.Threading;

namespace TerraEngine
{
	public sealed class NavigationAgent : Component
	{
		public const uint AbiVersion = 1;
		public NavigationAgentIdentity Identity => TryBind() ? _identity : default;
		private static long s_correlation;
		private NavigationAgentIdentity _identity;
		private bool _bound;

		internal override void OnObjectAssigned() { _identity = default; _bound = false; }

		// A successful identity is immutable for this wrapper. This prevents a stale
		// managed reference from silently attaching to a later component incarnation.
		private bool TryBind()
		{
			if (_bound) return _identity.IsValid;
			var request = new NativeNavigationAgentResolveRequest {
				Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentResolveRequest>(), OwnerObjectEntity = Object.ID
			};
			var snapshot = new NativeNavigationAgentIdentitySnapshot { Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentIdentitySnapshot>() };
			if (InternalCalls.Native_NavigationAgent_ResolveIdentity(ref request, ref snapshot) != NavigationStatus.Success) return false;
			_identity = snapshot.Identity;
			_bound = _identity.IsValid;
			return _bound;
		}

		private static NavigationAgentReceipt Rejected() => new() { State = NavigationAgentReceiptState.Rejected, Outcome = NavigationOutcome.InvalidRequest };
		private static NavigationAgentReceipt Expired() => new() { State = NavigationAgentReceiptState.Expired, Outcome = NavigationOutcome.Expired };
		private static NavigationAgentReceipt ConvertReceipt(NativeNavigationAgentCommandReceipt value) => new() {
			State = (NavigationAgentReceiptState)value.ReceiptState, Outcome = (NavigationOutcome)value.Outcome,
			Reason = (NavigationReason)value.Reason, CorrelationId = value.CorrelationId, AdmittedSequence = value.AdmittedSequence
		};
		private static NavigationAgentObjectIdentity ConvertObject(NativeNavigationAgentObjectIdentity value) => new() {
			ObjectIdLow = value.ObjectIdLow, ObjectIdHigh = value.ObjectIdHigh, LifetimeGeneration = value.LifetimeGeneration
		};
		private static bool IsHardAbiFailure(NavigationStatus status) => status == NavigationStatus.VersionMismatch || status == NavigationStatus.InvalidArgument;

		public NavigationAgentReceipt SetDestination(NavigationLocation destination) => !TryBind() ? Rejected() : Submit(NativeNavigationAgentCommandKind.SetLocationDestination, destination, 0);
		public NavigationAgentReceipt SetDestination(Object target)
		{
			if (target == null) throw new ArgumentNullException(nameof(target));
			return !TryBind() ? Rejected() : Submit(NativeNavigationAgentCommandKind.SetObjectDestination, default, target.ID);
		}
		public NavigationAgentReceipt Stop() => !TryBind() ? Rejected() : Submit(NativeNavigationAgentCommandKind.Stop, default, 0);

		public NavigationAgentReceipt ReadReceipt(ulong correlationId)
		{
			if (!TryBind()) return Expired();
			var query = new NativeNavigationAgentReceiptQuery { Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentReceiptQuery>(), Identity = _identity, CorrelationId = correlationId };
			var native = new NativeNavigationAgentCommandReceipt { Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentCommandReceipt>() };
			NavigationStatus status = InternalCalls.Native_NavigationAgent_ReadReceipt(ref query, ref native);
			if (status != NavigationStatus.Success && status != NavigationStatus.Accepted && native.CorrelationId == 0) return Expired();
			return ConvertReceipt(native);
		}

		public NavigationAgentStatus Status {
			get {
				if (!TryBind()) return new() { State = NavigationAgentState.Failed, Outcome = NavigationOutcome.InvalidRequest };
				var query = new NativeNavigationAgentStatusQuery { Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentStatusQuery>(), Identity = _identity };
				var native = new NativeNavigationAgentStatusSnapshot { Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentStatusSnapshot>() };
				NavigationStatus status = InternalCalls.Native_NavigationAgent_ReadStatus(ref query, ref native);
				if (IsHardAbiFailure(status)) return new() { State = NavigationAgentState.Failed, Outcome = NavigationOutcome.Failed };
				return new() { State = (NavigationAgentState)native.State, Outcome = (NavigationOutcome)native.Outcome, Reason = (NavigationReason)native.Reason, ConfigurationRevision = native.ConfigurationRevision, IntentSequence = native.IntentSequence, PublicationRevision = native.PublicationRevision };
			}
		}

		public NavigationAgentGuidance Guidance {
			get {
				if (!TryBind()) return default;
				var query = new NativeNavigationAgentGuidanceQuery { Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentGuidanceQuery>(), Identity = _identity };
				var native = new NativeNavigationAgentGuidanceSnapshot { Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentGuidanceSnapshot>() };
				NavigationStatus status = InternalCalls.Native_NavigationAgent_ReadGuidance(ref query, ref native);
				if (IsHardAbiFailure(status)) return default;
				return new() {
					Identity = _identity, State = (NavigationAgentState)native.State,
					Outcome = (NavigationOutcome)native.Outcome, Reason = (NavigationReason)native.Reason,
					NextWaypoint = native.NextWaypoint,
					SuggestedDirection = new Vector3((float)native.DirectionX, (float)native.DirectionY, (float)native.DirectionZ),
					RemainingDistanceMetres = native.RemainingDistanceMetres, Usable = native.Usable != 0,
					ConfigurationRevision = native.ConfigurationRevision, IntentSequence = native.IntentSequence,
					PublicationGeneration = native.PublicationGeneration, RouteGeneration = native.RouteGeneration,
					DestinationKind = native.DestinationKind, DestinationLocation = native.DestinationLocation,
					DestinationObject = ConvertObject(native.DestinationObject), SceneGeneration = native.SceneGeneration,
					SourceTransformRevision = native.SourceTransformRevision, DestinationTransformRevision = native.DestinationTransformRevision,
					TerrainPublicationGeneration = native.TerrainPublicationGeneration, WorldPublicationGeneration = native.WorldPublicationGeneration,
					SourceIndexRevision = native.SourceIndexRevision
				};
			}
		}

		private NavigationAgentReceipt Submit(NativeNavigationAgentCommandKind kind, NavigationLocation location, uint target)
		{
			ulong correlation = unchecked((ulong)Interlocked.Increment(ref s_correlation));
			var command = new NativeNavigationAgentCommand {
				Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentCommand>(), Identity = _identity, Kind = kind,
				TargetObjectEntity = target, Location = location, ExpectedConfigurationRevision = 0, ExpectedIntentSequence = 0,
				ProducerOrderingToken = correlation, CorrelationId = correlation
			};
			var receipt = new NativeNavigationAgentCommandReceipt { Header = NativeNavigationAgentAbiHeader.For<NativeNavigationAgentCommandReceipt>() };
			NavigationStatus status = InternalCalls.Native_NavigationAgent_SubmitCommand(ref command, ref receipt);
			if (status != NavigationStatus.Success && status != NavigationStatus.Accepted && receipt.CorrelationId == 0) return Rejected();
			return ConvertReceipt(receipt);
		}
	}
}
