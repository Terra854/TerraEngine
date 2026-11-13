// Validation-only deterministic dense baseline baker. Prerequisite: bundled
// MeshCompiler and its normal bundled codec/Assimp closure. Invoke with one output
// path; parent CMake must compare two independent bakes and pin the resulting hash.
#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Tests/Support/ReviewRemediation/AnimatedConsumerCompanion.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

int Bake(const std::filesystem::path& outputRoot) {
	// Companion imports/cooks actual assets before mesh cooking, so no fabricated
	// skeleton fingerprint can accidentally qualify the mounted renderer fixture.
	TerraEngine::Tests::Review::AnimatedConsumerCompanion companion; std::string diagnostic;
	std::filesystem::create_directories(outputRoot);
	if(TerraEngine::Tests::Review::BakeAnimatedConsumerCompanion(outputRoot,companion,diagnostic)!=TerraEngine::Animation::AnimationStatus::Ok) { std::cerr<<diagnostic<<'\n';return 1; }
	if(companion.skeleton.joints.size()!=1U || companion.skeleton.compatibilityFingerprint==0U) return 1;
	const auto output=outputRoot/"TerraEngineProductionShapeFixture.temf";
	SkinnedMeshCookInput input;
	constexpr unsigned width = 50U, height = 57U;
	for(unsigned y = 0; y <= height; ++y) for(unsigned x = 0; x <= width; ++x) {
		const float fx = static_cast<float>(x) / width, fy = static_cast<float>(y) / height;
		SkinnedMeshCookVertex vertex; vertex.baseVertex.position = {fx * 2.0F - 1.0F, fy * 2.0F - 1.0F, 0.05F * std::sin(fx * 12.0F) * std::cos(fy * 10.0F)};
		vertex.baseVertex.normal = {0.0F,0.0F,1.0F}; vertex.baseVertex.uv = {fx,fy}; vertex.influences = {{0U,1.0F}}; input.vertices.push_back(std::move(vertex));
	}
	for(unsigned y = 0; y < height; ++y) for(unsigned x = 0; x < width; ++x) {
		const unsigned a = y * (width + 1U) + x, b = a + 1U, c = a + width + 1U, d = c + 1U;
		input.indices.insert(input.indices.end(), {a,c,b,b,c,d});
	}
	SkinnedMeshMorphTarget morph; morph.stableTargetId = 1U; morph.positionDeltas.resize(input.vertices.size(), {0.0F,0.0F,0.01F}); input.morphTargets.push_back(std::move(morph));
	SkinnedMeshCookSettings settings; settings.sourceSkeletonJointCount = static_cast<std::uint32_t>(companion.skeleton.joints.size()); settings.skeletonCompatibilityFingerprint = companion.skeleton.compatibilityFingerprint;
	SkinnedMeshCookReport report;
	std::filesystem::create_directories(output.parent_path());
	if(!CompileSkinnedMeshFromRaw("Review/MalformedProductionShapeBaseline", output, input, &report, settings)) { std::cerr << report.diagnosticMessage << '\n'; return 1; }
	std::ofstream manifest(outputRoot/"TerraEngineProductionShapeFixture.identity");
	manifest<<"skeletonCompatibilityFingerprint "<<settings.skeletonCompatibilityFingerprint<<'\n'
		<<"sourceSkeletonJointCount "<<settings.sourceSkeletonJointCount<<'\n'
		<<"skeleton "<<companion.skeletonPath.generic_string()<<'\n'<<"clip "<<companion.clipPath.generic_string()<<'\n';
	if(!manifest.good()) return 1;
	return 0;
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) { return argc==2 ? Bake(std::filesystem::path(argv[1])) : 2; }
#else
int main(int argc,char** argv) { return argc==2 ? Bake(std::filesystem::u8path(argv[1])) : 2; }
#endif
