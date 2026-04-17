using System;

namespace TerraEngine
{
	public enum AssetReferenceKind
	{
		Mesh,
		Material,
		Texture,
		Shader
	}

	[AttributeUsage(AttributeTargets.Field)]
	public sealed class SerializeFieldAttribute : Attribute
	{
	}

	[AttributeUsage(AttributeTargets.Field)]
	public sealed class HideInInspectorAttribute : Attribute
	{
	}

	[AttributeUsage(AttributeTargets.Field)]
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
	public sealed class TooltipAttribute : Attribute
	{
		public TooltipAttribute(string text)
		{
			Text = text;
		}

		public string Text { get; }
	}

	[AttributeUsage(AttributeTargets.Field)]
	public sealed class AssetReferenceAttribute : Attribute
	{
		public AssetReferenceAttribute(AssetReferenceKind assetKind)
		{
			AssetKind = assetKind;
		}

		public AssetReferenceKind AssetKind { get; }
	}
}
