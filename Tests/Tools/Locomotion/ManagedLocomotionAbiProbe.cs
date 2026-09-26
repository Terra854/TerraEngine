// Runs real CLR x64 layout checks against the installed managed engine assembly.
using System;
using System.Reflection;
using System.Runtime.InteropServices;

internal static class ManagedLocomotionAbiProbe
{
	private static int Main()
	{
		if(IntPtr.Size != 8) {
			Console.Error.WriteLine("FAIL Locomotion ABI probe requires an x64 CLR process.");
			return 1;
		}
		Assembly assembly = Assembly.Load("TerraEngine.CSharp");
		var layouts = new (string Name, int Bytes)[] {
			("NativeLocomotionAbiHeader", 16),
			("NativeLocomotionHandle", 64),
			("NativeLocomotionResolveRequest", 32),
			("NativeLocomotionResolveResult", 104),
			("NativeLocomotionCommandBatch", 24),
			("NativeLocomotionCommand", 192),
			("NativeLocomotionReceiptQuery", 104),
			("NativeLocomotionReceipt", 136),
			("NativeLocomotionObservationQuery", 80),
			("NativeLocomotionObservation", 360),
			("NativeLocomotionBufferInfo", 40)
		};
		int failures = 0;
		foreach(var item in layouts) {
			Type? type = assembly.GetType("TerraEngine.Locomotion." + item.Name);
			if(type == null || Marshal.SizeOf(type) != item.Bytes
				|| type.StructLayoutAttribute?.Pack != 8) {
				Console.Error.WriteLine($"FAIL {item.Name}: expected {item.Bytes} bytes, Pack=8");
				++failures;
			} else Console.WriteLine($"PASS {item.Name}: {item.Bytes} bytes, Pack=8");
		}

		var offsets = new (string Type, string Field, int Bytes)[] {
			("NativeLocomotionHandle", "ObjectId", 24),
			("NativeLocomotionResolveResult", "Handle", 24),
			("NativeLocomotionResolveResult", "ConfigurationRevision", 88),
			("NativeLocomotionCommand", "Handle", 16),
			("NativeLocomotionCommand", "DirectionX", 152),
			("NativeLocomotionCommand", "DirectionY", 160),
			("NativeLocomotionCommand", "RequestedSpeed", 176),
			("NativeLocomotionCommand", "TransferProducerId", 184),
			("NativeLocomotionReceipt", "State", 104),
			("NativeLocomotionReceipt", "OwnerToken", 128),
			("NativeLocomotionObservation", "DesiredVelocityX", 208),
			("NativeLocomotionObservation", "DesiredVelocityZ", 224),
			("NativeLocomotionObservation", "ObservedPlanarSpeed", 232),
			("NativeLocomotionObservation", "ObservedLocalDirectionZ", 256),
			("NativeLocomotionObservation", "NavigationControllerValue", 264),
			("NativeLocomotionObservation", "ObservationDurationSeconds", 280),
			("NativeLocomotionObservation", "RemainingRouteDistanceMetres", 288),
			("NativeLocomotionObservation", "RequestedTranslationX", 296),
			("NativeLocomotionObservation", "RealizedTranslationX", 320),
			("NativeLocomotionObservation", "RequestedYawRadians", 344),
			("NativeLocomotionObservation", "RealizedYawRadians", 352),
			("NativeLocomotionBufferInfo", "PublicationEpoch", 32)
		};
		foreach(var item in offsets) {
			Type? type = assembly.GetType("TerraEngine.Locomotion." + item.Type);
			if(type == null || Marshal.OffsetOf(type, item.Field).ToInt64() != item.Bytes) {
				Console.Error.WriteLine($"FAIL {item.Type}.{item.Field}: expected offset {item.Bytes}");
				++failures;
			} else Console.WriteLine($"PASS {item.Type}.{item.Field}: offset {item.Bytes}");
		}

		var enums = new (string Name, string Member, int Value)[] {
			("NativeLocomotionRecordKind", "Handle", 1),
			("NativeLocomotionRecordKind", "BufferInfo", 10),
			("LocomotionCommandKind", "Acquire", 0),
			("LocomotionCommandKind", "Release", 6),
			("LocomotionReceiptState", "Queued", 0),
			("LocomotionReceiptState", "Expired", 4),
			("LocomotionReason", "ExternalExclusive", 15)
		};
		foreach(var item in enums) {
			Type? type = assembly.GetType("TerraEngine.Locomotion." + item.Name);
			object? value = type == null ? null : Enum.Parse(type, item.Member, ignoreCase: false);
			if(value == null || Convert.ToInt32(value) != item.Value) {
				Console.Error.WriteLine($"FAIL {item.Name}.{item.Member}: expected wire value {item.Value}");
				++failures;
			} else Console.WriteLine($"PASS {item.Name}.{item.Member}: wire value {item.Value}");
		}

		failures += CheckManagedReceiptCache(assembly);
		Console.WriteLine($"{layouts.Length} CLR strides, {offsets.Length} field offsets, {enums.Length} wire values, 3 managed cache behavior checks; {failures} failures.");
		return failures == 0 ? 0 : 1;
	}

	// This deterministic probe invokes only a private managed cache helper; it
	// does not cross the native ABI or substitute for an engine runtime test.
	private static int CheckManagedReceiptCache(Assembly assembly)
	{
		Type? wrapperType = assembly.GetType("TerraEngine.Locomotion.CharacterLocomotion");
		if(wrapperType == null) {
			Console.Error.WriteLine("FAIL managed receipt cache: CharacterLocomotion type is absent");
			return 1;
		}
		object? wrapper = Activator.CreateInstance(wrapperType);
		var producerField = wrapperType.GetField("_producerId", BindingFlags.Instance | BindingFlags.NonPublic);
		var epochField = wrapperType.GetField("_authorityEpoch", BindingFlags.Instance | BindingFlags.NonPublic);
		var tokenField = wrapperType.GetField("_ownerToken", BindingFlags.Instance | BindingFlags.NonPublic);
		var observeMethod = wrapperType.GetMethod("ObserveAppliedReceipt", BindingFlags.Instance | BindingFlags.NonPublic);
		if(wrapper == null || producerField == null || epochField == null || tokenField == null || observeMethod == null) {
			Console.Error.WriteLine("FAIL managed receipt cache: expected private cache helper/fields are unavailable");
			return 1;
		}

		producerField.SetValue(wrapper, 77UL);
		epochField.SetValue(wrapper, 10UL);
		tokenField.SetValue(wrapper, 123UL);
		var older = new TerraEngine.Locomotion.CharacterLocomotionReceipt {
			State = TerraEngine.Locomotion.LocomotionReceiptState.Applied,
			ProducerId = 77UL, AuthorityEpoch = 9UL, OwnerToken = 999UL
		};
		observeMethod.Invoke(wrapper, new object[] { older });
		ulong epochAfterOlder = (ulong)epochField.GetValue(wrapper)!;
		ulong tokenAfterOlder = (ulong)tokenField.GetValue(wrapper)!;
		bool olderIgnored = epochAfterOlder == 10UL && tokenAfterOlder == 123UL;
		Console.WriteLine($"INFO managed cache after older receipt: epoch={epochAfterOlder}, token={tokenAfterOlder}");

		var fresh = new TerraEngine.Locomotion.CharacterLocomotionReceipt {
			State = TerraEngine.Locomotion.LocomotionReceiptState.Applied,
			ProducerId = 77UL, AuthorityEpoch = 11UL, OwnerToken = 456UL
		};
		observeMethod.Invoke(wrapper, new object[] { fresh });
		ulong epochAfterFresh = (ulong)epochField.GetValue(wrapper)!;
		ulong tokenAfterFresh = (ulong)tokenField.GetValue(wrapper)!;
		bool freshAccepted = epochAfterFresh == 11UL && tokenAfterFresh == 456UL;
		Console.WriteLine($"INFO managed cache after fresh receipt: epoch={epochAfterFresh}, token={tokenAfterFresh}");

		var foreign = new TerraEngine.Locomotion.CharacterLocomotionReceipt {
			State = TerraEngine.Locomotion.LocomotionReceiptState.Applied,
			ProducerId = 88UL, AuthorityEpoch = 12UL, OwnerToken = 789UL
		};
		observeMethod.Invoke(wrapper, new object[] { foreign });
		bool foreignIgnored = (ulong)epochField.GetValue(wrapper)! == 11UL
			&& (ulong)tokenField.GetValue(wrapper)! == 456UL;

		CheckCache("older applied receipt ignored", olderIgnored);
		CheckCache("fresh matching receipt accepted", freshAccepted);
		CheckCache("foreign producer receipt ignored", foreignIgnored);
		return (olderIgnored && freshAccepted && foreignIgnored) ? 0 : 1;
	}

	private static void CheckCache(string name, bool passed)
	{
		Console.WriteLine($"{(passed ? "PASS" : "FAIL")} managed cache: {name}");
	}
}
