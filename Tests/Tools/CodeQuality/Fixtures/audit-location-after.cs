using System.Runtime.CompilerServices;
class LocationFixture {
	static void Log(string value, [CallerLineNumber] int line = 0) {}
	static void Test() {
		var ordinary = "// data";
		var verbatim = @"quote "" /* data */";
		var interpolated = $@"{Call("literal")} {{escaped}}";
		var nested = $"{Call($"{1}")}";
		var raw = """"quotes """ // data"""";
		       Log(ordinary);
	}
}
