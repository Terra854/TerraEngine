// ExportValidator.cpp — Pre-export validation implementation
//
// Implements the validation rules for build profiles and C# source code, utilizing regex-based scanning to detect unsupported reflection or dynamic binder usage. It aggregates findings into a diagnostic report used to prevent failed or unstable exported builds.

#include "ExportValidator.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <array>
#include <nlohmann/json.hpp>

#include "File Formats/TerraEngineMaterialFile.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Resource/MaterialRuntimeContract.h"

// --- Helpers ---

static void AddDiagnostic(ExportValidationResult& result,
	ExportDiagnosticSeverity severity,
	const std::string& category,
	const std::string& message,
	const std::string& filePath = {},
	int line = 0)
{
	std::string categoryCode;
	categoryCode.reserve(category.size());
	for(unsigned char character : category) {
		if(std::isalnum(character) != 0) categoryCode.push_back(static_cast<char>(std::tolower(character)));
		else if(categoryCode.empty() || categoryCode.back() != '_') categoryCode.push_back('_');
	}
	while(!categoryCode.empty() && categoryCode.back() == '_') categoryCode.pop_back();
	if(categoryCode.empty()) categoryCode = "general";

	ExportDiagnostic diag;
	diag.severity = severity;
	diag.category = category;
	diag.message = message;
	diag.filePath = filePath;
	diag.line = line;
	diag.stage = "validation";
	diag.code = "export.validation." + categoryCode;
	result.diagnostics.push_back(std::move(diag));

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (severity == ExportDiagnosticSeverity::Error) {
		result.errorCount++;
		result.passed = false;
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (severity == ExportDiagnosticSeverity::Warning) {
		result.warningCount++;
	}
}

// ShouldScanManagedSourceFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static bool ShouldScanManagedSourceFile(const std::filesystem::path& projectRoot, const std::filesystem::path& filePath)
{
	std::error_code ec;
	const std::filesystem::path relativePath = std::filesystem::relative(filePath, projectRoot, ec);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ec)
	{
		return true;
	}

	const std::string normalized = relativePath.generic_string();
	return normalized.rfind("Build/", 0) != 0
		&& normalized.rfind("Library/", 0) != 0
		&& normalized.rfind("Temp/", 0) != 0
		&& normalized.rfind("obj/", 0) != 0
		&& normalized.rfind("bin/", 0) != 0
		&& normalized.rfind("ProjectSettings/", 0) != 0
		&& normalized.rfind("UserSettings/", 0) != 0;
}

// CalculateLineNumber is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static int CalculateLineNumber(std::string_view text, std::size_t characterOffset)
{
	int lineNumber = 1;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0; index < std::min(characterOffset, text.size()); ++index)
	{
		if (text[index] == '\n')
		{
			++lineNumber;
		}
	}

	return lineNumber;
}

// IsCSharpIdentifierCharacter is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static bool IsCSharpIdentifierCharacter(char value)
{
	return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
}

// ContainsCSharpToken is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static bool ContainsCSharpToken(std::string_view text, std::string_view token)
{
	if (token.empty() || text.size() < token.size())
	{
		return false;
	}

	std::size_t searchStart = 0;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (searchStart < text.size())
	{
		const std::size_t matchIndex = text.find(token, searchStart);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!hasIdentifierBefore && !hasIdentifierAfter)
		{
			return true;
		}

		searchStart = matchIndex + token.size();
	}

	return false;
}

// StripCSharpCommentsAndLiterals is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::string StripCSharpCommentsAndLiterals(std::string_view text)
{
	std::string sanitized(text);

	// ScanState names the supported states explicitly so serialization and switches stay readable.
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
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0; index < sanitized.size(); ++index)
	{
		char& current = sanitized[index];
		const char next = (index + 1) < sanitized.size() ? sanitized[index + 1] : '\0';

		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (state)
		{
		case ScanState::Code:
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (current == '/' && next == '/')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				state = ScanState::LineComment;
				++index;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current == '/' && next == '*')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				state = ScanState::BlockComment;
				++index;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current == '@' && next == '"')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				state = ScanState::VerbatimStringLiteral;
				++index;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current == '"')
			{
				current = ' ';
				state = ScanState::StringLiteral;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current == '\'')
			{
				current = ' ';
				state = ScanState::CharLiteral;
			}
			break;

		case ScanState::LineComment:
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (current == '*' && next == '/')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				state = ScanState::Code;
				++index;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current != '\n' && current != '\r')
			{
				current = ' ';
			}
			break;

		case ScanState::StringLiteral:
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (current == '\\' && next != '\0')
			{
				current = ' ';
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (sanitized[index + 1] != '\n' && sanitized[index + 1] != '\r')
				{
					sanitized[index + 1] = ' ';
				}
				++index;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current == '"')
			{
				current = ' ';
				state = ScanState::Code;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current != '\n' && current != '\r')
			{
				current = ' ';
			}
			break;

		case ScanState::VerbatimStringLiteral:
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (current == '"' && next == '"')
			{
				current = ' ';
				sanitized[index + 1] = ' ';
				++index;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current == '"')
			{
				current = ' ';
				state = ScanState::Code;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current != '\n' && current != '\r')
			{
				current = ' ';
			}
			break;

		case ScanState::CharLiteral:
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (current == '\\' && next != '\0')
			{
				current = ' ';
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (sanitized[index + 1] != '\n' && sanitized[index + 1] != '\r')
				{
					sanitized[index + 1] = ' ';
				}
				++index;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current == '\'')
			{
				current = ' ';
				state = ScanState::Code;
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (current != '\n' && current != '\r')
			{
				current = ' ';
			}
			break;
		}
	}

	return sanitized;
}

// IsSupportedNativeAotFieldType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static bool IsSupportedNativeAotFieldType(std::string typeName)
{
	typeName.erase(std::remove_if(typeName.begin(), typeName.end(), [](unsigned char value) {
		return std::isspace(value) != 0;
	}), typeName.end());

	const std::size_t lastScope = typeName.find_last_of(':');
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (lastScope != std::string::npos)
	{
		typeName = typeName.substr(lastScope + 1);
	}
	const std::size_t lastDot = typeName.find_last_of('.');
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (profile.name.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile", "Build profile name cannot be empty.");
	}

	// ID must exist.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (profile.id.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile", "Build profile ID is missing. Save the profile to generate one.");
	}

	// Output directory should be set.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	const std::string platformName = ExportPlatformToString(profile.platform);
	const std::string managedRuntimeName = GetExportManagedRuntimeLibraryName(profile.platform);
	const std::string engineRuntimeName = GetExportEngineLibraryName(profile.platform);
	const std::string glfwRuntimeName = GetExportGlfwLibraryName(profile.platform);
	AddDiagnostic(result, ExportDiagnosticSeverity::Info,
		"Profile",
		"Export platform '" + platformName + "' expects Managed/" + managedRuntimeName +
			", " + engineRuntimeName + ", " + glfwRuntimeName +
			", and Game.tepak to be staged with the launcher.");

	if (profile.platform == ExportPlatform::Linux) {
		// Linux failures are often environmental, so surface the display/GPU
		// contract before the build pipeline starts copying artifacts.
		AddDiagnostic(result, ExportDiagnosticSeverity::Info,
			"Profile",
			"Linux startup smoke requires a working display server and Vulkan-capable GPU/driver; headless or unsupported hosts should fail within 30 seconds with display or GPU diagnostics.");
	}

	if (profile.enableManagedCompilation && !IsExportPlatformNativeToHost(profile.platform)) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile",
			"Cross-OS NativeAOT export is not supported by dotnet. This host can publish " +
				GetExportRuntimeIdentifier(GetHostExportPlatform()) + "; the selected platform requires " +
				GetExportRuntimeIdentifier(profile.platform) + ". Switch the profile platform or export from the target OS.");
	}

	return result;
}

static void ValidateMaterialFile(const std::filesystem::path& materialPath, ExportValidationResult& result)
{
	std::ifstream input(materialPath, std::ios::binary);
	if (!input)
	{
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Material", "Could not read material file.", materialPath.string());
		return;
	}

	const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	if (bytes.size() < sizeof(MaterialHeader))
	{
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Material", "TMAT file is too small.", materialPath.string());
		return;
	}

	MaterialHeader header{};
	std::memcpy(&header, bytes.data(), sizeof(MaterialHeader));
	std::uint32_t magic = 0u;
	std::memcpy(&magic, header.magic, sizeof(magic));
	if (magic != kMaterialMagicValue || header.version != kMaterialCurrentVersion || header.headerSize > bytes.size())
	{
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Material", "TMAT header is invalid or unsupported.", materialPath.string());
		return;
	}

	std::span<const TextureBinding> textures;
	if (header.textureCount > 0u)
	{
		const std::size_t textureBytes = static_cast<std::size_t>(header.textureCount) * sizeof(TextureBinding);
		if (header.textureArrayOffset < header.headerSize || header.textureArrayOffset + textureBytes > bytes.size())
		{
			AddDiagnostic(result, ExportDiagnosticSeverity::Error,
				"Material", "TMAT texture array is out of bounds.", materialPath.string());
			return;
		}
		textures = std::span<const TextureBinding>(reinterpret_cast<const TextureBinding*>(bytes.data() + header.textureArrayOffset), header.textureCount);
	}

	const MaterialRuntimeContract contract = ResolveMaterialRuntimeContract(header, textures);
	for (const std::string& warning : contract.warnings)
	{
		const std::string message = "Material fallback: " + warning;
		result.materialFallbackDiagnostics.push_back(materialPath.generic_string() + ": " + warning);
		AddDiagnostic(result, ExportDiagnosticSeverity::Warning,
			"Material", message, materialPath.string());
	}
}

static ExportValidationResult ValidateMaterialAssets(const std::filesystem::path& projectRoot)
{
	ExportValidationResult result;
	const std::filesystem::path assetsRoot = projectRoot / "Assets";
	if (!std::filesystem::exists(assetsRoot))
	{
		return result;
	}

	for (const auto& entry : std::filesystem::recursive_directory_iterator(assetsRoot))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".tmat")
		{
			ValidateMaterialFile(entry.path(), result);
		}
	}

	return result;
}

// Physics collider recipes are editor input, whereas .tepcoll files are the
// immutable runtime envelopes.  Validate the latter before export staging so a
// creator can recook a bad asset without first producing a partial package.
static bool ContainsPlaceholderMarker(const std::vector<std::byte>& bytes)
{
	static constexpr std::string_view kMarker = "placeholder";
	return std::search(bytes.begin(), bytes.end(), kMarker.begin(), kMarker.end(),
		[](std::byte left, char right) {
			return static_cast<char>(std::tolower(static_cast<unsigned char>(std::to_integer<unsigned char>(left)))) == right;
		}) != bytes.end();
}

static void ValidateCookedColliderFile(const std::filesystem::path& colliderPath, ExportValidationResult& result)
{
	std::ifstream input(colliderPath, std::ios::binary);
	if(!input) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error, "PhysicsCollider",
			"Cooked collider could not be read. Re-cook the collider before exporting.", colliderPath.string());
		return;
	}

	const std::vector<char> rawBytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	std::vector<std::byte> bytes;
	bytes.reserve(rawBytes.size());
	for(const char value : rawBytes) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
	if(bytes.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error, "PhysicsCollider",
			"Cooked collider payload is empty. Re-cook the collider before exporting.", colliderPath.string());
		return;
	}
	if(ContainsPlaceholderMarker(bytes)) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error, "PhysicsCollider",
			"Cooked collider contains a placeholder payload. Replace it with a completed Jolt cook before exporting.", colliderPath.string());
		return;
	}

	TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
	const auto collider = TerraEngine::Physics::CookedColliderAsset::Deserialize(bytes, diagnostics);
	if(!collider) {
		const std::string detail = diagnostics.entries.empty() ? "The envelope is malformed." : diagnostics.entries.front().message;
		AddDiagnostic(result, ExportDiagnosticSeverity::Error, "PhysicsCollider",
			"Cooked collider is corrupt or incomplete: " + detail + " Re-cook the collider before exporting.", colliderPath.string());
		return;
	}

	const auto compatibility = collider->CheckCompatibility(
		TerraEngine::Physics::CookedColliderAsset::HostPlatform(),
		TerraEngine::Physics::CookedColliderProviderFamily::Jolt,
		TerraEngine::Physics::kJoltCookProviderVersion,
		TerraEngine::Physics::kJoltCookRevision);
	if(!compatibility.compatible) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error, "PhysicsCollider",
			"Cooked collider does not match Jolt 5.6.0 revision e77f175595e64cb44218cc9d9d56fc365ad0e36a: " + compatibility.message + " Re-cook the collider with this project version.",
			colliderPath.string());
	}
}

static ExportValidationResult ValidatePhysicsColliderAssets(const std::filesystem::path& projectRoot)
{
	ExportValidationResult result;
	const std::filesystem::path assetsRoot = projectRoot / "Assets";
	const std::filesystem::path compiledRoot = projectRoot / "Build" / "Assets";
	if(!std::filesystem::exists(assetsRoot)) return result;

	// A recipe without its matching cooked envelope is a source-only dependency.
	// It cannot be shipped because the exported runtime intentionally has no cook
	// compiler or source-model dependency.
	for(const auto& entry : std::filesystem::recursive_directory_iterator(assetsRoot)) {
		if(!entry.is_regular_file() || entry.path().extension() != ".tecollider") continue;
		std::filesystem::path expectedCooked = std::filesystem::relative(entry.path(), assetsRoot);
		expectedCooked.replace_extension(".tepcoll");
		if(!std::filesystem::exists(compiledRoot / expectedCooked)) {
			AddDiagnostic(result, ExportDiagnosticSeverity::Error, "PhysicsCollider",
				"Source-only collider recipe has no cooked .tepcoll dependency. Run the physics collider cooker before exporting.", entry.path().string());
		}
	}

	if(!std::filesystem::exists(compiledRoot)) return result;
	for(const auto& entry : std::filesystem::recursive_directory_iterator(compiledRoot)) {
		if(entry.is_regular_file() && entry.path().extension() == ".tepcoll") {
			ValidateCookedColliderFile(entry.path(), result);
		}
	}
	return result;
}

// --- Managed source validation ---

ExportValidationResult ExportValidator::ValidateManagedSource(const std::filesystem::path& projectRoot) const {
	ExportValidationResult result;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::recursive_directory_iterator(projectDir)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != ".cs") continue;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!ShouldScanManagedSourceFile(projectRoot, entry.path())) continue;

		scannedFiles++;

		std::ifstream file(entry.path());
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!file) continue;

		const std::string fileText((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		const std::string sanitizedFileText = StripCSharpCommentsAndLiterals(fileText);
		std::istringstream lineStream(sanitizedFileText);
		std::string line;
		int lineNumber = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (std::getline(lineStream, line)) {
			lineNumber++;

			// Check for reflection usage.
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::regex_search(line, reflectionPattern)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"Reflection API usage detected. NativeAOT export does not support runtime reflection.",
						entry.path().string(), lineNumber);
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::regex_search(line, reflectionEmitPattern)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"System.Reflection.Emit usage detected. NativeAOT export requires closed-world code generation and cannot support runtime IL emission.",
					entry.path().string(), lineNumber);
			}

			// Check for dynamic keyword usage.
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ContainsCSharpToken(line, "dynamic")) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"'dynamic' keyword usage detected. NativeAOT export does not support runtime binder generation.",
					entry.path().string(), lineNumber);
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::sregex_iterator matchIt(sanitizedFileText.begin(), sanitizedFileText.end(), serializedFieldPattern), endIt; matchIt != endIt; ++matchIt) {
			const std::smatch& match = *matchIt;
			const std::string typeName = match[1].str();
			const int fieldLine = CalculateLineNumber(sanitizedFileText, static_cast<std::size_t>(match.position(1)));
			const bool obviousCollectionShape = typeName.find('<') != std::string::npos || typeName.find('[') != std::string::npos;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// Searches for the directory containing compiled shaders relative to the project root and current path.
// Input: The project's root path.
// Output: The path to the shaders directory, or an empty path if not found.
static std::filesystem::path FindShadersDirectory(
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& runtimeShaderDirectory) {
	if(!runtimeShaderDirectory.empty()
		&& std::filesystem::exists(runtimeShaderDirectory)
		&& std::filesystem::is_directory(runtimeShaderDirectory)) {
		return std::filesystem::canonical(runtimeShaderDirectory);
	}
	if (!projectRoot.empty()) {
		if (std::filesystem::exists(projectRoot / "Shaders") && std::filesystem::is_directory(projectRoot / "Shaders")) {
			return projectRoot / "Shaders";
		}
		if (std::filesystem::exists(projectRoot / "Build/Editor/Shaders") && std::filesystem::is_directory(projectRoot / "Build/Editor/Shaders")) {
			return projectRoot / "Build/Editor/Shaders";
		}
	}

	// Do not inspect the process working directory. Validation must behave the
	// same on a clean CI runner and a developer tree containing stale builds.
	// Production export supplies its installed shader directory explicitly;
	// isolated tests may provide a project-owned Shaders directory above.
	return {};
}

// Validates all compiled shader outputs, missing compiled asset outputs, and stale .meta/compiled path mismatches.
// Input: The project's root path.
// Output: An ExportValidationResult containing the diagnosed errors/warnings.
static ExportValidationResult ValidateAssetsAndShaders(
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& runtimeShaderDirectory) {
	ExportValidationResult result;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	const std::filesystem::path assetsRoot = projectRoot / "Assets";
	if (!std::filesystem::exists(assetsRoot)) {
		return result;
	}

	// 1. Missing Compiled Shader Outputs Check
	const std::filesystem::path shadersDir = FindShadersDirectory(projectRoot, runtimeShaderDirectory);
	
	static constexpr std::array<std::string_view, 24> kRequiredShaders = {
		"default.task.tspf",
		"default.mesh.tspf",
		"default.frag.tspf",
		"editor/sceneviewgrid.vert.spv",
		"editor/sceneviewgrid.frag.spv",
		"sky/skybackground.task.tspf",
		"sky/skybackground.mesh.tspf",
		"sky/skybackground.frag.tspf",
		"ui/ui_instanced.vert.spv",
		"ui/ui_instanced.frag.spv",
		"ui/ui_static.vert.spv",
		"ui/ui_static.frag.spv",
		"ui/ui_sdf_text.vert.spv",
		"ui/ui_sdf_text.frag.spv",
		"lighting/clusterdepthmask.comp.spv",
		"lighting/clustergrid.comp.spv",
		"lighting/clusterlightcount.comp.spv",
		"lighting/clusterlightfill.comp.spv",
		"lighting/clusterlightprefix.comp.spv",
		"lighting/lightcull.comp.spv",
		"postprocess/hizgenerate.comp.spv",
		"postprocess/vrsgenerate.comp.spv",
		"postprocess/gtao.comp.spv",
		"postprocess/gtaoblur.comp.spv"
	};

	if (!shadersDir.empty()) {
		for (std::string_view requiredShader : kRequiredShaders) {
			std::filesystem::path fullShaderPath = shadersDir / requiredShader;
			if (!std::filesystem::exists(fullShaderPath)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"Shaders", "Missing compiled shader output: " + std::string(requiredShader), fullShaderPath.string());
			}
		}
	}

	// 2. Missing Compiled Asset Outputs and Stale .meta/compiled path mismatches Checks
	const std::filesystem::path buildRoot = projectRoot / "Build" / "Assets";

	if (std::filesystem::exists(assetsRoot)) {
		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetsRoot)) {
			if (entry.is_regular_file() && entry.path().extension() == ".meta") {
				std::filesystem::path metaFilePath = entry.path();
				std::filesystem::path sourceFilePath = metaFilePath;
				sourceFilePath.replace_extension(""); // Remove .meta extension to get source file path

				// Verify if the source file actually exists. If not, it is a stale .meta file (orphaned metadata).
				if (!std::filesystem::exists(sourceFilePath)) {
					AddDiagnostic(result, ExportDiagnosticSeverity::Error,
						"Assets", "Orphaned metadata: Source file does not exist for .meta.", metaFilePath.string());
					continue;
				}

				// Load and parse the .meta JSON file
				std::ifstream file(metaFilePath);
				if (!file.is_open()) {
					AddDiagnostic(result, ExportDiagnosticSeverity::Error,
						"Assets", "Could not open .meta file for validation.", metaFilePath.string());
					continue;
				}

				nlohmann::json j;
				try {
					file >> j;
				} catch (const std::exception& ex) {
					AddDiagnostic(result, ExportDiagnosticSeverity::Error,
						"Assets", "Failed to parse .meta JSON: " + std::string(ex.what()), metaFilePath.string());
					continue;
				}

				std::string importerName = j.value("importerName", "");
				std::string sourcePathStr = j.value("sourcePath", "");
				std::string compiledPathStr = j.value("compiledPath", "");

				// Normalize slash direction to handle Windows/Unix path format differences
				auto normalizePath = [](std::string pathStr) {
					std::replace(pathStr.begin(), pathStr.end(), '\\', '/');
					return pathStr;
				};

				// Check stale source path mismatch in .meta
				std::filesystem::path expectedSourcePath = std::filesystem::relative(sourceFilePath, assetsRoot);
				if (normalizePath(sourcePathStr) != normalizePath(expectedSourcePath.generic_string())) {
					AddDiagnostic(result, ExportDiagnosticSeverity::Error,
						"Assets", "Source path mismatch in metadata. Expected: " + expectedSourcePath.generic_string() + ", Found: " + sourcePathStr, metaFilePath.string());
				}

				// Only perform compiled path mismatch and existence checks for compiling importers
				const bool isCompilingImporter = (importerName == "MeshImporter" ||
												  importerName == "TextureImporter" ||
												  importerName == "ShaderImporter" ||
												  importerName == "MaterialImporter");

				if (isCompilingImporter) {
					std::filesystem::path expectedCompiledPath = expectedSourcePath;
					if (importerName == "MeshImporter") {
						expectedCompiledPath.replace_extension(".temf");
					} else if (importerName == "TextureImporter") {
						expectedCompiledPath.replace_extension(".tetf");
					} else if (importerName == "ShaderImporter") {
						expectedCompiledPath.replace_extension(".tspf");
					} else if (importerName == "MaterialImporter") {
						expectedCompiledPath.replace_extension(".tmat");
					}

					if (normalizePath(compiledPathStr) != normalizePath(expectedCompiledPath.generic_string())) {
						AddDiagnostic(result, ExportDiagnosticSeverity::Error,
							"Assets", "Compiled path mismatch in metadata. Expected: " + expectedCompiledPath.generic_string() + ", Found: " + compiledPathStr, metaFilePath.string());
					}

					// Check missing compiled asset outputs
					if (!compiledPathStr.empty()) {
						std::filesystem::path fullCompiledPath = buildRoot / compiledPathStr;
						if (!std::filesystem::exists(fullCompiledPath)) {
							AddDiagnostic(result, ExportDiagnosticSeverity::Error,
								"Assets", "Missing compiled asset output: " + compiledPathStr, metaFilePath.string());
						}
					}
				}
			}
		}
	}

	return result;
}

// --- Combined validation ---

ExportValidationResult ExportValidator::ValidateAll(
	const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& runtimeShaderDirectory) const {
	ExportValidationResult result = ValidateProfile(profile);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (profile.enableManagedCompilation) {
		auto sourceResult = ValidateManagedSource(projectRoot);
		
		result.errorCount += sourceResult.errorCount;
		result.warningCount += sourceResult.warningCount;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!sourceResult.passed) {
			result.passed = false;
		}
		result.diagnostics.insert(result.diagnostics.end(),
			sourceResult.diagnostics.begin(),
			sourceResult.diagnostics.end());
	}

	auto materialResult = ValidateMaterialAssets(projectRoot);
	result.errorCount += materialResult.errorCount;
	result.warningCount += materialResult.warningCount;
	if (!materialResult.passed) {
		result.passed = false;
	}
	result.materialFallbackDiagnostics.insert(result.materialFallbackDiagnostics.end(),
		materialResult.materialFallbackDiagnostics.begin(),
		materialResult.materialFallbackDiagnostics.end());
	result.diagnostics.insert(result.diagnostics.end(),
		materialResult.diagnostics.begin(),
		materialResult.diagnostics.end());

	// Physics is validated against its cooked runtime representation here, before
	// staging excludes editor-only recipes. This keeps export failures actionable
	// and guarantees the package validator receives only loadable dependencies.
	auto physicsResult = ValidatePhysicsColliderAssets(projectRoot);
	result.errorCount += physicsResult.errorCount;
	result.warningCount += physicsResult.warningCount;
	if(!physicsResult.passed) {
		result.passed = false;
	}
	result.diagnostics.insert(result.diagnostics.end(),
		physicsResult.diagnostics.begin(),
		physicsResult.diagnostics.end());

	// Run shader, asset and .meta mismatch validations
	auto additionalResult = ValidateAssetsAndShaders(projectRoot, runtimeShaderDirectory);
	result.errorCount += additionalResult.errorCount;
	result.warningCount += additionalResult.warningCount;
	if (!additionalResult.passed) {
		result.passed = false;
	}
	result.diagnostics.insert(result.diagnostics.end(),
		additionalResult.diagnostics.begin(),
		additionalResult.diagnostics.end());

	return result;
}
