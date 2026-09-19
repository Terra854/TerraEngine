// Runs real CLR layout checks and compiles the controller-owned sample alongside
// them. This deliberately does not claim to simulate physics or editor gameplay.
using System;
using System.Runtime.InteropServices;
using TerraEngine;

internal static class ManagedAgentContractProbe
{
	private static int Main()
	{
		var assembly = typeof(NavigationAgent).Assembly;
		var expected = new (string Name, int Bytes)[] {
			("NavigationAgentIdentity", 64),
			("NativeNavigationAgentResolveRequest", 16),
			("NativeNavigationAgentIdentitySnapshot", 80),
			("NativeNavigationAgentCommand", 176),
			("NativeNavigationAgentCommandReceipt", 104),
			("NativeNavigationAgentStatusQuery", 72),
			("NativeNavigationAgentStatusSnapshot", 48),
			("NativeNavigationAgentGuidanceQuery", 72),
			("NativeNavigationAgentGuidanceSnapshot", 304),
			("NativeNavigationAgentReceiptQuery", 80)
		};
		int failures = 0;
		foreach (var item in expected) {
			var type = assembly.GetType("TerraEngine." + item.Name);
			if (type == null || Marshal.SizeOf(type) != item.Bytes
				|| type.StructLayoutAttribute?.Pack != 8) {
				Console.Error.WriteLine($"FAIL {item.Name}: expected {item.Bytes} bytes, Pack=8");
				++failures;
			} else Console.WriteLine($"PASS {item.Name}: {item.Bytes} bytes, Pack=8");
		}
		// Equal packet sizes alone do not prove that native and managed members
		// overlap correctly. Check the boundaries where padding is most relevant.
		var offsets = new (string Type, string Field, int Offset)[] {
			("NavigationAgentIdentity", "Object", 16),
			("NavigationAgentIdentity", "RuntimeSlotGeneration", 56),
			("NativeNavigationAgentCommand", "Identity", 8),
			("NativeNavigationAgentCommand", "Location", 80),
			("NativeNavigationAgentCommand", "CorrelationId", 168),
			("NativeNavigationAgentGuidanceSnapshot", "NextWaypoint", 104),
			("NativeNavigationAgentGuidanceSnapshot", "DestinationLocation", 216),
			("NativeNavigationAgentGuidanceSnapshot", "DestinationObject", 280)
		};
		foreach (var item in offsets) {
			var type = assembly.GetType("TerraEngine." + item.Type);
			if (type == null || Marshal.OffsetOf(type, item.Field).ToInt64() != item.Offset) {
				Console.Error.WriteLine($"FAIL {item.Type}.{item.Field}: expected offset {item.Offset}");
				++failures;
			} else Console.WriteLine($"PASS {item.Type}.{item.Field}: offset {item.Offset}");
		}
		// These classes are compiled, not instantiated: real callback and native
		// outcome execution belongs to the installed editor acceptance evidence.
		Console.WriteLine($"{expected.Length} CLR layouts checked; {failures} failures. Agent gameplay and managed outcome fixtures compiled (not executed).");
		return failures == 0 ? 0 : 1;
	}
}
