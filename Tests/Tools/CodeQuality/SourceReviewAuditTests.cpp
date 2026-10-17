#include "SourceReviewAudit.h"

#include <iostream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
	int failures = 0;
	int assertions = 0;
	std::string Read(const std::filesystem::path& path);
	std::vector<std::filesystem::path> Collect(const std::filesystem::path& root);
	bool IgnoredDirectory(std::string name);
	// Keep assertions active in Release builds; ordinary assert() would disappear.
	void Check(bool condition, const char* name)
	{
		++assertions;
		if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
	}
	int SelfTest(const std::filesystem::path& fixtures)
	{
		using namespace SourceReviewAudit;
		const auto cpp = Scan("void f() {\r\n // filler\r\n auto l=[] { return 1'000; };\r\n}\r\n");
		Check(cpp.ok() && cpp.tokens.size() > 10, "C++ tokens and digit separators");
		Check(cpp.comments.size() == 1, "CRLF comment extent");
		const auto* outer = MeasureSpan(cpp, 9);
		Check(outer && outer->firstLine == 1 && outer->lastLine == 4 && outer->codeLines == 3,
			"exact outer extent includes nested lambda and excludes comment-only line");
		Check(cpp.braces.size() == 2, "nested lambda inventory");
		Check(!Scan("/* unterminated").ok(), "unterminated comment fails closed");
		Check(!Scan("R\"bad delimiter(x)bad delimiter\"").ok(), "invalid raw delimiter fails closed");
		Check(!Scan("void f() {").ok(), "unbounded brace fails closed");
		const auto literals = Scan("auto a=R\"tag(/* is an implementation entry point; */)tag\"; // groups related state and operations\n");
		Check(literals.ok() && literals.comments.size() == 1 && literals.occurrences.size() == 1,
			"raw-string data excluded from production comment matches");
		const auto cs = Scan("var a=@\"quote \"\" // data\"; var b=$\"{Call(\"text\")} {{x}}\"; // tail\n", Language::CSharp);
		Check(cs.ok() && cs.comments.size() == 1, "C# verbatim and interpolated strings");
		std::string reason;
		Check(Equivalent("int x; // before\n", "int x; /* intent */\n", Language::Cpp, reason), "comment-only equivalence");
		Check(!Equivalent("x + +y;", "x ++y;", Language::Cpp, reason), "punctuation boundaries preserved");
		Check(!Equivalent("int x=1;", "int x=2;", Language::Cpp, reason), "changed executable token rejected");
		Check(!Equivalent("// note\nint x=__LINE__;", "\n\nint x=__LINE__;", Language::Cpp, reason), "source-sensitive line drift rejected");
		Check(!Equivalent("#define X 1\n+2\n", "#define X 1 +2\n", Language::Cpp, reason), "directive boundary preserved");
		const auto splice = Scan("// continued\\\n is an implementation entry point;\nint x;");
		Check(splice.ok() && splice.comments.size() == 1 && splice.occurrences.size() == 1,
			"C++ phase-two line splice precedes comment recognition");
		Check(Scan("auto x=u8R\"x(\" // literal)x\";").ok(), "prefixed raw literal");
		Check(!Scan("var x=$$\"\"\"unsupported interpolation\"\"\";", Language::CSharp).ok(), "unsupported raw interpolation fails closed");
		Check(Scan("var x=\"\"\"// data\nraw\"\"\";", Language::CSharp).ok(), "C# raw string data");
		Check(Scan("var x=@$\"{new { Value = \"x\" }}\";", Language::CSharp).ok(), "C# nested interpolation braces");
		Check(!Scan("\"unfinished").ok(), "unterminated ordinary string");
		Check(!Equivalent("const char* s=\"before\";", "const char* s=\"after\";", Language::Cpp, reason), "literal content preserved");
		Check(!Equivalent("#define X 1\\\r\n+2\r\n", "#define X 1 +2\n", Language::Cpp, reason), "non-comment directive reformatting conservatively rejected");
		Check(!Equivalent("x/**/y;", "xy;", Language::Cpp, reason), "identifier separator preserved");
		Check(!MeasureSpan(cpp, 10), "unknown opening offset rejected");
		Check(!Equivalent("auto x=R\"(a\\\nb)\";", "auto x=R\"(ab)\";", Language::Cpp, reason), "raw-string restored splice spelling preserved");
		Check(!Equivalent("auto x=\"x\"_s;", "auto x=\"x\" _s;", Language::Cpp, reason), "user-defined literal suffix adjacency preserved");
		Check(!Equivalent("#define X /*a\nb*/ 1\n", "#define X /*a b*/ 1\n", Language::Cpp, reason), "directive comment physical line change rejected");
		Check(!Equivalent("var @name=1;", "var @ name=1;", Language::CSharp, reason), "C# escaped identifier adjacency protected");
		Check(!Equivalent("// note\nint x;\n", "int x;\n", Language::Cpp, reason), "comment line removal rejected for indirectly imported source-location macros");
		Check(Equivalent("// note\nint x;\n", "\nint x;\n", Language::Cpp, reason), "comment removal preserving physical newlines accepted");
		Check(!Equivalent("#define F/**/(x) x\n", "#define F(x) x\n", Language::Cpp, reason), "macro function adjacency changes rejected");
		Check(!Equivalent("#include <a/*b*/c>\n", "#include <ac>\n", Language::Cpp, reason), "header-name comment lookalike remains executable content");
		Check(!Equivalent("#if __has_include(<a/*b*/c>)\n#endif\n", "#if __has_include(<a/*x*/c>)\n#endif\n", Language::Cpp, reason), "header-probe lookalike comments remain filename bytes");
		// Includes and default arguments can import column observers invisibly.
		// These assertions apply to every token, without a sourceSensitive opt-in.
		const auto columns = Scan("\tint x;");
		Check(columns.ok() && columns.tokens.front().column == 2, "original byte column records tabs without pretending display width");
		Check(!Equivalent("auto c=/*long*/std::source_location::current().column();", "auto c=/*x*/std::source_location::current().column();", Language::Cpp, reason), "source_location column shift rejected");
		Check(!Equivalent("/*long*/CALL_IMPORTED_LOCATION();", "/*x*/CALL_IMPORTED_LOCATION();", Language::Cpp, reason), "indirect column-sensitive macro shift rejected universally");
		Check(Equivalent("auto c=/*old*/std::source_location::current();", "auto c=/*new*/std::source_location::current();", Language::Cpp, reason), "same-width ASCII inline intent rewrite preserves column");
		Check(Equivalent("int x=/*old*/1;", "int x=       1;", Language::Cpp, reason), "padded inline comment removal preserves executable spelling and column");
		Check(!Equivalent("/*a\nbbbb*/int x;", "/*a\nb*/int x;", Language::Cpp, reason), "multiline closing-comment width cannot move following code");
		Check(!Equivalent("/*\t*/int x;", "/* */int x;", Language::Cpp, reason), "equal byte width cannot replace a prefix tab");
		Check(Equivalent("/*\t*/int x;", "  \t  int x;", Language::Cpp, reason), "inline removal may preserve exact prefix tab position");
		Check(!Equivalent("/*\xc3\xa9*/int x;", "/*xx*/int x;", Language::Cpp, reason), "equal byte width cannot silently alter Unicode column prefix spelling");
		Check(Equivalent("// old\r\nint x=__LINE__;\r\n", "\r\nint x=__LINE__;\r\n", Language::Cpp, reason), "CRLF blank comment line preserves __LINE__");
		Check(!Equivalent("#line 20 \"name.cpp\"\nint x;", "#line 21 \"name.cpp\"\nint x;", Language::Cpp, reason), "line directive executable edit rejected");
		Check(!Equivalent("#line 20 \"name.cpp\"\nint x;", "#line 20 \"other.cpp\"\nint x;", Language::Cpp, reason), "line directive filename edit rejected");
		Check(!Scan("auto s=R\"(a\\\nb)\";").ok(), "C++ raw LF splice explicitly fails closed");
		Check(!Scan("auto s=u8R\"tag(a\\\r\nb)tag\";").ok(), "C++ raw CRLF splice explicitly fails closed");
		Check(!Scan("var x=$\"\"\"{value}\"\"\";", Language::CSharp).ok(), "single-dollar C# raw interpolation explicitly fails closed");
		Check(!Scan("var x=$$$\"\"\"{{{value}}}\"\"\";", Language::CSharp).ok(), "multiple-dollar C# raw interpolation explicitly fails closed");
		Check(!Equivalent("var s=$\"{1 /*old*/}\";", "var s=$\"{1 /*new*/}\";", Language::CSharp, reason), "interpolation island comments conservatively remain part of exact literal token");
		Check(Scan("var s=\"\\u2028\";", Language::CSharp).ok(), "escaped Unicode newline text is not a physical terminator");
		Check(Equivalent("// old\r\nvar x=1;\r\n", "// new\r\nvar x=1;\r\n", Language::CSharp, reason), "C# CRLF comments remain supported");
		Check(!Scan("// note\rint x=1;").ok(), "C++ bare CR explicitly fails closed before comment recognition");
		Check(!Equivalent("// note\rint x=1;", "// note\rint x=2;", Language::Cpp, reason), "C++ bare CR edit rejected as unsupported source");
		Check(!Equivalent("// note\rint x=1;", "// note\rint x=1;", Language::Cpp, reason), "unchanged unsupported C++ CR input cannot waive validation");
		Check(Scan("auto s=\"\\r\";").ok(), "C++ escaped CR text is not a physical terminator");
		for(const auto terminator : {std::string("\r"), std::string("\xc2\x85"), std::string("\xe2\x80\xa8"), std::string("\xe2\x80\xa9")})
		{
			const auto before = "// note" + terminator + "int x=1;";
			const auto after = "// note" + terminator + "int x=2;";
			Check(!Scan(before, Language::CSharp).ok(), "unsupported C# physical terminator explicitly fails closed");
			Check(!Equivalent(before, after, Language::CSharp, reason), "C# physical terminator cannot conceal executable edit");
			Check(!Equivalent(before, before, Language::CSharp, reason), "unchanged unsupported C# source cannot waive validation");
		}
		const auto locationCppBefore = Read(fixtures / "audit-location-before.cpp");
		const auto locationCppAfter = Read(fixtures / "audit-location-after.cpp");
		const auto locationCsBefore = Read(fixtures / "audit-location-before.cs");
		const auto locationCsAfter = Read(fixtures / "audit-location-after.cs");
		Check(Equivalent(locationCppBefore, locationCppAfter, Language::Cpp, reason), "C++ custom raw CRLF and source-location paired corpus");
		Check(Equivalent(locationCsBefore, locationCsAfter, Language::CSharp, reason), "C# quoted verbatim interpolated nested raw and CallerLineNumber paired corpus");
		auto changedRaw = locationCppAfter;
		const auto payload = changedRaw.find("groups related state and operations");
		Check(payload != std::string::npos, "raw payload negative control locates real fixture data");
		if(payload != std::string::npos) changedRaw[payload] = 'G';
		Check(!Equivalent(locationCppBefore, changedRaw, Language::Cpp, reason), "C++ raw payload mutation rejected despite nearby comment cleanup");
		auto changedCs = locationCsAfter;
		const auto ordinary = changedCs.find("// data");
		Check(ordinary != std::string::npos, "C# payload negative control locates real fixture data");
		if(ordinary != std::string::npos) changedCs[ordinary + 3] = 'D';
		Check(!Equivalent(locationCsBefore, changedCs, Language::CSharp, reason), "C# literal payload mutation rejected despite nearby comment cleanup");
		Check(!Equivalent("// old\nLog();", "\n\nLog();", Language::CSharp, reason), "CallerLineNumber through imported logging API keeps physical line");
		// Installed tests read the corpus beside the executable. Standalone local
		// compilation may pass its source fixture directory explicitly.
		const auto positiveCpp = Scan(Read(fixtures / "audit-positive.cpp"));
		Check(positiveCpp.ok() && positiveCpp.braces.size() == 3 && positiveCpp.occurrences.empty(), "positive C++ file corpus");
		Check(Scan(Read(fixtures / "audit-positive.cs"), Language::CSharp).ok(), "positive C# file corpus");
		Check(!Scan(Read(fixtures / "audit-negative.cpp")).ok(), "negative C++ file corpus");
		Check(!Scan(Read(fixtures / "audit-negative.cs"), Language::CSharp).ok(), "negative C# file corpus");
		Check(IgnoredDirectory("obj") && IgnoredDirectory("RelWithDebInfo") && IgnoredDirectory("BUILD") && !IgnoredDirectory("Object"), "generated directory names and case folding");
		const auto traversal = Collect(fixtures / "audit-traversal");
		Check(traversal.size() == 1 && traversal.front().filename() == "retained.cpp", "generated directory corpus omitted without hiding actual source");
		Check(Collect(fixtures / "audit-empty").empty(), "empty source selection remains explicit");
		std::cout << "SourceReviewAudit self-test assertions=" << assertions << '\n';
		std::cout << "SourceReviewAudit self-test failures=" << failures << '\n';
		return failures == 0 ? 0 : 1;
	}
	std::string Read(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input) throw std::runtime_error("cannot read " + path.string());
		std::string text((std::istreambuf_iterator<char>(input)), {});
		if (input.bad()) throw std::runtime_error("read failed " + path.string());
		return text;
	}
	SourceReviewAudit::Language Kind(const std::filesystem::path& path)
	{ return path.extension() == ".cs" ? SourceReviewAudit::Language::CSharp : SourceReviewAudit::Language::Cpp; }
	std::string Json(std::string_view value)
	{
		std::string out = "\"";
		constexpr char hex[] = "0123456789abcdef";
		for (unsigned char c : value)
		{
			if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
			else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
			else out += static_cast<char>(c);
		}
		return out + '"';
	}
	std::size_t Number(const char* value)
	{
		std::string text(value); std::size_t consumed = 0;
		if (text.empty() || text.front() == '-') throw std::runtime_error("expected nonnegative byte offset or limit");
		const auto number = std::stoull(text, &consumed);
		if (consumed != text.size() || number > static_cast<unsigned long long>(SIZE_MAX)) throw std::runtime_error("invalid numeric argument");
		return static_cast<std::size_t>(number);
	}
	bool SourceFile(const std::filesystem::path& path)
	{
		const auto ext = path.extension().string();
		return ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".cpp" ||
			ext == ".cc" || ext == ".cxx" || ext == ".c" || ext == ".cs" || ext == ".inl";
	}
	bool IgnoredDirectory(std::string name)
	{
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return name == "bin" || name == "obj" || name == "out" || name == "build" || name == "intermediate" ||
			name == ".git" || name == ".vs" || name == ".cache" || name == "debug" || name == "release" || name == "relwithdebinfo";
	}
	std::vector<std::filesystem::path> Collect(const std::filesystem::path& root)
	{
		std::vector<std::filesystem::path> paths;
		if (std::filesystem::is_regular_file(root)) paths.push_back(root);
		else
		{
			// A repository root deliberately scans only production Engine/Editor.
			// A module directory may also be passed explicitly for bounded audits.
			std::vector<std::filesystem::path> roots = {root};
			if (std::filesystem::is_directory(root / "Engine") && std::filesystem::is_directory(root / "Editor")) roots = {root / "Engine", root / "Editor"};
			for (const auto& directory : roots)
			{
				for (auto it = std::filesystem::recursive_directory_iterator(directory); it != std::filesystem::recursive_directory_iterator(); ++it)
				{
					if (it->is_directory() && IgnoredDirectory(it->path().filename().string())) { it.disable_recursion_pending(); continue; }
					if (it->is_regular_file() && SourceFile(it->path())) paths.push_back(it->path());
				}
			}
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	}
	int Inventory(const std::filesystem::path& root, bool strict)
	{
		const auto paths = Collect(root);
		if (paths.empty()) throw std::runtime_error("no source files selected");
		std::size_t errors = 0, occurrences = 0;
		std::cout << "{\"scope\":\"structural-only; stage manifest and occurrence dispositions require separate qualification\",\"files\":[";
		bool first = true;
		for (const auto& path : paths)
		{
			const auto result = SourceReviewAudit::Scan(Read(path), Kind(path));
			if (!first) std::cout << ','; first = false;
			std::cout << "\n{\"path\":" << Json(path.generic_string()) << ",\"codeLines\":" << std::count(result.codeLines.begin(), result.codeLines.end(), true) << ",\"errors\":[";
			for (std::size_t i = 0; i < result.errors.size(); ++i) { if (i) std::cout << ','; std::cout << Json(result.errors[i]); }
			std::cout << "],\"occurrences\":[";
			for (std::size_t i = 0; i < result.occurrences.size(); ++i)
			{
				if (i) std::cout << ',';
				const auto& match = result.occurrences[i];
				std::cout << "{\"offset\":" << match.offset << ",\"line\":" << match.line << ",\"pattern\":" << Json(match.pattern) << '}';
			}
			std::cout << "],\"braceSpans\":[";
			for (std::size_t i = 0; i < result.braces.size(); ++i)
			{
				if (i) std::cout << ',';
				const auto& span = result.braces[i];
				std::cout << "{\"openingBraceOffset\":" << span.begin << ",\"spanEndOffsetExclusive\":" << span.end << ",\"firstLine\":" << span.firstLine << ",\"lastLine\":" << span.lastLine << ",\"codeLines\":" << span.codeLines << '}';
			}
			std::cout << "]}";
			errors += result.errors.size(); occurrences += result.occurrences.size();
		}
		std::cout << "\n],\"fileCount\":" << paths.size() << ",\"errorCount\":" << errors << ",\"occurrenceCount\":" << occurrences << "}\n";
		return errors || (strict && occurrences) ? 1 : 0;
	}
}

int main(int argc, char** argv)
{
	try
	{
		if ((argc == 2 || argc == 3) && std::string(argv[1]) == "--self-test")
			return SelfTest(argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::absolute(argv[0]).parent_path() / "Fixtures");
		if (argc == 3 && (std::string(argv[1]) == "--inventory" || std::string(argv[1]) == "--strict-final"))
			return Inventory(argv[2], std::string(argv[1]) == "--strict-final");
		if (argc == 4 && std::string(argv[1]) == "--compare")
		{
			if (Kind(argv[2]) != Kind(argv[3])) throw std::runtime_error("source languages differ");
			std::string reason;
			const auto equivalent = SourceReviewAudit::Equivalent(Read(argv[2]), Read(argv[3]), Kind(argv[2]), reason);
			std::cout << reason << '\n'; return equivalent ? 0 : 1;
		}
		if ((argc == 5 && std::string(argv[1]) == "--span") || (argc == 4 && std::string(argv[1]) == "--file-budget"))
		{
			const auto result = SourceReviewAudit::Scan(Read(argv[2]), Kind(argv[2]));
			for (const auto& error : result.errors) std::cerr << error << '\n';
			if (!result.ok()) return 1;
			std::size_t count = 0, limit = 0;
			if (argc == 5)
			{
				const auto* span = SourceReviewAudit::MeasureSpan(result, Number(argv[3]));
				if (!span) throw std::runtime_error("opening brace offset absent or ambiguous");
				count = span->codeLines; limit = Number(argv[4]);
				std::cout << "firstLine=" << span->firstLine << " lastLine=" << span->lastLine << ' ';
			}
			else { count = static_cast<std::size_t>(std::count(result.codeLines.begin(), result.codeLines.end(), true)); limit = Number(argv[3]); }
			std::cout << "codeLines=" << count << " limit=" << limit << '\n'; return count <= limit ? 0 : 1;
		}
		std::cerr << "Usage: --self-test [FIXTURES] | --inventory ROOT | --strict-final ROOT | --compare BEFORE AFTER | --span FILE OPENING_BYTE LIMIT | --file-budget FILE LIMIT\n";
		return 2;
	}
	catch (const std::exception& error) { std::cerr << "SourceReviewAudit: " << error.what() << '\n'; return 2; }
}
