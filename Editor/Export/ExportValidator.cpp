// ExportValidator.cpp — Pre-export validation implementation.

#include "ExportValidator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <sstream>
#include <unordered_set>

// --- Helpers ---

static void AddDiagnostic(ExportValidationResult& result,
	ExportDiagnosticSeverity severity,
	const std::string& category,
	const std::string& message,
	const std::string& filePath = {},
	int line = 0)
{
	ExportDiagnostic diag;
	diag.severity = severity;
	diag.category = category;
	diag.message = message;
	diag.filePath = filePath;
	diag.line = line;
	result.diagnostics.push_back(std::move(diag));

	if (severity == ExportDiagnosticSeverity::Error) {
		result.errorCount++;
		result.passed = false;
	}
	else if (severity == ExportDiagnosticSeverity::Warning) {
		result.warningCount++;
	}
}

static bool ShouldScanManagedSourceFile(const std::filesystem::path& projectRoot, const std::filesystem::path& filePath)
{
	std::error_code ec;
	const std::filesystem::path relativePath = std::filesystem::relative(filePath, projectRoot, ec);
	if (ec)
	{
		return true;
	}

	const std::string normalized = relativePath.generic_string();
	return normalized.rfind("Build/", 0) != 0
		&& normalized.rfind("Library/", 0) != 0
		&& normalized.rfind("Temp/", 0) != 0
		&& normalized.rfind("ProjectSettings/", 0) != 0
		&& normalized.rfind("UserSettings/", 0) != 0;
}

static int CalculateLineNumber(std::string_view text, std::size_t characterOffset)
{
	int lineNumber = 1;
	for (std::size_t index = 0; index < std::min(characterOffset, text.size()); ++index)
	{
		if (text[index] == '\n')
		{
			++lineNumber;
		}
	}

	return lineNumber;
}

static bool IsCSharpIdentifierCharacter(char value)
{
	return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

static bool ContainsCSharpToken(std::string_view text, std::string_view token)
{
	if (token.empty() || text.size() < token.size())
	{
		return false;
	}

	std::size_t searchStart = 0;
	while (searchStart < text.size())
	{
		const std::size_t matchIndex = text.find(token, searchStart);
		if (matchIndex == std::string_view::npos)
		{
			return false;
		}

		// Treat escaped identifiers such as `@dynamic` as user-defined names,
		// not as keyword hits that should fail NativeAOT validation.
		const bool hasIdentifierBefore = matchIndex > 0
			&& (IsCSharpIdentifierCharacter(text[matchIndex - 1]) || text[matchIndex - 1] == '@');
		const std::size_t tokenEnd = matchIndex + token.size();
		const bool hasIdentifierAfter = tokenEnd < text.size() && IsCSharpIdentifierCharacter(text[tokenEnd]);
		if (!hasIdentifierBefore && !hasIdentifierAfter)
		{
			return true;
		}

		searchStart = matchIndex + token.size();
	}

	return false;
}

static std::string StripCSharpCommentsAndLiterals(std::string_view text)
{
	std::string sanitized(text);

	enum class ScanState
	{
		Code,
		LineComment,
		BlockComment,
		StringLiteral,
		VerbatimStringLiteral,
		CharLiteral
	};

	ScanState state = ScanState::Code;
	for (std::size_t index = 0; index < sanitized.size(); ++index)
	{
		char& current = sanitized[index];
		const char next = (index + 1) < sanitized.size() ? sanitized[index + 1] : '\0';

		switch (state)
		{
		case ScanState::Code:
			if (current == '/' && next == '/')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				state = ScanState::LineComment;
				++index;
			}
			else if (current == '/' && next == '*')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				state = ScanState::BlockComment;
				++index;
			}
			else if (current == '@' && next == '"')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				state = ScanState::VerbatimStringLiteral;
				++index;
			}
			else if (current == '"')
			{
				current = ' ';
				state = ScanState::StringLiteral;
			}
			else if (current == '\'')
			{
				current = ' ';
				state = ScanState::CharLiteral;
			}
			break;

		case ScanState::LineComment:
			if (current == '\n')
			{
				state = ScanState::Code;
			}
			else
			{
				current = ' ';
			}
			break;

		case ScanState::BlockComment:
			if (current == '*' && next == '/')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				state = ScanState::Code;
				++index;
			}
			else if (current != '\n' && current != '\r')
			{
				current = ' ';
			}
			break;

		case ScanState::StringLiteral:
			if (current == '\\' && next != '\0')
			{
				current = ' ';
				if (sanitized[index + 1] != '\n' && sanitized[index + 1] != '\r')
				{
					sanitized[index + 1] = ' ';
				}
				++index;
			}
			else if (current == '"')
			{
				current = ' ';
				state = ScanState::Code;
			}
			else if (current != '\n' && current != '\r')
			{
				current = ' ';
			}
			break;

		case ScanState::VerbatimStringLiteral:
			if (current == '"' && next == '"')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				++index;
			}
			else if (current == '"')
			{
				current = ' ';
				state = ScanState::Code;
			}
			else if (current != '\n' && current != '\r')
			{
				current = ' ';
			}
			break;

		case ScanState::CharLiteral:
			if (current == '\\' && next != '\0')
			{
				current = ' ';
				if (sanitized[index + 1] != '\n' && sanitized[index + 1] != '\r')
				{
					sanitized[index + 1] = ' ';
				}
				++index;
			}
			else if (current == '\'')
			{
				current = ' ';
				state = ScanState::Code;
			}
			else if (current != '\n' && current != '\r')
			{
				current = ' ';
			}
			break;
		}
	}

	return sanitized;
}

static bool IsSupportedNativeAotFieldType(std::string typeName)
{
	typeName.erase(std::remove_if(typeName.begin(), typeName.end(), [](unsigned char value) {
		return std::isspace(value) != 0;
	}), typeName.end());

	const std::size_t lastScope = typeName.find_last_of(':');
	if (lastScope != std::string::npos)
	{
		typeName = typeName.substr(lastScope + 1);
	}
	const std::size_t lastDot = typeName.find_last_of('.');
	if (lastDot != std::string::npos)
	{
		typeName = typeName.substr(lastDot + 1);
	}

	static const std::unordered_set<std::string> supportedTypes = {
		"bool",
		"int",
		"float",
		"uint",
		"Vector2",
		"Vector3",
		"Vector4",
		"Quaternion"
	};

	return supportedTypes.count(typeName) > 0;
}

// --- Profile validation ---

ExportValidationResult ExportValidator::ValidateProfile(const BuildProfile& profile) const {
	ExportValidationResult result;

	// Name must be non-empty.
	if (profile.name.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile", "Build profile name cannot be empty.");
	}

	// ID must exist.
	if (profile.id.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile", "Build profile ID is missing. Save the profile to generate one.");
	}

	// Output directory should be set.
	if (profile.outputDirectory.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Warning,
			"Profile", "Output directory is not set. A default will be used.");
	}

	// Every export must nominate a startup scene because exported games now
	// always boot through a concrete scene instead of supporting a no-scene path.
	if (profile.startupScene.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile", "Startup scene must be selected.");
	}

	return result;
}

// --- Managed source validation ---

ExportValidationResult ExportValidator::ValidateManagedSource(const std::filesystem::path& projectRoot) const {
	ExportValidationResult result;

	if (projectRoot.empty() || !std::filesystem::exists(projectRoot)) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"ManagedSource", "Project root directory does not exist: " + projectRoot.string());
		return result;
	}

	// Resolve the source directory from the .csproj path.
	const auto projectDir = projectRoot;

	// Scan authored .cs files for patterns that NativeAOT export cannot safely
	// support. The checks stay regex-based so validation remains lightweight and
	// can run inside the editor before a full managed compile begins.
	std::regex reflectionPattern(R"(\bType\.GetType\b|\bActivator\.CreateInstance\b|\bAssembly\.Load\b|\bMethodInfo\b|\bFieldInfo\b)");
	std::regex reflectionEmitPattern(R"(\bSystem\.Reflection\.Emit\b|\bAssemblyBuilder\b|\bDynamicMethod\b|\bILGenerator\b)");
	std::regex serializedFieldPattern(R"(\[SerializeField\]\s*(?:\r?\n\s*)*(?:public|private|protected|internal|readonly|volatile|new|unsafe|\s)+([A-Za-z_][A-Za-z0-9_:\.<>[\]]*)\s+[A-Za-z_][A-Za-z0-9_]*)");

	uint32_t scannedFiles = 0;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(projectDir)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != ".cs") continue;
		if (!ShouldScanManagedSourceFile(projectRoot, entry.path())) continue;

		scannedFiles++;

		std::ifstream file(entry.path());
		if (!file) continue;

		const std::string fileText((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		const std::string sanitizedFileText = StripCSharpCommentsAndLiterals(fileText);
		std::istringstream lineStream(sanitizedFileText);
		std::string line;
		int lineNumber = 0;
		while (std::getline(lineStream, line)) {
			lineNumber++;

			// Check for reflection usage.
			if (std::regex_search(line, reflectionPattern)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"Reflection API usage detected. NativeAOT export does not support runtime reflection.",
						entry.path().string(), lineNumber);
			}

			if (std::regex_search(line, reflectionEmitPattern)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"System.Reflection.Emit usage detected. NativeAOT export requires closed-world code generation and cannot support runtime IL emission.",
					entry.path().string(), lineNumber);
			}

			// Check for dynamic keyword usage.
			if (ContainsCSharpToken(line, "dynamic")) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"'dynamic' keyword usage detected. NativeAOT export does not support runtime binder generation.",
					entry.path().string(), lineNumber);
			}
		}

		for (std::sregex_iterator matchIt(sanitizedFileText.begin(), sanitizedFileText.end(), serializedFieldPattern), endIt; matchIt != endIt; ++matchIt) {
			const std::smatch& match = *matchIt;
			const std::string typeName = match[1].str();
			const int fieldLine = CalculateLineNumber(sanitizedFileText, static_cast<std::size_t>(match.position(1)));
			const bool obviousCollectionShape = typeName.find('<') != std::string::npos || typeName.find('[') != std::string::npos;
			if (obviousCollectionShape || !IsSupportedNativeAotFieldType(typeName)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"Unsupported [SerializeField] type '" + typeName + "'. NativeAOT export v1 only supports bool, int, float, uint, Vector2, Vector3, Vector4, and Quaternion.",
					entry.path().string(), fieldLine);
			}
		}
	}

	AddDiagnostic(result, ExportDiagnosticSeverity::Info,
		"ManagedSource",
		"Scanned " + std::to_string(scannedFiles) + " C# source file(s).");

	return result;
}

// --- Combined validation ---

ExportValidationResult ExportValidator::ValidateAll(const BuildProfile& profile, const std::filesystem::path& projectRoot) const {
	ExportValidationResult result = ValidateProfile(profile);

	if (profile.enableManagedCompilation) {
		auto sourceResult = ValidateManagedSource(projectRoot);
		
		result.errorCount += sourceResult.errorCount;
		result.warningCount += sourceResult.warningCount;
		if (!sourceResult.passed) {
			result.passed = false;
		}
		result.diagnostics.insert(result.diagnostics.end(),
			sourceResult.diagnostics.begin(),
			sourceResult.diagnostics.end());
	}

	return result;
}
