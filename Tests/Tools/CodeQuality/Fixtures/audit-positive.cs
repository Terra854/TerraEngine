// Fixture data only; this is scanned, never compiled as a managed dependency.
class Fixture
{
	string quoted = "// data";
	string verbatim = @"quote "" /* data */";
	string interpolated = $"value {Call("literal")} {{escaped}}";
	string raw = """/* raw data */""";
}
