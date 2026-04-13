using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace TerraEngine
{
	internal enum ScriptType : int
	{
		Bool = 0,
		Int = 1,
		Float = 2,
		EntityID = 3,
		Vector2 = 4,
		Vector3 = 5,
		Vector4 = 6,
		Quaternion = 7
	}

	// Payload types mirror the JSON contracts shared between the native engine,
	// the managed bridge, and the live inspector UI.
	internal sealed class ScriptFieldValuePayload
	{
		public string FieldName { get; set; } = string.Empty;
		public string FieldPath { get; set; } = string.Empty;
		public string DisplayName { get; set; } = string.Empty;
		public string ValueKind { get; set; } = string.Empty;
		public JsonElement Value { get; set; }
		public string Compatibility { get; set; } = "Current";
		public string SourceTypeFingerprint { get; set; } = string.Empty;
		public string PersistenceState { get; set; } = "Current";
		public string DiagnosticMessage { get; set; } = string.Empty;
	}

	internal sealed class ScriptBindingPayload
	{
		public string BindingId { get; set; } = string.Empty;
		public string ClassName { get; set; } = string.Empty;
		public ulong ScriptId { get; set; }
		public List<ScriptFieldValuePayload> SerializedValues { get; set; } = new();
	}

	internal sealed class LiveFieldSnapshotPayload
	{
		public string FieldPath { get; set; } = string.Empty;
		public object? Value { get; set; }
		public string Source { get; set; } = "Authored";
		public string RefreshState { get; set; } = "Clean";
		public bool DiffersFromAuthored { get; set; }
		public long LastObservedVersion { get; set; }
	}

	internal sealed class LiveBindingSnapshotPayload
	{
		public string BindingId { get; set; } = string.Empty;
		public string FullTypeName { get; set; } = string.Empty;
		public string Status { get; set; } = "Attached";
		public bool RuntimeAvailable { get; set; }
		public int RuntimeOverrideCount { get; set; }
		public List<LiveFieldSnapshotPayload> Fields { get; set; } = new();
		public List<ScriptFieldDiagnosticPayload> Diagnostics { get; set; } = new();
	}

	internal sealed class BridgeWriteResultPayload
	{
		public bool Success { get; set; }
		public string BindingId { get; set; } = string.Empty;
		public string FieldPath { get; set; } = string.Empty;
		public string Message { get; set; } = string.Empty;
		public object? Value { get; set; }
		public long SnapshotVersion { get; set; }
	}

	internal sealed class MigrationFieldValuePayload
	{
		public string FieldName { get; set; } = string.Empty;
		public string FieldPath { get; set; } = string.Empty;
		public string DisplayName { get; set; } = string.Empty;
		public string ValueKind { get; set; } = string.Empty;
		public object? Value { get; set; }
		public string Compatibility { get; set; } = "Current";
		public string SourceTypeFingerprint { get; set; } = string.Empty;
		public string PersistenceState { get; set; } = "Current";
		public string DiagnosticMessage { get; set; } = string.Empty;
	}

	internal sealed class MigrationResultPayload
	{
		public List<MigrationFieldValuePayload> Fields { get; set; } = new();
		public List<string> PreservedFieldPaths { get; set; } = new();
		public List<string> ConvertedFieldPaths { get; set; } = new();
		public List<string> BrokenFieldPaths { get; set; } = new();
		public List<string> HiddenStoredFieldPaths { get; set; } = new();
		public List<ScriptFieldDiagnosticPayload> Diagnostics { get; set; } = new();
	}

	public static class InternalCalls
	{
		private sealed class NativeUtf8ReturnBuffer
		{
			private readonly object _syncRoot = new();
			private byte[] _buffer = new byte[] { 0 };
			private GCHandle _pinnedBuffer;

			public IntPtr WriteJson<T>(T payload)
			{
				return WriteText(JsonSerializer.Serialize(payload, s_jsonOptions));
			}

			public IntPtr WriteText(string text)
			{
				lock (_syncRoot)
				{
					if (_pinnedBuffer.IsAllocated)
					{
						_pinnedBuffer.Free();
					}

					// Native code expects a stable null-terminated UTF-8 pointer until
					// the next call overwrites this return buffer.
					_buffer = Encoding.UTF8.GetBytes((text ?? string.Empty) + '\0');
					_pinnedBuffer = GCHandle.Alloc(_buffer, GCHandleType.Pinned);
					return _pinnedBuffer.AddrOfPinnedObject();
				}
			}
		}

		private sealed class ScriptInstanceState
		{
			// bindingId, not objectId, is the stable key because one object may
			// host multiple bindings of the same script type at once.
			public Script Instance { get; init; } = null!;
			public Type ScriptType { get; init; } = null!;
			public string ClassName { get; set; } = string.Empty;
			public uint ObjectId { get; set; }
			public string LastHydratedSignature { get; set; } = string.Empty;
			public long ObservedVersion { get; set; }
			public object SyncRoot { get; } = new();
		}

		private static readonly object s_bridgeSync = new();
		private static readonly NativeUtf8ReturnBuffer s_descriptorReturnBuffer = new();
		private static readonly NativeUtf8ReturnBuffer s_snapshotReturnBuffer = new();
		private static readonly NativeUtf8ReturnBuffer s_writeReturnBuffer = new();
		private static readonly NativeUtf8ReturnBuffer s_migrationReturnBuffer = new();
		private static readonly JsonSerializerOptions s_jsonOptions = new()
		{
			PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
			DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
			WriteIndented = false
		};

		internal static string ReadNativeUtf8(IntPtr nativeText)
		{
			// Script glue returns pointers into static or thread-local UTF-8
			// buffers. Marshal them explicitly so .NET copies the text without
			// attempting to free engine-owned memory after the call returns.
			return nativeText == IntPtr.Zero
				? string.Empty
				: Marshal.PtrToStringUTF8(nativeText) ?? string.Empty;
		}

		// These caches all belong to the currently loaded gameplay assembly.
		// Loading a new Assembly-CSharp replaces every one of them atomically.
		private static GameplayScriptLoadContext? _gameplayLoadContext;
		private static Assembly? _gameplayAssembly;
		private static Dictionary<string, Type> _attachableTypes = new(StringComparer.Ordinal);
		private static Dictionary<string, ScriptTypeDescriptorPayload> _descriptorCache = new(StringComparer.Ordinal);
		private static Dictionary<string, ScriptInstanceState> _scriptInstances = new(StringComparer.Ordinal);

		private static List<ScriptInstanceState> DrainScriptInstances()
		{
			lock (s_bridgeSync)
			{
				var drainedInstances = new List<ScriptInstanceState>(_scriptInstances.Values);
				_scriptInstances.Clear();
				return drainedInstances;
			}
		}

		private static void DisposeScriptInstances()
		{
			foreach (ScriptInstanceState scriptState in DrainScriptInstances())
			{
				lock (scriptState.SyncRoot)
				{
					scriptState.Instance.InvokeDestroyed();
				}
			}
		}

		[UnmanagedCallersOnly]
		public static void Initialize()
		{
			try
			{
				Log.Info("TerraEngine Managed Initialized!");
			}
			catch (Exception ex)
			{
				Console.WriteLine($"[C#] Critical failure in Initialize:\n{ex}");
			}
		}

		[UnmanagedCallersOnly]
		public static int LoadGameplayAssembly(IntPtr assemblyPathPtr)
		{
			try
			{
				string assemblyPath = Marshal.PtrToStringAnsi(assemblyPathPtr) ?? string.Empty;
				if (string.IsNullOrWhiteSpace(assemblyPath))
				{
					Log.Error("LoadGameplayAssembly was called with an empty Assembly-CSharp path.");
					return 0;
				}

				// Load into a collectible context so the previous Assembly-CSharp can
				// be released after hot reload without restarting the editor.
				var nextLoadContext = new GameplayScriptLoadContext(assemblyPath);
				var nextAssembly = nextLoadContext.LoadFromAssemblyPath(assemblyPath);
				var nextCatalog = ScriptReflection.BuildAttachableTypeCatalog(nextAssembly);
				DisposeScriptInstances();

				lock (s_bridgeSync)
				{
					GameplayScriptLoadContext? previousContext = _gameplayLoadContext;
					_gameplayLoadContext = nextLoadContext;
					_gameplayAssembly = nextAssembly;
					_attachableTypes = nextCatalog;
					_descriptorCache = new Dictionary<string, ScriptTypeDescriptorPayload>(StringComparer.Ordinal);
					_scriptInstances = new Dictionary<string, ScriptInstanceState>(StringComparer.Ordinal);

					if (previousContext != null)
					{
						previousContext.Unload();
					}
				}

				GC.Collect();
				GC.WaitForPendingFinalizers();
				GC.Collect();

				Log.Info($"Loaded Assembly-CSharp from {assemblyPath} with {_attachableTypes.Count} attachable script types.");
				return 1;
			}
			catch (Exception ex)
			{
				Log.Error($"Exception while loading Assembly-CSharp:\n{ex}");
				return 0;
			}
		}

		[UnmanagedCallersOnly]
		public static void UnloadGameplayAssembly()
		{
			DisposeScriptInstances();

			lock (s_bridgeSync)
			{
				_descriptorCache.Clear();
				_attachableTypes.Clear();
			}

			if (_gameplayLoadContext != null)
			{
				_gameplayLoadContext.Unload();
				_gameplayLoadContext = null;
			}

			_gameplayAssembly = null;

			GC.Collect();
			GC.WaitForPendingFinalizers();
			GC.Collect();
		}

		[UnmanagedCallersOnly]
		public static void ResetGameplaySession()
		{
			// Stop should discard every managed runtime instance while keeping the
			// reflected type catalog warm for the next Play press.
			DisposeScriptInstances();
		}

		[UnmanagedCallersOnly]
		public static int IsScriptTypeAttachable(IntPtr classNamePtr)
		{
			string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
			lock (s_bridgeSync)
			{
				return !string.IsNullOrWhiteSpace(className) && _attachableTypes.ContainsKey(className) ? 1 : 0;
			}
		}

		[UnmanagedCallersOnly]
		public static IntPtr DescribeBindingType(IntPtr classNamePtr)
		{
			try
			{
				string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
				if (!TryResolveScriptType(className, out Type? scriptType))
				{
					return s_descriptorReturnBuffer.WriteJson(new ScriptTypeDescriptorPayload
					{
						FullTypeName = className,
						Diagnostics = new List<ScriptFieldDiagnosticPayload>
						{
							new()
							{
								DiagnosticId = "binding.missingType",
								Scope = "Binding",
								Severity = "Error",
								Code = "InvalidType",
								Message = $"The active gameplay assembly does not expose {className}."
							}
						}
					});
				}

				return s_descriptorReturnBuffer.WriteJson(GetOrCreateDescriptor(className, scriptType!));
			}
			catch (Exception ex)
			{
				Log.Error($"Exception while describing a script binding type:\n{ex}");
				return s_descriptorReturnBuffer.WriteJson(new ScriptTypeDescriptorPayload
				{
					Diagnostics = new List<ScriptFieldDiagnosticPayload>
					{
						new()
						{
							DiagnosticId = "binding.describeFailed",
							Scope = "Binding",
							Severity = "Error",
							Code = "BridgeFailure",
							Message = ex.Message
						}
					}
				});
			}
		}

		[UnmanagedCallersOnly]
		public static IntPtr ReadLiveBindingValues(uint objectId, IntPtr bindingIdPtr, IntPtr classNamePtr)
		{
			try
			{
				string bindingId = Marshal.PtrToStringAnsi(bindingIdPtr) ?? string.Empty;
				string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
				if (string.IsNullOrWhiteSpace(bindingId) || string.IsNullOrWhiteSpace(className))
				{
					return s_snapshotReturnBuffer.WriteJson(new LiveBindingSnapshotPayload
					{
						BindingId = bindingId,
						FullTypeName = className,
						Status = "InstanceUnavailable",
						RuntimeAvailable = false
					});
				}

				if (!TryResolveScriptType(className, out Type? scriptType))
				{
					return s_snapshotReturnBuffer.WriteJson(new LiveBindingSnapshotPayload
					{
						BindingId = bindingId,
						FullTypeName = className,
						Status = "BrokenType",
						RuntimeAvailable = false,
						Diagnostics = new List<ScriptFieldDiagnosticPayload>
						{
							new()
							{
								DiagnosticId = "binding.missingType",
								Scope = "Binding",
								Severity = "Error",
								Code = "InvalidType",
								Message = $"The active gameplay assembly does not expose {className}."
							}
						}
					});
				}

				ScriptInstanceState? scriptState = GetInstanceState(bindingId);
				if (scriptState == null || scriptState.ObjectId != objectId)
				{
					return s_snapshotReturnBuffer.WriteJson(new LiveBindingSnapshotPayload
					{
						BindingId = bindingId,
						FullTypeName = className,
						Status = "InstanceUnavailable",
						RuntimeAvailable = false,
						Diagnostics = new List<ScriptFieldDiagnosticPayload>
						{
							new()
							{
								DiagnosticId = "binding.instanceUnavailable",
								Scope = "Binding",
								Severity = "Warning",
								Code = "InstanceUnavailable",
								Message = "The selected object does not currently have a live managed script instance for this binding."
							}
						}
					});
				}

				ScriptTypeDescriptorPayload descriptor = GetOrCreateDescriptor(className, scriptType!);
				ScriptBindingPayload bindingPayload = GetBindingPayload(objectId, bindingId);
				var authoredLookup = BuildAuthoredValueLookup(bindingPayload);
				var snapshot = new LiveBindingSnapshotPayload
				{
					BindingId = bindingId,
					FullTypeName = descriptor.FullTypeName,
					Status = "LiveReady",
					RuntimeAvailable = true
				};
				snapshot.Diagnostics.AddRange(descriptor.Diagnostics);

				lock (scriptState.SyncRoot)
				{
					foreach (ScriptFieldDescriptorPayload rootDescriptor in descriptor.Fields)
					{
						FieldInfo? field = FindSerializableField(scriptState.ScriptType, rootDescriptor.SerializedName);
						if (field == null)
						{
							continue;
						}

						object? liveValue = field.GetValue(scriptState.Instance);
						object? payload = ConvertRuntimeValueToPayload(liveValue, field.FieldType);
						authoredLookup.TryGetValue(rootDescriptor.FieldPath, out ScriptFieldValuePayload? authoredValue);
						string normalizedRuntime = JsonSerializer.Serialize(payload, s_jsonOptions);
						string normalizedAuthored = authoredValue == null
							? string.Empty
							: authoredValue.Value.ValueKind == JsonValueKind.Undefined
								? string.Empty
								: authoredValue.Value.GetRawText();
						bool differs = !string.Equals(normalizedRuntime, normalizedAuthored, StringComparison.Ordinal);
						snapshot.Fields.Add(new LiveFieldSnapshotPayload
						{
							FieldPath = rootDescriptor.FieldPath,
							Value = payload,
							Source = differs ? "RuntimeOverride" : "Runtime",
							RefreshState = "Clean",
							DiffersFromAuthored = differs,
							LastObservedVersion = scriptState.ObservedVersion
						});
						if (differs)
						{
							snapshot.RuntimeOverrideCount++;
						}
					}
				}

				return s_snapshotReturnBuffer.WriteJson(snapshot);
			}
			catch (Exception ex)
			{
				Log.Error($"Exception while reading live binding values:\n{ex}");
				return s_snapshotReturnBuffer.WriteJson(new LiveBindingSnapshotPayload
				{
					Status = "InstanceUnavailable",
					RuntimeAvailable = false,
					Diagnostics = new List<ScriptFieldDiagnosticPayload>
					{
						new()
						{
							DiagnosticId = "binding.readFailed",
							Scope = "Binding",
							Severity = "Error",
							Code = "BridgeFailure",
							Message = ex.Message
						}
					}
				});
			}
		}

		[UnmanagedCallersOnly]
		public static IntPtr WriteLiveBindingValue(uint objectId, IntPtr bindingIdPtr, IntPtr fieldPathPtr, IntPtr valueJsonPtr)
		{
			try
			{
				string bindingId = Marshal.PtrToStringAnsi(bindingIdPtr) ?? string.Empty;
				string fieldPath = Marshal.PtrToStringAnsi(fieldPathPtr) ?? string.Empty;
				string valueJson = Marshal.PtrToStringAnsi(valueJsonPtr) ?? string.Empty;
				if (string.IsNullOrWhiteSpace(bindingId) || string.IsNullOrWhiteSpace(fieldPath))
				{
					return s_writeReturnBuffer.WriteJson(new BridgeWriteResultPayload
					{
						Success = false,
						BindingId = bindingId,
						FieldPath = fieldPath,
						Message = "The live write request did not include a valid binding ID and field path."
					});
				}

				ScriptInstanceState? scriptState = GetInstanceState(bindingId);
				if (scriptState == null || scriptState.ObjectId != objectId)
				{
					return s_writeReturnBuffer.WriteJson(new BridgeWriteResultPayload
					{
						Success = false,
						BindingId = bindingId,
						FieldPath = fieldPath,
						Message = "The live managed instance is not currently available for this binding."
					});
				}

				if (!TryResolveScriptType(scriptState.ClassName, out Type? scriptType))
				{
					return s_writeReturnBuffer.WriteJson(new BridgeWriteResultPayload
					{
						Success = false,
						BindingId = bindingId,
						FieldPath = fieldPath,
						Message = $"The active gameplay assembly no longer exposes {scriptState.ClassName}."
					});
				}

				FieldInfo? field = FindSerializableField(scriptType!, fieldPath);
				if (field == null)
				{
					return s_writeReturnBuffer.WriteJson(new BridgeWriteResultPayload
					{
						Success = false,
						BindingId = bindingId,
						FieldPath = fieldPath,
						Message = $"The live inspector could not resolve the writable field {fieldPath}."
					});
				}

				using JsonDocument valueDocument = string.IsNullOrWhiteSpace(valueJson)
					? JsonDocument.Parse("null")
					: JsonDocument.Parse(valueJson);
				if (!TryConvertJsonToType(field.FieldType, valueDocument.RootElement, out object? convertedValue, out string errorMessage))
				{
					return s_writeReturnBuffer.WriteJson(new BridgeWriteResultPayload
					{
						Success = false,
						BindingId = bindingId,
						FieldPath = fieldPath,
						Message = errorMessage
					});
				}

				object? readBackValue;
				lock (scriptState.SyncRoot)
				{
					field.SetValue(scriptState.Instance, convertedValue);
					readBackValue = field.GetValue(scriptState.Instance);
					scriptState.ObservedVersion++;
				}

				return s_writeReturnBuffer.WriteJson(new BridgeWriteResultPayload
				{
					Success = true,
					BindingId = bindingId,
					FieldPath = fieldPath,
					Message = "Applied the live inspector value to the active managed instance.",
					Value = ConvertRuntimeValueToPayload(readBackValue, field.FieldType),
					SnapshotVersion = scriptState.ObservedVersion
				});
			}
			catch (Exception ex)
			{
				Log.Error($"Exception while writing a live binding value:\n{ex}");
				return s_writeReturnBuffer.WriteJson(new BridgeWriteResultPayload
				{
					Success = false,
					Message = ex.Message
				});
			}
		}

		[UnmanagedCallersOnly]
		public static IntPtr MigrateBindingAuthoredState(IntPtr classNamePtr, IntPtr serializedValuesJsonPtr)
		{
			try
			{
				string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
				string serializedValuesJson = Marshal.PtrToStringAnsi(serializedValuesJsonPtr) ?? "[]";
				List<ScriptFieldValuePayload> storedValues = DeserializeFieldValues(serializedValuesJson);
				if (!TryResolveScriptType(className, out Type? scriptType))
				{
					return s_migrationReturnBuffer.WriteJson(BuildBrokenMigrationResult(className, storedValues, "The active gameplay assembly no longer exposes this script type."));
				}

				ScriptTypeDescriptorPayload descriptor = GetOrCreateDescriptor(className, scriptType!);
				var descriptorLookup = new Dictionary<string, ScriptFieldDescriptorPayload>(StringComparer.Ordinal);
				foreach (ScriptFieldDescriptorPayload rootDescriptor in descriptor.Fields)
				{
					descriptorLookup[rootDescriptor.FieldPath] = rootDescriptor;
				}

				var result = new MigrationResultPayload();
				foreach (ScriptFieldValuePayload storedValue in storedValues)
				{
					string stablePath = GetStableFieldPath(storedValue);
					if (!descriptorLookup.TryGetValue(stablePath, out ScriptFieldDescriptorPayload? rootDescriptor))
					{
						result.Fields.Add(new MigrationFieldValuePayload
						{
							FieldName = stablePath,
							FieldPath = stablePath,
							DisplayName = string.IsNullOrWhiteSpace(storedValue.DisplayName) ? ScriptReflection.NicifyIdentifier(stablePath) : storedValue.DisplayName,
							ValueKind = storedValue.ValueKind,
							Value = ConvertJsonElementToPlainObject(storedValue.Value),
							Compatibility = "Broken",
							PersistenceState = "Broken",
							SourceTypeFingerprint = storedValue.SourceTypeFingerprint,
							DiagnosticMessage = $"The field {stablePath} no longer exists on {className}."
						});
						result.BrokenFieldPaths.Add(stablePath);
						result.Diagnostics.Add(new ScriptFieldDiagnosticPayload
						{
							DiagnosticId = stablePath + ".missingAfterReload",
							Scope = "Field",
							FieldPath = stablePath,
							Severity = "Warning",
							Code = "IncompatibleReload",
							Message = $"The field {stablePath} no longer exists on {className}.",
							RepairActionIds = new List<string> { "clear:" + stablePath, "remove:" + stablePath }
						});
						continue;
					}

					FieldInfo? field = FindSerializableField(scriptType!, rootDescriptor.SerializedName);
					if (field == null)
					{
						continue;
					}

					if (!TryConvertJsonToType(field.FieldType, storedValue.Value, out object? convertedValue, out string errorMessage))
					{
						result.Fields.Add(new MigrationFieldValuePayload
						{
							FieldName = rootDescriptor.FieldPath,
							FieldPath = rootDescriptor.FieldPath,
							DisplayName = rootDescriptor.DisplayName,
							ValueKind = rootDescriptor.ValueKind,
							Value = ConvertJsonElementToPlainObject(storedValue.Value),
							Compatibility = "Broken",
							PersistenceState = "Broken",
							SourceTypeFingerprint = storedValue.SourceTypeFingerprint,
							DiagnosticMessage = errorMessage
						});
						result.BrokenFieldPaths.Add(rootDescriptor.FieldPath);
						result.Diagnostics.Add(new ScriptFieldDiagnosticPayload
						{
							DiagnosticId = rootDescriptor.FieldPath + ".incompatibleReload",
							Scope = "Field",
							FieldPath = rootDescriptor.FieldPath,
							Severity = "Warning",
							Code = "IncompatibleReload",
							Message = errorMessage,
							RepairActionIds = new List<string> { "clear:" + rootDescriptor.FieldPath, "remove:" + rootDescriptor.FieldPath }
						});
						continue;
					}

					string compatibility = string.Equals(storedValue.SourceTypeFingerprint, rootDescriptor.TypeFingerprint, StringComparison.Ordinal)
						? "Current"
						: "Migrated";
					result.Fields.Add(new MigrationFieldValuePayload
					{
						FieldName = rootDescriptor.FieldPath,
						FieldPath = rootDescriptor.FieldPath,
						DisplayName = rootDescriptor.DisplayName,
						ValueKind = rootDescriptor.ValueKind,
						Value = ConvertRuntimeValueToPayload(convertedValue, field.FieldType),
						Compatibility = compatibility,
						PersistenceState = compatibility,
						SourceTypeFingerprint = rootDescriptor.TypeFingerprint
					});

					if (compatibility == "Migrated")
					{
						result.ConvertedFieldPaths.Add(rootDescriptor.FieldPath);
					}
					else
					{
						result.PreservedFieldPaths.Add(rootDescriptor.FieldPath);
					}

					if (rootDescriptor.HiddenInInspector)
					{
						result.HiddenStoredFieldPaths.Add(rootDescriptor.FieldPath);
					}
				}

				return s_migrationReturnBuffer.WriteJson(result);
			}
			catch (Exception ex)
			{
				Log.Error($"Exception while migrating binding authored state:\n{ex}");
				return s_migrationReturnBuffer.WriteJson(new MigrationResultPayload
				{
					Diagnostics = new List<ScriptFieldDiagnosticPayload>
					{
						new()
						{
							DiagnosticId = "binding.migrationFailed",
							Scope = "Binding",
							Severity = "Error",
							Code = "BridgeFailure",
							Message = ex.Message
						}
					}
				});
			}
		}

		[UnmanagedCallersOnly]
		public static void UpdateScript(uint objectId, IntPtr bindingIdPtr, IntPtr classNamePtr, float deltaTime)
		{
			try
			{
				if (_gameplayAssembly == null)
				{
					return;
				}

				string bindingId = Marshal.PtrToStringAnsi(bindingIdPtr) ?? string.Empty;
				string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
				if (string.IsNullOrWhiteSpace(bindingId) || string.IsNullOrWhiteSpace(className))
				{
					return;
				}

				if (!TryResolveScriptType(className, out Type? scriptType))
				{
					Log.Error($"Could not find compiled gameplay script class {className} in the active Assembly-CSharp catalog.");
					return;
				}

				ScriptInstanceState scriptState;
				bool createdState = false;
				lock (s_bridgeSync)
				{
					if (!_scriptInstances.TryGetValue(bindingId, out scriptState!))
					{
						if (Activator.CreateInstance(scriptType!) is not Script script)
						{
							Log.Error($"Class {className} does not inherit from TerraEngine.Script.");
							return;
						}

						script.BindEngineInstance(objectId, bindingId);
						scriptState = new ScriptInstanceState
						{
							Instance = script,
							ScriptType = scriptType!,
							ClassName = className,
							ObjectId = objectId
						};
						_scriptInstances[bindingId] = scriptState;
						createdState = true;
					}
				}

				ScriptBindingPayload bindingPayload = GetBindingPayload(objectId, bindingId);
				string authoredSignature = JsonSerializer.Serialize(bindingPayload.SerializedValues, s_jsonOptions);

				lock (scriptState.SyncRoot)
				{
					scriptState.ObjectId = objectId;
					scriptState.ClassName = className;
					scriptState.Instance.BindEngineInstance(objectId, bindingId);

					if (createdState || !string.Equals(scriptState.LastHydratedSignature, authoredSignature, StringComparison.Ordinal))
					{
						HydrateScriptInstance(scriptState.Instance, scriptState.ScriptType, bindingPayload);
						scriptState.LastHydratedSignature = authoredSignature;
					}

					if (createdState)
					{
						scriptState.Instance.InvokeCreated();
					}

					scriptState.Instance.DispatchFrame(deltaTime);
					scriptState.ObservedVersion++;
				}
			}
			catch (Exception ex)
			{
				Log.Error($"Exception in UpdateScript for object {objectId}:\n{ex}");
			}
		}

		private static bool TryResolveScriptType(string className, out Type? scriptType)
		{
			lock (s_bridgeSync)
			{
				return _attachableTypes.TryGetValue(className, out scriptType);
			}
		}

		private static ScriptTypeDescriptorPayload GetOrCreateDescriptor(string className, Type scriptType)
		{
			lock (s_bridgeSync)
			{
				if (_descriptorCache.TryGetValue(className, out ScriptTypeDescriptorPayload? descriptor))
				{
					return descriptor;
				}

				descriptor = ScriptReflection.DescribeScriptType(scriptType);
				PopulateDescriptorDefaultValues(scriptType, descriptor);
				_descriptorCache[className] = descriptor;
				return descriptor;
			}
		}

		private static void PopulateDescriptorDefaultValues(Type scriptType, ScriptTypeDescriptorPayload descriptor)
		{
			// Create one detached sample instance so the inspector can reflect the
			// field initializers that gameplay code authored on the C# class.
			object? scriptSample = TryCreateDescriptorSample(scriptType);
			foreach (ScriptFieldDescriptorPayload rootDescriptor in descriptor.Fields)
			{
				FieldInfo? field = FindSerializableField(scriptType, rootDescriptor.SerializedName);
				if (field == null)
				{
					continue;
				}

				object? rootValue = scriptSample != null ? field.GetValue(scriptSample) : null;
				PopulateDescriptorDefaultValues(rootDescriptor, field.FieldType, rootValue);
			}
		}

		private static void PopulateDescriptorDefaultValues(ScriptFieldDescriptorPayload descriptor, Type valueType, object? value)
		{
			descriptor.DefaultValue = ConvertRuntimeValueToPayload(value, valueType);

			if (descriptor.ContainerKind == "NestedObject")
			{
				// Nested children read from the sampled object when present. When a
				// parent default is null, fall back to a detached sample so structs
				// and serializable classes can still seed newly materialized data.
				object? nestedSample = value ?? TryCreateDescriptorSample(valueType);
				foreach (ScriptFieldDescriptorPayload childDescriptor in descriptor.Children)
				{
					FieldInfo? childField = FindSerializableField(valueType, childDescriptor.SerializedName);
					if (childField == null)
					{
						continue;
					}

					object? childValue = nestedSample != null ? childField.GetValue(nestedSample) : null;
					PopulateDescriptorDefaultValues(childDescriptor, childField.FieldType, childValue);
				}

				return;
			}

			if (descriptor.ContainerKind == "Collection"
				&& descriptor.Children.Count > 0
				&& ScriptReflection.TryGetCollectionElementType(valueType, out Type? elementType, out _)
				&& elementType != null)
			{
				// Collection descriptors store one element template, so sample the
				// element type independently to seed the "+" button in inspector UI.
				object? elementSample = TryCreateDescriptorSample(elementType);
				PopulateDescriptorDefaultValues(descriptor.Children[0], elementType, elementSample);
			}
		}

		private static object? TryCreateDescriptorSample(Type valueType)
		{
			try
			{
				if (valueType == typeof(string))
				{
					return string.Empty;
				}

				if (valueType.IsArray)
				{
					Type? elementType = valueType.GetElementType();
					return elementType != null ? Array.CreateInstance(elementType, 0) : null;
				}

				return Activator.CreateInstance(valueType);
			}
			catch
			{
				return null;
			}
		}

		private static ScriptInstanceState? GetInstanceState(string bindingId)
		{
			lock (s_bridgeSync)
			{
				_scriptInstances.TryGetValue(bindingId, out ScriptInstanceState? state);
				return state;
			}
		}

		private static ScriptBindingPayload GetBindingPayload(uint objectId, string bindingId)
		{
			// The native ScriptComponent owns the authored JSON. Managed code pulls
			// a fresh copy whenever it needs to hydrate, snapshot, or migrate data.
			IntPtr payloadPtr = Native_ScriptComponent_GetBindingPayload(objectId, bindingId);
			string payloadJson = Marshal.PtrToStringAnsi(payloadPtr) ?? string.Empty;
			if (string.IsNullOrWhiteSpace(payloadJson))
			{
				return new ScriptBindingPayload
				{
					BindingId = bindingId
				};
			}

			return JsonSerializer.Deserialize<ScriptBindingPayload>(payloadJson, s_jsonOptions) ?? new ScriptBindingPayload
			{
				BindingId = bindingId
			};
		}

		private static Dictionary<string, ScriptFieldValuePayload> BuildAuthoredValueLookup(ScriptBindingPayload bindingPayload)
		{
			var lookup = new Dictionary<string, ScriptFieldValuePayload>(StringComparer.Ordinal);
			foreach (ScriptFieldValuePayload value in bindingPayload.SerializedValues)
			{
				string stablePath = GetStableFieldPath(value);
				if (!lookup.ContainsKey(stablePath))
				{
					lookup[stablePath] = value;
				}
			}

			return lookup;
		}

		private static void HydrateScriptInstance(Script instance, Type scriptType, ScriptBindingPayload bindingPayload)
		{
			// Reapply authored values after reloads or inspector edits so the live
			// managed instance always starts from the scene-backed binding payload.
			var authoredLookup = BuildAuthoredValueLookup(bindingPayload);
			ScriptTypeDescriptorPayload descriptor = GetOrCreateDescriptor(bindingPayload.ClassName, scriptType);
			foreach (ScriptFieldDescriptorPayload rootDescriptor in descriptor.Fields)
			{
				if (!authoredLookup.TryGetValue(rootDescriptor.FieldPath, out ScriptFieldValuePayload? authoredValue))
				{
					continue;
				}

				if (string.Equals(authoredValue.Compatibility, "Broken", StringComparison.OrdinalIgnoreCase))
				{
					continue;
				}

				FieldInfo? field = FindSerializableField(scriptType, rootDescriptor.SerializedName);
				if (field == null)
				{
					continue;
				}

				if (!TryConvertJsonToType(field.FieldType, authoredValue.Value, out object? convertedValue, out _))
				{
					continue;
				}

				field.SetValue(instance, convertedValue);
			}
		}

		private static FieldInfo? FindSerializableField(Type scriptType, string fieldPath)
		{
			string rootFieldName = ExtractRootFieldPath(fieldPath);
			foreach (FieldInfo field in ScriptReflection.GetEligibleFields(scriptType))
			{
				if (string.Equals(field.Name, rootFieldName, StringComparison.Ordinal))
				{
					return field;
				}
			}

			return null;
		}

		private static string ExtractRootFieldPath(string fieldPath)
		{
			if (string.IsNullOrWhiteSpace(fieldPath))
			{
				return string.Empty;
			}

			int dotIndex = fieldPath.IndexOf('.');
			int bracketIndex = fieldPath.IndexOf('[');
			int cutIndex = dotIndex < 0 ? bracketIndex : bracketIndex < 0 ? dotIndex : Math.Min(dotIndex, bracketIndex);
			return cutIndex < 0 ? fieldPath : fieldPath[..cutIndex];
		}

		private static string GetStableFieldPath(ScriptFieldValuePayload payload)
		{
			return string.IsNullOrWhiteSpace(payload.FieldPath) ? payload.FieldName : payload.FieldPath;
		}

		private static List<ScriptFieldValuePayload> DeserializeFieldValues(string serializedValuesJson)
		{
			if (string.IsNullOrWhiteSpace(serializedValuesJson))
			{
				return new List<ScriptFieldValuePayload>();
			}

			return JsonSerializer.Deserialize<List<ScriptFieldValuePayload>>(serializedValuesJson, s_jsonOptions) ?? new List<ScriptFieldValuePayload>();
		}

		private static MigrationResultPayload BuildBrokenMigrationResult(string className, IEnumerable<ScriptFieldValuePayload> storedValues, string reason)
		{
			var result = new MigrationResultPayload();
			foreach (ScriptFieldValuePayload value in storedValues)
			{
				string stablePath = GetStableFieldPath(value);
				result.Fields.Add(new MigrationFieldValuePayload
				{
					FieldName = stablePath,
					FieldPath = stablePath,
					DisplayName = string.IsNullOrWhiteSpace(value.DisplayName) ? ScriptReflection.NicifyIdentifier(stablePath) : value.DisplayName,
					ValueKind = value.ValueKind,
					Value = ConvertJsonElementToPlainObject(value.Value),
					Compatibility = "Broken",
					PersistenceState = "Broken",
					SourceTypeFingerprint = value.SourceTypeFingerprint,
					DiagnosticMessage = reason
				});
				result.BrokenFieldPaths.Add(stablePath);
			}

			result.Diagnostics.Add(new ScriptFieldDiagnosticPayload
			{
				DiagnosticId = "binding.missingType",
				Scope = "Binding",
				Severity = "Error",
				Code = "InvalidType",
				Message = $"{className}: {reason}",
				RepairActionIds = new List<string> { "removeBinding" }
			});
			return result;
		}

		private static object? ConvertRuntimeValueToPayload(object? value, Type valueType)
		{
			if (value == null)
			{
				return null;
			}

			if (valueType == typeof(bool) || valueType == typeof(int) || valueType == typeof(float) || valueType == typeof(string) || valueType == typeof(uint))
			{
				return value;
			}

			if (valueType.IsEnum)
			{
				string? enumName = Enum.GetName(valueType, value);
				return enumName is not null ? enumName : Convert.ToInt32(value);
			}

			if (valueType == typeof(Vector2))
			{
				Vector2 vector = (Vector2)value;
				return new[] { vector.X, vector.Y };
			}

			if (valueType == typeof(Vector3))
			{
				Vector3 vector = (Vector3)value;
				return new[] { vector.X, vector.Y, vector.Z };
			}

			if (valueType == typeof(Vector4))
			{
				Vector4 vector = (Vector4)value;
				return new[] { vector.X, vector.Y, vector.Z, vector.W };
			}

			if (valueType == typeof(Quaternion))
			{
				Quaternion rotation = (Quaternion)value;
				return new[] { rotation.X, rotation.Y, rotation.Z, rotation.W };
			}

			if (valueType == typeof(Object))
			{
				return ((Object)value).ID;
			}

			if (ScriptReflection.TryGetCollectionElementType(valueType, out Type? elementType, out _))
			{
				var list = new List<object?>();
				foreach (object? item in (IEnumerable)value)
				{
					list.Add(ConvertRuntimeValueToPayload(item, elementType!));
				}
				return list;
			}

			if (ScriptReflection.IsNestedSerializableType(valueType))
			{
				var objectPayload = new Dictionary<string, object?>(StringComparer.Ordinal);
				foreach (FieldInfo field in ScriptReflection.GetEligibleFields(valueType))
				{
					objectPayload[field.Name] = ConvertRuntimeValueToPayload(field.GetValue(value), field.FieldType);
				}
				return objectPayload;
			}

			return value.ToString();
		}

		private static bool TryConvertJsonToType(Type targetType, JsonElement jsonValue, out object? convertedValue, out string errorMessage)
		{
			errorMessage = string.Empty;
			convertedValue = null;

			// This conversion path is shared by live inspector writes and reload
			// migration, so it intentionally accepts the normalized JSON payloads
			// emitted by ConvertRuntimeValueToPayload instead of raw source syntax.
			if (targetType == typeof(bool))
			{
				if (jsonValue.ValueKind == JsonValueKind.True || jsonValue.ValueKind == JsonValueKind.False)
				{
					convertedValue = jsonValue.GetBoolean();
					return true;
				}
			}
			else if (targetType == typeof(int))
			{
				if (jsonValue.ValueKind == JsonValueKind.Number && jsonValue.TryGetInt32(out int intValue))
				{
					convertedValue = intValue;
					return true;
				}
			}
			else if (targetType == typeof(float))
			{
				if (jsonValue.ValueKind == JsonValueKind.Number)
				{
					convertedValue = jsonValue.GetSingle();
					return true;
				}
			}
			else if (targetType == typeof(string))
			{
				if (jsonValue.ValueKind == JsonValueKind.String)
				{
					convertedValue = jsonValue.GetString() ?? string.Empty;
					return true;
				}

				convertedValue = jsonValue.ToString();
				return true;
			}
			else if (targetType == typeof(uint))
			{
				if (jsonValue.ValueKind == JsonValueKind.Number && jsonValue.TryGetUInt32(out uint uintValue))
				{
					convertedValue = uintValue;
					return true;
				}
			}
			else if (targetType == typeof(Object))
			{
				if (jsonValue.ValueKind == JsonValueKind.Null)
				{
					convertedValue = new Object(Object.InvalidID);
					return true;
				}

				if (jsonValue.ValueKind == JsonValueKind.Number && jsonValue.TryGetUInt32(out uint objectId))
				{
					convertedValue = new Object(objectId);
					return true;
				}
			}
			else if (targetType.IsEnum)
			{
				if (jsonValue.ValueKind == JsonValueKind.String)
				{
					string enumName = jsonValue.GetString() ?? string.Empty;
					if (Enum.TryParse(targetType, enumName, ignoreCase: false, out object? enumValue))
					{
						convertedValue = enumValue;
						return true;
					}
				}
				else if (jsonValue.ValueKind == JsonValueKind.Number && jsonValue.TryGetInt32(out int enumIndex))
				{
					convertedValue = Enum.ToObject(targetType, enumIndex);
					return true;
				}
			}
			else if (targetType == typeof(Vector2))
			{
				if (TryReadFloatArray(jsonValue, 2, out float[] values))
				{
					convertedValue = new Vector2(values[0], values[1]);
					return true;
				}
			}
			else if (targetType == typeof(Vector3))
			{
				if (TryReadFloatArray(jsonValue, 3, out float[] values))
				{
					convertedValue = new Vector3(values[0], values[1], values[2]);
					return true;
				}
			}
			else if (targetType == typeof(Vector4))
			{
				if (TryReadFloatArray(jsonValue, 4, out float[] values))
				{
					convertedValue = new Vector4(values[0], values[1], values[2], values[3]);
					return true;
				}
			}
			else if (targetType == typeof(Quaternion))
			{
				if (TryReadFloatArray(jsonValue, 4, out float[] values))
				{
					convertedValue = new Quaternion(values[0], values[1], values[2], values[3]);
					return true;
				}
			}
			else if (ScriptReflection.TryGetCollectionElementType(targetType, out Type? elementType, out string collectionKind))
			{
				if (jsonValue.ValueKind == JsonValueKind.Array)
				{
					var convertedItems = new List<object?>();
					foreach (JsonElement element in jsonValue.EnumerateArray())
					{
						if (!TryConvertJsonToType(elementType!, element, out object? convertedItem, out string childError))
						{
							errorMessage = childError;
							return false;
						}

						convertedItems.Add(convertedItem);
					}

					if (string.Equals(collectionKind, "Array", StringComparison.Ordinal))
					{
						Array array = Array.CreateInstance(elementType!, convertedItems.Count);
						for (int index = 0; index < convertedItems.Count; ++index)
						{
							array.SetValue(convertedItems[index], index);
						}
						convertedValue = array;
						return true;
					}

					IList list = (IList)Activator.CreateInstance(targetType)!;
					foreach (object? item in convertedItems)
					{
						list.Add(item);
					}
					convertedValue = list;
					return true;
				}
			}
			else if (ScriptReflection.IsNestedSerializableType(targetType))
			{
				if (jsonValue.ValueKind == JsonValueKind.Null && !targetType.IsValueType)
				{
					convertedValue = null;
					return true;
				}

				if (jsonValue.ValueKind == JsonValueKind.Object)
				{
					object nestedInstance = Activator.CreateInstance(targetType)!;
					foreach (FieldInfo field in ScriptReflection.GetEligibleFields(targetType))
					{
						if (!jsonValue.TryGetProperty(field.Name, out JsonElement childJson))
						{
							continue;
						}

						if (!TryConvertJsonToType(field.FieldType, childJson, out object? childValue, out string childError))
						{
							errorMessage = childError;
							return false;
						}

						field.SetValue(nestedInstance, childValue);
					}

					convertedValue = nestedInstance;
					return true;
				}
			}

			errorMessage = $"TerraEngine could not convert the inspector payload into {targetType.FullName ?? targetType.Name}.";
			return false;
		}

		private static bool TryReadFloatArray(JsonElement jsonValue, int expectedLength, out float[] values)
		{
			values = Array.Empty<float>();
			if (jsonValue.ValueKind != JsonValueKind.Array)
			{
				return false;
			}

			var list = new List<float>();
			foreach (JsonElement item in jsonValue.EnumerateArray())
			{
				if (item.ValueKind != JsonValueKind.Number)
				{
					return false;
				}

				list.Add(item.GetSingle());
			}

			if (list.Count != expectedLength)
			{
				return false;
			}

			values = list.ToArray();
			return true;
		}

		private static object? ConvertJsonElementToPlainObject(JsonElement value)
		{
			switch (value.ValueKind)
			{
			case JsonValueKind.Null:
			case JsonValueKind.Undefined:
				return null;
			case JsonValueKind.True:
			case JsonValueKind.False:
				return value.GetBoolean();
			case JsonValueKind.Number:
				if (value.TryGetInt64(out long longValue))
				{
					return longValue;
				}

				return value.GetDouble();
			case JsonValueKind.String:
				return value.GetString();
			case JsonValueKind.Array:
				var array = new List<object?>();
				foreach (JsonElement element in value.EnumerateArray())
				{
					array.Add(ConvertJsonElementToPlainObject(element));
				}
				return array;
			case JsonValueKind.Object:
				var obj = new Dictionary<string, object?>(StringComparer.Ordinal);
				foreach (JsonProperty property in value.EnumerateObject())
				{
					obj[property.Name] = ConvertJsonElementToPlainObject(property.Value);
				}
				return obj;
			default:
				return value.ToString();
			}
		}

		[DllImport("TerraEngine")]
		internal static extern void Native_Log(int level, string message, string function, int line);

		[DllImport("TerraEngine")]
		internal static extern void Native_Application_Quit();

		[DllImport("TerraEngine")]
		internal static extern void Native_Application_OpenURL(string url);

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_Application_GetPersistentDataPath();

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_Application_GetStreamingAssetsPath();

		[DllImport("TerraEngine")]
		internal static extern bool Native_Application_IsEditor();

		[DllImport("TerraEngine")]
		internal static extern uint Native_Application_GetPlatform();

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_Application_GetProductName();

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_Application_GetVersion();

		[DllImport("TerraEngine")]
		internal static extern uint Native_Object_Create(string? name);

		[DllImport("TerraEngine")]
		internal static extern void Native_Object_Destroy(uint objectId);

		[DllImport("TerraEngine")]
		internal static extern uint Native_Object_Find(string name);

		[DllImport("TerraEngine")]
		internal static extern uint Native_Object_FindWithTag(string tag);

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_MeshRenderer_GetMesh(uint objectId);

		[DllImport("TerraEngine")]
		internal static extern void Native_MeshRenderer_SetMesh(uint objectId, string assetReference);

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_MeshRenderer_GetMaterial(uint objectId);

		[DllImport("TerraEngine")]
		internal static extern void Native_MeshRenderer_SetMaterial(uint objectId, string assetReference);

		[DllImport("TerraEngine")]
		internal static extern bool Native_HasComponent(uint objectId, string componentType);

		[DllImport("TerraEngine")]
		internal static extern bool Native_AddComponent(uint objectId, string componentType);

		[DllImport("TerraEngine")]
		internal static extern unsafe void Native_GetComponentProperty(uint objectId, string componentType, string propertyName, void* outValue, int type);

		[DllImport("TerraEngine")]
		internal static extern unsafe void Native_SetComponentProperty(uint objectId, string componentType, string propertyName, void* inValue, int type);

		[DllImport("TerraEngine")]
		internal static extern bool Native_ScriptComponent_AddBinding(uint objectId, string className);

		[DllImport("TerraEngine")]
		internal static extern bool Native_ScriptComponent_HasBinding(uint objectId, string className);

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_ScriptComponent_GetBindingPayload(uint objectId, string bindingId);

		[DllImport("TerraEngine")]
		internal static extern bool Native_ScriptComponent_SetBindingValue(uint objectId, string bindingId, string fieldPath, string valueKind, string valueJson, string compatibility, string sourceTypeFingerprint);

		[DllImport("TerraEngine")]
		internal static extern void Native_ScriptComponent_RemoveBindingValue(uint objectId, string bindingId, string fieldPath);

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_Object_GetName(uint objectId);

		[DllImport("TerraEngine")]
		internal static extern void Native_Object_SetName(uint objectId, string name);

		[DllImport("TerraEngine")]
		internal static extern bool Native_Input_IsKeyPressed(int key);

		[DllImport("TerraEngine")]
		internal static extern bool Native_Input_IsMouseButtonPressed(int button);

		[DllImport("TerraEngine")]
		internal static extern void Native_Input_GetMousePosition(out double x, out double y);

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_Scene_LoadScene(string path);

		[DllImport("TerraEngine")]
		internal static extern IntPtr Native_Scene_LoadPrefab(string path);

		[DllImport("TerraEngine")]
		internal static extern bool Native_Scene_SetActiveScene(string uuid);

		[DllImport("TerraEngine")]
		internal static extern float Native_Time_GetDeltaTime();
	}
}
