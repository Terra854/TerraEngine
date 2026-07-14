// ColliderRecipe.cpp - validation, canonical normalization, and stable hashes.

#include "ColliderRecipe.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Physics
{
	namespace
	{
		constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

		[[nodiscard]] ColliderRecipeValidationResult Reject(
			ColliderRecipeValidationCode code, std::string message, std::size_t location = 0U)
		{
			return {false, code, std::move(message), location};
		}

		[[nodiscard]] bool IsEmpty(const TerraEngine::UUID& id)
		{
			return id == TerraEngine::EMPTY_UUID;
		}

		[[nodiscard]] bool IsFinitePositive(float value)
		{
			return std::isfinite(value) && value > 0.0F;
		}

		[[nodiscard]] bool IsUniformPositiveScale(const PhysicsVector3& scale)
		{
			return scale.IsFinite() && scale.x > 0.0F && scale.y > 0.0F && scale.z > 0.0F
				&& scale.x == scale.y && scale.y == scale.z;
		}

		[[nodiscard]] float CanonicalFloat(float value)
		{
			// Negative zero has no geometric meaning but would otherwise fragment
			// equivalent cache entries because its IEEE-754 bit pattern differs.
			return value == 0.0F ? 0.0F : value;
		}

		[[nodiscard]] PhysicsVector3 CanonicalVector(PhysicsVector3 value)
		{
			value.x = CanonicalFloat(value.x);
			value.y = CanonicalFloat(value.y);
			value.z = CanonicalFloat(value.z);
			return value;
		}

		[[nodiscard]] bool VectorLess(const PhysicsVector3& left, const PhysicsVector3& right)
		{
			if(left.x != right.x) return left.x < right.x;
			if(left.y != right.y) return left.y < right.y;
			return left.z < right.z;
		}

		[[nodiscard]] bool VectorEqual(const PhysicsVector3& left, const PhysicsVector3& right)
		{
			return left.x == right.x && left.y == right.y && left.z == right.z;
		}

		[[nodiscard]] PhysicsVector3 Subtract(const PhysicsVector3& left, const PhysicsVector3& right)
		{
			return {left.x - right.x, left.y - right.y, left.z - right.z};
		}

		[[nodiscard]] PhysicsVector3 Cross(const PhysicsVector3& left, const PhysicsVector3& right)
		{
			return {
				left.y * right.z - left.z * right.y,
				left.z * right.x - left.x * right.z,
				left.x * right.y - left.y * right.x};
		}

		[[nodiscard]] float Dot(const PhysicsVector3& left, const PhysicsVector3& right)
		{
			return left.x * right.x + left.y * right.y + left.z * right.z;
		}

		[[nodiscard]] bool IsZeroLength(const PhysicsVector3& value, float epsilon)
		{
			return Dot(value, value) <= epsilon * epsilon;
		}

		[[nodiscard]] bool HasUsableRotation(const PhysicsQuaternion& rotation, float epsilon)
		{
			// Backends normalize rotations. Reject a zero-length quaternion here so
			// that normalization cannot divide by zero or choose provider-specific data.
			const float lengthSquared = rotation.x * rotation.x + rotation.y * rotation.y
				+ rotation.z * rotation.z + rotation.w * rotation.w;
			return std::isfinite(lengthSquared) && lengthSquared > epsilon * epsilon;
		}

		[[nodiscard]] bool IsPointSetThreeDimensional(const std::vector<PhysicsVector3>& vertices, float epsilon)
		{
			if(vertices.size() < 4U) return false;
			const PhysicsVector3 first = vertices.front();
			for(std::size_t secondIndex = 1U; secondIndex < vertices.size(); ++secondIndex)
			{
				const PhysicsVector3 edge = Subtract(vertices[secondIndex], first);
				if(IsZeroLength(edge, epsilon)) continue;
				for(std::size_t thirdIndex = secondIndex + 1U; thirdIndex < vertices.size(); ++thirdIndex)
				{
					const PhysicsVector3 plane = Cross(edge, Subtract(vertices[thirdIndex], first));
					if(IsZeroLength(plane, epsilon)) continue;
					for(std::size_t fourthIndex = thirdIndex + 1U; fourthIndex < vertices.size(); ++fourthIndex)
					{
						if(std::fabs(Dot(plane, Subtract(vertices[fourthIndex], first))) > epsilon)
							return true;
					}
				}
			}
			return false;
		}

		void HashByte(std::uint64_t& hash, std::uint8_t value)
		{
			hash ^= value;
			hash *= kFnvPrime;
		}

		template<typename Value>
		void HashValue(std::uint64_t& hash, Value value)
		{
			static_assert(std::is_trivially_copyable_v<Value>);
			const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(Value)>>(value);
			for(const std::uint8_t byte : bytes) HashByte(hash, byte);
		}

		void HashString(std::uint64_t& hash, std::string_view value)
		{
			HashValue(hash, static_cast<std::uint64_t>(value.size()));
			for(const char character : value) HashByte(hash, static_cast<std::uint8_t>(character));
		}

		void HashUuid(std::uint64_t& hash, const TerraEngine::UUID& value)
		{
			for(const std::uint8_t byte : value) HashByte(hash, byte);
		}

		void HashVector(std::uint64_t& hash, const PhysicsVector3& value)
		{
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(value.x)));
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(value.y)));
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(value.z)));
		}

		void HashPose(std::uint64_t& hash, const PhysicsPose& value)
		{
			HashVector(hash, value.position);
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(value.rotation.x)));
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(value.rotation.y)));
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(value.rotation.z)));
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(value.rotation.w)));
		}

		ColliderRecipeValidationResult ValidateRecipe(
			const ColliderRecipe& recipe,
			const ColliderRecipeLimits& limits,
			std::size_t depth,
			std::unordered_set<const ColliderRecipe*>& activeRecipes)
		{
			if(recipe.schemaVersion != kColliderRecipeSchemaVersion)
				return Reject(ColliderRecipeValidationCode::UnsupportedSchema, "Collider recipe schema requires an explicit migration.");
			if(recipe.kind != ColliderRecipeKind::ConvexHull && recipe.kind != ColliderRecipeKind::TriangleMesh && recipe.kind != ColliderRecipeKind::Compound)
				return Reject(ColliderRecipeValidationCode::UnsupportedKind, "Collider recipe kind is not engine-owned.");
			if(IsEmpty(recipe.provenance.sourceAssetId))
				return Reject(ColliderRecipeValidationCode::MissingSourceAsset, "Collider recipes require a stable source asset GUID.");
			if(recipe.provenance.sourceContentHash == 0U)
				return Reject(ColliderRecipeValidationCode::MissingSourceHash, "Collider recipes require a nonzero immutable source content hash.");
			if(recipe.provenance.selectedSubmesh.size() > limits.maximumSelectedSubmeshLength
				|| recipe.provenance.platformProfile.size() > limits.maximumPlatformProfileLength)
				return Reject(ColliderRecipeValidationCode::InvalidProvenance, "Collider provenance text exceeds its bounded limit.");
			if(!IsUniformPositiveScale(recipe.sourceScale))
				return Reject(ColliderRecipeValidationCode::InvalidScale, "Collider source scale must be finite, positive, and uniform before normalization.");
			if(!IsFinitePositive(recipe.cookSettings.weldTolerance)
				|| !std::isfinite(recipe.cookSettings.simplificationRatio)
				|| recipe.cookSettings.simplificationRatio <= 0.0F || recipe.cookSettings.simplificationRatio > 1.0F
				|| recipe.cookSettings.maximumConvexVertices < 4U
				|| recipe.cookSettings.maximumConvexVertices > limits.maximumVertices)
				return Reject(ColliderRecipeValidationCode::InvalidCookSettings, "Collider cook settings are outside engine-owned safe limits.");
			if(recipe.vertices.size() > limits.maximumVertices)
				return Reject(ColliderRecipeValidationCode::VertexLimitExceeded, "Collider vertex count exceeds the configured limit.");
			if(recipe.indices.size() > limits.maximumIndices)
				return Reject(ColliderRecipeValidationCode::IndexLimitExceeded, "Collider index count exceeds the configured limit.");
			if(recipe.materialSlots.size() > limits.maximumMaterialSlots)
				return Reject(ColliderRecipeValidationCode::MaterialSlotLimitExceeded, "Collider material slot count exceeds the configured limit.");

			std::unordered_set<std::uint32_t> knownMaterialSlots;
			for(const ColliderRecipeMaterialSlot& slot : recipe.materialSlots)
			{
				if(!knownMaterialSlots.insert(slot.sourceMaterialSlot).second)
					return Reject(ColliderRecipeValidationCode::InvalidMaterialMapping, "Collider material slot identifiers must be unique.", slot.sourceMaterialSlot);
			}
			for(std::size_t index = 0U; index < recipe.vertices.size(); ++index)
			{
				const PhysicsVector3& vertex = recipe.vertices[index];
				if(!vertex.IsFinite())
					return Reject(ColliderRecipeValidationCode::NonFiniteGeometry, "Collider vertices must be finite.", index);
				if(std::fabs(vertex.x) > limits.maximumCoordinateMagnitude || std::fabs(vertex.y) > limits.maximumCoordinateMagnitude || std::fabs(vertex.z) > limits.maximumCoordinateMagnitude)
					return Reject(ColliderRecipeValidationCode::CoordinateLimitExceeded, "Collider vertex coordinate exceeds the configured world-safe limit.", index);
			}

			if(recipe.kind == ColliderRecipeKind::ConvexHull)
			{
				if(recipe.vertices.size() < 4U || recipe.vertices.size() > recipe.cookSettings.maximumConvexVertices)
					return Reject(ColliderRecipeValidationCode::InvalidConvexGeometry, "A convex hull needs at least four vertices within its configured convex limit.");
				if(!recipe.indices.empty() || !recipe.triangleMaterialSlots.empty())
					return Reject(ColliderRecipeValidationCode::InvalidConvexGeometry, "A convex hull recipe uses points only; mesh index and triangle material data are forbidden.");
				if(!IsPointSetThreeDimensional(recipe.vertices, limits.degenerateEpsilon))
					return Reject(ColliderRecipeValidationCode::DegenerateGeometry, "Convex points must enclose non-coplanar three-dimensional volume.");
			}
			else if(recipe.kind == ColliderRecipeKind::TriangleMesh)
			{
				if(recipe.vertices.size() < 3U || recipe.indices.size() < 3U || recipe.indices.size() % 3U != 0U)
					return Reject(ColliderRecipeValidationCode::InvalidMeshGeometry, "A triangle mesh needs vertices and an index count divisible by three.");
				const std::size_t triangleCount = recipe.indices.size() / 3U;
				if(!recipe.triangleMaterialSlots.empty() && recipe.triangleMaterialSlots.size() != triangleCount)
					return Reject(ColliderRecipeValidationCode::InvalidMaterialMapping, "Triangle material mapping must be empty or contain exactly one entry per triangle.");
				for(std::size_t index = 0U; index < recipe.indices.size(); ++index)
				{
					if(recipe.indices[index] >= recipe.vertices.size())
						return Reject(ColliderRecipeValidationCode::InvalidIndex, "Triangle mesh index is outside its vertex array.", index);
				}
				for(std::size_t triangle = 0U; triangle < triangleCount; ++triangle)
				{
					const PhysicsVector3& first = recipe.vertices[recipe.indices[triangle * 3U]];
					const PhysicsVector3& second = recipe.vertices[recipe.indices[triangle * 3U + 1U]];
					const PhysicsVector3& third = recipe.vertices[recipe.indices[triangle * 3U + 2U]];
					if(IsZeroLength(Cross(Subtract(second, first), Subtract(third, first)), limits.degenerateEpsilon))
						return Reject(ColliderRecipeValidationCode::DegenerateGeometry, "Triangle mesh contains a zero-area triangle.", triangle);
					if(!recipe.triangleMaterialSlots.empty() && knownMaterialSlots.find(recipe.triangleMaterialSlots[triangle]) == knownMaterialSlots.end())
						return Reject(ColliderRecipeValidationCode::InvalidMaterialMapping, "Triangle material mapping references an unknown material slot.", triangle);
				}
			}
			else
			{
				if(!recipe.vertices.empty() || !recipe.indices.empty() || !recipe.materialSlots.empty() || !recipe.triangleMaterialSlots.empty())
					return Reject(ColliderRecipeValidationCode::InvalidMeshGeometry, "Compound recipes contain only ordered child recipes.");
				if(recipe.compoundChildren.empty())
					return Reject(ColliderRecipeValidationCode::MissingCompoundChild, "A compound recipe needs at least one child.");
				if(recipe.compoundChildren.size() > limits.maximumCompoundChildren)
					return Reject(ColliderRecipeValidationCode::CompoundChildLimitExceeded, "Compound child count exceeds the configured limit.");
				if(!activeRecipes.insert(&recipe).second)
					return Reject(ColliderRecipeValidationCode::CompoundCycle, "Compound recipes may not contain a cycle.");
				if(depth >= limits.maximumCompoundDepth)
				{
					activeRecipes.erase(&recipe);
					return Reject(ColliderRecipeValidationCode::CompoundRecursionLimitExceeded, "Compound recipe nesting exceeds the configured recursion limit.");
				}
				std::unordered_set<TerraEngine::UUID> childIds;
				for(std::size_t childIndex = 0U; childIndex < recipe.compoundChildren.size(); ++childIndex)
				{
					const ColliderRecipeCompoundChild& child = recipe.compoundChildren[childIndex];
					if(IsEmpty(child.childId) || !childIds.insert(child.childId).second)
					{
						activeRecipes.erase(&recipe);
						return Reject(ColliderRecipeValidationCode::DuplicateCompoundChildId, "Compound child IDs must be nonempty and unique.", childIndex);
					}
					if(!child.localPose.IsFinite() || !HasUsableRotation(child.localPose.rotation, limits.degenerateEpsilon))
					{
						activeRecipes.erase(&recipe);
						return Reject(ColliderRecipeValidationCode::InvalidCompoundPose, "Compound child local pose must be finite and use a nonzero rotation quaternion.", childIndex);
					}
					if(!child.recipe)
					{
						activeRecipes.erase(&recipe);
						return Reject(ColliderRecipeValidationCode::MissingCompoundChild, "Compound child has no recipe.", childIndex);
					}
					const ColliderRecipeValidationResult childValidation = ValidateRecipe(*child.recipe, limits, depth + 1U, activeRecipes);
					if(!childValidation.accepted)
					{
						activeRecipes.erase(&recipe);
						return childValidation;
					}
				}
				activeRecipes.erase(&recipe);
			}
			return {true, ColliderRecipeValidationCode::None, {}, 0U};
		}

		void HashRecipe(std::uint64_t& hash, const ColliderRecipe& recipe)
		{
			HashValue(hash, recipe.schemaVersion);
			HashValue(hash, static_cast<std::uint8_t>(recipe.kind));
			HashUuid(hash, recipe.provenance.sourceAssetId);
			HashValue(hash, recipe.provenance.sourceContentHash);
			HashValue(hash, recipe.provenance.settingsHash);
			HashString(hash, recipe.provenance.selectedSubmesh);
			HashString(hash, recipe.provenance.platformProfile);
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(recipe.cookSettings.weldTolerance)));
			HashValue(hash, std::bit_cast<std::uint32_t>(CanonicalFloat(recipe.cookSettings.simplificationRatio)));
			HashValue(hash, recipe.cookSettings.maximumConvexVertices);
			HashValue(hash, recipe.cookSettings.weldVertices);
			HashVector(hash, recipe.sourceScale);
			HashValue(hash, static_cast<std::uint64_t>(recipe.vertices.size()));
			for(const PhysicsVector3& vertex : recipe.vertices) HashVector(hash, vertex);
			HashValue(hash, static_cast<std::uint64_t>(recipe.indices.size()));
			for(const std::uint32_t index : recipe.indices) HashValue(hash, index);
			HashValue(hash, static_cast<std::uint64_t>(recipe.materialSlots.size()));
			for(const ColliderRecipeMaterialSlot& slot : recipe.materialSlots)
			{
				HashValue(hash, slot.sourceMaterialSlot);
				HashUuid(hash, slot.materialAssetId);
			}
			HashValue(hash, static_cast<std::uint64_t>(recipe.triangleMaterialSlots.size()));
			for(const std::uint32_t slot : recipe.triangleMaterialSlots) HashValue(hash, slot);
			HashValue(hash, static_cast<std::uint64_t>(recipe.compoundChildren.size()));
			for(const ColliderRecipeCompoundChild& child : recipe.compoundChildren)
			{
				HashUuid(hash, child.childId);
				HashPose(hash, child.localPose);
				HashRecipe(hash, *child.recipe);
			}
		}

		[[nodiscard]] nlohmann::json ToJsonVector(const PhysicsVector3& value)
		{
			return nlohmann::json::array({value.x, value.y, value.z});
		}

		[[nodiscard]] nlohmann::json ToJsonPose(const PhysicsPose& value)
		{
			return {
				{"position", ToJsonVector(value.position)},
				{"rotation", nlohmann::json::array({value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w})}
			};
		}

		[[nodiscard]] bool ReadVector(const nlohmann::json& value, PhysicsVector3& destination)
		{
			if(!value.is_array() || value.size() != 3U || !value[0].is_number() || !value[1].is_number() || !value[2].is_number()) return false;
			destination = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
			return destination.IsFinite();
		}

		[[nodiscard]] bool ReadPose(const nlohmann::json& value, PhysicsPose& destination)
		{
			if(!value.is_object() || !value.contains("position") || !value.contains("rotation")) return false;
			if(!ReadVector(value["position"], destination.position)) return false;
			const nlohmann::json& rotation = value["rotation"];
			if(!rotation.is_array() || rotation.size() != 4U || !rotation[0].is_number() || !rotation[1].is_number() || !rotation[2].is_number() || !rotation[3].is_number()) return false;
			destination.rotation = {rotation[0].get<float>(), rotation[1].get<float>(), rotation[2].get<float>(), rotation[3].get<float>()};
			return destination.rotation.IsFinite();
		}

		[[nodiscard]] bool ReadUuid(const nlohmann::json& object, std::string_view key, TerraEngine::UUID& destination)
		{
			const auto iterator = object.find(std::string(key));
			if(iterator == object.end() || !iterator->is_string()) return false;
			const auto parsed = TerraEngine::ParseUUID(iterator->get<std::string>());
			if(!parsed) return false;
			destination = *parsed;
			return true;
		}

		[[nodiscard]] bool ParseRecipeJson(const nlohmann::json& source, ColliderRecipe& destination, ColliderRecipeValidationResult& validation, std::size_t depth)
		{
			if(!source.is_object())
			{
				validation = Reject(ColliderRecipeValidationCode::InvalidProvenance, "Collider recipe JSON must be an object.");
				return false;
			}
			if(depth > 64U)
			{
				validation = Reject(ColliderRecipeValidationCode::CompoundRecursionLimitExceeded, "Collider recipe JSON nesting exceeds the parser safety limit.");
				return false;
			}
			ColliderRecipe parsed;
			try
			{
				if(!source.contains("schemaVersion") || !source["schemaVersion"].is_number_unsigned()
					|| !source.contains("kind") || !source["kind"].is_string()) throw std::runtime_error("missing schema or kind");
				parsed.schemaVersion = source["schemaVersion"].get<std::uint32_t>();
				const std::string kind = source["kind"].get<std::string>();
				if(kind == "ConvexHull") parsed.kind = ColliderRecipeKind::ConvexHull;
				else if(kind == "TriangleMesh") parsed.kind = ColliderRecipeKind::TriangleMesh;
				else if(kind == "Compound") parsed.kind = ColliderRecipeKind::Compound;
				else throw std::runtime_error("unknown kind");
				if(!source.contains("provenance") || !source["provenance"].is_object()) throw std::runtime_error("missing provenance");
				const nlohmann::json& provenance = source["provenance"];
				if(!ReadUuid(provenance, "sourceAssetId", parsed.provenance.sourceAssetId)
					|| !provenance.contains("sourceContentHash") || !provenance["sourceContentHash"].is_number_unsigned()) throw std::runtime_error("invalid source identity");
				parsed.provenance.sourceContentHash = provenance["sourceContentHash"].get<std::uint64_t>();
				parsed.provenance.settingsHash = provenance.value("settingsHash", 0ULL);
				parsed.provenance.selectedSubmesh = provenance.value("selectedSubmesh", std::string{});
				parsed.provenance.platformProfile = provenance.value("platformProfile", std::string{});
				if(!source.contains("cookSettings") || !source["cookSettings"].is_object()) throw std::runtime_error("missing cook settings");
				const nlohmann::json& settings = source["cookSettings"];
				parsed.cookSettings.weldTolerance = settings.value("weldTolerance", parsed.cookSettings.weldTolerance);
				parsed.cookSettings.simplificationRatio = settings.value("simplificationRatio", parsed.cookSettings.simplificationRatio);
				parsed.cookSettings.maximumConvexVertices = settings.value("maximumConvexVertices", parsed.cookSettings.maximumConvexVertices);
				parsed.cookSettings.weldVertices = settings.value("weldVertices", parsed.cookSettings.weldVertices);
				if(!source.contains("sourceScale") || !ReadVector(source["sourceScale"], parsed.sourceScale)) throw std::runtime_error("invalid scale");
				if(source.contains("vertices"))
				{
					if(!source["vertices"].is_array()) throw std::runtime_error("invalid vertices");
					for(const nlohmann::json& vertex : source["vertices"])
					{
						PhysicsVector3 point;
						if(!ReadVector(vertex, point)) throw std::runtime_error("invalid vertex");
						parsed.vertices.push_back(point);
					}
				}
				if(source.contains("indices")) parsed.indices = source["indices"].get<std::vector<std::uint32_t>>();
				if(source.contains("materialSlots"))
				{
					if(!source["materialSlots"].is_array()) throw std::runtime_error("invalid material slots");
					for(const nlohmann::json& slot : source["materialSlots"])
					{
						ColliderRecipeMaterialSlot materialSlot;
						if(!slot.is_object() || !slot.contains("sourceMaterialSlot") || !slot["sourceMaterialSlot"].is_number_unsigned()
							|| !ReadUuid(slot, "materialAssetId", materialSlot.materialAssetId)) throw std::runtime_error("invalid material slot");
						materialSlot.sourceMaterialSlot = slot["sourceMaterialSlot"].get<std::uint32_t>();
						parsed.materialSlots.push_back(materialSlot);
					}
				}
				if(source.contains("triangleMaterialSlots")) parsed.triangleMaterialSlots = source["triangleMaterialSlots"].get<std::vector<std::uint32_t>>();
				if(source.contains("compoundChildren"))
				{
					if(!source["compoundChildren"].is_array()) throw std::runtime_error("invalid compound children");
					for(const nlohmann::json& sourceChild : source["compoundChildren"])
					{
						ColliderRecipeCompoundChild child;
						ColliderRecipe childRecipe;
						if(!sourceChild.is_object() || !ReadUuid(sourceChild, "childId", child.childId)
							|| !sourceChild.contains("localPose") || !ReadPose(sourceChild["localPose"], child.localPose)
							|| !sourceChild.contains("recipe") || !ParseRecipeJson(sourceChild["recipe"], childRecipe, validation, depth + 1U))
							throw std::runtime_error("invalid compound child");
						child.recipe = std::make_shared<const ColliderRecipe>(std::move(childRecipe));
						parsed.compoundChildren.push_back(std::move(child));
					}
				}
			}
			catch(const std::exception&)
			{
				if(validation.accepted || validation.code == ColliderRecipeValidationCode::None)
					validation = Reject(ColliderRecipeValidationCode::InvalidProvenance, "Collider recipe JSON contains a missing or incorrectly typed engine-owned field.");
				return false;
			}
			destination = std::move(parsed);
			return true;
		}
	}

	ColliderRecipeValidationResult ColliderRecipe::Validate(const ColliderRecipeLimits& limits) const
	{
		if(!IsFinitePositive(limits.maximumCoordinateMagnitude) || !IsFinitePositive(limits.degenerateEpsilon)
			|| limits.maximumCompoundDepth == 0U)
			return Reject(ColliderRecipeValidationCode::InvalidCookSettings, "Collider recipe limits must be finite, positive, and permit at least one nesting level.");
		std::unordered_set<const ColliderRecipe*> activeRecipes;
		return ValidateRecipe(*this, limits, 0U, activeRecipes);
	}

	bool ColliderRecipe::TryNormalize(ColliderRecipe& normalized, ColliderRecipeValidationResult& validation, const ColliderRecipeLimits& limits) const
	{
		validation = Validate(limits);
		if(!validation.accepted) return false;
		normalized = *this;
		const float uniformScale = sourceScale.x;
		normalized.sourceScale = {1.0F, 1.0F, 1.0F};
		for(PhysicsVector3& vertex : normalized.vertices)
			vertex = CanonicalVector({vertex.x * uniformScale, vertex.y * uniformScale, vertex.z * uniformScale});
		if(normalized.kind == ColliderRecipeKind::ConvexHull)
		{
			std::sort(normalized.vertices.begin(), normalized.vertices.end(), VectorLess);
			normalized.vertices.erase(std::unique(normalized.vertices.begin(), normalized.vertices.end(), VectorEqual), normalized.vertices.end());
		}
		for(ColliderRecipeCompoundChild& child : normalized.compoundChildren)
		{
			ColliderRecipe childNormalized;
			ColliderRecipeValidationResult childValidation;
			if(!child.recipe->TryNormalize(childNormalized, childValidation, limits))
			{
				validation = std::move(childValidation);
				return false;
			}
			child.recipe = std::make_shared<const ColliderRecipe>(std::move(childNormalized));
		}
		validation = normalized.Validate(limits);
		return validation.accepted;
	}

	nlohmann::json ColliderRecipe::ToJson() const
	{
		// Do not serialize an invalid or cyclic public record. A caller must repair
		// it first rather than emitting a JSON file another tool cannot load safely.
		if(!Validate().accepted) return nlohmann::json();
		nlohmann::json result = {
			{"schemaVersion", schemaVersion},
			{"kind", ToString(kind)},
			{"provenance", {
				{"sourceAssetId", TerraEngine::ToHexString(provenance.sourceAssetId)},
				{"sourceContentHash", provenance.sourceContentHash},
				{"settingsHash", provenance.settingsHash},
				{"selectedSubmesh", provenance.selectedSubmesh},
				{"platformProfile", provenance.platformProfile}}},
			{"cookSettings", {
				{"weldTolerance", cookSettings.weldTolerance},
				{"simplificationRatio", cookSettings.simplificationRatio},
				{"maximumConvexVertices", cookSettings.maximumConvexVertices},
				{"weldVertices", cookSettings.weldVertices}}},
			{"sourceScale", ToJsonVector(sourceScale)},
			{"vertices", nlohmann::json::array()},
			{"indices", indices},
			{"materialSlots", nlohmann::json::array()},
			{"triangleMaterialSlots", triangleMaterialSlots},
			{"compoundChildren", nlohmann::json::array()}
		};
		for(const PhysicsVector3& vertex : vertices) result["vertices"].push_back(ToJsonVector(vertex));
		for(const ColliderRecipeMaterialSlot& slot : materialSlots)
			result["materialSlots"].push_back({{"sourceMaterialSlot", slot.sourceMaterialSlot}, {"materialAssetId", TerraEngine::ToHexString(slot.materialAssetId)}});
		for(const ColliderRecipeCompoundChild& child : compoundChildren)
		{
			result["compoundChildren"].push_back({
				{"childId", TerraEngine::ToHexString(child.childId)},
				{"localPose", ToJsonPose(child.localPose)},
				{"recipe", child.recipe->ToJson()}});
		}
		return result;
	}

	bool ColliderRecipe::LoadJson(
		const nlohmann::json& source,
		ColliderRecipe& destination,
		ColliderRecipeValidationResult& validation,
		const ColliderRecipeLimits& limits)
	{
		ColliderRecipe parsed;
		validation = {false, ColliderRecipeValidationCode::None, {}, 0U};
		if(!ParseRecipeJson(source, parsed, validation, 0U)) return false;
		validation = parsed.Validate(limits);
		if(!validation.accepted) return false;
		destination = std::move(parsed);
		return true;
	}

	std::uint64_t ColliderRecipe::StableHash(const ColliderRecipeLimits& limits) const
	{
		ColliderRecipe normalized;
		ColliderRecipeValidationResult validation;
		if(!TryNormalize(normalized, validation, limits)) return 0U;
		std::uint64_t hash = kFnvOffsetBasis;
		HashRecipe(hash, normalized);
		return hash == 0U ? 1U : hash;
	}

	std::string_view ToString(ColliderRecipeKind kind)
	{
		switch(kind)
		{
		case ColliderRecipeKind::ConvexHull: return "ConvexHull";
		case ColliderRecipeKind::TriangleMesh: return "TriangleMesh";
		case ColliderRecipeKind::Compound: return "Compound";
		}
		return "Unknown";
	}

	std::string_view ToString(ColliderRecipeValidationCode code)
	{
		switch(code)
		{
		case ColliderRecipeValidationCode::None: return "None";
		case ColliderRecipeValidationCode::UnsupportedSchema: return "UnsupportedSchema";
		case ColliderRecipeValidationCode::UnsupportedKind: return "UnsupportedKind";
		case ColliderRecipeValidationCode::MissingSourceAsset: return "MissingSourceAsset";
		case ColliderRecipeValidationCode::MissingSourceHash: return "MissingSourceHash";
		case ColliderRecipeValidationCode::InvalidProvenance: return "InvalidProvenance";
		case ColliderRecipeValidationCode::InvalidScale: return "InvalidScale";
		case ColliderRecipeValidationCode::NonFiniteGeometry: return "NonFiniteGeometry";
		case ColliderRecipeValidationCode::CoordinateLimitExceeded: return "CoordinateLimitExceeded";
		case ColliderRecipeValidationCode::VertexLimitExceeded: return "VertexLimitExceeded";
		case ColliderRecipeValidationCode::IndexLimitExceeded: return "IndexLimitExceeded";
		case ColliderRecipeValidationCode::MaterialSlotLimitExceeded: return "MaterialSlotLimitExceeded";
		case ColliderRecipeValidationCode::CompoundChildLimitExceeded: return "CompoundChildLimitExceeded";
		case ColliderRecipeValidationCode::CompoundRecursionLimitExceeded: return "CompoundRecursionLimitExceeded";
		case ColliderRecipeValidationCode::CompoundCycle: return "CompoundCycle";
		case ColliderRecipeValidationCode::MissingCompoundChild: return "MissingCompoundChild";
		case ColliderRecipeValidationCode::DuplicateCompoundChildId: return "DuplicateCompoundChildId";
		case ColliderRecipeValidationCode::InvalidCompoundPose: return "InvalidCompoundPose";
		case ColliderRecipeValidationCode::InvalidConvexGeometry: return "InvalidConvexGeometry";
		case ColliderRecipeValidationCode::InvalidMeshGeometry: return "InvalidMeshGeometry";
		case ColliderRecipeValidationCode::DegenerateGeometry: return "DegenerateGeometry";
		case ColliderRecipeValidationCode::InvalidIndex: return "InvalidIndex";
		case ColliderRecipeValidationCode::InvalidMaterialMapping: return "InvalidMaterialMapping";
		case ColliderRecipeValidationCode::InvalidCookSettings: return "InvalidCookSettings";
		}
		return "Unknown";
	}
}
