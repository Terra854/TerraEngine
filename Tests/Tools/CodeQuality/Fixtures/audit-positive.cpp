// Fixture data: literal lookalikes must never become comment inventory entries.
void fixture()
{
	const char* quoted = "/* groups related state and operations */";
	const char* raw = R"edge(// is an implementation entry point;)edge";
	auto nested = [] { return [] { return 1'000; }(); };
}
