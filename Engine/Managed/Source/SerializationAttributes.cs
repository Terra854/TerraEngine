using System;

namespace TerraEngine
{
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
}
