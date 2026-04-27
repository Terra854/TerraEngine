// UI.cs — Contains the managed UI API surface, properties, native-call bindings, and script/editor workflow helpers for C# scripting API types that mirror native engine objects, components, and services.

using System;
using System.Collections.Generic;

namespace TerraEngine
{
	// UIElementType is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public enum UIElementType : int
	{
		Root = 0,
		VisualElement = 1,
		Label = 2,
		Button = 3,
		Image = 4
	}

	// UIEventType is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public enum UIEventType : int
	{
		Click = 0,
		MouseEnter = 1,
		MouseLeave = 2
	}

	// UIStyleProperty is the managed-facing contract for this subsystem; keep it predictable for scripts.
	internal enum UIStyleProperty : int
	{
		Width = 0,
		Height = 1,
		Color = 2,
		BackgroundColor = 3
	}

	// UI keeps the original polling helpers so existing scripts continue to
	// compile while the newer wrapper layer exposes event-style callbacks.
	public static class UI
	{
		public static bool WasElementClicked(string elementId)
			=> !string.IsNullOrWhiteSpace(elementId) && InternalCalls.Native_UI_WasElementClicked(elementId);

		// IsElementHovered bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static bool IsElementHovered(string elementId)
			=> !string.IsNullOrWhiteSpace(elementId) && InternalCalls.Native_UI_IsElementHovered(elementId);
	}

	// IStyle is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public interface IStyle
	{
		float Width { get; set; }
		float Height { get; set; }
		Vector4 Color { get; set; }
		Vector4 BackgroundColor { get; set; }
	}

	// UIStyle is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class UIStyle : IStyle
	{
		private readonly VisualElement _owner;

		internal UIStyle(VisualElement owner)
		{
			_owner = owner;
		}

		public float Width
		{
			get
			{
				_owner.EnsureAlive(nameof(Width));
				return InternalCalls.Native_UI_GetStyleFloat(_owner.NativeHandle, (int)UIStyleProperty.Width);
			}
			set
			{
				_owner.EnsureAlive(nameof(Width));
				InternalCalls.Native_UI_SetStyleFloat(_owner.NativeHandle, (int)UIStyleProperty.Width, value);
			}
		}

		public float Height
		{
			get
			{
				_owner.EnsureAlive(nameof(Height));
				return InternalCalls.Native_UI_GetStyleFloat(_owner.NativeHandle, (int)UIStyleProperty.Height);
			}
			set
			{
				_owner.EnsureAlive(nameof(Height));
				InternalCalls.Native_UI_SetStyleFloat(_owner.NativeHandle, (int)UIStyleProperty.Height, value);
			}
		}

		public Vector4 Color
		{
			get
			{
				_owner.EnsureAlive(nameof(Color));
				InternalCalls.Native_UI_GetStyleColor(_owner.NativeHandle, (int)UIStyleProperty.Color, out float x, out float y, out float z, out float w);
				return new Vector4(x, y, z, w);
			}
			set
			{
				_owner.EnsureAlive(nameof(Color));
				InternalCalls.Native_UI_SetStyleColor(_owner.NativeHandle, (int)UIStyleProperty.Color, value.X, value.Y, value.Z, value.W);
			}
		}

		public Vector4 BackgroundColor
		{
			get
			{
				_owner.EnsureAlive(nameof(BackgroundColor));
				InternalCalls.Native_UI_GetStyleColor(_owner.NativeHandle, (int)UIStyleProperty.BackgroundColor, out float x, out float y, out float z, out float w);
				return new Vector4(x, y, z, w);
			}
			set
			{
				_owner.EnsureAlive(nameof(BackgroundColor));
				InternalCalls.Native_UI_SetStyleColor(_owner.NativeHandle, (int)UIStyleProperty.BackgroundColor, value.X, value.Y, value.Z, value.W);
			}
		}
	}

	// UIEvent is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class UIEvent
	{
		public VisualElement Target { get; }
		public Vector2 MousePosition { get; }
		public UIEventType Type { get; }

		internal UIEvent(VisualElement target, Vector2 mousePosition, UIEventType type)
		{
			Target = target;
			MousePosition = mousePosition;
			Type = type;
		}
	}

	// PointerEvent is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class PointerEvent : UIEvent
	{
		internal PointerEvent(VisualElement target, Vector2 mousePosition, UIEventType type)
			: base(target, mousePosition, type)
		{
		}
	}

	// ClickEvent is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class ClickEvent : PointerEvent
	{
		internal ClickEvent(VisualElement target, Vector2 mousePosition)
			: base(target, mousePosition, UIEventType.Click)
		{
		}
	}

	// MouseEnterEvent is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class MouseEnterEvent : PointerEvent
	{
		internal MouseEnterEvent(VisualElement target, Vector2 mousePosition)
			: base(target, mousePosition, UIEventType.MouseEnter)
		{
		}
	}

	// MouseLeaveEvent is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class MouseLeaveEvent : PointerEvent
	{
		internal MouseLeaveEvent(VisualElement target, Vector2 mousePosition)
			: base(target, mousePosition, UIEventType.MouseLeave)
		{
		}
	}

	// VisualElement is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class VisualElement : IDisposable
	{
		private bool _disposed;
		private bool _lastHovered;
		private bool _usesPollingFallback;
		private UIStyle? _style;
		private Action<ClickEvent>? _clicked;
		private Action<MouseEnterEvent>? _mouseEnter;
		private Action<MouseLeaveEvent>? _mouseLeave;

		public event Action<ClickEvent>? Clicked
		{
			add
			{
				_clicked += value;
				ConfigureEventBridge(UIEventType.Click, _clicked != null);
			}
			remove
			{
				_clicked -= value;
				ConfigureEventBridge(UIEventType.Click, _clicked != null);
			}
		}

		public event Action<MouseEnterEvent>? MouseEnter
		{
			add
			{
				_mouseEnter += value;
				ConfigureEventBridge(UIEventType.MouseEnter, _mouseEnter != null);
			}
			remove
			{
				_mouseEnter -= value;
				ConfigureEventBridge(UIEventType.MouseEnter, _mouseEnter != null);
			}
		}

		public event Action<MouseLeaveEvent>? MouseLeave
		{
			add
			{
				_mouseLeave += value;
				ConfigureEventBridge(UIEventType.MouseLeave, _mouseLeave != null);
			}
			remove
			{
				_mouseLeave -= value;
				ConfigureEventBridge(UIEventType.MouseLeave, _mouseLeave != null);
			}
		}

		internal IntPtr NativeHandle { get; private set; }

		public VisualElement()
			: this(InternalCalls.Native_UI_CreateElement((int)UIElementType.VisualElement))
		{
		}

		protected VisualElement(UIElementType elementType)
			: this(InternalCalls.Native_UI_CreateElement((int)elementType))
		{
		}

		internal VisualElement(IntPtr nativeHandle)
		{
			NativeHandle = nativeHandle;
			_style = new UIStyle(this);
			UIBridge.Register(this);
		}

		~VisualElement()
		{
			Dispose(false);
		}

		public string Id
		{
			get
			{
				EnsureAlive(nameof(Id));
				return InternalCalls.ReadNativeUtf8(InternalCalls.Native_UI_GetElementId(NativeHandle));
			}
			set
			{
				EnsureAlive(nameof(Id));
				InternalCalls.Native_UI_SetElementId(NativeHandle, value ?? string.Empty);
			}
		}

		public string Name
		{
			get => Id;
			set => Id = value;
		}

		public bool Visible
		{
			get
			{
				EnsureAlive(nameof(Visible));
				return InternalCalls.Native_UI_GetElementVisible(NativeHandle);
			}
			set
			{
				EnsureAlive(nameof(Visible));
				InternalCalls.Native_UI_SetElementVisible(NativeHandle, value);
			}
		}

		public IStyle Style => _style ??= new UIStyle(this);

		// Add bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void Add(VisualElement child)
		{
			EnsureAlive(nameof(Add));
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (child == null)
			{
				throw new ArgumentNullException(nameof(child));
			}

			child.EnsureAlive(nameof(Add));
			InternalCalls.Native_UI_AddChild(NativeHandle, child.NativeHandle);
		}

		// Remove bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void Remove(VisualElement child)
		{
			EnsureAlive(nameof(Remove));
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (child == null)
			{
				throw new ArgumentNullException(nameof(child));
			}

			child.EnsureAlive(nameof(Remove));
			InternalCalls.Native_UI_RemoveChild(NativeHandle, child.NativeHandle);
		}

		// Clear bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void Clear()
		{
			EnsureAlive(nameof(Clear));
			InternalCalls.Native_UI_ClearChildren(NativeHandle);
		}

		// Q bridges managed callers to engine state; validate arguments before forwarding to native code.
		public VisualElement? Q(string id)
		{
			EnsureAlive(nameof(Q));
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (string.IsNullOrWhiteSpace(id))
			{
				return null;
			}

			return WrapHandle(InternalCalls.Native_UI_FindElementById(NativeHandle, id));
		}

		public T? Q<T>(string id) where T : VisualElement
		{
			return Q(id) as T;
		}

		public bool IsAlive => !_disposed
			&& NativeHandle != IntPtr.Zero
			&& InternalCalls.Native_UI_IsHandleAlive(NativeHandle);

		// Dispose bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void Dispose()
		{
			Dispose(true);
			GC.SuppressFinalize(this);
		}

		// Dispose bridges managed callers to engine state; validate arguments before forwarding to native code.
		protected virtual void Dispose(bool disposing)
		{
			if (_disposed)
			{
				return;
			}

			UIBridge.Unregister(this);
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (NativeHandle != IntPtr.Zero)
			{
				InternalCalls.Native_UI_ReleaseHandle(NativeHandle);
				NativeHandle = IntPtr.Zero;
			}

			_disposed = true;
		}

		// WrapHandle bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static VisualElement? WrapHandle(IntPtr nativeHandle)
		{
			if (nativeHandle == IntPtr.Zero)
			{
				return null;
			}

			UIElementType elementType = (UIElementType)InternalCalls.Native_UI_GetElementType(nativeHandle);
			return elementType switch
			{
				UIElementType.Button => new Button(nativeHandle),
				UIElementType.Label => new Label(nativeHandle),
				_ => new VisualElement(nativeHandle)
			};
		}

		// EnsureAlive bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal void EnsureAlive(string operation)
		{
			if (IsAlive)
			{
				return;
			}

			throw new InvalidOperationException($"UI element handle is stale during {operation}. Query the element again from the current UI tree.");
		}

		// ConfigureEventBridge bridges managed callers to engine state; validate arguments before forwarding to native code.
		private void ConfigureEventBridge(UIEventType eventType, bool enabled)
		{
			EnsureAlive(nameof(ConfigureEventBridge));
			bool nativeRegistered = InternalCalls.Native_UI_SetEventCallback(NativeHandle, (int)eventType, enabled);
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (!nativeRegistered && enabled)
			{
				_usesPollingFallback = true;
			}
			else if (nativeRegistered && _clicked == null && _mouseEnter == null && _mouseLeave == null)
			{
				_usesPollingFallback = false;
			}
		}

		// PumpInteractionEvents bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal void PumpInteractionEvents()
		{
			if (!IsAlive || !_usesPollingFallback)
			{
				return;
			}

			string id = Id;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (string.IsNullOrWhiteSpace(id))
			{
				_lastHovered = false;
				return;
			}

			Vector2 mousePosition = Input.MousePosition;
			bool hovered = UI.IsElementHovered(id);
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (hovered && !_lastHovered)
			{
				_mouseEnter?.Invoke(new MouseEnterEvent(this, mousePosition));
			}
			else if (!hovered && _lastHovered)
			{
				_mouseLeave?.Invoke(new MouseLeaveEvent(this, mousePosition));
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (UI.WasElementClicked(id))
			{
				_clicked?.Invoke(new ClickEvent(this, mousePosition));
			}

			_lastHovered = hovered;
		}

		// DispatchNativeEvent bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal void DispatchNativeEvent(UIEventType eventType, Vector2 pointerPosition)
		{
			switch (eventType)
			{
			case UIEventType.Click:
				_clicked?.Invoke(new ClickEvent(this, pointerPosition));
				break;
			case UIEventType.MouseEnter:
				_mouseEnter?.Invoke(new MouseEnterEvent(this, pointerPosition));
				break;
			case UIEventType.MouseLeave:
				_mouseLeave?.Invoke(new MouseLeaveEvent(this, pointerPosition));
				break;
			}
		}
	}

	// Button is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Button : VisualElement
	{
		public Button()
			: base(UIElementType.Button)
		{
		}

		internal Button(IntPtr nativeHandle)
			: base(nativeHandle)
		{
		}

		public string Text
		{
			get
			{
				EnsureAlive(nameof(Text));
				return InternalCalls.ReadNativeUtf8(InternalCalls.Native_UI_GetElementText(NativeHandle));
			}
			set
			{
				EnsureAlive(nameof(Text));
				InternalCalls.Native_UI_SetElementText(NativeHandle, value ?? string.Empty);
			}
		}
	}

	// Label is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class Label : VisualElement
	{
		public Label()
			: base(UIElementType.Label)
		{
		}

		internal Label(IntPtr nativeHandle)
			: base(nativeHandle)
		{
		}

		public string Text
		{
			get
			{
				EnsureAlive(nameof(Text));
				return InternalCalls.ReadNativeUtf8(InternalCalls.Native_UI_GetElementText(NativeHandle));
			}
			set
			{
				EnsureAlive(nameof(Text));
				InternalCalls.Native_UI_SetElementText(NativeHandle, value ?? string.Empty);
			}
		}
	}

	// UIComponent is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public class UIComponent : Component
	{
		private VisualElement? _cachedRoot;

		// GetRoot bridges managed callers to engine state; validate arguments before forwarding to native code.
		public VisualElement GetRoot()
		{
			if (_cachedRoot != null && _cachedRoot.IsAlive)
			{
				return _cachedRoot;
			}

			_cachedRoot = VisualElement.WrapHandle(InternalCalls.Native_UI_GetRootElement(Object.ID))
				?? throw new InvalidOperationException("The UIComponent does not currently expose a runtime root element.");
			return _cachedRoot;
		}
	}

	// UIBridge is the managed-facing contract for this subsystem; keep it predictable for scripts.
	internal static class UIBridge
	{
		private static readonly object s_syncRoot = new();
		private static readonly List<WeakReference<VisualElement>> s_knownElements = new();
		private static readonly Dictionary<nint, WeakReference<VisualElement>> s_elementsByHandle = new();
		private static ulong s_lastProcessedFrame;

		// Register bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static void Register(VisualElement element)
		{
			lock (s_syncRoot)
			{
				s_knownElements.Add(new WeakReference<VisualElement>(element));
				s_elementsByHandle[element.NativeHandle] = new WeakReference<VisualElement>(element);
			}
		}

		// Unregister bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static void Unregister(VisualElement element)
		{
			lock (s_syncRoot)
			{
				for (int index = s_knownElements.Count - 1; index >= 0; --index)
				{
					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (!s_knownElements[index].TryGetTarget(out VisualElement? knownElement) || ReferenceEquals(knownElement, element))
					{
						s_knownElements.RemoveAt(index);
					}
				}

				s_elementsByHandle.Remove(element.NativeHandle);
			}
		}

		// PumpPendingEvents bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static void PumpPendingEvents()
		{
			ulong frame = InternalCalls.Native_UI_GetInteractionFrame();
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (frame == s_lastProcessedFrame)
			{
				return;
			}

			List<VisualElement> liveElements = new();
			lock (s_syncRoot)
			{
				for (int index = s_knownElements.Count - 1; index >= 0; --index)
				{
					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (!s_knownElements[index].TryGetTarget(out VisualElement? element) || !element.IsAlive)
					{
						s_knownElements.RemoveAt(index);
						continue;
					}

					liveElements.Add(element);
				}
			}

			foreach (VisualElement element in liveElements)
			{
				element.PumpInteractionEvents();
			}

			s_lastProcessedFrame = frame;
		}

		// DispatchNativeEvent bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static void DispatchNativeEvent(IntPtr elementHandle, UIEventType eventType, Vector2 pointerPosition)
		{
			VisualElement? target = null;
			lock (s_syncRoot)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (s_elementsByHandle.TryGetValue(elementHandle, out WeakReference<VisualElement>? weakElement))
				{
					if (!weakElement.TryGetTarget(out target) || !target.IsAlive)
					{
						s_elementsByHandle.Remove(elementHandle);
						target = null;
					}
				}
			}

			target?.DispatchNativeEvent(eventType, pointerPosition);
		}
	}
}
