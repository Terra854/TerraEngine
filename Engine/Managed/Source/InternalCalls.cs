// InternalCalls.cs — Central hub for managed-to-native engine communication.
//
// This file implements the primary bridge between the C# scripting layer and
// the C++ engine core. It handles assembly loading, script instance lifecycle,
// data serialization for the inspector, and provides the low-level P/Invoke
// declarations for all engine services exposed to managed code.

using System;
using System.Collections;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;

namespace TerraEngine
{
	// ScriptType is the managed-facing contract for this subsystem; keep it predictable for scripts.
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

	// ScriptBindingPayload is the managed-facing contract for this subsystem; keep it predictable for scripts.
	internal sealed class ScriptBindingPayload
	{
		public string BindingId { get; set; } = string.Empty;
		public string ClassName { get; set; } = string.Empty;
		public ulong ScriptId { get; set; }
		public ulong AuthoredStateVersion { get; set; } = 1;
		public List<ScriptFieldValuePayload> SerializedValues { get; set; } = new();
	}

	[StructLayout(LayoutKind.Sequential)]
	internal readonly struct ScriptBatchItemNative
	{
		public readonly uint ObjectId;
		public readonly IntPtr BindingIdPtr;
		public readonly IntPtr ClassNamePtr;
		public readonly ulong FrameOrder;
		public readonly ulong ObjectGeneration;
		public readonly ulong ComponentGeneration;
		public readonly ulong BindingGeneration;
		public readonly int DiagnosticsRequested;
		public readonly int WorkerDispatch;
		public readonly int ExportMode;
		public readonly ulong AuthoredStateVersion;
	}

	// LiveFieldSnapshotPayload is the managed-facing contract for this subsystem; keep it predictable for scripts.
	internal sealed class LiveFieldSnapshotPayload
	{
		public string FieldPath { get; set; } = string.Empty;
		public object? Value { get; set; }
		public string Source { get; set; } = "Authored";
		public string RefreshState { get; set; } = "Clean";
		public bool DiffersFromAuthored { get; set; }
		public long LastObservedVersion { get; set; }
	}

	// LiveBindingSnapshotPayload is the managed-facing contract for this subsystem; keep it predictable for scripts.
	internal sealed class LiveBindingSnapshotPayload
	{
		public string BindingId { get; set; } = string.Empty;
		public string FullTypeName { get; set; } = string.Empty;
		public string Status { get; set; } = "Attached";
		public bool RuntimeAvailable { get; set; }
		public int RuntimeOverrideCount { get; set; }
		public ulong SnapshotVersion { get; set; }
		public List<LiveFieldSnapshotPayload> Fields { get; set; } = new();
		public List<ScriptFieldDiagnosticPayload> Diagnostics { get; set; } = new();
	}

	// BridgeWriteResultPayload is the managed-facing contract for this subsystem; keep it predictable for scripts.
	internal sealed class BridgeWriteResultPayload
	{
		public bool Success { get; set; }
		public string BindingId { get; set; } = string.Empty;
		public string FieldPath { get; set; } = string.Empty;
		public string Message { get; set; } = string.Empty;
		public object? Value { get; set; }
		public long SnapshotVersion { get; set; }
	}

	// MigrationFieldValuePayload is the managed-facing contract for this subsystem; keep it predictable for scripts.
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

	// MigrationResultPayload is the managed-facing contract for this subsystem; keep it predictable for scripts.
	internal sealed class MigrationResultPayload
	{
		public List<MigrationFieldValuePayload> Fields { get; set; } = new();
		public List<string> PreservedFieldPaths { get; set; } = new();
		public List<string> ConvertedFieldPaths { get; set; } = new();
		public List<string> BrokenFieldPaths { get; set; } = new();
		public List<string> HiddenStoredFieldPaths { get; set; } = new();
		public List<ScriptFieldDiagnosticPayload> Diagnostics { get; set; } = new();
	}

	// InternalCalls is the managed-facing contract for this subsystem; keep it predictable for scripts.
	public static class InternalCalls
	{
		private static volatile bool s_scriptingDiagnosticsActive;
		private static long s_bridgeCacheHits;
		private static long s_bridgeCacheMisses;
		private static long s_bridgeCallsAvoided;

		internal static bool ScriptingDiagnosticsActive => s_scriptingDiagnosticsActive;

		public static void PushScriptDispatchContext(uint objectId, IntPtr bindingIdPtr, ulong frameOrder, int workerDispatch, int exportMode, ulong objectGeneration, ulong componentGeneration, ulong bindingGeneration)
		{
			// Export generated code runs in the gameplay assembly, so it cannot
			// reach the internal P/Invoke declaration directly. This public
			// wrapper keeps NativeAOT batch dispatch on the same deferred-mutation
			// contract as editor script dispatch without exposing extra native
			// state to ordinary component wrappers.
			Native_Scripting_PushDispatchContext(objectId, bindingIdPtr, frameOrder, workerDispatch, exportMode, objectGeneration, componentGeneration, bindingGeneration);
		}

		public static void PopScriptDispatchContext()
		{
			// Always pair this with PushScriptDispatchContext in a finally block;
			// the native side stores the context in thread-local state used by
			// component setters to decide whether writes must be deferred.
			Native_Scripting_PopDispatchContext();
		}

		internal static void RecordManagedCacheHit()
		{
			if (!s_scriptingDiagnosticsActive)
			{
				return;
			}
			Interlocked.Increment(ref s_bridgeCacheHits);
			Interlocked.Increment(ref s_bridgeCallsAvoided);
		}

		internal static void RecordManagedCacheMiss()
		{
			if (!s_scriptingDiagnosticsActive)
			{
				return;
			}
			Interlocked.Increment(ref s_bridgeCacheMisses);
		}

		private static void ResetManagedBridgeCacheCounters()
		{
			Interlocked.Exchange(ref s_bridgeCacheHits, 0);
			Interlocked.Exchange(ref s_bridgeCacheMisses, 0);
			Interlocked.Exchange(ref s_bridgeCallsAvoided, 0);
		}

		private static void FlushManagedBridgeCacheCounters()
		{
			long hits = Interlocked.Exchange(ref s_bridgeCacheHits, 0);
			long misses = Interlocked.Exchange(ref s_bridgeCacheMisses, 0);
			long avoided = Interlocked.Exchange(ref s_bridgeCallsAvoided, 0);
			if (hits != 0 || misses != 0 || avoided != 0)
			{
				Native_Scripting_ReportBridgeCacheCounters((ulong)hits, (ulong)misses, (ulong)avoided);
			}
		}

		private sealed class NativeUtf8ReturnBuffer
		{
			private readonly object _syncRoot = new();
			private byte[] _buffer = new byte[] { 0 };
			private GCHandle _pinnedBuffer;

			public IntPtr WriteJson<T>(T payload)
			{
				return WriteText(JsonSerializer.Serialize(payload, s_jsonOptions));
			}

			// WriteText bridges managed callers to engine state; validate arguments before forwarding to native code.
			public IntPtr WriteText(string text)
			{
				lock (_syncRoot)
				{
					// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

		// ScriptInstanceState is the managed-facing contract for this subsystem; keep it predictable for scripts.
		private sealed class ScriptInstanceState
		{
			// bindingId, not objectId, is the stable key because one object may
			// host multiple bindings of the same script type at once.
			public Script Instance { get; init; } = null!;
			public Type ScriptType { get; init; } = null!;
			public string ClassName { get; set; } = string.Empty;
			public uint ObjectId { get; set; }
			public string BindingId { get; set; } = string.Empty;
			public string LastHydratedSignature { get; set; } = string.Empty;
			public ulong LastAuthoredStateVersion { get; set; }
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

		// ReadNativeUtf8 bridges managed callers to engine state; validate arguments before forwarding to native code.
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
		private static ConcurrentDictionary<string, ScriptInstanceState> _scriptInstances = new(StringComparer.Ordinal);
		private static ulong s_lastPumpedUiFrame;

		// DrainScriptInstances bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static List<ScriptInstanceState> DrainScriptInstances()
		{
			var drainedInstances = new List<ScriptInstanceState>(_scriptInstances.Values);
			_scriptInstances.Clear();
			return drainedInstances;
		}

		// DisposeScriptInstances bridges managed callers to engine state; validate arguments before forwarding to native code.
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
		// Initialize bridges managed callers to engine state; validate arguments before forwarding to native code.
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
		// LoadGameplayAssembly bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static int LoadGameplayAssembly(IntPtr assemblyPathPtr)
		{
			try
			{
				string assemblyPath = Marshal.PtrToStringAnsi(assemblyPathPtr) ?? string.Empty;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
					_scriptInstances = new ConcurrentDictionary<string, ScriptInstanceState>(StringComparer.Ordinal);
					s_lastPumpedUiFrame = 0;

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
		// UnloadGameplayAssembly bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static void UnloadGameplayAssembly()
		{
			DisposeScriptInstances();

			lock (s_bridgeSync)
			{
				_descriptorCache = new Dictionary<string, ScriptTypeDescriptorPayload>(StringComparer.Ordinal);
				_attachableTypes = new Dictionary<string, Type>(StringComparer.Ordinal);
				_scriptInstances = new ConcurrentDictionary<string, ScriptInstanceState>(StringComparer.Ordinal);
				s_lastPumpedUiFrame = 0;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
		// ResetGameplaySession bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static void ResetGameplaySession()
		{
			// Stop should discard every managed runtime instance while keeping the
			// reflected type catalog warm for the next Play press.
			DisposeScriptInstances();
		}

		[UnmanagedCallersOnly]
		// IsScriptTypeAttachable bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static int IsScriptTypeAttachable(IntPtr classNamePtr)
		{
			string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
			Dictionary<string, Type> attachableTypes = _attachableTypes;
			return !string.IsNullOrWhiteSpace(className) && attachableTypes.ContainsKey(className) ? 1 : 0;
		}

		[UnmanagedCallersOnly]
		// DescribeBindingType bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static IntPtr DescribeBindingType(IntPtr classNamePtr)
		{
			try
			{
				string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
		// ReadLiveBindingValues bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static IntPtr ReadLiveBindingValues(uint objectId, IntPtr bindingIdPtr, IntPtr classNamePtr)
		{
			try
			{
				string bindingId = Marshal.PtrToStringAnsi(bindingIdPtr) ?? string.Empty;
				string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
					RuntimeAvailable = true,
					SnapshotVersion = scriptState.LastAuthoredStateVersion
				};
				snapshot.Diagnostics.AddRange(descriptor.Diagnostics);

				lock (scriptState.SyncRoot)
				{
					foreach (ScriptFieldDescriptorPayload rootDescriptor in descriptor.Fields)
					{
						FieldInfo? field = FindSerializableField(scriptState.ScriptType, rootDescriptor.SerializedName);
						// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
						// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
		// WriteLiveBindingValue bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static IntPtr WriteLiveBindingValue(uint objectId, IntPtr bindingIdPtr, IntPtr fieldPathPtr, IntPtr valueJsonPtr)
		{
			try
			{
				string bindingId = Marshal.PtrToStringAnsi(bindingIdPtr) ?? string.Empty;
				string fieldPath = Marshal.PtrToStringAnsi(fieldPathPtr) ?? string.Empty;
				string valueJson = Marshal.PtrToStringAnsi(valueJsonPtr) ?? string.Empty;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
		// MigrateBindingAuthoredState bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static IntPtr MigrateBindingAuthoredState(IntPtr classNamePtr, IntPtr serializedValuesJsonPtr)
		{
			try
			{
				string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
				string serializedValuesJson = Marshal.PtrToStringAnsi(serializedValuesJsonPtr) ?? "[]";
				List<ScriptFieldValuePayload> storedValues = DeserializeFieldValues(serializedValuesJson);
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
					// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (field == null)
					{
						continue;
					}

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (compatibility == "Migrated")
					{
						result.ConvertedFieldPaths.Add(rootDescriptor.FieldPath);
					}
					else
					{
						result.PreservedFieldPaths.Add(rootDescriptor.FieldPath);
					}

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
		// PumpFrameEvents runs managed event queues once per engine scripting frame.
		// Native code supplies the frame id so worker-dispatched scripts can share
		// one event pump without each script repeating the same queue work.
		public static void PumpFrameEvents(ulong frameId)
		{
			PumpFrameEventsCore(frameId);
		}

		private static void PumpFrameEventsCore(ulong frameId)
		{
			try
			{
				lock (s_bridgeSync)
				{
					if (s_lastPumpedUiFrame == frameId)
					{
						return;
					}

					s_lastPumpedUiFrame = frameId;
				}

				UIBridge.PumpPendingEvents();
			}
			catch (Exception ex)
			{
				Log.Error($"Exception while pumping managed UI events:\n{ex}");
			}
		}

		[UnmanagedCallersOnly]
		// UpdateScript bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static void UpdateScript(uint objectId, IntPtr bindingIdPtr, IntPtr classNamePtr, float deltaTime)
		{
			UpdateScriptCore(objectId, bindingIdPtr, classNamePtr, deltaTime);
		}

		private static void UpdateScriptCore(uint objectId, IntPtr bindingIdPtr, IntPtr classNamePtr, float deltaTime, ulong dispatchedAuthoredStateVersion = 0)
		{
			try
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (_gameplayAssembly == null)
				{
					return;
				}

				string bindingId = Marshal.PtrToStringAnsi(bindingIdPtr) ?? string.Empty;
				string className = Marshal.PtrToStringAnsi(classNamePtr) ?? string.Empty;
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (string.IsNullOrWhiteSpace(bindingId) || string.IsNullOrWhiteSpace(className))
				{
					return;
				}

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (!TryResolveScriptType(className, out Type? scriptType))
				{
					Log.Error($"Could not find compiled gameplay script class {className} in the active Assembly-CSharp catalog.");
					return;
				}

				ScriptInstanceState scriptState;
				bool createdState = false;
				if (!_scriptInstances.TryGetValue(bindingId, out scriptState!))
				{
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
								ObjectId = objectId,
								BindingId = bindingId
							};
							_scriptInstances[bindingId] = scriptState;
							createdState = true;
						}
					}
				}

				ulong authoredStateVersion = dispatchedAuthoredStateVersion != 0
					? dispatchedAuthoredStateVersion
					: Native_ScriptComponent_GetBindingVersion(objectId, bindingId);

				lock (scriptState.SyncRoot)
				{
					bool identityChanged = scriptState.ObjectId != objectId
						|| !string.Equals(scriptState.BindingId, bindingId, StringComparison.Ordinal);
					if (identityChanged)
					{
						scriptState.Instance.BindEngineInstance(objectId, bindingId);
						scriptState.BindingId = bindingId;
					}

					scriptState.ObjectId = objectId;
					scriptState.ClassName = className;

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (createdState || authoredStateVersion == 0 || authoredStateVersion != scriptState.LastAuthoredStateVersion)
					{
						ScriptBindingPayload bindingPayload = GetBindingPayload(objectId, bindingId);
						string authoredSignature = JsonSerializer.Serialize(bindingPayload.SerializedValues, s_jsonOptions);
						HydrateScriptInstance(scriptState.Instance, scriptState.ScriptType, bindingPayload);
						scriptState.LastHydratedSignature = authoredSignature;
						scriptState.LastAuthoredStateVersion = bindingPayload.AuthoredStateVersion != 0 ? bindingPayload.AuthoredStateVersion : authoredStateVersion;
						Native_ScriptComponent_ReportObservedVersion(objectId, bindingId, scriptState.LastAuthoredStateVersion);
					}

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

		[UnmanagedCallersOnly]
		public static unsafe void UpdateScriptBatch(IntPtr itemsPtr, int itemCount, float deltaTime, ulong frameId)
		{
			if (itemsPtr == IntPtr.Zero || itemCount <= 0)
			{
				return;
			}

			PumpFrameEventsCore(frameId);
			ScriptBatchItemNative* items = (ScriptBatchItemNative*)itemsPtr;
			bool previousDiagnosticsState = s_scriptingDiagnosticsActive;
			bool diagnosticsActive = false;
			for (int index = 0; index < itemCount; ++index)
			{
				ScriptBatchItemNative item = items[index];
				diagnosticsActive = diagnosticsActive || item.DiagnosticsRequested != 0;
			}

			s_scriptingDiagnosticsActive = diagnosticsActive;
			if (diagnosticsActive)
			{
				ResetManagedBridgeCacheCounters();
			}

			try
			{
				for (int index = 0; index < itemCount; ++index)
				{
					ScriptBatchItemNative item = items[index];
					Object.SeedDispatchCacheGenerations(item.ObjectId, item.ObjectGeneration, item.ComponentGeneration, item.BindingGeneration);
					Native_Scripting_PushDispatchContext(item.ObjectId, item.BindingIdPtr, item.FrameOrder, item.WorkerDispatch, item.ExportMode, item.ObjectGeneration, item.ComponentGeneration, item.BindingGeneration);
					try
					{
						UpdateScriptCore(item.ObjectId, item.BindingIdPtr, item.ClassNamePtr, deltaTime, item.AuthoredStateVersion);
					}
					finally
					{
						Native_Scripting_PopDispatchContext();
					}
				}
			}
			finally
			{
				if (diagnosticsActive)
				{
					FlushManagedBridgeCacheCounters();
				}
				s_scriptingDiagnosticsActive = previousDiagnosticsState;
			}
		}

		[UnmanagedCallersOnly]
		// DispatchNativeUIEvent bridges managed callers to engine state; validate arguments before forwarding to native code.
		public static void DispatchNativeUIEvent(IntPtr elementHandle, int eventType, float pointerX, float pointerY)
		{
			try
			{
				UIBridge.DispatchNativeEvent(elementHandle, (UIEventType)eventType, new Vector2(pointerX, pointerY));
			}
			catch (Exception ex)
			{
				Log.Error($"Exception while dispatching a native UI event:\n{ex}");
			}
		}

		// TryResolveScriptType bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static bool TryResolveScriptType(string className, out Type? scriptType)
		{
			Dictionary<string, Type> attachableTypes = _attachableTypes;
			return attachableTypes.TryGetValue(className, out scriptType);
		}

		// GetOrCreateDescriptor bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static ScriptTypeDescriptorPayload GetOrCreateDescriptor(string className, Type scriptType)
		{
			lock (s_bridgeSync)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

		// PopulateDescriptorDefaultValues bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static void PopulateDescriptorDefaultValues(Type scriptType, ScriptTypeDescriptorPayload descriptor)
		{
			// Create one detached sample instance so the inspector can reflect the
			// field initializers that gameplay code authored on the C# class.
			object? scriptSample = TryCreateDescriptorSample(scriptType);
			foreach (ScriptFieldDescriptorPayload rootDescriptor in descriptor.Fields)
			{
				FieldInfo? field = FindSerializableField(scriptType, rootDescriptor.SerializedName);
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (field == null)
				{
					continue;
				}

				object? rootValue = scriptSample != null ? field.GetValue(scriptSample) : null;
				PopulateDescriptorDefaultValues(rootDescriptor, field.FieldType, rootValue);
			}
		}

		// PopulateDescriptorDefaultValues bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static void PopulateDescriptorDefaultValues(ScriptFieldDescriptorPayload descriptor, Type valueType, object? value)
		{
			descriptor.DefaultValue = ConvertRuntimeValueToPayload(value, valueType);

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (descriptor.ContainerKind == "NestedObject")
			{
				// Nested children read from the sampled object when present. When a
				// parent default is null, fall back to a detached sample so structs
				// and serializable classes can still seed newly materialized data.
				object? nestedSample = value ?? TryCreateDescriptorSample(valueType);
				foreach (ScriptFieldDescriptorPayload childDescriptor in descriptor.Children)
				{
					FieldInfo? childField = FindSerializableField(valueType, childDescriptor.SerializedName);
					// Validate this condition before crossing into native or editor state that may outlive the managed object.
					if (childField == null)
					{
						continue;
					}

					object? childValue = nestedSample != null ? childField.GetValue(nestedSample) : null;
					PopulateDescriptorDefaultValues(childDescriptor, childField.FieldType, childValue);
				}

				return;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

		// TryCreateDescriptorSample bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static object? TryCreateDescriptorSample(Type valueType)
		{
			try
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (valueType == typeof(string))
				{
					return string.Empty;
				}

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

		// GetInstanceState bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static ScriptInstanceState? GetInstanceState(string bindingId)
		{
			lock (s_bridgeSync)
			{
				_scriptInstances.TryGetValue(bindingId, out ScriptInstanceState? state);
				return state;
			}
		}

		// GetBindingPayload bridges managed callers to engine state; validate arguments before forwarding to native code.
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

		// BuildAuthoredValueLookup bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static Dictionary<string, ScriptFieldValuePayload> BuildAuthoredValueLookup(ScriptBindingPayload bindingPayload)
		{
			var lookup = new Dictionary<string, ScriptFieldValuePayload>(StringComparer.Ordinal);
			foreach (ScriptFieldValuePayload value in bindingPayload.SerializedValues)
			{
				string stablePath = GetStableFieldPath(value);
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (!lookup.ContainsKey(stablePath))
				{
					lookup[stablePath] = value;
				}
			}

			return lookup;
		}

		// HydrateScriptInstance bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static void HydrateScriptInstance(Script instance, Type scriptType, ScriptBindingPayload bindingPayload)
		{
			// Reapply authored values after reloads or inspector edits so the live
			// managed instance always starts from the scene-backed binding payload.
			var authoredLookup = BuildAuthoredValueLookup(bindingPayload);
			ScriptTypeDescriptorPayload descriptor = GetOrCreateDescriptor(bindingPayload.ClassName, scriptType);
			foreach (ScriptFieldDescriptorPayload rootDescriptor in descriptor.Fields)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (!authoredLookup.TryGetValue(rootDescriptor.FieldPath, out ScriptFieldValuePayload? authoredValue))
				{
					continue;
				}

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (string.Equals(authoredValue.Compatibility, "Broken", StringComparison.OrdinalIgnoreCase))
				{
					continue;
				}

				FieldInfo? field = FindSerializableField(scriptType, rootDescriptor.SerializedName);
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (field == null)
				{
					continue;
				}

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (!TryConvertJsonToType(field.FieldType, authoredValue.Value, out object? convertedValue, out _))
				{
					continue;
				}

				field.SetValue(instance, convertedValue);
			}
		}

		// FindSerializableField bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static FieldInfo? FindSerializableField(Type scriptType, string fieldPath)
		{
			string rootFieldName = ExtractRootFieldPath(fieldPath);
			foreach (FieldInfo field in ScriptReflection.GetEligibleFields(scriptType))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (string.Equals(field.Name, rootFieldName, StringComparison.Ordinal))
				{
					return field;
				}
			}

			return null;
		}

		// ExtractRootFieldPath bridges managed callers to engine state; validate arguments before forwarding to native code.
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

		// GetStableFieldPath bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static string GetStableFieldPath(ScriptFieldValuePayload payload)
		{
			return string.IsNullOrWhiteSpace(payload.FieldPath) ? payload.FieldName : payload.FieldPath;
		}

		// DeserializeFieldValues bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static List<ScriptFieldValuePayload> DeserializeFieldValues(string serializedValuesJson)
		{
			if (string.IsNullOrWhiteSpace(serializedValuesJson))
			{
				return new List<ScriptFieldValuePayload>();
			}

			return JsonSerializer.Deserialize<List<ScriptFieldValuePayload>>(serializedValuesJson, s_jsonOptions) ?? new List<ScriptFieldValuePayload>();
		}

		// BuildBrokenMigrationResult bridges managed callers to engine state; validate arguments before forwarding to native code.
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

		// ConvertRuntimeValueToPayload bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static object? ConvertRuntimeValueToPayload(object? value, Type valueType)
		{
			if (value == null)
			{
				return null;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (valueType == typeof(bool) || valueType == typeof(int) || valueType == typeof(float) || valueType == typeof(string) || valueType == typeof(uint))
			{
				return value;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (valueType.IsEnum)
			{
				string? enumName = Enum.GetName(valueType, value);
				return enumName is not null ? enumName : Convert.ToInt32(value);
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (valueType == typeof(Vector2))
			{
				Vector2 vector = (Vector2)value;
				return new[] { vector.X, vector.Y };
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (valueType == typeof(Vector3))
			{
				Vector3 vector = (Vector3)value;
				return new[] { vector.X, vector.Y, vector.Z };
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (valueType == typeof(Vector4))
			{
				Vector4 vector = (Vector4)value;
				return new[] { vector.X, vector.Y, vector.Z, vector.W };
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (valueType == typeof(Quaternion))
			{
				Quaternion rotation = (Quaternion)value;
				return new[] { rotation.X, rotation.Y, rotation.Z, rotation.W };
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (valueType == typeof(Object))
			{
				return ((Object)value).ID;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (ScriptReflection.TryGetCollectionElementType(valueType, out Type? elementType, out _))
			{
				var list = new List<object?>();
				foreach (object? item in (IEnumerable)value)
				{
					list.Add(ConvertRuntimeValueToPayload(item, elementType!));
				}
				return list;
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

		// TryConvertJsonToType bridges managed callers to engine state; validate arguments before forwarding to native code.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.Number && jsonValue.TryGetInt32(out int intValue))
				{
					convertedValue = intValue;
					return true;
				}
			}
			else if (targetType == typeof(float))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.Number)
				{
					convertedValue = jsonValue.GetSingle();
					return true;
				}
			}
			else if (targetType == typeof(string))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.Number && jsonValue.TryGetUInt32(out uint uintValue))
				{
					convertedValue = uintValue;
					return true;
				}
			}
			else if (targetType == typeof(Object))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.Null)
				{
					convertedValue = new Object(Object.InvalidID);
					return true;
				}

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.Number && jsonValue.TryGetUInt32(out uint objectId))
				{
					convertedValue = new Object(objectId);
					return true;
				}
			}
			else if (targetType.IsEnum)
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.String)
				{
					string enumName = jsonValue.GetString() ?? string.Empty;
					// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (TryReadFloatArray(jsonValue, 2, out float[] values))
				{
					convertedValue = new Vector2(values[0], values[1]);
					return true;
				}
			}
			else if (targetType == typeof(Vector3))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (TryReadFloatArray(jsonValue, 3, out float[] values))
				{
					convertedValue = new Vector3(values[0], values[1], values[2]);
					return true;
				}
			}
			else if (targetType == typeof(Vector4))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (TryReadFloatArray(jsonValue, 4, out float[] values))
				{
					convertedValue = new Vector4(values[0], values[1], values[2], values[3]);
					return true;
				}
			}
			else if (targetType == typeof(Quaternion))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (TryReadFloatArray(jsonValue, 4, out float[] values))
				{
					convertedValue = new Quaternion(values[0], values[1], values[2], values[3]);
					return true;
				}
			}
			else if (ScriptReflection.TryGetCollectionElementType(targetType, out Type? elementType, out string collectionKind))
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.Array)
				{
					var convertedItems = new List<object?>();
					foreach (JsonElement element in jsonValue.EnumerateArray())
					{
						// Validate this condition before crossing into native or editor state that may outlive the managed object.
						if (!TryConvertJsonToType(elementType!, element, out object? convertedItem, out string childError))
						{
							errorMessage = childError;
							return false;
						}

						convertedItems.Add(convertedItem);
					}

					// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.Null && !targetType.IsValueType)
				{
					convertedValue = null;
					return true;
				}

				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (jsonValue.ValueKind == JsonValueKind.Object)
				{
					object nestedInstance = Activator.CreateInstance(targetType)!;
					foreach (FieldInfo field in ScriptReflection.GetEligibleFields(targetType))
					{
						// Validate this condition before crossing into native or editor state that may outlive the managed object.
						if (!jsonValue.TryGetProperty(field.Name, out JsonElement childJson))
						{
							continue;
						}

						// Validate this condition before crossing into native or editor state that may outlive the managed object.
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

		// TryReadFloatArray bridges managed callers to engine state; validate arguments before forwarding to native code.
		private static bool TryReadFloatArray(JsonElement jsonValue, int expectedLength, out float[] values)
		{
			values = Array.Empty<float>();
			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (jsonValue.ValueKind != JsonValueKind.Array)
			{
				return false;
			}

			var list = new List<float>();
			foreach (JsonElement item in jsonValue.EnumerateArray())
			{
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
				if (item.ValueKind != JsonValueKind.Number)
				{
					return false;
				}

				list.Add(item.GetSingle());
			}

			// Validate this condition before crossing into native or editor state that may outlive the managed object.
			if (list.Count != expectedLength)
			{
				return false;
			}

			values = list.ToArray();
			return true;
		}

		// ConvertJsonElementToPlainObject bridges managed callers to engine state; validate arguments before forwarding to native code.
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
				// Validate this condition before crossing into native or editor state that may outlive the managed object.
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
		// Native_Log bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_Log(int level, string message, string function, int line);

		[DllImport("TerraEngine")]
		// Native_Application_Quit bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_Application_Quit();

		[DllImport("TerraEngine")]
		// Native_Application_OpenURL bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_Application_OpenURL(string url);

		[DllImport("TerraEngine")]
		// Native_Application_GetPersistentDataPath bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_Application_GetPersistentDataPath();

		[DllImport("TerraEngine")]
		// Native_Application_GetStreamingAssetsPath bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_Application_GetStreamingAssetsPath();

		[DllImport("TerraEngine")]
		// Native_Application_IsEditor bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_Application_IsEditor();

		[DllImport("TerraEngine")]
		// Native_Application_GetPlatform bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern uint Native_Application_GetPlatform();

		[DllImport("TerraEngine")]
		// Native_Application_GetProductName bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_Application_GetProductName();

		[DllImport("TerraEngine")]
		// Native_Application_GetVersion bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_Application_GetVersion();

		[DllImport("TerraEngine")]
		// Native_Object_Create bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern uint Native_Object_Create(string? name);

		[DllImport("TerraEngine")]
		// Native_Object_Destroy bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_Object_Destroy(uint objectId);

		[DllImport("TerraEngine")]
		// Native_Object_Find bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern uint Native_Object_Find(string name);

		[DllImport("TerraEngine")]
		// Native_Object_FindWithTag bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern uint Native_Object_FindWithTag(string tag);

		[DllImport("TerraEngine")]
		// Native_MeshRenderer_GetMesh bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_MeshRenderer_GetMesh(uint objectId);

		[DllImport("TerraEngine")]
		// Native_MeshRenderer_SetMesh bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_MeshRenderer_SetMesh(uint objectId, string assetReference);

		[DllImport("TerraEngine")]
		// Native_MeshRenderer_GetMaterial bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_MeshRenderer_GetMaterial(uint objectId);

		[DllImport("TerraEngine")]
		// Native_MeshRenderer_SetMaterial bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_MeshRenderer_SetMaterial(uint objectId, string assetReference);

		[DllImport("TerraEngine")]
		// Native_ResourceStreaming_Preload sends a non-blocking streaming hint to native ResourceManager.
		internal static extern bool Native_ResourceStreaming_Preload(string assetReference, int assetType);

		[DllImport("TerraEngine")]
		// Native_HasComponent bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_HasComponent(uint objectId, string componentType);

		[DllImport("TerraEngine")]
		// Native_AddComponent bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_AddComponent(uint objectId, string componentType);

		[DllImport("TerraEngine")]
		internal static extern ulong Native_Object_GetComponentGeneration(uint objectId);

		[DllImport("TerraEngine")]
		internal static extern ulong Native_Object_GetMetadataGeneration(uint objectId);

		[DllImport("TerraEngine")]
		internal static extern ulong Native_Object_GetBindingGeneration(uint objectId);

		[DllImport("TerraEngine")]
		internal static extern void Native_Scripting_PushDispatchContext(uint objectId, IntPtr bindingIdPtr, ulong frameOrder, int workerDispatch, int exportMode, ulong objectGeneration, ulong componentGeneration, ulong bindingGeneration);

		[DllImport("TerraEngine")]
		internal static extern void Native_Scripting_PopDispatchContext();

		[DllImport("TerraEngine")]
		internal static extern void Native_Scripting_ReportBridgeCacheCounters(ulong hits, ulong misses, ulong avoidedCalls);

		[DllImport("TerraEngine")]
		// Native_GetComponentProperty bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern unsafe void Native_GetComponentProperty(uint objectId, string componentType, string propertyName, void* outValue, int type);

		[DllImport("TerraEngine")]
		// Native_SetComponentProperty bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern unsafe void Native_SetComponentProperty(uint objectId, string componentType, string propertyName, void* inValue, int type);

		[DllImport("TerraEngine")]
		// Native_ScriptComponent_AddBinding bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_ScriptComponent_AddBinding(uint objectId, string className);

		[DllImport("TerraEngine")]
		// Native_ScriptComponent_HasBinding bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_ScriptComponent_HasBinding(uint objectId, string className);

		[DllImport("TerraEngine")]
		// Native_ScriptComponent_GetBindingPayload bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_ScriptComponent_GetBindingPayload(uint objectId, string bindingId);

		[DllImport("TerraEngine")]
		internal static extern ulong Native_ScriptComponent_GetBindingVersion(uint objectId, string bindingId);

		[DllImport("TerraEngine")]
		internal static extern void Native_ScriptComponent_ReportObservedVersion(uint objectId, string bindingId, ulong observedVersion);

		[DllImport("TerraEngine")]
		// Native_ScriptComponent_SetBindingValue bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_ScriptComponent_SetBindingValue(uint objectId, string bindingId, string fieldPath, string valueKind, string valueJson, string compatibility, string sourceTypeFingerprint);

		[DllImport("TerraEngine")]
		// Native_ScriptComponent_RemoveBindingValue bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_ScriptComponent_RemoveBindingValue(uint objectId, string bindingId, string fieldPath);

		[DllImport("TerraEngine")]
		// Native_Object_GetName bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_Object_GetName(uint objectId);

		[DllImport("TerraEngine")]
		// Native_Object_SetName bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_Object_SetName(uint objectId, string name);

		[DllImport("TerraEngine")]
		// Native_Input_IsKeyPressed bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_Input_IsKeyPressed(int key);

		[DllImport("TerraEngine")]
		// Native_Input_IsMouseButtonPressed bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_Input_IsMouseButtonPressed(int button);

		[DllImport("TerraEngine")]
		// Native_Input_GetMousePosition bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_Input_GetMousePosition(out double x, out double y);

		[DllImport("TerraEngine")]
		// Native_UI_WasElementClicked bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_UI_WasElementClicked(string elementId);

		[DllImport("TerraEngine")]
		// Native_UI_IsElementHovered bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_UI_IsElementHovered(string elementId);

		[DllImport("TerraEngine")]
		// Native_UI_GetInteractionFrame bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern ulong Native_UI_GetInteractionFrame();

		[DllImport("TerraEngine")]
		// Native_UI_CreateElement bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_UI_CreateElement(int elementType);

		[DllImport("TerraEngine")]
		// Native_UI_ReleaseHandle bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_ReleaseHandle(IntPtr elementHandle);

		[DllImport("TerraEngine")]
		// Native_UI_GetRootElement bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_UI_GetRootElement(uint objectId);

		[DllImport("TerraEngine")]
		// Native_UI_FindElementById bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_UI_FindElementById(IntPtr elementHandle, string elementId);

		[DllImport("TerraEngine")]
		// Native_UI_GetElementType bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern int Native_UI_GetElementType(IntPtr elementHandle);

		[DllImport("TerraEngine")]
		// Native_UI_IsHandleAlive bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_UI_IsHandleAlive(IntPtr elementHandle);

		[DllImport("TerraEngine")]
		// Native_UI_GetElementId bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_UI_GetElementId(IntPtr elementHandle);

		[DllImport("TerraEngine")]
		// Native_UI_SetElementId bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_SetElementId(IntPtr elementHandle, string elementId);

		[DllImport("TerraEngine")]
		// Native_UI_GetElementText bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_UI_GetElementText(IntPtr elementHandle);

		[DllImport("TerraEngine")]
		// Native_UI_SetElementText bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_SetElementText(IntPtr elementHandle, string text);

		[DllImport("TerraEngine")]
		// Native_UI_GetElementVisible bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_UI_GetElementVisible(IntPtr elementHandle);

		[DllImport("TerraEngine")]
		// Native_UI_SetElementVisible bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_SetElementVisible(IntPtr elementHandle, bool visible);

		[DllImport("TerraEngine")]
		// Native_UI_AddChild bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_AddChild(IntPtr parentHandle, IntPtr childHandle);

		[DllImport("TerraEngine")]
		// Native_UI_RemoveChild bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_RemoveChild(IntPtr parentHandle, IntPtr childHandle);

		[DllImport("TerraEngine")]
		// Native_UI_ClearChildren bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_ClearChildren(IntPtr parentHandle);

		[DllImport("TerraEngine")]
		// Native_UI_SetStyleFloat bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_SetStyleFloat(IntPtr elementHandle, int property, float value);

		[DllImport("TerraEngine")]
		// Native_UI_GetStyleFloat bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern float Native_UI_GetStyleFloat(IntPtr elementHandle, int property);

		[DllImport("TerraEngine")]
		// Native_UI_SetStyleColor bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_SetStyleColor(IntPtr elementHandle, int property, float x, float y, float z, float w);

		[DllImport("TerraEngine")]
		// Native_UI_GetStyleColor bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern void Native_UI_GetStyleColor(IntPtr elementHandle, int property, out float x, out float y, out float z, out float w);

		[DllImport("TerraEngine")]
		// Native_UI_SetEventCallback bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_UI_SetEventCallback(IntPtr elementHandle, int eventType, bool enabled);

		[DllImport("TerraEngine")]
		// Native_Scene_LoadScene bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_Scene_LoadScene(string path);

		[DllImport("TerraEngine")]
		// Native_Scene_LoadPrefab bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern IntPtr Native_Scene_LoadPrefab(string path);

		[DllImport("TerraEngine")]
		// Native_Scene_SetActiveScene bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern bool Native_Scene_SetActiveScene(string uuid);

		[DllImport("TerraEngine")]
		// Native_Scene_SubmitTransactionJson bridges managed worker-safe mutation payloads to the protected native transaction queue.
		internal static extern IntPtr Native_Scene_SubmitTransactionJson(string transactionJson);

		[DllImport("TerraEngine")]
		// Native_Time_GetDeltaTime bridges managed callers to engine state; validate arguments before forwarding to native code.
		internal static extern float Native_Time_GetDeltaTime();
	}
}
