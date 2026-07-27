// ScriptRegistryGenerator.cs — Roslyn source generator for script registration
//
// Implements a C# source generator that automatically discovers script types and generates the registration code required for NativeAOT export. It performs static analysis on the codebase to build field accessors and ensure unique script identifiers for the export runtime.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace TerraEngine.Export.Generator
{
	[Generator]
	// ScriptRegistryGenerator is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public sealed class ScriptRegistryGenerator : ISourceGenerator
	{
		private static readonly DiagnosticDescriptor s_collisionDiagnostic = new(
			id: "TEEXP001",
			title: "NativeAOT script ID collision",
			messageFormat: "Type '{0}' hashes to the same exported scriptId as '{1}'. Rename one type or namespace to make the NativeAOT identifier unique.",
			category: "TerraEngine.Export",
			defaultSeverity: DiagnosticSeverity.Error,
			isEnabledByDefault: true);

		private static readonly DiagnosticDescriptor s_partialRequiredDiagnostic = new(
			id: "TEEXP002",
			title: "Partial script required for NativeAOT export fields",
			messageFormat: "Type '{0}' declares supported serialized fields for NativeAOT export and must be marked partial so generated accessors can reach those fields",
			category: "TerraEngine.Export",
			defaultSeverity: DiagnosticSeverity.Error,
			isEnabledByDefault: true);

		// Initialize bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void Initialize(GeneratorInitializationContext context)
		{
		}

		// Execute bridges managed callers to engine state; validate arguments before forwarding to native code.
		public void Execute(GeneratorExecutionContext context)
		{
			INamedTypeSymbol? scriptBaseType = context.Compilation.GetTypeByMetadataName("TerraEngine.Script");
			INamedTypeSymbol? serializeFieldAttribute = context.Compilation.GetTypeByMetadataName("TerraEngine.SerializeFieldAttribute");
			INamedTypeSymbol? nonSerializedAttribute = context.Compilation.GetTypeByMetadataName("System.NonSerializedAttribute");
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (scriptBaseType == null || serializeFieldAttribute == null || nonSerializedAttribute == null)
			{
				return;
			}

			List<ScriptTypeModel> models = DiscoverScriptTypes(context, scriptBaseType, serializeFieldAttribute, nonSerializedAttribute);
			ReportCollisions(context, models);
			ReportMissingPartialTypes(context, models);
			ResolveHelperChains(models);
			context.AddSource("TerraEngine_Export_Generated.g.cs", SourceText.From(BuildGeneratedSource(models), Encoding.UTF8));
		}

		// DiscoverScriptTypes bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static List<ScriptTypeModel> DiscoverScriptTypes(
			GeneratorExecutionContext context,
			INamedTypeSymbol scriptBaseType,
			INamedTypeSymbol serializeFieldAttribute,
			INamedTypeSymbol nonSerializedAttribute)
		{
			var discoveredTypes = new Dictionary<INamedTypeSymbol, ScriptTypeModel>(SymbolEqualityComparer.Default);

			foreach (SyntaxTree syntaxTree in context.Compilation.SyntaxTrees)
			{
				SemanticModel semanticModel = context.Compilation.GetSemanticModel(syntaxTree);
				foreach (ClassDeclarationSyntax classDeclaration in syntaxTree.GetRoot().DescendantNodes().OfType<ClassDeclarationSyntax>())
				{
					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (semanticModel.GetDeclaredSymbol(classDeclaration) is not INamedTypeSymbol typeSymbol)
					{
						continue;
					}

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (!InheritsFrom(typeSymbol, scriptBaseType))
					{
						continue;
					}

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (typeSymbol.TypeKind != TypeKind.Class || typeSymbol.IsGenericType || typeSymbol.ContainingType != null)
					{
						continue;
					}

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (discoveredTypes.ContainsKey(typeSymbol))
					{
						continue;
					}

					var model = new ScriptTypeModel
					{
						Symbol = typeSymbol,
						FullTypeName = typeSymbol.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat),
						QualifiedTypeName = typeSymbol.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
						NamespaceName = typeSymbol.ContainingNamespace?.IsGlobalNamespace == false
							? typeSymbol.ContainingNamespace.ToDisplayString()
							: string.Empty,
						TypeName = typeSymbol.Name,
						TypeModifiers = BuildTypeModifiers(typeSymbol),
						IsPartial = IsPartial(typeSymbol),
						IsAttachable = IsAttachable(typeSymbol, scriptBaseType),
						Location = typeSymbol.Locations.FirstOrDefault(location => location.IsInSource)
					};

					model.OwnFields.AddRange(CollectSupportedOwnFields(typeSymbol, serializeFieldAttribute, nonSerializedAttribute));
					model.ScriptId = ComputeFnv64(model.FullTypeName);
					discoveredTypes.Add(typeSymbol, model);
				}
			}

			return discoveredTypes.Values
				.OrderBy(model => model.FullTypeName, StringComparer.Ordinal)
				.ToList();
		}

		// ReportCollisions bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static void ReportCollisions(GeneratorExecutionContext context, IEnumerable<ScriptTypeModel> models)
		{
			var seenIds = new Dictionary<ulong, ScriptTypeModel>();
			foreach (ScriptTypeModel model in models.Where(model => model.IsAttachable))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (seenIds.TryGetValue(model.ScriptId, out ScriptTypeModel? previous))
				{
					if (!string.Equals(previous.FullTypeName, model.FullTypeName, StringComparison.Ordinal))
					{
						context.ReportDiagnostic(Diagnostic.Create(
							s_collisionDiagnostic,
							model.Location,
							model.FullTypeName,
							previous.FullTypeName));
					}

					continue;
				}

				seenIds.Add(model.ScriptId, model);
			}
		}

		// ReportMissingPartialTypes bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static void ReportMissingPartialTypes(GeneratorExecutionContext context, IEnumerable<ScriptTypeModel> models)
		{
			foreach (ScriptTypeModel model in models)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (model.OwnFields.Count == 0 || model.IsPartial)
				{
					continue;
				}

				context.ReportDiagnostic(Diagnostic.Create(
					s_partialRequiredDiagnostic,
					model.Location,
					model.FullTypeName));
			}
		}

		// ResolveHelperChains bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static void ResolveHelperChains(IReadOnlyList<ScriptTypeModel> models)
		{
			var map = new Dictionary<INamedTypeSymbol, ScriptTypeModel>(SymbolEqualityComparer.Default);
			foreach (ScriptTypeModel model in models)
			{
				map[model.Symbol] = model;
			}

			foreach (ScriptTypeModel model in models)
			{
				model.HelperBaseModel = FindNearestHelperBase(map, model.Symbol.BaseType);

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (model.OwnFields.Count > 0 && model.IsPartial)
				{
					model.AccessorProviderModel = model;
				}
				else
				{
					model.AccessorProviderModel = model.HelperBaseModel;
				}
			}
		}

		// FindNearestHelperBase bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static ScriptTypeModel? FindNearestHelperBase(
			IReadOnlyDictionary<INamedTypeSymbol, ScriptTypeModel> models,
			INamedTypeSymbol? baseType)
		{
			for (INamedTypeSymbol? current = baseType; current != null; current = current.BaseType)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (!models.TryGetValue(current, out ScriptTypeModel? model))
				{
					continue;
				}

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (model.OwnFields.Count > 0 && model.IsPartial)
				{
					return model;
				}
			}

			return null;
		}

		// CollectSupportedOwnFields bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static List<ExportFieldModel> CollectSupportedOwnFields(
			INamedTypeSymbol typeSymbol,
			INamedTypeSymbol serializeFieldAttribute,
			INamedTypeSymbol nonSerializedAttribute)
		{
			var fields = typeSymbol.GetMembers()
				.OfType<IFieldSymbol>()
				.Where(field => SymbolEqualityComparer.Default.Equals(field.ContainingType, typeSymbol))
				.Where(field => ShouldSerializeField(field, serializeFieldAttribute, nonSerializedAttribute))
				.OrderBy(field => field.DeclaringSyntaxReferences.FirstOrDefault()?.Span.Start ?? int.MaxValue)
				.ThenBy(field => field.Name, StringComparer.Ordinal);

			var supportedFields = new List<ExportFieldModel>();
			foreach (IFieldSymbol field in fields)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (!TryCreateFieldModel(field, out ExportFieldModel? model))
				{
					continue;
				}

				supportedFields.Add(model!);
			}

			return supportedFields;
		}

		// ShouldSerializeField bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static bool ShouldSerializeField(
			IFieldSymbol field,
			INamedTypeSymbol serializeFieldAttribute,
			INamedTypeSymbol nonSerializedAttribute)
		{
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (field.IsStatic || field.IsConst || field.IsReadOnly)
			{
				return false;
			}

			foreach (AttributeData attribute in field.GetAttributes())
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (SymbolEqualityComparer.Default.Equals(attribute.AttributeClass, nonSerializedAttribute))
				{
					return false;
				}
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (field.DeclaredAccessibility == Accessibility.Public)
			{
				return true;
			}

			foreach (AttributeData attribute in field.GetAttributes())
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (SymbolEqualityComparer.Default.Equals(attribute.AttributeClass, serializeFieldAttribute))
				{
					return true;
				}
			}

			return false;
		}

		// TryCreateFieldModel bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static bool TryCreateFieldModel(IFieldSymbol field, out ExportFieldModel? model)
		{
			model = null;
			ITypeSymbol fieldType = field.Type;
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (fieldType.SpecialType == SpecialType.System_Boolean)
			{
				model = CreateFieldModel(field, "bool", "global::TerraEngine.Export.ExportFieldValueKind.Bool", 1);
				return true;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (fieldType.SpecialType == SpecialType.System_Int32)
			{
				model = CreateFieldModel(field, "int", "global::TerraEngine.Export.ExportFieldValueKind.Int", 4);
				return true;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (fieldType.SpecialType == SpecialType.System_Single)
			{
				model = CreateFieldModel(field, "float", "global::TerraEngine.Export.ExportFieldValueKind.Float", 4);
				return true;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (fieldType.SpecialType == SpecialType.System_UInt32)
			{
				model = CreateFieldModel(field, "uint", "global::TerraEngine.Export.ExportFieldValueKind.EntityReference", 4);
				return true;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (fieldType.SpecialType == SpecialType.System_String)
			{
				model = CreateFieldModel(field, "string", "global::TerraEngine.Export.ExportFieldValueKind.String", 256);
				return true;
			}

			string qualifiedName = fieldType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (string.Equals(qualifiedName, "global::TerraEngine.Vector2", StringComparison.Ordinal))
			{
				model = CreateFieldModel(field, "global::TerraEngine.Vector2", "global::TerraEngine.Export.ExportFieldValueKind.Vector2", 8);
				return true;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (string.Equals(qualifiedName, "global::TerraEngine.Vector3", StringComparison.Ordinal))
			{
				model = CreateFieldModel(field, "global::TerraEngine.Vector3", "global::TerraEngine.Export.ExportFieldValueKind.Vector3", 12);
				return true;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (string.Equals(qualifiedName, "global::TerraEngine.Vector4", StringComparison.Ordinal))
			{
				model = CreateFieldModel(field, "global::TerraEngine.Vector4", "global::TerraEngine.Export.ExportFieldValueKind.Vector4", 16);
				return true;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (string.Equals(qualifiedName, "global::TerraEngine.Quaternion", StringComparison.Ordinal))
			{
				model = CreateFieldModel(field, "global::TerraEngine.Quaternion", "global::TerraEngine.Export.ExportFieldValueKind.Quaternion", 16);
				return true;
			}

			return false;
		}

		// CreateFieldModel bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static ExportFieldModel CreateFieldModel(IFieldSymbol field, string typeName, string valueKindExpression, int valueSize)
		{
			return new ExportFieldModel
			{
				FieldName = field.Name,
				TypeName = typeName,
				ValueKindExpression = valueKindExpression,
				ValueSize = valueSize,
				NameHash = ComputeFnv64(field.Name)
			};
		}

		// InheritsFrom bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static bool InheritsFrom(INamedTypeSymbol typeSymbol, INamedTypeSymbol baseType)
		{
			for (INamedTypeSymbol? current = typeSymbol; current != null; current = current.BaseType)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (SymbolEqualityComparer.Default.Equals(current, baseType))
				{
					return true;
				}
			}

			return false;
		}

		// IsAttachable bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static bool IsAttachable(INamedTypeSymbol typeSymbol, INamedTypeSymbol scriptBaseType)
		{
			return typeSymbol.TypeKind == TypeKind.Class
				&& !typeSymbol.IsAbstract
				&& typeSymbol.DeclaredAccessibility == Accessibility.Public
				&& InheritsFrom(typeSymbol, scriptBaseType)
				&& typeSymbol.ContainingType == null;
		}

		// IsPartial bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static bool IsPartial(INamedTypeSymbol typeSymbol)
		{
			foreach (SyntaxReference declarationReference in typeSymbol.DeclaringSyntaxReferences)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (declarationReference.GetSyntax() is not ClassDeclarationSyntax declaration)
				{
					continue;
				}

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (declaration.Modifiers.Any(modifier => modifier.IsKind(Microsoft.CodeAnalysis.CSharp.SyntaxKind.PartialKeyword)))
				{
					return true;
				}
			}

			return false;
		}

		// BuildTypeModifiers bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static string BuildTypeModifiers(INamedTypeSymbol typeSymbol)
		{
			var tokens = new List<string>();
			switch (typeSymbol.DeclaredAccessibility)
			{
			case Accessibility.Public:
				tokens.Add("public");
				break;
			case Accessibility.Internal:
				tokens.Add("internal");
				break;
			default:
				tokens.Add("internal");
				break;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (typeSymbol.IsAbstract)
			{
				tokens.Add("abstract");
			}
			else if (typeSymbol.IsSealed)
			{
				tokens.Add("sealed");
			}

			return string.Join(" ", tokens);
		}

		// ComputeFnv64 bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static ulong ComputeFnv64(string text)
		{
			const ulong offsetBasis = 14695981039346656037UL;
			const ulong prime = 1099511628211UL;

			ulong hash = offsetBasis;
			foreach (byte value in Encoding.UTF8.GetBytes(text))
			{
				hash ^= value;
				hash *= prime;
			}

			return hash;
		}

		// BuildGeneratedSource bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static string BuildGeneratedSource(IReadOnlyList<ScriptTypeModel> models)
		{
			var source = new StringBuilder();
			source.AppendLine("// <auto-generated/>");
			source.AppendLine("#nullable enable");
			source.AppendLine("using System;");
			source.AppendLine("using System.Collections.Generic;");
			source.AppendLine("using System.Runtime.CompilerServices;");
			source.AppendLine("using System.Runtime.InteropServices;");
			source.AppendLine("using TerraEngine.Export;");
			source.AppendLine();

			foreach (ScriptTypeModel model in models)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (!model.IsPartial || model.OwnFields.Count == 0)
				{
					continue;
				}

				EmitHelperPartial(source, model);
			}

			EmitRegistry(source, models);
			return source.ToString();
		}

		// EmitHelperPartial bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static void EmitHelperPartial(StringBuilder source, ScriptTypeModel model)
		{
			if (!string.IsNullOrEmpty(model.NamespaceName))
			{
				source.Append("namespace ").Append(model.NamespaceName).AppendLine();
				source.AppendLine("{");
			}

			source.Append('\t').Append(model.TypeModifiers).Append(" unsafe partial class ").Append(model.TypeName).AppendLine();
			source.Append('\t').AppendLine("{");

			string baseCountExpression = model.HelperBaseModel != null
				? model.HelperBaseModel.QualifiedTypeName + ".__TerraEngine_ExportFieldCount"
				: "0";
			string baseDescriptorExpression = model.HelperBaseModel != null
				? model.HelperBaseModel.QualifiedTypeName + ".__TerraEngine_ExportFieldDescriptors"
				: "Array.Empty<global::TerraEngine.Export.ExportFieldDescriptor>()";

			source.Append("\t\tinternal static readonly int __TerraEngine_BaseFieldCount = ").Append(baseCountExpression).AppendLine(";");
			source.Append("\t\tinternal static readonly global::TerraEngine.Export.ExportFieldDescriptor[] __TerraEngine_OwnExportFields = new global::TerraEngine.Export.ExportFieldDescriptor[]").AppendLine();
			source.AppendLine("\t\t{");
			for (int index = 0; index < model.OwnFields.Count; ++index)
			{
				ExportFieldModel field = model.OwnFields[index];
				source.Append("\t\t\tglobal::TerraEngine.Export.Generated.GeneratedScriptRegistry.CreateDescriptor(")
					.Append(index.ToString(CultureInfo.InvariantCulture))
					.Append(", ")
					.Append(field.ValueKindExpression)
					.Append(", ")
					.Append(field.ValueSize.ToString(CultureInfo.InvariantCulture))
					.Append(", ")
					.Append(field.NameHash.ToString(CultureInfo.InvariantCulture))
					.Append("UL, \"")
					.Append(EscapeStringLiteral(field.FieldName))
					.AppendLine("\"),");
			}
			source.AppendLine("\t\t};");
			source.Append("\t\tinternal static readonly global::TerraEngine.Export.ExportFieldDescriptor[] __TerraEngine_ExportFieldDescriptors = global::TerraEngine.Export.Generated.GeneratedScriptRegistry.CombineDescriptors(")
				.Append(baseDescriptorExpression)
				.AppendLine(", __TerraEngine_OwnExportFields);");
			source.Append("\t\tinternal static readonly int __TerraEngine_ExportFieldCount = __TerraEngine_BaseFieldCount + ")
				.Append(model.OwnFields.Count.ToString(CultureInfo.InvariantCulture))
				.AppendLine(";");
			source.AppendLine();

			source.AppendLine("\t\tinternal static void __TerraEngine_GetFieldValue(global::TerraEngine.Script instance, int fieldIndex, void* valueBuffer, int valueSize)");
			source.AppendLine("\t\t{");
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (model.HelperBaseModel != null)
			{
				source.AppendLine("\t\t\tif (fieldIndex < __TerraEngine_BaseFieldCount)");
				source.AppendLine("\t\t\t{");
				source.Append('\t').Append('\t').Append('\t').Append(model.HelperBaseModel.QualifiedTypeName).AppendLine(".__TerraEngine_GetFieldValue(instance, fieldIndex, valueBuffer, valueSize);");
				source.AppendLine("\t\t\t\treturn;");
				source.AppendLine("\t\t\t}");
			}
			source.Append('\t').Append('\t').Append(model.QualifiedTypeName).AppendLine(" typedInstance = (" + model.QualifiedTypeName + ")instance;");
			source.AppendLine("\t\t\tswitch (fieldIndex - __TerraEngine_BaseFieldCount)");
			source.AppendLine("\t\t\t{");
			for (int index = 0; index < model.OwnFields.Count; ++index)
			{
				ExportFieldModel field = model.OwnFields[index];
				source.Append("\t\t\tcase ").Append(index.ToString(CultureInfo.InvariantCulture)).AppendLine(":");
				source.AppendLine("\t\t\t{");
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (string.Equals(field.TypeName, "string", StringComparison.Ordinal))
				{
					source.Append("\t\t\t\tglobal::TerraEngine.Export.ExportBridge.TryWriteString(valueBuffer, valueSize, typedInstance.")
						.Append(field.FieldName)
						.AppendLine(");");
				}
				else
				{
					source.Append("\t\t\t\tglobal::TerraEngine.Export.ExportBridge.TryWriteValue(valueBuffer, valueSize, typedInstance.")
						.Append(field.FieldName)
						.AppendLine(");");
				}
				source.AppendLine("\t\t\t\treturn;");
				source.AppendLine("\t\t\t}");
			}
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();

			source.AppendLine("\t\tinternal static void __TerraEngine_SetFieldValue(global::TerraEngine.Script instance, int fieldIndex, void* valueBuffer, int valueSize)");
			source.AppendLine("\t\t{");
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (model.HelperBaseModel != null)
			{
				source.AppendLine("\t\t\tif (fieldIndex < __TerraEngine_BaseFieldCount)");
				source.AppendLine("\t\t\t{");
				source.Append('\t').Append('\t').Append('\t').Append(model.HelperBaseModel.QualifiedTypeName).AppendLine(".__TerraEngine_SetFieldValue(instance, fieldIndex, valueBuffer, valueSize);");
				source.AppendLine("\t\t\t\treturn;");
				source.AppendLine("\t\t\t}");
			}
			source.Append('\t').Append('\t').Append(model.QualifiedTypeName).AppendLine(" typedInstance = (" + model.QualifiedTypeName + ")instance;");
			source.AppendLine("\t\t\tswitch (fieldIndex - __TerraEngine_BaseFieldCount)");
			source.AppendLine("\t\t\t{");
			for (int index = 0; index < model.OwnFields.Count; ++index)
			{
				ExportFieldModel field = model.OwnFields[index];
				source.Append("\t\t\tcase ").Append(index.ToString(CultureInfo.InvariantCulture)).AppendLine(":");
				source.AppendLine("\t\t\t{");
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (string.Equals(field.TypeName, "string", StringComparison.Ordinal))
				{
					source.AppendLine("\t\t\t\tif (global::TerraEngine.Export.ExportBridge.TryReadString(valueBuffer, valueSize, out string nextValue))");
				}
				else
				{
					source.Append("\t\t\t\tif (global::TerraEngine.Export.ExportBridge.TryReadValue(valueBuffer, valueSize, out ")
						.Append(field.TypeName)
						.AppendLine(" nextValue))");
				}
				source.AppendLine("\t\t\t\t{");
				source.Append("\t\t\t\t\ttypedInstance.").Append(field.FieldName).AppendLine(" = nextValue;");
				source.AppendLine("\t\t\t\t}");
				source.AppendLine("\t\t\t\treturn;");
				source.AppendLine("\t\t\t}");
			}
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.Append('\t').AppendLine("}");

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (!string.IsNullOrEmpty(model.NamespaceName))
			{
				source.AppendLine("}");
			}

			source.AppendLine();
		}

		// EmitRegistry bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static void EmitRegistry(StringBuilder source, IReadOnlyList<ScriptTypeModel> models)
		{
			source.AppendLine("namespace TerraEngine.Export.Generated");
			source.AppendLine("{");
			source.AppendLine("\tinternal static unsafe class GeneratedScriptRegistry");
			source.AppendLine("\t{");
			source.AppendLine("\t\tinternal unsafe delegate void FieldAccessorDelegate(global::TerraEngine.Script instance, int fieldIndex, void* valueBuffer, int valueSize);");
			source.AppendLine();
			source.AppendLine("\t\tinternal readonly struct ScriptEntry");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tinternal ScriptEntry(ulong id, string name, Func<global::TerraEngine.Script> factory, global::TerraEngine.Export.ExportFieldDescriptor[] fields, FieldAccessorDelegate? getFieldValue, FieldAccessorDelegate? setFieldValue)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tId = id;");
			source.AppendLine("\t\t\t\tName = name;");
			source.AppendLine("\t\t\t\tFactory = factory;");
			source.AppendLine("\t\t\t\tFields = fields;");
			source.AppendLine("\t\t\t\tGetFieldValue = getFieldValue;");
			source.AppendLine("\t\t\t\tSetFieldValue = setFieldValue;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tinternal ulong Id { get; }");
			source.AppendLine("\t\t\tinternal string Name { get; }");
			source.AppendLine("\t\t\tinternal Func<global::TerraEngine.Script> Factory { get; }");
			source.AppendLine("\t\t\tinternal global::TerraEngine.Export.ExportFieldDescriptor[] Fields { get; }");
			source.AppendLine("\t\t\tinternal FieldAccessorDelegate? GetFieldValue { get; }");
			source.AppendLine("\t\t\tinternal FieldAccessorDelegate? SetFieldValue { get; }");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\tinternal sealed class ScriptHandleRecord");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tinternal ScriptHandleRecord(global::TerraEngine.Script instance, ScriptEntry entry)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tInstance = instance;");
			source.AppendLine("\t\t\t\tEntry = entry;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tinternal global::TerraEngine.Script Instance { get; }");
			source.AppendLine("\t\t\tinternal ScriptEntry Entry { get; }");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\tinternal static readonly object s_InstanceLock = new();");
			source.AppendLine("\t\tinternal static readonly Dictionary<IntPtr, ScriptHandleRecord> s_Instances = new();");
			source.AppendLine();
			source.AppendLine("\t\tinternal static readonly ScriptEntry[] Entries = new ScriptEntry[]");
			source.AppendLine("\t\t{");

			foreach (ScriptTypeModel model in models.Where(model => model.IsAttachable))
			{
				string fieldArrayExpression;
				string getAccessorExpression;
				string setAccessorExpression;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (model.AccessorProviderModel != null)
				{
					fieldArrayExpression = model.AccessorProviderModel.QualifiedTypeName + ".__TerraEngine_ExportFieldDescriptors";
					getAccessorExpression = model.AccessorProviderModel.QualifiedTypeName + ".__TerraEngine_GetFieldValue";
					setAccessorExpression = model.AccessorProviderModel.QualifiedTypeName + ".__TerraEngine_SetFieldValue";
				}
				else
				{
					fieldArrayExpression = "Array.Empty<global::TerraEngine.Export.ExportFieldDescriptor>()";
					getAccessorExpression = "null";
					setAccessorExpression = "null";
				}

				source.Append("\t\t\tnew ScriptEntry(")
					.Append(model.ScriptId.ToString(CultureInfo.InvariantCulture))
					.Append("UL, \"")
					.Append(EscapeStringLiteral(model.FullTypeName))
					.Append("\", static () => new ")
					.Append(model.QualifiedTypeName)
					.Append("(), ")
					.Append(fieldArrayExpression)
					.Append(", ")
					.Append(getAccessorExpression)
					.Append(", ")
					.Append(setAccessorExpression)
					.AppendLine("),");
			}

			source.AppendLine("\t\t};");
			source.AppendLine();
			source.AppendLine("\t\tinternal static int ScriptCount => Entries.Length;");
			source.AppendLine();
			source.AppendLine("\t\tinternal static global::TerraEngine.Export.ExportFieldDescriptor CreateDescriptor(int fieldIndex, global::TerraEngine.Export.ExportFieldValueKind valueKind, int valueSize, ulong nameHash, string name)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tglobal::TerraEngine.Export.ExportFieldDescriptor descriptor = new global::TerraEngine.Export.ExportFieldDescriptor");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tFieldIndex = fieldIndex,");
			source.AppendLine("\t\t\t\tValueKind = (int)valueKind,");
			source.AppendLine("\t\t\t\tValueSize = valueSize,");
			source.AppendLine("\t\t\t\tNameHash = nameHash");
			source.AppendLine("\t\t\t};");
			source.AppendLine("\t\t\tglobal::TerraEngine.Export.ExportBridge.CopyUtf8Name(ref descriptor, name);");
			source.AppendLine("\t\t\treturn descriptor;");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\tinternal static global::TerraEngine.Export.ExportFieldDescriptor[] CombineDescriptors(global::TerraEngine.Export.ExportFieldDescriptor[] inheritedDescriptors, global::TerraEngine.Export.ExportFieldDescriptor[] ownDescriptors)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tif (inheritedDescriptors.Length == 0)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn ownDescriptors;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tif (ownDescriptors.Length == 0)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn inheritedDescriptors;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tglobal::TerraEngine.Export.ExportFieldDescriptor[] combined = new global::TerraEngine.Export.ExportFieldDescriptor[inheritedDescriptors.Length + ownDescriptors.Length];");
			source.AppendLine("\t\t\tArray.Copy(inheritedDescriptors, 0, combined, 0, inheritedDescriptors.Length);");
			source.AppendLine("\t\t\tArray.Copy(ownDescriptors, 0, combined, inheritedDescriptors.Length, ownDescriptors.Length);");
			source.AppendLine("\t\t\treturn combined;");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\tinternal static bool TryFindById(ulong scriptId, out ScriptEntry entry)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tfor (int index = 0; index < Entries.Length; ++index)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tif (Entries[index].Id == scriptId)");
			source.AppendLine("\t\t\t\t{");
			source.AppendLine("\t\t\t\t\tentry = Entries[index];");
			source.AppendLine("\t\t\t\t\treturn true;");
			source.AppendLine("\t\t\t\t}");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tentry = default;");
			source.AppendLine("\t\t\treturn false;");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\tinternal static ScriptHandleRecord? GetHandleRecord(IntPtr handle)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tlock (s_InstanceLock)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\ts_Instances.TryGetValue(handle, out ScriptHandleRecord? record);");
			source.AppendLine("\t\t\t\treturn record;");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine("\t}");
			source.AppendLine();
			source.AppendLine("\tpublic static unsafe class ExportBootstrap");
			source.AppendLine("\t{");
			// The legacy caller cannot report its buffer capacity, so it may receive
			// only the released v1 prefix. New hosts select the capacity-aware v3
			// entry point before the generator writes the physics and animation tails.
			source.AppendLine("\t\t[UnmanagedCallersOnly(EntryPoint = \"InitManagedRuntime\", CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tpublic static int InitManagedRuntime(global::TerraEngine.Export.ManagedVTable* outTable)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tif (outTable == null)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn -1;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\ttry");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tPopulateV1(outTable);");
			source.AppendLine("\t\t\t\tLogRegisteredScripts();");
			source.AppendLine("\t\t\t\treturn 0;");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t\tcatch (Exception ex)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tglobal::TerraEngine.Log.Error($\"InitManagedRuntime failed: {ex}\");");
			source.AppendLine("\t\t\t\treturn -1;");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(EntryPoint = \"InitManagedRuntimeV3\", CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tpublic static int InitManagedRuntimeV3(global::TerraEngine.Export.ManagedVTable* outTable, ulong hostCapacity)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tif ((ulong)sizeof(global::TerraEngine.Export.ManagedVTable) != global::TerraEngine.Export.ManagedVTable.V3Size)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn -3;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tif (outTable == null || hostCapacity < global::TerraEngine.Export.ManagedVTable.V3Size)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn -2;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\ttry");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tPopulateV3(outTable);");
			source.AppendLine("\t\t\t\tLogRegisteredScripts();");
			source.AppendLine("\t\t\t\treturn 0;");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t\tcatch (Exception ex)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tglobal::TerraEngine.Log.Error($\"InitManagedRuntimeV3 failed: {ex}\");");
			source.AppendLine("\t\t\t\treturn -1;");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\tprivate static void PopulateV1(global::TerraEngine.Export.ManagedVTable* outTable)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\toutTable->Version = 1;");
			source.AppendLine("\t\t\toutTable->GetScriptCount = &GetScriptCount;");
			source.AppendLine("\t\t\toutTable->GetScriptIds = &GetScriptIds;");
			source.AppendLine("\t\t\toutTable->CreateScript = &CreateScript;");
			source.AppendLine("\t\t\toutTable->DestroyScript = &DestroyScript;");
			source.AppendLine("\t\t\toutTable->InvokeAwake = &InvokeAwake;");
			source.AppendLine("\t\t\toutTable->InvokeStart = &InvokeStart;");
			source.AppendLine("\t\t\toutTable->InvokeUpdate = &InvokeUpdate;");
			source.AppendLine("\t\t\toutTable->InvokeDisable = &InvokeDisable;");
			source.AppendLine("\t\t\toutTable->GetFieldCount = &GetFieldCount;");
			source.AppendLine("\t\t\toutTable->GetFieldDescriptors = &GetFieldDescriptors;");
			source.AppendLine("\t\t\toutTable->GetFieldValue = &GetFieldValue;");
			source.AppendLine("\t\t\toutTable->SetFieldValue = &SetFieldValue;");
			source.AppendLine("\t\t\toutTable->InvokeUpdateBatch = &InvokeUpdateBatch;");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\tprivate static void PopulateV3(global::TerraEngine.Export.ManagedVTable* outTable)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tPopulateV1(outTable);");
			source.AppendLine("\t\t\toutTable->Version = 3;");
			source.AppendLine("\t\t\toutTable->TableSize = global::TerraEngine.Export.ManagedVTable.V3Size;");
			source.AppendLine("\t\t\toutTable->GetPhysicsAbiVersion = &GetPhysicsAbiVersion;");
			source.AppendLine("\t\t\toutTable->DispatchPhysicsEventBatch = &DispatchPhysicsEventBatch;");
			source.AppendLine("\t\t\toutTable->GetAnimationAbiVersion = &GetAnimationAbiVersion;");
			source.AppendLine("\t\t\toutTable->DispatchAnimationEventBatch = &DispatchAnimationEventBatch;");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\tprivate static void LogRegisteredScripts()");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tfor (int index = 0; index < GeneratedScriptRegistry.Entries.Length; ++index)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tGeneratedScriptRegistry.ScriptEntry entry = GeneratedScriptRegistry.Entries[index];");
			source.AppendLine("\t\t\t\tglobal::TerraEngine.Log.Info($\"[Managed] Registered Script: {entry.Name} (ID: {entry.Id})\");");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static int GetScriptCount() => GeneratedScriptRegistry.ScriptCount;");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static uint GetPhysicsAbiVersion() => global::TerraEngine.PhysicsAbiHeader.Version;");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static ushort GetAnimationAbiVersion() => global::TerraEngine.Animation.AnimationAbi.Version;");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void DispatchPhysicsEventBatch(void* events, uint eventCount, ulong worldId, uint worldGeneration, ulong tick)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\ttry");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tglobal::TerraEngine.PhysicsEvents.DispatchProtectedNativeCore(events, eventCount, worldId, worldGeneration, tick);");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t\tcatch (Exception exception)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tglobal::TerraEngine.Log.Error($\"Physics event batch dispatch failed safely: {exception.Message}\");");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void DispatchAnimationEventBatch(void* events, uint eventCount, long sourceTick, ulong publicationGeneration, uint overflowed, uint droppedCountLowerBound)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\ttry");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tglobal::TerraEngine.Animation.AnimationEvents.DispatchProtectedNativeCore(events, eventCount, sourceTick, publicationGeneration, overflowed, droppedCountLowerBound);");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t\tcatch (Exception exception)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tglobal::TerraEngine.Log.Error($\"Animation event batch dispatch failed safely: {exception.Message}\");");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void GetScriptIds(ulong* outputBuffer, int capacity)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tif (outputBuffer == null || capacity <= 0)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tint count = Math.Min(capacity, GeneratedScriptRegistry.Entries.Length);");
			source.AppendLine("\t\t\tfor (int index = 0; index < count; ++index)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\toutputBuffer[index] = GeneratedScriptRegistry.Entries[index].Id;");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static IntPtr CreateScript(ulong scriptId, uint objectId)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tif (!GeneratedScriptRegistry.TryFindById(scriptId, out GeneratedScriptRegistry.ScriptEntry entry))");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn IntPtr.Zero;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tglobal::TerraEngine.Script instance = entry.Factory();");
			source.AppendLine("\t\t\tinstance.BindEngineInstance(objectId);");
			source.AppendLine("\t\t\tGCHandle handle = GCHandle.Alloc(instance);");
			source.AppendLine("\t\t\tIntPtr handlePointer = GCHandle.ToIntPtr(handle);");
			source.AppendLine("\t\t\tlock (GeneratedScriptRegistry.s_InstanceLock)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tGeneratedScriptRegistry.s_Instances[handlePointer] = new GeneratedScriptRegistry.ScriptHandleRecord(instance, entry);");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t\treturn handlePointer;");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void DestroyScript(IntPtr handle)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tif (handle == IntPtr.Zero)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tlock (GeneratedScriptRegistry.s_InstanceLock)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tGeneratedScriptRegistry.s_Instances.Remove(handle);");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tGCHandle.FromIntPtr(handle).Free();");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void InvokeAwake(IntPtr handle)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tGeneratedScriptRegistry.ScriptHandleRecord? record = GeneratedScriptRegistry.GetHandleRecord(handle);");
			source.AppendLine("\t\t\tif (record != null)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\trecord.Instance.InvokeCreated();");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void InvokeStart(IntPtr handle)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tGeneratedScriptRegistry.ScriptHandleRecord? record = GeneratedScriptRegistry.GetHandleRecord(handle);");
			source.AppendLine("\t\t\tif (record != null)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\trecord.Instance.InvokeStartIfNeeded();");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void InvokeUpdate(IntPtr handle, float deltaTime)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tglobal::TerraEngine.UI.PumpPendingEvents();");
			source.AppendLine("\t\t\tGeneratedScriptRegistry.ScriptHandleRecord? record = GeneratedScriptRegistry.GetHandleRecord(handle);");
			source.AppendLine("\t\t\tif (record != null)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\trecord.Instance.DispatchFrame(deltaTime);");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void InvokeUpdateBatch(global::TerraEngine.Export.ExportScriptBatchItem* items, int itemCount)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tif (items == null || itemCount <= 0)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tglobal::TerraEngine.UI.PumpPendingEvents();");
			source.AppendLine();
			source.AppendLine("\t\t\tfor (int index = 0; index < itemCount; ++index)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\tglobal::TerraEngine.Export.ExportScriptBatchItem item = items[index];");
			source.AppendLine("\t\t\t\tGeneratedScriptRegistry.ScriptHandleRecord? record = GeneratedScriptRegistry.GetHandleRecord(item.Handle);");
			source.AppendLine("\t\t\t\tif (record != null)");
			source.AppendLine("\t\t\t\t{");
			source.AppendLine("\t\t\t\t\tglobal::TerraEngine.InternalCalls.PushScriptDispatchContext(item.ObjectId, IntPtr.Zero, item.FrameOrder, item.WorkerDispatch, 1, item.ObjectGeneration, item.ComponentGeneration, item.BindingGeneration);");
			source.AppendLine("\t\t\t\t\ttry");
			source.AppendLine("\t\t\t\t\t{");
			source.AppendLine("\t\t\t\t\t\trecord.Instance.DispatchFrame(item.DeltaTime);");
			source.AppendLine("\t\t\t\t\t}");
			source.AppendLine("\t\t\t\t\tfinally");
			source.AppendLine("\t\t\t\t\t{");
			source.AppendLine("\t\t\t\t\t\tglobal::TerraEngine.InternalCalls.PopScriptDispatchContext();");
			source.AppendLine("\t\t\t\t\t}");
			source.AppendLine("\t\t\t\t}");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void InvokeDisable(IntPtr handle)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tGeneratedScriptRegistry.ScriptHandleRecord? record = GeneratedScriptRegistry.GetHandleRecord(handle);");
			source.AppendLine("\t\t\tif (record != null)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\trecord.Instance.OnDisable();");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static int GetFieldCount(ulong scriptId)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\treturn GeneratedScriptRegistry.TryFindById(scriptId, out GeneratedScriptRegistry.ScriptEntry entry) ? entry.Fields.Length : 0;");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void GetFieldDescriptors(ulong scriptId, global::TerraEngine.Export.ExportFieldDescriptor* outputBuffer, int capacity)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tif (outputBuffer == null || capacity <= 0)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tif (!GeneratedScriptRegistry.TryFindById(scriptId, out GeneratedScriptRegistry.ScriptEntry entry))");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\treturn;");
			source.AppendLine("\t\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t\tint count = Math.Min(capacity, entry.Fields.Length);");
			source.AppendLine("\t\t\tfor (int index = 0; index < count; ++index)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\toutputBuffer[index] = entry.Fields[index];");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void GetFieldValue(IntPtr handle, int fieldIndex, void* valueBuffer, int valueSize)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tGeneratedScriptRegistry.ScriptHandleRecord? record = GeneratedScriptRegistry.GetHandleRecord(handle);");
			source.AppendLine("\t\t\tif (record != null)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\trecord.Entry.GetFieldValue?.Invoke(record.Instance, fieldIndex, valueBuffer, valueSize);");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine();
			source.AppendLine("\t\t[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
			source.AppendLine("\t\tprivate static void SetFieldValue(IntPtr handle, int fieldIndex, void* valueBuffer, int valueSize)");
			source.AppendLine("\t\t{");
			source.AppendLine("\t\t\tGeneratedScriptRegistry.ScriptHandleRecord? record = GeneratedScriptRegistry.GetHandleRecord(handle);");
			source.AppendLine("\t\t\tif (record != null)");
			source.AppendLine("\t\t\t{");
			source.AppendLine("\t\t\t\trecord.Entry.SetFieldValue?.Invoke(record.Instance, fieldIndex, valueBuffer, valueSize);");
			source.AppendLine("\t\t\t}");
			source.AppendLine("\t\t}");
			source.AppendLine("\t}");
			source.AppendLine("}");
		}

		// EscapeStringLiteral bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static string EscapeStringLiteral(string value)
		{
			return value.Replace("\\", "\\\\").Replace("\"", "\\\"");
		}

		// ScriptTypeModel is the managed-facing contract for this subsystem; keep it predictable for scripts.
		private sealed class ScriptTypeModel
		{
			public INamedTypeSymbol Symbol { get; set; } = null!;
			public string FullTypeName { get; set; } = string.Empty;
			public string QualifiedTypeName { get; set; } = string.Empty;
			public string NamespaceName { get; set; } = string.Empty;
			public string TypeName { get; set; } = string.Empty;
			public string TypeModifiers { get; set; } = "internal";
			public bool IsPartial { get; set; }
			public bool IsAttachable { get; set; }
			public ulong ScriptId { get; set; }
			public Location? Location { get; set; }
			public List<ExportFieldModel> OwnFields { get; } = new();
			public ScriptTypeModel? HelperBaseModel { get; set; }
			public ScriptTypeModel? AccessorProviderModel { get; set; }
		}

		// ExportFieldModel is the managed-facing contract for this subsystem; keep it predictable for scripts.
		private sealed class ExportFieldModel
		{
			public string FieldName { get; set; } = string.Empty;
			public string TypeName { get; set; } = string.Empty;
			public string ValueKindExpression { get; set; } = string.Empty;
			public int ValueSize { get; set; }
			public ulong NameHash { get; set; }
		}
	}
}
