namespace TerraEngine
{
	// UI exposes frame-based queries for authored runtime UI so gameplay scripts
	// can react to button clicks and hover state in both the editor Game View
	// and exported builds through the same managed API.
	public static class UI
	{
		public static bool WasElementClicked(string elementId)
			=> !string.IsNullOrWhiteSpace(elementId) && InternalCalls.Native_UI_WasElementClicked(elementId);

		public static bool IsElementHovered(string elementId)
			=> !string.IsNullOrWhiteSpace(elementId) && InternalCalls.Native_UI_IsElementHovered(elementId);
	}
}
