// Copied camera authoring data. Native validation owns availability and publication.
using System;
using System.Collections.Generic;

namespace TerraEngine
{
	/// <summary>An engine UUID in canonical raw hexadecimal byte order.</summary>
	public readonly struct PostProcessAssetReference : IEquatable<PostProcessAssetReference>
	{
		private readonly string? _assetId;
		public string AssetId => _assetId ?? string.Empty;
		public bool IsNone => string.IsNullOrEmpty(_assetId);
		public static PostProcessAssetReference None => default;
		public PostProcessAssetReference(string assetId)
		{
			ArgumentNullException.ThrowIfNull(assetId);
			_ = GraphicsSettingsWire.ParseUuid(assetId);
			if (assetId == new string('0', 32)) throw new ArgumentException("Use None to clear an optional profile reference.", nameof(assetId));
			_assetId = assetId.ToLowerInvariant();
		}
		public bool Equals(PostProcessAssetReference other) => AssetId == other.AssetId;
		public override bool Equals(object? other) => other is PostProcessAssetReference value && Equals(value);
		public override int GetHashCode() => StringComparer.Ordinal.GetHashCode(AssetId);
		public override string ToString() => AssetId;
		public static bool operator ==(PostProcessAssetReference a, PostProcessAssetReference b) => a.Equals(b);
		public static bool operator !=(PostProcessAssetReference a, PostProcessAssetReference b) => !a.Equals(b);
	}

	public enum PostProcessParameterKind : uint { Boolean, Signed, Unsigned, Scalar, Vector2, Vector3, Vector4 }
	public enum PostProcessInsertionPoint : uint { HdrScene, DisplayImage, BeforeUI }
	public enum PostProcessColorDomain : uint { LinearHdr, DisplayLinear }
	public enum PostProcessInputKind : uint { SourceColor, Depth, Normals, Roughness, Motion, AssetTexture }

	/// <summary>One owned logical value; no boxed arrays or unmanaged pointers escape.</summary>
	public readonly struct PostProcessParameterValue
	{
		public string Name { get; }
		public PostProcessParameterKind Kind { get; }
		public object Value { get; }
		internal PostProcessParameterValue(string name, PostProcessParameterKind kind, object value)
		{
			if (string.IsNullOrWhiteSpace(name) || name.Length > 128) throw new ArgumentException("Invalid custom parameter name.", nameof(name));
			foreach (char character in name)
				if (!char.IsAsciiLetterOrDigit(character) && character != '_' && character != '-' && character != '.')
					throw new ArgumentException("Custom parameter names use ASCII letters, digits, underscore, dash or dot.", nameof(name));
			Name = name; Kind = kind; Value = value;
		}
		public static PostProcessParameterValue Boolean(string name, bool value) => new(name, PostProcessParameterKind.Boolean, value);
		public static PostProcessParameterValue Signed(string name, int value) => new(name, PostProcessParameterKind.Signed, value);
		public static PostProcessParameterValue Unsigned(string name, uint value) => new(name, PostProcessParameterKind.Unsigned, value);
		public static PostProcessParameterValue Scalar(string name, float value) { Finite(value); return new(name, PostProcessParameterKind.Scalar, value); }
		public static PostProcessParameterValue Vector2(string name, Vector2 value) { Finite(value.X); Finite(value.Y); return new(name, PostProcessParameterKind.Vector2, value); }
		public static PostProcessParameterValue Vector3(string name, Vector3 value) { Finite(value.X); Finite(value.Y); Finite(value.Z); return new(name, PostProcessParameterKind.Vector3, value); }
		public static PostProcessParameterValue Vector4(string name, Vector4 value) { Finite(value.X); Finite(value.Y); Finite(value.Z); Finite(value.W); return new(name, PostProcessParameterKind.Vector4, value); }
		private static void Finite(float value) { if (!float.IsFinite(value)) throw new ArgumentOutOfRangeException(nameof(value), "Custom values must be finite."); }
	}

	public sealed class PostProcessParameterDescriptor
	{
		public string Name { get; internal init; } = string.Empty;
		public PostProcessParameterKind Kind { get; internal init; }
		public uint OffsetBytes { get; internal init; }
		public PostProcessParameterValue DefaultValue { get; internal init; }
		public PostProcessParameterValue? Minimum { get; internal init; }
		public PostProcessParameterValue? Maximum { get; internal init; }
	}
	public sealed class PostProcessInputDescriptor
	{
		public PostProcessInputKind Kind { get; internal init; }
		public uint Binding { get; internal init; }
		public bool CombinedSampler { get; internal init; }
		public PostProcessAssetReference AssetId { get; internal init; }
	}
	public sealed class PostProcessEffectDescriptor
	{
		public PostProcessAssetReference AssetId { get; internal init; }
		public PostProcessAssetReference ShaderAssetId { get; internal init; }
		public ulong Revision { get; internal init; }
		public string StableId { get; internal init; } = string.Empty;
		public string EntryPoint { get; internal init; } = string.Empty;
		public PostProcessInsertionPoint Stage { get; internal init; }
		public PostProcessColorDomain Domain { get; internal init; }
		public uint ParameterBytes { get; internal init; }
		public ulong RequiredCapabilities { get; internal init; }
		public IReadOnlyList<PostProcessInputDescriptor> Inputs { get; internal init; } = Array.Empty<PostProcessInputDescriptor>();
		public IReadOnlyList<PostProcessParameterDescriptor> Parameters { get; internal init; } = Array.Empty<PostProcessParameterDescriptor>();
		public IReadOnlyList<PostProcessAssetReference> AssetDependencies { get; internal init; } = Array.Empty<PostProcessAssetReference>();
		public IReadOnlyList<string> After { get; internal init; } = Array.Empty<string>();
		public bool Available { get; internal init; }
		public GraphicsRequestReason Reason { get; internal init; }
		public string Detail { get; internal init; } = string.Empty;
	}
	public sealed class PostProcessEffectState
	{
		public PostProcessAssetReference AssetId { get; internal init; }
		public bool Enabled { get; internal init; }
		public IReadOnlyList<PostProcessParameterValue> Parameters { get; internal init; } = Array.Empty<PostProcessParameterValue>();
	}

	/// <summary>A transient camera patch; omitted enabled state inherits its value.</summary>
	public sealed class PostProcessCustomEffectPatch
	{
		public PostProcessAssetReference AssetId { get; init; }
		public bool? Enabled { get; init; }
		public IReadOnlyList<PostProcessParameterValue> Parameters { get; init; } = Array.Empty<PostProcessParameterValue>();
		public IReadOnlyList<string> ResetParameters { get; init; } = Array.Empty<string>();
	}
	public sealed class PostProcessAuthoringSnapshot
	{
		public GraphicsSettingsTarget Target { get; internal init; }
		public GraphicsSettingsLifecycle Lifecycle { get; internal init; }
		/// <summary>Local authoring revision, independent of merged graphics revision.</summary>
		public ulong Revision { get; internal init; }
		public PostProcessAssetReference AuthoredProfileAssetId { get; internal init; }
		public ulong SceneVisibleFrameId { get; internal init; }
		/// <summary>Actual accepted camera render frame; zero supplies no render proof.</summary>
		public ulong EffectiveFrameId { get; internal init; }
		public IReadOnlyList<PostProcessEffectDescriptor> Descriptors { get; internal init; } = Array.Empty<PostProcessEffectDescriptor>();
		public IReadOnlyList<PostProcessEffectState> RequestedEffects { get; internal init; } = Array.Empty<PostProcessEffectState>();
		public IReadOnlyList<PostProcessEffectState> EffectiveEffects { get; internal init; } = Array.Empty<PostProcessEffectState>();
	}
	public sealed class PostProcessAuthoringReadResult
	{
		public PostProcessAuthoringSnapshot? Snapshot { get; internal init; }
		public GraphicsRequestReason Reason { get; internal init; }
		public string Detail { get; internal init; } = string.Empty;
	}
	public sealed class PostProcessAuthoringReceipt
	{
		public GraphicsRequestReceipt Request { get; internal init; } = new();
		public ulong SceneVisibleFrameId { get; internal init; }
		public ulong EffectiveFrameId { get; internal init; }
		// Admission proves only queue ownership; it does not prove a scene flush
		// or a rendered effect. Read the native receipt for those separate facts.
		public bool HasRequest => Request.RequestId != 0;
	}
}
