// SerializationAttributes.cs — Contains the managed Serialization Attributes API surface, properties, native-call bindings, and script/editor workflow helpers for C# scripting API types that mirror native engine objects, components, and services.

using System;

namespace TerraEngine
{
	// AssetReferenceKind is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public enum AssetReferenceKind
	{
		Mesh,
		Material,
		Texture,
		Shader
	}

	[AttributeUsage(AttributeTargets.Field)]
	// SerializeFieldAttribute is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class SerializeFieldAttribute : Attribute
	{
	}

	[AttributeUsage(AttributeTargets.Field)]
	// HideInInspectorAttribute is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class HideInInspectorAttribute : Attribute
	{
	}

	[AttributeUsage(AttributeTargets.Field)]
	// RangeAttribute is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class RangeAttribute : Attribute
	{
		public RangeAttribute(float min, float max)
		{
			Min = min;
			Max = max;
		}

		public float Min { get; }
		public float Max { get; }
	}

	[AttributeUsage(AttributeTargets.Field)]
	// TooltipAttribute is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class TooltipAttribute : Attribute
	{
		public TooltipAttribute(string text)
		{
			Text = text;
		}

		public string Text { get; }
	}

	[AttributeUsage(AttributeTargets.Field)]
	// AssetReferenceAttribute is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class AssetReferenceAttribute : Attribute
	{
		public AssetReferenceAttribute(AssetReferenceKind assetKind)
		{
			AssetKind = assetKind;
		}

		public AssetReferenceKind AssetKind { get; }
	}
}
