// NavigationAgent.cs - gameplay-owned destination and route observation facade.

namespace TerraEngine
{
	public sealed class NavigationAgent
	{
		private readonly Object? owner;
		private NavigationCommandHandle activeCommand;
		private NavigationRequestHandle activeRequest;
		private NavigationRouteHandle activeRoute;
		private ulong intentGeneration;

		public NavigationAgent(NavigationAgentHandle handle)
		{
			Handle = handle;
		}

		private NavigationAgent(Object ownerObject, NavigationAgentState state)
		{
			owner = ownerObject;
			Handle = state.Agent;
			ApplyState(state);
		}

		public NavigationAgentHandle Handle { get; }
		public NavigationCommandHandle ActiveCommand => activeCommand;
		public NavigationRequestHandle ActiveRequest => activeRequest;
		public NavigationRouteHandle ActiveRoute => activeRoute;
		public ulong IntentGeneration => intentGeneration;
		public Object? Owner => owner;

		/// <summary>
		/// Resolves the engine-owned agent registered for a normal scene object.
		/// A null result means the component is absent, disabled, unpublished, or
		/// configured with a profile that the current scene did not register.
		/// </summary>
		public static NavigationAgent? Resolve(Object value)
		{
			NavigationAgentState state = Navigation.ResolveObjectAgent(value);
			return state.IsAvailable ? new NavigationAgent(value, state) : null;
		}

		public NavigationCommandResult SetDestination(NavigationLocation destination, uint routePolicy = 0)
		{
			NavigationCommandResult result = Navigation.SetDestination(
				Handle, destination, routePolicy, intentGeneration);
			if (result.Status == NavigationStatus.Accepted)
			{
				activeCommand = result.Command;
			}
			return result;
		}

		/// <summary>
		/// Sets a world-space destination for the resolved object's current
		/// navigation world. Navigation computes the route; gameplay still moves
		/// the Transform, character controller, or rigid body along its points.
		/// </summary>
		public NavigationCommandResult SetDestination(WorldPosition destination, uint routePolicy = 0)
		{
			NavigationAgentState state = RefreshState();
			NavigationLocation location = state.CurrentPosition;
			location.CellX = destination.Cell.X;
			location.CellY = destination.Cell.Y;
			location.CellZ = destination.Cell.Z;
			location.OffsetX = destination.OffsetX;
			location.OffsetY = destination.OffsetY;
			location.OffsetZ = destination.OffsetZ;
			return SetDestination(location, routePolicy);
		}

		/// <summary>Refreshes copied engine state for this generation-qualified handle.</summary>
		public NavigationAgentState RefreshState()
		{
			NavigationAgentState state = Navigation.ReadAgent(Handle);
			if (state.IsAvailable) ApplyState(state);
			return state;
		}

		public NavigationCommandResult RefreshDestinationCommand()
		{
			NavigationCommandResult result = Navigation.ReadCommand(activeCommand);
			if (result.State == NavigationCommandState.Applied)
			{
				intentGeneration = result.ResultingIntentGeneration;
				activeRequest = result.ResultingRequest;
				activeRoute = default;
				activeCommand = default;
			}
			else if (result.State == NavigationCommandState.Rejected || result.State == NavigationCommandState.Expired)
			{
				activeCommand = default;
			}
			return result;
		}

		public NavigationStatus Cancel()
		{
			NavigationStatus status = Navigation.CancelRoute(activeRequest);
			if (status == NavigationStatus.Success || status == NavigationStatus.StaleTarget || status == NavigationStatus.Expired)
			{
				activeRequest = default;
				activeRoute = default;
			}
			return status;
		}

		public NavigationRouteResult ReadRoute()
		{
			NavigationRouteResult request = Navigation.ReadRequest(activeRequest);
			if (request.Route.IsValid)
			{
				activeRoute = request.Route;
			}
			if (request.Status == NavigationStatus.Expired || request.Status == NavigationStatus.StaleTarget)
			{
				activeRequest = default;
				activeRoute = default;
			}
			return request;
		}

		private void ApplyState(NavigationAgentState state)
		{
			intentGeneration = state.IntentGeneration;
			activeRequest = state.ActiveRequest;
			activeRoute = state.ActiveRoute;
		}
	}
}
