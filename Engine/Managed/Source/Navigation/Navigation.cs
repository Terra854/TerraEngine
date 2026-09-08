// Navigation.cs - bounded managed facade over the engine-owned navigation ABI.

using System;

namespace TerraEngine
{
	public static unsafe class Navigation
	{
		public const uint AbiVersionConstant = 1;
		public const int MaximumRoutePoints = 4096;
		public const int MaximumInvalidations = 4096;
		internal const int MaximumDiagnosticBytes = 192;

		public static uint AbiVersion => InternalCalls.Native_Navigation_GetAbiVersion();

		public static NavigationRouteResult RequestRoute(
			NavigationOwnerHandle owner,
			NavigationLocation start,
			NavigationLocation destination,
			NavigationProfile profile,
			bool allowPartial = false,
			bool allowEndpointAdjustment = false,
			uint maximumExpansions = 4096,
			ulong correlationId = 0)
		{
			NativeNavigationRouteRequest request = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationRouteRequest>(), Owner = owner,
				Profile = profile, Start = start, Destination = destination,
				AllowPartial = allowPartial ? 1U : 0U,
				AllowEndpointAdjustment = allowEndpointAdjustment ? 1U : 0U,
				MaximumExpansions = maximumExpansions, CorrelationId = correlationId
			};
			NativeNavigationSubmitReceipt receipt = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationSubmitReceipt>()
			};
			NavigationStatus status = InternalCalls.Native_Navigation_SubmitRoute(ref request, ref receipt);
			byte* diagnostic = receipt.Diagnostic;
			return new NavigationRouteResult
			{
				Status = status, Request = receipt.Request,
				Diagnostic = NavigationInterop.ReadDiagnostic(diagnostic)
			};
		}

		public static NavigationRouteResult ReadRequest(NavigationRequestHandle request)
		{
			NativeNavigationRequestSnapshot snapshot = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationRequestSnapshot>()
			};
			NavigationStatus status = InternalCalls.Native_Navigation_ReadRequest(ref request, ref snapshot);
			NavigationLocation[] points = Array.Empty<NavigationLocation>();
			if (snapshot.Route.IsValid)
			{
				points = CopyRoute(snapshot.Route, out NavigationStatus copyStatus);
				if (status == NavigationStatus.Success && copyStatus != NavigationStatus.Success)
				{
					status = copyStatus;
				}
			}
			return new NavigationRouteResult
			{
				Status = status, Lifecycle = snapshot.Lifecycle, Outcome = snapshot.Outcome,
				Reason = snapshot.Reason, Request = snapshot.Request, Route = snapshot.Route,
				IntentGeneration = snapshot.IntentGeneration, Points = points
			};
		}

		public static NavigationRouteResult ReadRoute(NavigationRouteHandle route)
		{
			NavigationLocation[] points = CopyRoute(route, out NavigationStatus status);
			return new NavigationRouteResult { Status = status, Route = route, Points = points };
		}

		public static NavigationStatus CancelRoute(NavigationRequestHandle request)
		{
			NativeNavigationCancellationRequest cancellation = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationCancellationRequest>(), Request = request
			};
			NativeNavigationSubmitReceipt receipt = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationSubmitReceipt>()
			};
			return InternalCalls.Native_Navigation_CancelRequest(ref cancellation, ref receipt);
		}

		public static NavigationStatus ReleaseResult(NavigationRequestHandle request)
		{
			NativeNavigationCancellationRequest release = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationCancellationRequest>(), Request = request
			};
			NativeNavigationSubmitReceipt receipt = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationSubmitReceipt>()
			};
			return InternalCalls.Native_Navigation_ReleaseResult(ref release, ref receipt);
		}

		public static NavigationRouteValidation ValidateRoute(
			NavigationRouteHandle route, NavigationLocation currentPosition,
			ulong expectedSourceRevision = 0)
		{
			NativeNavigationRouteValidationRequest request = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationRouteValidationRequest>(),
				Route = route, CurrentPosition = currentPosition,
				ExpectedSourceRevision = expectedSourceRevision
			};
			NativeNavigationRouteValidationResult result = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationRouteValidationResult>()
			};
			NavigationStatus status = InternalCalls.Native_Navigation_ValidateRoute(ref request, ref result);
			return new NavigationRouteValidation
			{
				Status = status, Usable = result.Usable != 0, Reason = result.Reason,
				ObservedSourceRevision = result.ObservedSourceRevision
			};
		}

		public static NavigationCommandResult SetDestination(
			NavigationAgentHandle agent, NavigationLocation destination, uint routePolicy = 0,
			ulong expectedIntentGeneration = 0, ulong correlationId = 0)
		{
			NativeNavigationDestinationRequest request = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationDestinationRequest>(), Agent = agent,
				Destination = destination, RoutePolicy = routePolicy,
				ExpectedIntentGeneration = expectedIntentGeneration, CorrelationId = correlationId
			};
			NativeNavigationCommandReceipt receipt = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationCommandReceipt>()
			};
			NavigationStatus status = InternalCalls.Native_Navigation_SetDestination(ref request, ref receipt);
			return ToPublicCommand(status, receipt);
		}

		public static NavigationCommandResult ReadCommand(NavigationCommandHandle command)
		{
			NativeNavigationCommandQuery query = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationCommandQuery>(), Command = command
			};
			NativeNavigationCommandReceipt receipt = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationCommandReceipt>()
			};
			NavigationStatus status = InternalCalls.Native_Navigation_ReadCommand(ref query, ref receipt);
			return ToPublicCommand(status, receipt);
		}

		/// <summary>
		/// Resolves the generation-qualified runtime agent owned by an ordinary
		/// scene object. The object must have an enabled NavigationAgentComponent,
		/// and registration becomes visible after a protected navigation boundary.
		/// </summary>
		public static NavigationAgentState ResolveObjectAgent(Object value)
		{
			if (value == null) throw new ArgumentNullException(nameof(value));
			NativeNavigationAgentSnapshot snapshot = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationAgentSnapshot>()
			};
			NavigationStatus status = InternalCalls.Native_Navigation_ResolveObjectAgent(value.ID, ref snapshot);
			return ToPublicAgent(status, snapshot);
		}

		/// <summary>
		/// Reads copied intent, pose, request, and route state without exposing
		/// engine containers or granting gameplay ownership of locomotion.
		/// </summary>
		public static NavigationAgentState ReadAgent(NavigationAgentHandle agent)
		{
			NativeNavigationAgentSnapshot snapshot = new()
			{
				Header = NavigationAbiHeader.For<NativeNavigationAgentSnapshot>()
			};
			NavigationStatus status = InternalCalls.Native_Navigation_ReadAgent(ref agent, ref snapshot);
			return ToPublicAgent(status, snapshot);
		}

		/// <summary>
		/// Releases a runtime handle. Scene-owned agents are recreated from their
		/// enabled component at a later protected boundary when still applicable.
		/// </summary>
		public static NavigationStatus ReleaseAgent(NavigationAgentHandle agent)
		{
			return InternalCalls.Native_Navigation_ReleaseAgent(ref agent);
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
			{
				return new NavigationInvalidationResult { Status = status, NextCursor = info.PinnedGeneration };
			}
			if (info.RequiredCount > MaximumInvalidations)
			{
				return new NavigationInvalidationResult { Status = NavigationStatus.LimitExceeded, NextCursor = info.PinnedGeneration };
			}

			NativeNavigationInvalidationRecord[] native = new NativeNavigationInvalidationRecord[info.RequiredCount];
			fixed (NativeNavigationInvalidationRecord* pointer = native)
			{
				info = new NativeNavigationBufferInfo { Header = NavigationAbiHeader.For<NativeNavigationBufferInfo>() };
				status = InternalCalls.Native_Navigation_CopyInvalidations(ref cursor, pointer, (uint)native.Length, ref info);
			}
			if (status != NavigationStatus.Success || info.CopiedCount != native.Length)
			{
				return new NavigationInvalidationResult { Status = status, NextCursor = info.PinnedGeneration };
			}

			NavigationInvalidation[] records = new NavigationInvalidation[native.Length];
			for (int index = 0; index < native.Length; ++index)
			{
				records[index] = new NavigationInvalidation
				{
					Owner = native[index].Owner, Route = native[index].Route,
					Reason = native[index].Reason, Outcome = native[index].Outcome,
					SourceRevision = native[index].SourceRevision,
					ObservationGeneration = native[index].ObservationGeneration
				};
			}
			return new NavigationInvalidationResult
			{
				Status = status, NextCursor = info.PinnedGeneration, Records = records
			};
		}

		private static NavigationLocation[] CopyRoute(NavigationRouteHandle route, out NavigationStatus status)
		{
			NativeNavigationBufferInfo info = new() { Header = NavigationAbiHeader.For<NativeNavigationBufferInfo>() };
			status = InternalCalls.Native_Navigation_CopyRoute(ref route, null, 0, ref info);
			if (status != NavigationStatus.Success || info.RequiredCount == 0)
			{
				return Array.Empty<NavigationLocation>();
			}
			if (info.RequiredCount > MaximumRoutePoints)
			{
				status = NavigationStatus.LimitExceeded;
				return Array.Empty<NavigationLocation>();
			}

			NativeNavigationRoutePoint[] native = new NativeNavigationRoutePoint[info.RequiredCount];
			fixed (NativeNavigationRoutePoint* pointer = native)
			{
				info = new NativeNavigationBufferInfo { Header = NavigationAbiHeader.For<NativeNavigationBufferInfo>() };
				status = InternalCalls.Native_Navigation_CopyRoute(ref route, pointer, (uint)native.Length, ref info);
			}
			if (status != NavigationStatus.Success || info.CopiedCount != native.Length)
			{
				return Array.Empty<NavigationLocation>();
			}

			NavigationLocation[] points = new NavigationLocation[native.Length];
			for (int index = 0; index < native.Length; ++index)
			{
				points[index] = native[index].Location;
			}
			return points;
		}

		private static NavigationCommandResult ToPublicCommand(NavigationStatus status, NativeNavigationCommandReceipt receipt)
		{
			byte* diagnostic = receipt.Diagnostic;
			return new NavigationCommandResult
			{
				Status = status, State = receipt.State, Command = receipt.Command,
				ResultingRequest = receipt.ResultingRequest,
				ResultingIntentGeneration = receipt.ResultingIntentGeneration,
				Diagnostic = NavigationInterop.ReadDiagnostic(diagnostic)
			};
		}

		private static NavigationAgentState ToPublicAgent(NavigationStatus status, NativeNavigationAgentSnapshot snapshot)
		{
			return new NavigationAgentState
			{
				Status = status,
				Agent = snapshot.Agent,
				IntentGeneration = snapshot.IntentGeneration,
				PoseGeneration = snapshot.PoseGeneration,
				ActiveRequest = snapshot.ActiveRequest,
				ActiveRoute = snapshot.ActiveRoute,
				CurrentPosition = snapshot.CurrentPosition,
				Destination = snapshot.Destination,
				RoutePolicy = snapshot.RoutePolicy
			};
		}
	}
}
