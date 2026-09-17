// Navigation.cs - component-free managed routes over copied native packets.

using System;

namespace TerraEngine
{
	public static unsafe class Navigation
	{
		public const uint AbiVersionConstant = 2;
		public const int MaximumRoutePoints = 4096;
		public const int MaximumInvalidations = 4096;
		internal const int MaximumDiagnosticBytes = 192;

		public static uint AbiVersion => InternalCalls.Native_Navigation_GetAbiVersion();

		// The native boundary resolves the Object only long enough to copy its UUID,
		// lifetime, canonical pose, explicit profile, and destination into a route.
		public static NavigationRouteResult RequestRoute(Object source, NavigationLocation destination,
			NavigationProfile profile, uint routePolicy = 0, ulong replacementKey = 0, ulong correlationId = 0)
		{
			if (source == null) throw new ArgumentNullException(nameof(source));
			NativeNavigationObjectToLocationRequest request = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationObjectToLocationRequest>(),
				SourceObjectEntity = source.ID, Destination = destination, Profile = profile,
				RoutePolicy = routePolicy, ReplacementKey = replacementKey, CorrelationId = correlationId
			};
			return SubmitObjectLocation(ref request);
		}

		// An object target is captured once; navigation plans a route and never moves
		// either Object or creates a persistent agent/controller on it.
		public static NavigationRouteResult RequestRoute(Object source, Object target,
			NavigationProfile profile, uint routePolicy = 0, ulong replacementKey = 0, ulong correlationId = 0)
		{
			if (source == null) throw new ArgumentNullException(nameof(source));
			if (target == null) throw new ArgumentNullException(nameof(target));
			NativeNavigationObjectToObjectRequest request = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationObjectToObjectRequest>(),
				SourceObjectEntity = source.ID, TargetObjectEntity = target.ID, Profile = profile,
				RoutePolicy = routePolicy, ReplacementKey = replacementKey, CorrelationId = correlationId
			};
			NativeNavigationSubmitReceipt receipt = new() { Header = NavigationAbiHeader.For<NativeNavigationSubmitReceipt>() };
			NavigationStatus status = InternalCalls.Native_Navigation_RequestObjectToObject(ref request, ref receipt);
			byte* diagnostic = receipt.Diagnostic;
			return new NavigationRouteResult { Status = status, Request = receipt.Request, Diagnostic = NavigationInterop.ReadDiagnostic(diagnostic) };
		}

		public static NavigationRouteResult ReadRequest(NavigationRequestHandle request)
		{
			NativeNavigationRequestSnapshot snapshot = new() { Header = NavigationAbiHeader.For<NativeNavigationRequestSnapshot>() };
			NavigationStatus status = InternalCalls.Native_Navigation_ReadRequest(ref request, ref snapshot);
			NavigationStatus copyStatus = NavigationStatus.Success;
			NavigationLocation[] points = snapshot.Route.IsValid
				? CopyRoute(snapshot.Route, out copyStatus) : Array.Empty<NavigationLocation>();
			if (status == NavigationStatus.Success && snapshot.Route.IsValid && copyStatus != NavigationStatus.Success) status = copyStatus;
			return new NavigationRouteResult
			{
				Status = status, Lifecycle = snapshot.Lifecycle, Outcome = snapshot.Outcome, Reason = snapshot.Reason,
				Request = snapshot.Request, Route = snapshot.Route, IntentGeneration = snapshot.IntentGeneration, Points = points
			};
		}

		public static NavigationRouteResult ReadRoute(NavigationRouteHandle route)
		{
			NavigationLocation[] points = CopyRoute(route, out NavigationStatus status);
			return new NavigationRouteResult { Status = status, Route = route, Points = points };
		}

		public static NavigationStatus CancelRoute(NavigationRequestHandle request) => SubmitRequestAction(request, false);
		public static NavigationStatus ReleaseResult(NavigationRequestHandle request) => SubmitRequestAction(request, true);

		public static NavigationRouteValidation ValidateRoute(NavigationRouteHandle route)
		{
			NativeNavigationRouteValidationRequest request = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationRouteValidationRequest>(), Route = route
			};
			NativeNavigationRouteValidationResult result = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationRouteValidationResult>()
			};
			NavigationStatus status = InternalCalls.Native_Navigation_ValidateRoute(ref request, ref result);
			return new NavigationRouteValidation { Status = status, Usable = result.Usable != 0, Outcome = result.Outcome, Reason = result.Reason };
		}

		public static NavigationInvalidationResult ReadInvalidations(NavigationOwnerHandle owner, ulong afterCursor = 0)
		{
			NativeNavigationInvalidationCursor cursor = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationInvalidationCursor>(), Owner = owner, Cursor = afterCursor
			};
			NativeNavigationBufferInfo info = new() { Header = NavigationAbiHeader.For<NativeNavigationBufferInfo>() };
			NavigationStatus status = InternalCalls.Native_Navigation_CopyInvalidations(ref cursor, null, 0, ref info);
			if (status != NavigationStatus.Success || info.RequiredCount == 0)
				return new NavigationInvalidationResult { Status = status, NextCursor = info.PinnedGeneration };
			if (info.RequiredCount > MaximumInvalidations)
				return new NavigationInvalidationResult { Status = NavigationStatus.LimitExceeded, NextCursor = info.PinnedGeneration };
			NativeNavigationInvalidationRecord[] native = new NativeNavigationInvalidationRecord[info.RequiredCount];
			fixed (NativeNavigationInvalidationRecord* pointer = native)
			{
				info = new NativeNavigationBufferInfo { Header = NavigationAbiHeader.For<NativeNavigationBufferInfo>() };
				status = InternalCalls.Native_Navigation_CopyInvalidations(ref cursor, pointer, (uint)native.Length, ref info);
			}
			if (status != NavigationStatus.Success || info.CopiedCount != native.Length)
				return new NavigationInvalidationResult { Status = status, NextCursor = info.PinnedGeneration };
			NavigationInvalidation[] records = new NavigationInvalidation[native.Length];
			for (int index = 0; index < native.Length; ++index)
			{
				records[index] = new NavigationInvalidation
				{
					Owner = native[index].Owner, Route = native[index].Route, Reason = native[index].Reason,
					Outcome = native[index].Outcome, SourceRevision = native[index].SourceRevision,
					ObservationGeneration = native[index].ObservationGeneration
				};
			}
			return new NavigationInvalidationResult { Status = status, NextCursor = info.PinnedGeneration, Records = records };
		}

		private static NavigationRouteResult SubmitObjectLocation(ref NativeNavigationObjectToLocationRequest request)
		{
			NativeNavigationSubmitReceipt receipt = new() { Header = NavigationAbiHeader.For<NativeNavigationSubmitReceipt>() };
			NavigationStatus status = InternalCalls.Native_Navigation_RequestObjectToLocation(ref request, ref receipt);
			byte* diagnostic = receipt.Diagnostic;
			return new NavigationRouteResult { Status = status, Request = receipt.Request, Diagnostic = NavigationInterop.ReadDiagnostic(diagnostic) };
		}

		private static NavigationStatus SubmitRequestAction(NavigationRequestHandle request, bool release)
		{
			NativeNavigationCancellationRequest action = new() { Header = NavigationAbiHeader.For<NativeNavigationCancellationRequest>(), Request = request };
			NativeNavigationSubmitReceipt receipt = new() { Header = NavigationAbiHeader.For<NativeNavigationSubmitReceipt>() };
			return release ? InternalCalls.Native_Navigation_ReleaseResult(ref action, ref receipt)
				: InternalCalls.Native_Navigation_CancelRequest(ref action, ref receipt);
		}

		private static NavigationLocation[] CopyRoute(NavigationRouteHandle route, out NavigationStatus status)
		{
			NativeNavigationBufferInfo info = new() { Header = NavigationAbiHeader.For<NativeNavigationBufferInfo>() };
			status = InternalCalls.Native_Navigation_CopyRoute(ref route, null, 0, ref info);
			if (status != NavigationStatus.Success || info.RequiredCount == 0) return Array.Empty<NavigationLocation>();
			if (info.RequiredCount > MaximumRoutePoints) { status = NavigationStatus.LimitExceeded; return Array.Empty<NavigationLocation>(); }
			NativeNavigationRoutePoint[] native = new NativeNavigationRoutePoint[info.RequiredCount];
			fixed (NativeNavigationRoutePoint* pointer = native)
			{
				info = new NativeNavigationBufferInfo { Header = NavigationAbiHeader.For<NativeNavigationBufferInfo>() };
				status = InternalCalls.Native_Navigation_CopyRoute(ref route, pointer, (uint)native.Length, ref info);
			}
			if (status != NavigationStatus.Success || info.CopiedCount != native.Length) return Array.Empty<NavigationLocation>();
			NavigationLocation[] points = new NavigationLocation[native.Length];
			for (int index = 0; index < native.Length; ++index) points[index] = native[index].Location;
			return points;
		}
	}
}
