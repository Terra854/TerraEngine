using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.Loader;

namespace TerraEngine
{
	// Collectible load contexts let TerraEngine swap gameplay assemblies at
	// runtime without restarting the editor process after each successful save.
	internal sealed class GameplayScriptLoadContext : AssemblyLoadContext
	{
		private static readonly Assembly s_engineBridgeAssembly = typeof(Script).Assembly;
		private static readonly string s_engineBridgeAssemblyName = s_engineBridgeAssembly.GetName().Name ?? "TerraEngine.CSharp";
		private readonly AssemblyDependencyResolver _dependencyResolver;

		public GameplayScriptLoadContext(string mainAssemblyPath)
			: base("TerraEngine.Gameplay", isCollectible: true)
		{
			_dependencyResolver = new AssemblyDependencyResolver(mainAssemblyPath);
		}

		protected override Assembly? Load(AssemblyName assemblyName)
		{
			// Reuse the already loaded engine bridge so gameplay Script types inherit
			// from the same TerraEngine.Script identity across every reload.
			if (string.Equals(assemblyName.Name, s_engineBridgeAssemblyName, StringComparison.Ordinal))
			{
				return s_engineBridgeAssembly;
			}

			string? resolvedAssemblyPath = _dependencyResolver.ResolveAssemblyToPath(assemblyName);
			if (resolvedAssemblyPath != null)
			{
				return LoadFromAssemblyPath(resolvedAssemblyPath);
			}

			return null;
		}

		protected override IntPtr LoadUnmanagedDll(string unmanagedDllName)
		{
			string? resolvedPath = _dependencyResolver.ResolveUnmanagedDllToPath(unmanagedDllName);
			if (resolvedPath != null)
			{
				return LoadUnmanagedDllFromPath(resolvedPath);
			}

			return IntPtr.Zero;
		}
	}

	// Reflection payloads describe the public editable surface of a script type
	// in a JSON-friendly way that native code can persist and display.
	internal sealed class ScriptTypeDescriptorPayload
	{
		public string FullTypeName { get; set; } = string.Empty;
		public List<ScriptFieldDescriptorPayload> Fields { get; set; } = new();
		public List<ScriptFieldDiagnosticPayload> Diagnostics { get; set; } = new();
	}

	internal sealed class ScriptFieldDescriptorPayload
	{
		public string FieldPath { get; set; } = string.Empty;
		public string SerializedName { get; set; } = string.Empty;
		public string DisplayName { get; set; } = string.Empty;
		public string ValueKind { get; set; } = "Unsupported";
		public string ContainerKind { get; set; } = "Scalar";
		public bool Editable { get; set; }
		public bool HiddenInInspector { get; set; }
		public string ReadOnlyReason { get; set; } = string.Empty;
		public int Order { get; set; }
		public string Tooltip { get; set; } = string.Empty;
		public double? RangeMin { get; set; }
		public double? RangeMax { get; set; }
		public string TypeFingerprint { get; set; } = string.Empty;
		public object? DefaultValue { get; set; }
		public List<string> EnumOptions { get; set; } = new();
		public List<ScriptFieldDescriptorPayload> Children { get; set; } = new();
	}

	internal sealed class ScriptFieldDiagnosticPayload
	{
		public string DiagnosticId { get; set; } = string.Empty;
		public string Scope { get; set; } = "Field";
		public string FieldPath { get; set; } = string.Empty;
		public string Severity { get; set; } = "Info";
		public string Code { get; set; } = string.Empty;
		public string Message { get; set; } = string.Empty;
		public List<string> RepairActionIds { get; set; } = new();
	}

	public static class ScriptReflection
	{
		public static bool IsAttachableScriptType(Type? type)
		{
			return type != null
				&& type.IsClass
				&& !type.IsAbstract
				&& type.IsPublic
				&& typeof(Script).IsAssignableFrom(type);
		}

		public static Dictionary<string, Type> BuildAttachableTypeCatalog(Assembly? assembly)
		{
			var catalog = new Dictionary<string, Type>(StringComparer.Ordinal);
			if (assembly == null)
			{
				return catalog;
			}

			foreach (Type type in assembly.GetTypes())
			{
				if (!IsAttachableScriptType(type))
				{
					continue;
				}

				string key = type.FullName ?? type.Name;
				catalog[key] = type;
			}

			return catalog;
		}

		internal static ScriptTypeDescriptorPayload DescribeScriptType(Type? type)
		{
			var payload = new ScriptTypeDescriptorPayload
			{
				FullTypeName = type?.FullName ?? type?.Name ?? string.Empty
			};

			if (!IsAttachableScriptType(type))
			{
				if (!string.IsNullOrWhiteSpace(payload.FullTypeName))
				{
					payload.Diagnostics.Add(CreateDiagnostic(
						"binding.invalidType",
						string.Empty,
						"Binding",
						"Error",
						"InvalidType",
						$"{payload.FullTypeName} is not an attachable TerraEngine.Script type."));
				}

				return payload;
			}

			int nextOrder = 0;
			foreach (FieldInfo field in GetEligibleFields(type!))
			{
				payload.Fields.Add(BuildDescriptorForField(field, field.Name, ref nextOrder, payload.Diagnostics, new HashSet<Type>()));
			}

			return payload;
		}

		public static string NicifyIdentifier(string identifier)
		{
			if (string.IsNullOrWhiteSpace(identifier))
			{
				return string.Empty;
			}

			var characters = new List<char>(identifier.Length * 2);
			for (int index = 0; index < identifier.Length; ++index)
			{
				char current = identifier[index];
				if (current == '_' || current == '-')
				{
					if (characters.Count > 0 && characters[^1] != ' ')
					{
						characters.Add(' ');
					}
					continue;
				}

				bool shouldInsertSpace = false;
				if (index > 0 && characters.Count > 0 && characters[^1] != ' ')
				{
					char previous = identifier[index - 1];
					char next = index + 1 < identifier.Length ? identifier[index + 1] : '\0';
					shouldInsertSpace =
						(char.IsUpper(current) && (char.IsLower(previous) || char.IsDigit(previous)))
						|| (char.IsUpper(current) && char.IsUpper(previous) && next != '\0' && char.IsLower(next))
						|| (char.IsDigit(current) && char.IsLetter(previous))
						|| (char.IsLetter(current) && char.IsDigit(previous));
				}

				if (shouldInsertSpace)
				{
					characters.Add(' ');
				}

				characters.Add(current);
			}

			return new string(characters.ToArray());
		}

		internal static IEnumerable<FieldInfo> GetEligibleFields(Type type)
		{
			var inheritance = new Stack<Type>();
			for (Type? current = type; current != null && current != typeof(Script) && current != typeof(Component) && current != typeof(object); current = current.BaseType)
			{
				inheritance.Push(current);
			}

			while (inheritance.Count > 0)
			{
				Type current = inheritance.Pop();
				// Walk base-to-derived so inspector order stays stable and matches
				// the way inherited serialized fields appear in the final object.
				foreach (FieldInfo field in current.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
				{
					if (ShouldSerializeField(field))
					{
						yield return field;
					}
				}
			}
		}

		private static bool ShouldSerializeField(FieldInfo field)
		{
			if (field.IsStatic || field.IsInitOnly || field.IsLiteral)
			{
				return false;
			}

			if (field.IsDefined(typeof(NonSerializedAttribute), inherit: true))
			{
				return false;
			}

			return field.IsPublic || field.IsDefined(typeof(SerializeFieldAttribute), inherit: true);
		}

		private static ScriptFieldDescriptorPayload BuildDescriptorForField(FieldInfo field, string fieldPath, ref int nextOrder, List<ScriptFieldDiagnosticPayload> diagnostics, HashSet<Type> recursionGuard)
		{
			RangeAttribute? range = field.GetCustomAttribute<RangeAttribute>(inherit: true);
			string tooltip = field.GetCustomAttribute<TooltipAttribute>(inherit: true)?.Text ?? string.Empty;
			bool hidden = field.IsDefined(typeof(HideInInspectorAttribute), inherit: true);
			return BuildDescriptorForType(
				field.FieldType,
				fieldPath,
				field.Name,
				NicifyIdentifier(field.Name),
				ref nextOrder,
				diagnostics,
				recursionGuard,
				hidden,
				tooltip,
				range);
		}

		private static ScriptFieldDescriptorPayload BuildDescriptorForType(Type valueType, string fieldPath, string serializedName, string displayName, ref int nextOrder, List<ScriptFieldDiagnosticPayload> diagnostics, HashSet<Type> recursionGuard, bool hiddenInInspector, string tooltip, RangeAttribute? range)
		{
			var descriptor = new ScriptFieldDescriptorPayload
			{
				FieldPath = fieldPath,
				SerializedName = serializedName,
				DisplayName = displayName,
				HiddenInInspector = hiddenInInspector,
				Tooltip = tooltip,
				Order = nextOrder++,
				TypeFingerprint = BuildTypeFingerprint(valueType),
				Editable = !hiddenInInspector
			};

			if (TryConfigureScalarDescriptor(valueType, descriptor, range))
			{
				FinalizeDescriptor(descriptor);
				return descriptor;
			}

			if (valueType.IsEnum)
			{
				descriptor.ValueKind = "Enum";
				descriptor.ContainerKind = "Scalar";
				descriptor.EnumOptions = new List<string>(Enum.GetNames(valueType));
				FinalizeDescriptor(descriptor);
				return descriptor;
			}

			if (TryGetCollectionElementType(valueType, out Type? elementType, out string collectionKind))
			{
				descriptor.ValueKind = collectionKind;
				descriptor.ContainerKind = "Collection";
				descriptor.ReadOnlyReason = hiddenInInspector ? "Hidden in inspector." : string.Empty;
				// Collections reuse one element template descriptor so native code
				// and the inspector can materialize per-index children on demand.
				int templateOrder = 0;
				descriptor.Children.Add(BuildDescriptorForType(
					elementType!,
					fieldPath + "[]",
					serializedName,
					"Element",
					ref templateOrder,
					diagnostics,
					new HashSet<Type>(recursionGuard),
					false,
					string.Empty,
					null));
				FinalizeDescriptor(descriptor);
				return descriptor;
			}

			if (IsNestedSerializableType(valueType))
			{
				descriptor.ValueKind = "NestedObject";
				descriptor.ContainerKind = "NestedObject";
				descriptor.ReadOnlyReason = hiddenInInspector ? "Hidden in inspector." : string.Empty;

				if (!recursionGuard.Add(valueType))
				{
					descriptor.ValueKind = "Unsupported";
					descriptor.ContainerKind = "Scalar";
					descriptor.Editable = false;
					descriptor.ReadOnlyReason = "Recursive object graphs are not supported by the first-release script inspector.";
					diagnostics.Add(CreateDiagnostic(
						fieldPath + ".recursive",
						fieldPath,
						"Field",
						"Warning",
						"UnsupportedType",
						descriptor.ReadOnlyReason));
					FinalizeDescriptor(descriptor);
					return descriptor;
				}

				foreach (FieldInfo childField in EnumerateNestedFields(valueType))
				{
					string childPath = fieldPath + "." + childField.Name;
					descriptor.Children.Add(BuildDescriptorForField(childField, childPath, ref nextOrder, diagnostics, new HashSet<Type>(recursionGuard)));
				}

				recursionGuard.Remove(valueType);
				FinalizeDescriptor(descriptor);
				return descriptor;
			}

			descriptor.ValueKind = "Unsupported";
			descriptor.ContainerKind = "Scalar";
			descriptor.Editable = false;
			// Unsupported fields stay visible as diagnostics instead of silently
			// disappearing, which would make authored data loss harder to notice.
			descriptor.ReadOnlyReason = BuildUnsupportedReason(valueType);
			diagnostics.Add(CreateDiagnostic(
				fieldPath + ".unsupported",
				fieldPath,
				"Field",
				"Warning",
				"UnsupportedType",
				descriptor.ReadOnlyReason));
			FinalizeDescriptor(descriptor);
			return descriptor;
		}

		private static void FinalizeDescriptor(ScriptFieldDescriptorPayload descriptor)
		{
			if (descriptor.HiddenInInspector)
			{
				descriptor.Editable = false;
				if (string.IsNullOrWhiteSpace(descriptor.ReadOnlyReason))
				{
					descriptor.ReadOnlyReason = "Hidden in inspector.";
				}
			}
		}

		private static bool TryConfigureScalarDescriptor(Type valueType, ScriptFieldDescriptorPayload descriptor, RangeAttribute? range)
		{
			if (valueType == typeof(bool))
			{
				descriptor.ValueKind = "Bool";
				return true;
			}

			if (valueType == typeof(int))
			{
				descriptor.ValueKind = "Int";
				descriptor.RangeMin = range?.Min;
				descriptor.RangeMax = range?.Max;
				return true;
			}

			if (valueType == typeof(float))
			{
				descriptor.ValueKind = "Float";
				descriptor.RangeMin = range?.Min;
				descriptor.RangeMax = range?.Max;
				return true;
			}

			if (valueType == typeof(string))
			{
				descriptor.ValueKind = "String";
				return true;
			}

			if (valueType == typeof(Vector2))
			{
				descriptor.ValueKind = "Vector2";
				return true;
			}

			if (valueType == typeof(Vector3))
			{
				descriptor.ValueKind = "Vector3";
				return true;
			}

			if (valueType == typeof(Vector4))
			{
				descriptor.ValueKind = "Vector4";
				return true;
			}

			if (valueType == typeof(Quaternion))
			{
				descriptor.ValueKind = "Quaternion";
				return true;
			}

			if (valueType == typeof(Object) || valueType == typeof(uint))
			{
				descriptor.ValueKind = "EntityReference";
				return true;
			}

			return false;
		}

		private static IEnumerable<FieldInfo> EnumerateNestedFields(Type type)
		{
			foreach (FieldInfo field in GetEligibleFields(type))
			{
				yield return field;
			}
		}

		internal static bool IsNestedSerializableType(Type valueType)
		{
			if (valueType.IsPrimitive || valueType.IsEnum || valueType == typeof(string))
			{
				return false;
			}

			if (typeof(Script).IsAssignableFrom(valueType) || typeof(Component).IsAssignableFrom(valueType) || valueType == typeof(Object))
			{
				return false;
			}

			if (TryGetCollectionElementType(valueType, out _, out _))
			{
				return false;
			}

			return valueType.IsValueType || valueType.IsDefined(typeof(SerializableAttribute), inherit: false);
		}

		internal static bool TryGetCollectionElementType(Type valueType, out Type? elementType, out string collectionKind)
		{
			if (valueType.IsArray)
			{
				elementType = valueType.GetElementType();
				collectionKind = "Array";
				return elementType != null;
			}

			if (valueType.IsGenericType && valueType.GetGenericTypeDefinition() == typeof(List<>))
			{
				elementType = valueType.GetGenericArguments()[0];
				collectionKind = "List";
				return true;
			}

			elementType = null;
			collectionKind = string.Empty;
			return false;
		}

		private static string BuildTypeFingerprint(Type type)
		{
			return type.AssemblyQualifiedName ?? type.FullName ?? type.Name;
		}

		private static string BuildUnsupportedReason(Type unsupportedType)
		{
			string displayName = unsupportedType.FullName ?? unsupportedType.Name;
			return $"TerraEngine does not yet expose a safe default inspector for {displayName}. The field stays visible as a diagnostic so authored data is not silently lost.";
		}

		private static ScriptFieldDiagnosticPayload CreateDiagnostic(string diagnosticId, string fieldPath, string scope, string severity, string code, string message)
		{
			return new ScriptFieldDiagnosticPayload
			{
				DiagnosticId = diagnosticId,
				FieldPath = fieldPath,
				Scope = scope,
				Severity = severity,
				Code = code,
				Message = message
			};
		}
	}
}
