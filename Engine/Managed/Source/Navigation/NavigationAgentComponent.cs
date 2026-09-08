// NavigationAgentComponent.cs - ordinary scene-object access to ground routing.

namespace TerraEngine
{
	/// <summary>
	/// Authoring component that asks the engine to maintain a runtime navigation
	/// agent for this object. RuntimeAgent becomes available after the component
	/// and its Transform cross their protected publication boundary.
	/// </summary>
	public sealed class NavigationAgentComponent : Component
	{
		public NavigationAgent? RuntimeAgent => NavigationAgent.Resolve(Object);

		/// <summary>
		/// Convenience entry point for gameplay. The returned command is queued;
		/// poll the runtime agent to observe the request and completed route.
		/// </summary>
		public NavigationCommandResult SetDestination(WorldPosition destination, uint routePolicy = 0)
		{
			NavigationAgent? runtime = RuntimeAgent;
			return runtime == null
				? new NavigationCommandResult { Status = NavigationStatus.Unavailable, Diagnostic = "The object has no published navigation agent." }
				: runtime.SetDestination(destination, routePolicy);
		}
	}
}
