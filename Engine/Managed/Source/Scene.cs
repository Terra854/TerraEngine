// Scene.cs — Managed API for scene and prefab lifecycle management.
//
// This file provides the C# interface for loading and switching between scenes
// and prefabs. It coordinates with the native SceneManager to handle asset
// resolution and runtime scene graph updates, allowing scripts to control the
// high-level organization of the gameplay environment.

using System;
using System.Text.Json;

namespace TerraEngine
{
	public struct SceneRenderSettings
	{
		public float IndirectStrength;
		public float AmbientOcclusionStrength;
		public uint AmbientOcclusionQuality;
		public ulong Generation { get; internal set; }
	}

	// Structural scene edits are synchronized by the native mutation queue at
	// the end of the engine update pass. Scripts can request scene changes
	// during gameplay, and systems observe them on the next stable frame
	// boundary instead of midway through component iteration.
	// Scene is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public static class Scene
	{
		public static string ActiveScene => InternalCalls.ReadNativeUtf8(InternalCalls.Native_Scene_GetActiveScene());

		public static string[] LoadedScenes
		{
			get
			{
				string json = InternalCalls.ReadNativeUtf8(InternalCalls.Native_Scene_GetLoadedScenes());
				try { return JsonSerializer.Deserialize<string[]>(json) ?? Array.Empty<string>(); }
				catch (JsonException) { return Array.Empty<string>(); }
			}
		}

		public static SceneRenderSettings RenderSettings
		{
			get
			{
				NativeSceneRenderSettings native = new() { Header = NativeRuntimeAbiHeader.For<NativeSceneRenderSettings>() };
				InternalCalls.Native_Scene_ReadRenderSettings(ref native);
				return new SceneRenderSettings
				{
					IndirectStrength = native.IndirectStrength,
					AmbientOcclusionStrength = native.AmbientOcclusionStrength,
					AmbientOcclusionQuality = native.AmbientOcclusionQuality,
					Generation = native.Generation
				};
			}
			set
			{
				RuntimeStatus status = ApplyRenderSettings(value);
				if (status != RuntimeStatus.Accepted && status != RuntimeStatus.Success)
					throw new ArgumentException($"Native scene rendering rejected the settings with status {status}.", nameof(value));
			}
		}

		public static RuntimeStatus ApplyRenderSettings(SceneRenderSettings settings)
		{
			NativeSceneRenderSettings native = new()
			{
				Header = NativeRuntimeAbiHeader.For<NativeSceneRenderSettings>(),
				IndirectStrength = settings.IndirectStrength,
				AmbientOcclusionStrength = settings.AmbientOcclusionStrength,
				AmbientOcclusionQuality = settings.AmbientOcclusionQuality,
				Generation = settings.Generation
			};
			return InternalCalls.Native_Scene_SubmitRenderSettings(ref native);
		}
		public static RuntimeStatus LoadScene(string path)
		{
			if (string.IsNullOrWhiteSpace(path)) return RuntimeStatus.InvalidArgument;
			return InternalCalls.Native_Scene_RequestLoad(path);
		}

		// LoadPrefab bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static RuntimeStatus LoadPrefab(string path)
		{
			if (string.IsNullOrWhiteSpace(path)) return RuntimeStatus.InvalidArgument;
			return InternalCalls.Native_Scene_RequestLoadPrefab(path);
		}

		// SetActiveScene bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static RuntimeStatus SetActiveScene(string uuid)
		{
			if (string.IsNullOrWhiteSpace(uuid)) return RuntimeStatus.InvalidArgument;
			return InternalCalls.Native_Scene_RequestSetActive(uuid);
		}

		// Prefab publication is deferred so scripts cannot invalidate object
		// iteration midway through the current scheduler phase.
		public static RuntimeStatus InstantiatePrefab(string path, Object? parent = null)
		{
			if (string.IsNullOrWhiteSpace(path)) return RuntimeStatus.InvalidArgument;
			return InternalCalls.Native_Scene_InstantiatePrefab(path, parent?.ID ?? Object.InvalidID);
		}

		public static RuntimeStatus UnloadAll() => InternalCalls.Native_Scene_RequestUnloadAll();

		// SubmitTransactionJson queues a validated scene transaction for the next
		// protected mutation flush. Scripts can prepare these payloads on worker
		// callbacks without directly mutating live native scene storage.
		public static string SubmitTransactionJson(string transactionJson)
		{
			return InternalCalls.ReadNativeUtf8(InternalCalls.Native_Scene_SubmitTransactionJson(transactionJson ?? string.Empty));
		}
	}
}
