using System;
#if !TERRAENGINE_EXPORT
using System.Globalization;
using System.IO;
#endif
using TerraEngine;

// Observe the production hover/click publication; never synthesize pointer input
// or increment a count merely because a control is visible. Labels make actual
// installed input observations readable in the Game View and exported window.
public sealed partial class TextureUiObserver : Script
{
#if !TERRAENGINE_EXPORT
	// The runner precreates its owned directory. Optional receipts supplement
	// screenshots and real pointer observation; they cannot prove physical input.
	private readonly string? _observationRoot = Environment.GetEnvironmentVariable("TERRAENGINE_TEXTURE_UI_OBSERVATION_ROOT");
	private ulong _updates;
	private bool _lastPointerDown;
#endif
	private Label? _hoverLabel;
	private Label? _clickLabel;
	private VisualElement? _root;
	private int _translatedClicks;
	private int _frontClicks;
	private int _backClicks;
	private int _hiddenClicks;
	private string _lastHover = string.Empty;
	private string _lastCounts = string.Empty;

	public override void OnUpdate()
	{
		if (_root == null || !_root.IsAlive)
		{
			var document = Object.GetComponent<UIComponent>();
			if (document == null) return;
			// The first script frame can precede asynchronous document publication.
			// Wait for the normal root; no diagnostic bypass or replacement tree.
			try { _root = document.GetRoot(); }
			catch (InvalidOperationException) { return; }
			_hoverLabel = _root.Q<Label>("hover-status");
			_clickLabel = _root.Q<Label>("click-status");
			var hiddenParent = _root.Q("hidden-parent");
			if (_hoverLabel == null || _clickLabel == null || hiddenParent == null)
				throw new InvalidOperationException("The installed UI observation document is incomplete.");
			hiddenParent.Visible = false;
		}

		if (UI.WasElementClicked("translated-button")) ++_translatedClicks;
		if (UI.WasElementClicked("overlap-front")) ++_frontClicks;
		if (UI.WasElementClicked("overlap-back")) ++_backClicks;
		if (UI.WasElementClicked("hidden-button")) ++_hiddenClicks;
		string hover = "Ready; hover translated: " + UI.IsElementHovered("translated-button")
			+ "; front: " + UI.IsElementHovered("overlap-front")
			+ "; back: " + UI.IsElementHovered("overlap-back")
			+ "; hidden: " + UI.IsElementHovered("hidden-button");
		string counts = "Translated clicks: " + _translatedClicks + "; front: " + _frontClicks
			+ "; back: " + _backClicks + "; hidden: " + _hiddenClicks;
		bool observationChanged = hover != _lastHover || counts != _lastCounts;
		// Avoid marking the document dirty when its visible observation is unchanged.
		if (hover != _lastHover) { _hoverLabel!.Text = hover; _lastHover = hover; }
		if (counts != _lastCounts) { _clickLabel!.Text = counts; _lastCounts = counts; }
#if !TERRAENGINE_EXPORT
		// This public input query includes both the initial press and held frames.
		// Persist transitions immediately so a real drag can distinguish a held
		// pointer from repeated clicks, including its eventual release.
		bool pointerDown = Input.IsMouseButtonPressed(MouseButton.Left);
		observationChanged |= pointerDown != _lastPointerDown;
		_lastPointerDown = pointerDown;
		++_updates;
		// This disposable validation observer samples each held update so receipt
		// revisions distinguish actual press/hold frames without changing the UI.
		if (!string.IsNullOrWhiteSpace(_observationRoot) && (observationChanged || pointerDown || _updates % 60 == 0))
		{
			string receipt = "{\"schemaVersion\":1,\"ready\":true,\"updates\":" + _updates.ToString(CultureInfo.InvariantCulture)
				+ ",\"pointerDown\":" + pointerDown.ToString().ToLowerInvariant()
				+ ",\"translatedClicks\":" + _translatedClicks.ToString(CultureInfo.InvariantCulture)
				+ ",\"frontClicks\":" + _frontClicks.ToString(CultureInfo.InvariantCulture)
				+ ",\"backClicks\":" + _backClicks.ToString(CultureInfo.InvariantCulture)
				+ ",\"hiddenClicks\":" + _hiddenClicks.ToString(CultureInfo.InvariantCulture)
				+ ",\"translatedHovered\":" + UI.IsElementHovered("translated-button").ToString().ToLowerInvariant()
				+ ",\"frontHovered\":" + UI.IsElementHovered("overlap-front").ToString().ToLowerInvariant()
				+ ",\"backHovered\":" + UI.IsElementHovered("overlap-back").ToString().ToLowerInvariant()
				+ ",\"hiddenHovered\":" + UI.IsElementHovered("hidden-button").ToString().ToLowerInvariant() + "}";
			try
			{
				string temporary = Path.Combine(_observationRoot, "texture-ui.pending");
				File.WriteAllText(temporary, receipt);
				File.Move(temporary, Path.Combine(_observationRoot, "texture-ui.json"), true);
			}
			catch (IOException error) { Log.Warning("UI observation receipt could not be persisted: " + error.Message); }
		}
#endif
	}
}
