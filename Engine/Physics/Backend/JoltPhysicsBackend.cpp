// JoltPhysicsBackend.cpp - contained Jolt Physics 5.6 implementation.

#include "JoltPhysicsBackend.h"

#include "PhysicsJobAdapter.h"
#include "Physics/PhysicsCommands.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/GearConstraint.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cfloat>
#include <future>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace TerraEngine::Physics::Backend {
	namespace {
		constexpr std::uint32_t kJoltObjectLayerCount = 32U;
		constexpr std::size_t kMaximumCookedPayloadBytes = 64U * 1024U * 1024U;
		constexpr std::size_t kMaximumCookVertices = 4U * 1024U * 1024U;
		constexpr std::size_t kMaximumCookTriangles = 4U * 1024U * 1024U;
		constexpr std::string_view kJoltCookFormatFamily = "TerraEngine.Jolt.ShapeWithChildren";
		constexpr std::string_view kJoltProviderName = "Jolt";
		constexpr std::string_view kJoltProviderVersion = "5.6.0";
		constexpr std::string_view kJoltProviderRevision = "e77f175595e64cb44218cc9d9d56fc365ad0e36a";
		constexpr std::uint32_t kJoltCookFormatVersion = 1U;

		PhysicsBackendResult Success(std::string message = {}) {
			return {PhysicsResultStatus::Success, PhysicsBackendErrorCode::None, std::move(message)};
		}

		PhysicsBackendResult Rejected(PhysicsBackendErrorCode error, std::string message) {
			return {PhysicsResultStatus::Rejected, error, std::move(message)};
		}

		PhysicsBackendResult ProviderFailure(std::string message, bool fatal = false) {
			return {
				fatal ? PhysicsResultStatus::InternalFailure : PhysicsResultStatus::ProviderFailure,
				fatal ? PhysicsBackendErrorCode::FatalProviderFailure : PhysicsBackendErrorCode::ProviderFailure,
				std::move(message)};
		}

		JPH::Vec3 ToJoltVector(const PhysicsVector3& value) {
			return JPH::Vec3(value.x, value.y, value.z);
		}

		JPH::RVec3 ToJoltPosition(const PhysicsVector3& value) {
			return JPH::RVec3(value.x, value.y, value.z);
		}

		JPH::Quat ToJoltRotation(const PhysicsQuaternion& value) {
			return JPH::Quat(value.x, value.y, value.z, value.w).Normalized();
		}

		PhysicsVector3 FromJoltVector(JPH::Vec3Arg value) {
			return {value.GetX(), value.GetY(), value.GetZ()};
		}

		PhysicsVector3 FromJoltPosition(JPH::RVec3Arg value) {
			return {
				static_cast<float>(value.GetX()),
				static_cast<float>(value.GetY()),
				static_cast<float>(value.GetZ())};
		}

		PhysicsQuaternion FromJoltRotation(JPH::QuatArg value) {
			return {value.GetX(), value.GetY(), value.GetZ(), value.GetW()};
		}

		JPH::EMotionType ToJoltMotionType(PhysicsMotionType type) {
			switch(type) {
			case PhysicsMotionType::Static: return JPH::EMotionType::Static;
			case PhysicsMotionType::Dynamic: return JPH::EMotionType::Dynamic;
			case PhysicsMotionType::Kinematic: return JPH::EMotionType::Kinematic;
			}
			return JPH::EMotionType::Static;
		}

		JPH::EMotionQuality ToJoltMotionQuality(PhysicsMotionQuality quality) {
			return quality == PhysicsMotionQuality::LinearCast
				? JPH::EMotionQuality::LinearCast
				: JPH::EMotionQuality::Discrete;
		}

		bool IsUsableRotation(const PhysicsQuaternion& rotation) {
			if(!rotation.IsFinite()) return false;
			const float lengthSquared = rotation.x * rotation.x + rotation.y * rotation.y
				+ rotation.z * rotation.z + rotation.w * rotation.w;
			return lengthSquared > 1.0e-8F;
		}

		bool IsZero(const PhysicsVector3& value) {
			return value.x == 0.0F && value.y == 0.0F && value.z == 0.0F;
		}

		bool IsIdentity(const PhysicsQuaternion& value) {
			return value.x == 0.0F && value.y == 0.0F && value.z == 0.0F && value.w == 1.0F;
		}

		float Magnitude(JPH::Vec3Arg value) {
			return std::sqrt(value.LengthSq());
		}

		float Magnitude(JPH::Vector<2> value) {
			return std::sqrt(value[0] * value[0] + value[1] * value[1]);
		}

		bool IsUsableAxis(const PhysicsVector3& axis) {
			return axis.IsFinite() && axis.x * axis.x + axis.y * axis.y + axis.z * axis.z > 1.0e-8F;
		}

		JPH::Vec3 ToNormalizedJoltAxis(const PhysicsVector3& axis) {
			return ToJoltVector(axis).Normalized();
		}

		JPH::EMotorState ToJoltMotorState(PhysicsBackendMotorMode mode) {
			switch(mode) {
			case PhysicsBackendMotorMode::Velocity: return JPH::EMotorState::Velocity;
			case PhysicsBackendMotorMode::Position: return JPH::EMotorState::Position;
			case PhysicsBackendMotorMode::PositionAndVelocity: return JPH::EMotorState::PositionAndVelocity;
			case PhysicsBackendMotorMode::Off: break;
			}
			return JPH::EMotorState::Off;
		}

		JPH::SpringSettings ToJoltSpring(const PhysicsConstraintAxisSettings& axis) {
			return JPH::SpringSettings(axis.springUsesFrequency
				? JPH::ESpringMode::FrequencyAndDamping
				: JPH::ESpringMode::StiffnessAndDamping,
				axis.springEnabled ? axis.springFrequency : 0.0F,
				axis.springEnabled ? axis.springDamping : 0.0F);
		}

		void ConfigureJoltMotor(
			JPH::MotorSettings& motor,
			const PhysicsConstraintAxisSettings& axis,
			bool angular) {
			motor.mSpringSettings = JPH::SpringSettings(axis.springUsesFrequency
				? JPH::ESpringMode::FrequencyAndDamping
				: JPH::ESpringMode::StiffnessAndDamping,
				std::max(0.0F, axis.springFrequency), std::max(0.0F, axis.springDamping));
			if(std::isfinite(axis.motorMaximumForce) && axis.motorMaximumForce > 0.0F) {
				if(angular) motor.SetTorqueLimit(axis.motorMaximumForce);
				else motor.SetForceLimit(axis.motorMaximumForce);
			}
		}

		JPH::ShapeSettings::ShapeResult ShapeError(const char* message) {
			JPH::ShapeSettings::ShapeResult result;
			result.SetError(message);
			return result;
		}

		std::uint64_t CookedPayloadChecksum(const std::vector<std::byte>& payload) {
			std::uint64_t checksum = 14695981039346656037ULL;
			for(const std::byte value : payload) {
				checksum ^= static_cast<std::uint8_t>(value);
				checksum *= 1099511628211ULL;
			}
			return checksum;
		}

		bool ValidCookedPayloadMetadata(const PhysicsShapeRecipe& recipe) {
			return recipe.cookedPayload != nullptr && !recipe.cookedPayload->empty()
				&& recipe.cookedPayload->size() <= kMaximumCookedPayloadBytes
				&& recipe.cookedFormatFamily == kJoltCookFormatFamily
				&& recipe.cookedProvider == kJoltProviderName
				&& recipe.cookedProviderVersion == kJoltProviderVersion
				&& recipe.cookedProviderRevision == kJoltProviderRevision
				&& recipe.cookedFormatVersion == kJoltCookFormatVersion
				&& recipe.cookedPayloadChecksum != 0U
				&& recipe.cookedPayloadChecksum == CookedPayloadChecksum(*recipe.cookedPayload);
		}

		bool ValidShapeDimensions(const PhysicsShapeRecipe& recipe, float maximumExtent) {
			if(!recipe.dimensions.IsFinite() || !recipe.localCenter.IsFinite()
				|| !IsUsableRotation(recipe.localRotation)) {
				return false;
			}
			// Runtime packages may omit source vertices entirely. Exact compatibility
			// metadata and the envelope checksum are validated before this payload is
			// restored, so source geometry is not required for a cooked recipe.
			if(recipe.cookedPayload != nullptr && !recipe.cookedPayload->empty()) {
				return ValidCookedPayloadMetadata(recipe);
			}
			const auto positiveBounded = [maximumExtent](float value) {
				return std::isfinite(value) && value > 0.0F && value <= maximumExtent;
			};
			if(recipe.kind == PhysicsShapeKind::Compound) {
				return !recipe.children.empty() && recipe.children.size() <= 1024U;
			}
			switch(recipe.kind) {
			case PhysicsShapeKind::Box:
				return positiveBounded(recipe.dimensions.x)
					&& positiveBounded(recipe.dimensions.y)
					&& positiveBounded(recipe.dimensions.z);
			case PhysicsShapeKind::Sphere:
				return positiveBounded(recipe.dimensions.x);
			case PhysicsShapeKind::Capsule:
			case PhysicsShapeKind::Cylinder:
				return positiveBounded(recipe.dimensions.x)
					&& positiveBounded(recipe.dimensions.y)
					&& recipe.dimensions.y >= 2.0F * recipe.dimensions.x;
			case PhysicsShapeKind::TaperedCapsule:
				return positiveBounded(recipe.dimensions.x)
					&& positiveBounded(recipe.dimensions.y)
					&& positiveBounded(recipe.dimensions.z)
					&& recipe.dimensions.y >= recipe.dimensions.x + recipe.dimensions.z;
			case PhysicsShapeKind::ConvexHull:
				return recipe.points.size() >= 4U
					&& recipe.points.size() <= kMaximumCookVertices;
			case PhysicsShapeKind::Mesh:
				return !recipe.points.empty()
					&& recipe.points.size() <= kMaximumCookVertices
					&& recipe.indices.size() >= 3U
					&& recipe.indices.size() % 3U == 0U
					&& recipe.indices.size() / 3U <= kMaximumCookTriangles;
			case PhysicsShapeKind::Compound:
				return true;
			}
			return false;
		}

		JPH::ShapeSettings::ShapeResult CreateRecipeShape(
			const PhysicsShapeRecipe& recipe,
			float maximumExtent,
			std::uint32_t depth,
			bool applyLocalTransform = true) {
			if(depth > 16U || !ValidShapeDimensions(recipe, maximumExtent)) {
				return ShapeError("The collider recipe has invalid dimensions, transform, or nesting depth.");
			}

			JPH::ShapeSettings::ShapeResult result;
			if(recipe.cookedPayload != nullptr && !recipe.cookedPayload->empty()) {
				// ShapeWithChildren is deliberately opaque to TerraEngine. Jolt restores
				// the provider graph while engine-owned metadata controls compatibility,
				// cache identity, scene-local placement, and subshape routing.
				std::string serialized(recipe.cookedPayload->size(), '\0');
				std::memcpy(serialized.data(), recipe.cookedPayload->data(), recipe.cookedPayload->size());
				std::istringstream bytes(serialized, std::ios::binary | std::ios::in);
				JPH::StreamInWrapper stream(bytes);
				JPH::Shape::IDToShapeMap shapes;
				JPH::Shape::IDToMaterialMap materials;
				result = JPH::Shape::sRestoreWithChildren(stream, shapes, materials);
				if(stream.IsFailed()) {
					return ShapeError("Jolt rejected a truncated or corrupt cooked shape payload.");
				}
			} else switch(recipe.kind) {
			case PhysicsShapeKind::Box:
				result = JPH::BoxShapeSettings(ToJoltVector(recipe.dimensions) * 0.5F).Create();
				break;
			case PhysicsShapeKind::Sphere:
				result = JPH::SphereShapeSettings(recipe.dimensions.x).Create();
				break;
			case PhysicsShapeKind::Capsule:
				result = JPH::CapsuleShapeSettings(
					0.5F * recipe.dimensions.y - recipe.dimensions.x,
					recipe.dimensions.x).Create();
				break;
			case PhysicsShapeKind::Cylinder:
				result = JPH::CylinderShapeSettings(
					0.5F * recipe.dimensions.y, recipe.dimensions.x).Create();
				break;
			case PhysicsShapeKind::TaperedCapsule:
				result = JPH::TaperedCapsuleShapeSettings(
					0.5F * (recipe.dimensions.y - recipe.dimensions.x - recipe.dimensions.z),
					recipe.dimensions.x, recipe.dimensions.z).Create();
				break;
			case PhysicsShapeKind::ConvexHull: {
				JPH::Array<JPH::Vec3> points;
				points.reserve(recipe.points.size());
				for(const PhysicsVector3& point : recipe.points) {
					if(!point.IsFinite()
						|| std::abs(point.x) > maximumExtent
						|| std::abs(point.y) > maximumExtent
						|| std::abs(point.z) > maximumExtent) {
						return ShapeError("A convex collider point is nonfinite or exceeds the configured extent.");
					}
					points.push_back(ToJoltVector(point));
				}
				result = JPH::ConvexHullShapeSettings(points).Create();
				break;
			}
			case PhysicsShapeKind::Mesh: {
				JPH::VertexList vertices;
				vertices.reserve(recipe.points.size());
				for(const PhysicsVector3& point : recipe.points) {
					if(!point.IsFinite()
						|| std::abs(point.x) > maximumExtent
						|| std::abs(point.y) > maximumExtent
						|| std::abs(point.z) > maximumExtent) {
						return ShapeError("A mesh collider vertex is nonfinite or exceeds the configured extent.");
					}
					vertices.emplace_back(point.x, point.y, point.z);
				}

				JPH::IndexedTriangleList triangles;
				triangles.reserve(recipe.indices.size() / 3U);
				for(std::size_t index = 0U; index < recipe.indices.size(); index += 3U) {
					const std::uint32_t first = recipe.indices[index];
					const std::uint32_t second = recipe.indices[index + 1U];
					const std::uint32_t third = recipe.indices[index + 2U];
					// Jolt's MeshShapeSettings constructor sanitizes immediately, so every
					// index must be checked before constructing it to prevent an OOB read.
					if(first >= vertices.size() || second >= vertices.size() || third >= vertices.size()) {
						return ShapeError("A mesh collider triangle references an out-of-range vertex.");
					}
					const PhysicsVector3& firstPoint = recipe.points[first];
					const PhysicsVector3& secondPoint = recipe.points[second];
					const PhysicsVector3& thirdPoint = recipe.points[third];
					const PhysicsVector3 firstEdge{
						secondPoint.x - firstPoint.x,
						secondPoint.y - firstPoint.y,
						secondPoint.z - firstPoint.z};
					const PhysicsVector3 secondEdge{
						thirdPoint.x - firstPoint.x,
						thirdPoint.y - firstPoint.y,
						thirdPoint.z - firstPoint.z};
					const PhysicsVector3 cross{
						firstEdge.y * secondEdge.z - firstEdge.z * secondEdge.y,
						firstEdge.z * secondEdge.x - firstEdge.x * secondEdge.z,
						firstEdge.x * secondEdge.y - firstEdge.y * secondEdge.x};
					const float doubledAreaSquared = cross.x * cross.x + cross.y * cross.y + cross.z * cross.z;
					if(!std::isfinite(doubledAreaSquared) || doubledAreaSquared <= 1.0e-12F) {
						return ShapeError("A mesh collider contains a zero-area triangle.");
					}
					const JPH::IndexedTriangle triangle(first, second, third, 0U,
						static_cast<JPH::uint>(index / 3U));
					if(triangle.IsDegenerate(vertices)) {
						return ShapeError("A mesh collider contains a degenerate triangle.");
					}
					triangles.push_back(triangle);
				}
				result = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
				break;
			}
			case PhysicsShapeKind::Compound: {
				JPH::StaticCompoundShapeSettings compound;
				for(const PhysicsShapeRecipe& child : recipe.children) {
					JPH::ShapeSettings::ShapeResult childResult = CreateRecipeShape(
						child, maximumExtent, depth + 1U, false);
					if(!childResult.IsValid()) return childResult;
					compound.AddShape(
						ToJoltVector(child.localCenter),
						ToJoltRotation(child.localRotation),
						childResult.Get().GetPtr(),
						child.subShapeKey);
				}
				result = compound.Create();
				break;
			}
			}

			if(!result.IsValid() || !applyLocalTransform
				|| (IsZero(recipe.localCenter) && IsIdentity(recipe.localRotation))) {
				if(result.IsValid()) {
					result.Get()->SetUserData((recipe.sensor ? (1ULL << 63U) : 0ULL)
						| static_cast<std::uint64_t>(recipe.subShapeKey));
				}
				return result;
			}
			result.Get()->SetUserData((recipe.sensor ? (1ULL << 63U) : 0ULL)
				| static_cast<std::uint64_t>(recipe.subShapeKey));
			JPH::ShapeSettings::ShapeResult transformed = JPH::RotatedTranslatedShapeSettings(
				ToJoltVector(recipe.localCenter),
				ToJoltRotation(recipe.localRotation),
				result.Get().GetPtr()).Create();
			if(transformed.IsValid()) transformed.Get()->SetUserData(result.Get()->GetUserData());
			return transformed;
		}

		std::uint64_t EncodeCollisionMetadata(
			std::uint8_t layer,
			std::uint32_t mask,
			PhysicsCombineMode friction,
			PhysicsCombineMode restitution) {
			return static_cast<std::uint64_t>(mask) << 32U
				| static_cast<std::uint64_t>(layer)
				| (static_cast<std::uint64_t>(friction) << 8U)
				| (static_cast<std::uint64_t>(restitution) << 10U);
		}

		PhysicsCombineMode DecodeCombineMode(std::uint64_t metadata, std::uint32_t shift) {
			return static_cast<PhysicsCombineMode>((metadata >> shift) & 0x3U);
		}

		float CombineMaterialValue(float left, float right, PhysicsCombineMode mode) {
			switch(mode) {
			case PhysicsCombineMode::Average: return 0.5F * (left + right);
			case PhysicsCombineMode::Minimum: return std::min(left, right);
			case PhysicsCombineMode::Multiply: return left * right;
			case PhysicsCombineMode::Maximum: return std::max(left, right);
			}
			return 0.5F * (left + right);
		}

		enum class RawPhysicsEventKind : std::uint8_t {
			Added,
			Persisted,
			Removed,
			Activated,
			Deactivated
		};

		// Provider callbacks may execute simultaneously on Jolt worker jobs. Raw
		// records contain only copyable provider IDs and numeric contact data; engine
		// object generations are resolved after Update has joined every worker.
		struct RawPhysicsEvent {
			RawPhysicsEventKind kind = RawPhysicsEventKind::Added;
			JPH::BodyID firstBody;
			JPH::BodyID secondBody;
			JPH::SubShapeID firstSubShape;
			JPH::SubShapeID secondSubShape;
			JPH::RVec3 point;
			JPH::Vec3 normal;
			JPH::Vec3 relativeVelocity;
			float penetration = 0.0F;
			PhysicsTick tick = 0U;
			bool sensor = false;
		};

		class TerraContactListener final
			: public JPH::ContactListener,
			  public JPH::BodyActivationListener {
		public:
			void Configure(std::size_t maximumEvents) {
				maximumEventsPerShard = std::max<std::size_t>(1U, maximumEvents / shards.size());
				maximumContactsPerShard = maximumEventsPerShard;
			}

			void BeginTick(PhysicsTick tick) noexcept {
				currentTick.store(tick, std::memory_order_release);
			}

			std::vector<RawPhysicsEvent> Drain(std::uint64_t& dropped) {
				std::vector<RawPhysicsEvent> result;
				for(Shard& shard : shards) {
					std::lock_guard lock(shard.mutex);
					result.insert(result.end(),
						std::make_move_iterator(shard.events.begin()),
						std::make_move_iterator(shard.events.end()));
					shard.events.clear();
				}
				dropped = droppedCount.exchange(0U, std::memory_order_acq_rel);
				return result;
			}

			void Clear() {
				for(Shard& shard : shards) {
					std::lock_guard lock(shard.mutex);
					shard.events.clear();
					shard.activeContacts.clear();
				}
				currentTick.store(0U, std::memory_order_release);
				droppedCount.store(0U, std::memory_order_release);
			}

			JPH::ValidateResult OnContactValidate(
				const JPH::Body& first,
				const JPH::Body& second,
				JPH::RVec3Arg,
				const JPH::CollideShapeResult&) override {
				const std::uint64_t firstMetadata = first.GetUserData();
				const std::uint64_t secondMetadata = second.GetUserData();
				const std::uint32_t firstMask = static_cast<std::uint32_t>(firstMetadata >> 32U);
				const std::uint32_t secondMask = static_cast<std::uint32_t>(secondMetadata >> 32U);
				const std::uint32_t firstLayer = static_cast<std::uint32_t>(firstMetadata & 0x1FU);
				const std::uint32_t secondLayer = static_cast<std::uint32_t>(secondMetadata & 0x1FU);
				return PhysicsSettings::LayersCanCollide(
					static_cast<std::uint8_t>(firstLayer), firstMask,
					static_cast<std::uint8_t>(secondLayer), secondMask)
					? JPH::ValidateResult::AcceptAllContactsForThisBodyPair
					: JPH::ValidateResult::RejectAllContactsForThisBodyPair;
			}

			void OnContactAdded(
				const JPH::Body& first,
				const JPH::Body& second,
				const JPH::ContactManifold& manifold,
				JPH::ContactSettings& settings) override {
				ApplyMaterial(first, second, manifold, settings);
				RecordContact(RawPhysicsEventKind::Added, first, second, manifold, settings.mIsSensor);
			}

			void OnContactPersisted(
				const JPH::Body& first,
				const JPH::Body& second,
				const JPH::ContactManifold& manifold,
				JPH::ContactSettings& settings) override {
				ApplyMaterial(first, second, manifold, settings);
				RecordContact(RawPhysicsEventKind::Persisted, first, second, manifold, settings.mIsSensor);
			}

			void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
				Shard& shard = shards[ShardIndex(pair.GetHash())];
				std::lock_guard lock(shard.mutex);
				RawPhysicsEvent event;
				const auto cached = shard.activeContacts.find(pair);
				if(cached != shard.activeContacts.end()) {
					event = cached->second;
					shard.activeContacts.erase(cached);
				} else {
					event.firstBody = pair.GetBody1ID();
					event.secondBody = pair.GetBody2ID();
					event.firstSubShape = pair.GetSubShapeID1();
					event.secondSubShape = pair.GetSubShapeID2();
				}
				event.kind = RawPhysicsEventKind::Removed;
				event.tick = currentTick.load(std::memory_order_acquire);
				PushLocked(shard, std::move(event));
			}

			void OnBodyActivated(const JPH::BodyID& bodyId, JPH::uint64) override {
				RecordActivation(RawPhysicsEventKind::Activated, bodyId);
			}

			void OnBodyDeactivated(const JPH::BodyID& bodyId, JPH::uint64) override {
				RecordActivation(RawPhysicsEventKind::Deactivated, bodyId);
			}

		private:
			struct Shard {
				std::mutex mutex;
				std::vector<RawPhysicsEvent> events;
				std::unordered_map<JPH::SubShapeIDPair, RawPhysicsEvent> activeContacts;
			};

			static constexpr std::size_t kShardCount = 16U;

			static std::size_t ShardIndex(std::uint64_t hash) noexcept {
				return static_cast<std::size_t>(hash % kShardCount);
			}

			void PushLocked(Shard& shard, RawPhysicsEvent event) {
				if(shard.events.size() >= maximumEventsPerShard) {
					droppedCount.fetch_add(1U, std::memory_order_relaxed);
					return;
				}
				shard.events.push_back(std::move(event));
			}

			void RecordContact(
				RawPhysicsEventKind kind,
				const JPH::Body& first,
				const JPH::Body& second,
				const JPH::ContactManifold& manifold,
				bool sensor) {
				RawPhysicsEvent event;
				event.kind = kind;
				event.firstBody = first.GetID();
				event.secondBody = second.GetID();
				event.firstSubShape = manifold.mSubShapeID1;
				event.secondSubShape = manifold.mSubShapeID2;
				event.point = manifold.mRelativeContactPointsOn1.empty()
					? manifold.mBaseOffset : manifold.GetWorldSpaceContactPointOn1(0U);
				event.normal = manifold.mWorldSpaceNormal;
				event.relativeVelocity = second.GetLinearVelocity() - first.GetLinearVelocity();
				event.penetration = manifold.mPenetrationDepth;
				event.tick = currentTick.load(std::memory_order_acquire);
				event.sensor = sensor;
				const JPH::SubShapeIDPair pair(
					event.firstBody, event.firstSubShape, event.secondBody, event.secondSubShape);
				Shard& shard = shards[ShardIndex(pair.GetHash())];
				std::lock_guard lock(shard.mutex);
				// Jolt may report Added followed by Persisted for LinearCast in one
				// update. Treat the duplicate Added as Stay to preserve one Enter.
				if(kind == RawPhysicsEventKind::Added && shard.activeContacts.contains(pair)) {
					event.kind = RawPhysicsEventKind::Persisted;
				}
				if(!shard.activeContacts.contains(pair)
					&& shard.activeContacts.size() >= maximumContactsPerShard) {
					droppedCount.fetch_add(1U, std::memory_order_relaxed);
					return;
				}
				shard.activeContacts.insert_or_assign(pair, event);
				PushLocked(shard, std::move(event));
			}

			void RecordActivation(RawPhysicsEventKind kind, const JPH::BodyID& bodyId) {
				RawPhysicsEvent event;
				event.kind = kind;
				event.firstBody = bodyId;
				event.tick = currentTick.load(std::memory_order_acquire);
				Shard& shard = shards[ShardIndex(bodyId.GetIndexAndSequenceNumber())];
				std::lock_guard lock(shard.mutex);
				PushLocked(shard, std::move(event));
			}

			static void ApplyMaterial(
				const JPH::Body& first,
				const JPH::Body& second,
				const JPH::ContactManifold& manifold,
				JPH::ContactSettings& settings) {
				const std::uint64_t firstMetadata = first.GetUserData();
				const std::uint64_t secondMetadata = second.GetUserData();
				// When materials disagree, the numerically stronger mode wins. This
				// deterministic precedence is provider-neutral and independent of body order.
				const PhysicsCombineMode frictionMode = static_cast<PhysicsCombineMode>(std::max(
					static_cast<std::uint8_t>(DecodeCombineMode(firstMetadata, 8U)),
					static_cast<std::uint8_t>(DecodeCombineMode(secondMetadata, 8U))));
				const PhysicsCombineMode restitutionMode = static_cast<PhysicsCombineMode>(std::max(
					static_cast<std::uint8_t>(DecodeCombineMode(firstMetadata, 10U)),
					static_cast<std::uint8_t>(DecodeCombineMode(secondMetadata, 10U))));
				settings.mCombinedFriction = std::max(0.0F, CombineMaterialValue(
					first.GetFriction(), second.GetFriction(), frictionMode));
				settings.mCombinedRestitution = std::max(0.0F, CombineMaterialValue(
					first.GetRestitution(), second.GetRestitution(), restitutionMode));
				const std::uint64_t firstSubShape = first.GetShape()->GetSubShapeUserData(manifold.mSubShapeID1);
				const std::uint64_t secondSubShape = second.GetShape()->GetSubShapeUserData(manifold.mSubShapeID2);
				settings.mIsSensor = settings.mIsSensor
					|| (firstSubShape & (1ULL << 63U)) != 0ULL
					|| (secondSubShape & (1ULL << 63U)) != 0ULL;
			}

			std::array<Shard, kShardCount> shards;
			std::size_t maximumEventsPerShard = 1024U;
			std::size_t maximumContactsPerShard = 1024U;
			std::atomic<PhysicsTick> currentTick{0U};
			std::atomic<std::uint64_t> droppedCount{0U};
		};

		template<typename Handle>
		std::uint64_t HandleKey(Handle handle) {
			return (static_cast<std::uint64_t>(handle.slotIndex) << 32U)
				| static_cast<std::uint64_t>(handle.slotGeneration);
		}

		bool HandleMatchesWorld(PhysicsWorldHandle world, PhysicsWorldHandle expected) {
			return world.IsValid() && world == expected;
		}

		template<typename Handle>
		bool HandleMatchesWorld(Handle handle, PhysicsWorldHandle expected) {
			return handle.IsValid()
				&& handle.worldId == expected.worldId
				&& handle.worldGeneration == expected.worldGeneration;
		}

		struct JoltRegistrationState {
			std::mutex mutex;
			std::uint32_t users = 0U;
			bool ownsFactory = false;
		};

		JoltRegistrationState& RegistrationState() {
			static JoltRegistrationState state;
			return state;
		}

		bool AcquireJoltTypes(std::string& message) {
			JoltRegistrationState& state = RegistrationState();
			std::lock_guard<std::mutex> lock(state.mutex);
			if(state.users == 0U) {
				if(JPH::Factory::sInstance != nullptr) {
					message = "Jolt Factory::sInstance is already owned outside TerraEngine.";
					return false;
				}
				JPH::Factory::sInstance = new JPH::Factory();
				state.ownsFactory = true;
				JPH::RegisterTypes();
			}
			++state.users;
			return true;
		}

		void ReleaseJoltTypes() {
			JoltRegistrationState& state = RegistrationState();
			std::lock_guard<std::mutex> lock(state.mutex);
			if(state.users == 0U) {
				return;
			}
			--state.users;
			if(state.users == 0U) {
				JPH::UnregisterTypes();
				if(state.ownsFactory) {
					delete JPH::Factory::sInstance;
					JPH::Factory::sInstance = nullptr;
					state.ownsFactory = false;
				}
			}
		}

		class TerraBroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
		public:
			void Configure(const PhysicsSettings& settings) {
				broadPhaseLayerCount = 1U;
				for(std::uint32_t index = 0; index < kJoltObjectLayerCount; ++index) {
					const CollisionLayer& layer = settings.layers[index];
					objectToBroadPhase[layer.id] = JPH::BroadPhaseLayer(layer.broadPhaseGroup);
					broadPhaseLayerCount = std::max<std::uint32_t>(
						broadPhaseLayerCount, static_cast<std::uint32_t>(layer.broadPhaseGroup) + 1U);
				}
			}

			JPH::uint GetNumBroadPhaseLayers() const override {
				return broadPhaseLayerCount;
			}

			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
				return layer < objectToBroadPhase.size()
					? objectToBroadPhase[layer]
					: JPH::cBroadPhaseLayerInvalid;
			}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override {
				return "TerraEngine Physics Group";
			}
#endif

		private:
			std::array<JPH::BroadPhaseLayer, kJoltObjectLayerCount> objectToBroadPhase{};
			std::uint32_t broadPhaseLayerCount = 1U;
		};

		class TerraObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
		public:
			void Configure(const PhysicsSettings& settings) {
				enabled.fill(false);
				for(const CollisionLayer& layer : settings.layers) {
					enabled[layer.id] = layer.enabled;
				}
			}

			bool ShouldCollide(JPH::ObjectLayer left, JPH::ObjectLayer right) const override {
				// Per-body mask overrides cannot be represented by Jolt's 5-bit object
				// layer alone. Keep this broad filter conservative and perform the exact
				// symmetric mask test in TerraContactListener before contacts are accepted.
				return left < enabled.size() && right < enabled.size()
					&& enabled[left] && enabled[right];
			}

			std::array<bool, kJoltObjectLayerCount> enabled{};
		};

		class TerraObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
		public:
			void Configure(
				const PhysicsSettings& settings,
				const TerraObjectLayerPairFilter& configuredPairFilter) {
				layerGroup.fill(0U);
				groupMembers.fill(0U);
				pairFilter = &configuredPairFilter;
				for(const CollisionLayer& layer : settings.layers) {
					layerGroup[layer.id] = layer.broadPhaseGroup;
					groupMembers[layer.broadPhaseGroup] |= (1U << layer.id);
				}
			}

			bool ShouldCollide(JPH::ObjectLayer objectLayer, JPH::BroadPhaseLayer broadPhaseLayer) const override {
				if(objectLayer >= kJoltObjectLayerCount || pairFilter == nullptr
					|| broadPhaseLayer.GetValue() >= groupMembers.size()) {
					return false;
				}
				std::uint32_t members = groupMembers[broadPhaseLayer.GetValue()];
				for(std::uint32_t candidate = 0U; candidate < kJoltObjectLayerCount; ++candidate) {
					if((members & (1U << candidate)) != 0U
						&& pairFilter->ShouldCollide(objectLayer, static_cast<JPH::ObjectLayer>(candidate))) {
						return true;
					}
				}
				return false;
			}

		private:
			std::array<std::uint8_t, kJoltObjectLayerCount> layerGroup{};
			std::array<std::uint32_t, kJoltObjectLayerCount> groupMembers{};
			const TerraObjectLayerPairFilter* pairFilter = nullptr;
		};

		class QueryObjectLayerFilter final : public JPH::ObjectLayerFilter {
		public:
			explicit QueryObjectLayerFilter(std::uint32_t mask)
				: mask(mask) {
			}

			bool ShouldCollide(JPH::ObjectLayer layer) const override {
				return layer < kJoltObjectLayerCount && (mask & (1U << layer)) != 0U;
			}

		private:
			std::uint32_t mask = 0U;
		};

		// Jolt's AllHitCollisionCollector grows without a caller-controlled bound.
		// This collector retains at most the calibrated 64 provider hits and, after
		// reaching capacity, replaces only a farther/worse hit. It deliberately does
		// not force early-out because a later broad-phase body may be closer.
		template<typename CollectorType>
		class BoundedHitCollector final : public CollectorType {
		public:
			using ResultType = typename CollectorType::ResultType;

			explicit BoundedHitCollector(std::size_t maximumHits)
				: maximumHits(std::clamp<std::size_t>(maximumHits, 1U, kPhysicsMaximumHitsPerQuery)) {
				hits.reserve(this->maximumHits);
			}

			void AddHit(const ResultType& hit) override {
				if(hits.size() < maximumHits) {
					hits.push_back(hit);
					return;
				}
				truncated = true;
				const auto worst = std::max_element(hits.begin(), hits.end(), [](const ResultType& left, const ResultType& right) {
					return left.GetEarlyOutFraction() < right.GetEarlyOutFraction();
				});
				if(worst != hits.end() && hit.GetEarlyOutFraction() < worst->GetEarlyOutFraction()) {
					*worst = hit;
				}
			}

			std::vector<ResultType> hits;
			bool truncated = false;

		private:
			std::size_t maximumHits = 1U;
		};

		// A single protected admission packet may invoke the established body-staging
		// routine repeatedly. This thread-local owner prevents recursive acquisition
		// of the backend mutex while the outer batch keeps concurrent callers out.
		thread_local JoltPhysicsBackend* batchAdmissionOwner = nullptr;
	}

	struct JoltPhysicsBackend::Impl {
		struct ShapeRecord {
			PhysicsShapeHandle handle;
			JPH::RefConst<JPH::Shape> shape;
			std::uint64_t stagingToken = 0U;
		};

		struct BodyRecord {
			PhysicsBodyHandle handle;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			JPH::BodyID bodyId;
			float mass = 0.0F;
			PhysicsMotionType motionType = PhysicsMotionType::Static;
			std::uint8_t collisionLayer = 0U;
			std::uint32_t collisionMask = 0U;
			PhysicsCombineMode frictionCombine = PhysicsCombineMode::Average;
			PhysicsCombineMode restitutionCombine = PhysicsCombineMode::Maximum;
			bool sensor = false;
			bool fullyLocked = false;
			bool startAwake = true;
			bool enabled = true;
			bool added = false;
			std::uint64_t stagingToken = 0U;
			std::uint32_t stableIndex = 0U;
			PhysicsBackendBodyState retainedState;
			PhysicsBodyStateChangeMask pendingChange = PhysicsBodyStateChangeMask::Created;
			bool hasRetainedState = false;
		};

		// Constraint and character pointers are strictly provider-private. Engine code
		// addresses them only through validated, generation-bearing handles.
		struct ConstraintRecord {
			PhysicsConstraintHandle handle;
			TerraEngine::UUID ownerObjectId{};
			PhysicsGeneration ownerObjectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			TerraEngine::UUID recordId{};
			PhysicsGeneration recordGeneration = 0U;
			PhysicsConstraintKind kind = PhysicsConstraintKind::Fixed;
			PhysicsBodyHandle bodyA;
			PhysicsBodyHandle bodyB;
			// Gear records retain the engine handles of their hinge references so
			// unrelated hinge lifecycle operations are never blocked conservatively.
			PhysicsConstraintHandle gearConstraintA;
			PhysicsConstraintHandle gearConstraintB;
			JPH::Ref<JPH::Constraint> constraint;
			PhysicsGeneration resetGeneration = 1U;
			bool enabled = true;
			bool added = false;
			std::uint64_t stagingToken = 0U;
		};

		struct CharacterRecord {
			PhysicsCharacterHandle handle;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			PhysicsShapeKind shapeKind = PhysicsShapeKind::Capsule;
			float radius = 0.0F;
			float height = 0.0F;
			PhysicsCharacterCreateRequest settings;
			std::unique_ptr<JPH::CharacterVirtual> character;
			bool enabled = true;
			bool committed = false;
			std::uint64_t stagingToken = 0U;
		};

		struct StagingRecord {
			struct PreparedActivationPartition {
				JPH::EActivation activation = JPH::EActivation::DontActivate;
				std::vector<JPH::BodyID> bodyIds;
				JPH::BodyInterface::AddState addState = nullptr;
				bool prepared = false;
			};

			PhysicsWorldHandle world;
			std::vector<std::uint64_t> shapeKeys;
			std::vector<std::uint64_t> bodyKeys;
			std::vector<std::uint64_t> constraintKeys;
			std::vector<std::uint64_t> characterKeys;
			// Unconstrained body packets prepare broad-phase insertion on background
			// workers. Jolt may reorder each private ID array, which must remain intact
			// until the matching finalize or abort call on the protected boundary.
			std::vector<PreparedActivationPartition> preparedActivationPartitions;
		};

		Impl(ThreadPool& pool, JoltPhysicsBackendConfiguration configuration)
			: sharedPool(pool), configuration(configuration) {
		}

		PhysicsBackendResult CheckReady() const {
			if(!initialized) {
				return Rejected(PhysicsBackendErrorCode::InvalidState, "The Jolt backend is not initialized.");
			}
			if(!healthy.load(std::memory_order_acquire)) {
				return ProviderFailure("The Jolt backend is contained after a fatal provider failure.", true);
			}
			return Success();
		}

		bool CheckProviderHealth() const {
			if(allocator == nullptr) {
				return healthy.load(std::memory_order_acquire);
			}
			const PhysicsAllocationDiagnostics diagnostics = allocator->Diagnostics();
			if(diagnostics.assertionFailures > assertionBaseline
				|| diagnostics.allocationFailures > allocationFailureBaseline) {
				healthy.store(false, std::memory_order_release);
			}
			return healthy.load(std::memory_order_acquire);
		}

		PhysicsBackendResult EnsureStaging(
			PhysicsBackendStagingToken& token,
			PhysicsWorldHandle requestedWorld) {
			if(!token.IsValid()) {
				std::uint64_t value = nextStagingToken++;
				if(value == 0U) value = nextStagingToken++;
				token = {requestedWorld, value};
				staging.emplace(value, StagingRecord{requestedWorld});
				return Success();
			}
			const auto found = staging.find(token.value);
			if(found == staging.end() || found->second.world != requestedWorld || token.world != requestedWorld) {
				return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The staging token does not belong to this world.");
			}
			return Success();
		}

		void AbortPreparedActivationPartitions(StagingRecord& record) {
			if(physicsSystem == nullptr) {
				record.preparedActivationPartitions.clear();
				return;
			}
			JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
			for(auto partition = record.preparedActivationPartitions.rbegin();
				partition != record.preparedActivationPartitions.rend(); ++partition) {
				if(!partition->prepared || partition->bodyIds.empty()) continue;
				bodyInterface.AddBodiesAbort(partition->bodyIds.data(),
					static_cast<int>(partition->bodyIds.size()), partition->addState);
				partition->prepared = false;
			}
			record.preparedActivationPartitions.clear();
		}

		bool NearlySame(PhysicsVector3 left, PhysicsVector3 right) {
			constexpr float epsilon = 1.0e-6F;
			return std::fabs(left.x - right.x) <= epsilon
				&& std::fabs(left.y - right.y) <= epsilon
				&& std::fabs(left.z - right.z) <= epsilon;
		}

		bool NearlySame(PhysicsPose left, PhysicsPose right) {
			constexpr float epsilon = 1.0e-6F;
			const float rotationDot = left.rotation.x * right.rotation.x
				+ left.rotation.y * right.rotation.y
				+ left.rotation.z * right.rotation.z
				+ left.rotation.w * right.rotation.w;
			return NearlySame(left.position, right.position)
				&& std::fabs(rotationDot) >= 1.0F - epsilon;
		}

		void RebuildBodyOrder() {
			bodyOrder.clear();
			bodyOrder.reserve(bodies.size());
			for(const auto& [key, body] : bodies) {
				if(body.added && body.stagingToken == 0U) bodyOrder.push_back(key);
			}
			std::sort(bodyOrder.begin(), bodyOrder.end());
			bodyOrderIndices.clear();
			bodyOrderIndices.reserve(bodyOrder.size());
			for(std::uint32_t index = 0U; index < bodyOrder.size(); ++index) {
				bodyOrderIndices[bodyOrder[index]] = index;
				auto body = bodies.find(bodyOrder[index]);
				if(body != bodies.end()) {
					body->second.stableIndex = index;
					body->second.retainedState.stableIndex = index;
				}
			}
			++bodyOrderGeneration;
			if(bodyOrderGeneration == 0U) ++bodyOrderGeneration;
		}

		void RebuildConstraintOrder() {
			constraintOrder.clear();
			constraintOrder.reserve(constraints.size());
			for(const auto& [key, constraint] : constraints) {
				if(constraint.stagingToken == 0U) constraintOrder.push_back(key);
			}
			std::sort(constraintOrder.begin(), constraintOrder.end());
		}

		void RebuildCharacterOrder() {
			characterOrder.clear();
			characterOrder.reserve(characters.size());
			for(const auto& [key, character] : characters) {
				if(character.stagingToken == 0U) characterOrder.push_back(key);
			}
			std::sort(characterOrder.begin(), characterOrder.end());
		}

		BodyRecord* FindBodyByObject(const TerraEngine::UUID& objectId) {
			const auto indexed = bodyObjectToKey.find(objectId);
			if(indexed == bodyObjectToKey.end()) return nullptr;
			const auto body = bodies.find(indexed->second);
			return body != bodies.end() && body->second.added ? &body->second : nullptr;
		}

		const BodyRecord* FindBodyByJoltId(JPH::BodyID bodyId) const {
			const auto indexed = bodyIdToKey.find(bodyId.GetIndexAndSequenceNumber());
			if(indexed == bodyIdToKey.end()) return nullptr;
			const auto body = bodies.find(indexed->second);
			return body != bodies.end() && body->second.bodyId == bodyId && body->second.added
				? &body->second : nullptr;
		}

		BodyRecord* FindBody(PhysicsBodyHandle handle) {
			if(!HandleMatchesWorld(handle, worldConfiguration.world)) return nullptr;
			auto found = bodies.find(HandleKey(handle));
			return found != bodies.end() && found->second.handle == handle ? &found->second : nullptr;
		}

		ConstraintRecord* FindConstraint(PhysicsConstraintHandle handle) {
			if(!HandleMatchesWorld(handle, worldConfiguration.world)) return nullptr;
			auto found = constraints.find(HandleKey(handle));
			return found != constraints.end() && found->second.handle == handle ? &found->second : nullptr;
		}

		PhysicsBackendResult BuildConstraint(
			const PhysicsConstraintCreateRequest& request,
			JPH::Ref<JPH::Constraint>& output) {
			if(!request.localFrameA.IsFinite() || !request.localFrameB.IsFinite()
				|| !IsUsableRotation(request.localFrameA.rotation) || !IsUsableRotation(request.localFrameB.rotation)
				|| !IsUsableAxis(request.axisA) || !IsUsableAxis(request.axisB)
				|| !IsUsableAxis(request.normalAxisA) || !IsUsableAxis(request.normalAxisB)
				|| request.ownerObjectId == TerraEngine::EMPTY_UUID || request.ownerObjectGeneration == 0U
				|| request.componentGeneration == 0U || request.recordId == TerraEngine::EMPTY_UUID
				|| request.recordGeneration == 0U || !std::isfinite(request.gearRatio) || request.gearRatio == 0.0F) {
				return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The constraint frame, identity, axis, or ratio is invalid.");
			}
			for(const PhysicsConstraintAxisSettings& axis : request.axes) {
				if(!std::isfinite(axis.minimum) || !std::isfinite(axis.maximum) || axis.minimum > axis.maximum
					|| !std::isfinite(axis.springFrequency) || axis.springFrequency < 0.0F
					|| !std::isfinite(axis.springDamping) || axis.springDamping < 0.0F
					|| !std::isfinite(axis.motorTargetPosition) || !std::isfinite(axis.motorTargetVelocity)
					|| !std::isfinite(axis.motorMaximumForce) || axis.motorMaximumForce < 0.0F) {
					return Rejected(PhysicsBackendErrorCode::InvalidArgument, "A constraint axis has invalid limits, spring, or motor settings.");
				}
			}
			// Jolt constraints do not own collision filtering. Connected collision is
			// therefore governed by the existing engine layer/mask policy; attempting
			// to mutate a body's shared Jolt collision group for one joint would make
			// unrelated constraints unsafe. The request is retained at the engine edge
			// for future pair-filter support without leaking a provider-only mechanism.

			BodyRecord* recordA = request.bodyA.IsValid() ? FindBody(request.bodyA) : nullptr;
			BodyRecord* recordB = request.bodyB.IsValid() ? FindBody(request.bodyB) : nullptr;
			if((request.bodyA.IsValid() && recordA == nullptr) || (request.bodyB.IsValid() && recordB == nullptr)
				|| (recordA == nullptr && recordB == nullptr) || (recordA != nullptr && recordB != nullptr && recordA == recordB)) {
				return Rejected(PhysicsBackendErrorCode::StaleHandle, "A constraint endpoint is stale, absent, or self-referential.");
			}

			JPH::Body* bodyA = &JPH::Body::sFixedToWorld;
			JPH::Body* bodyB = &JPH::Body::sFixedToWorld;
			// Constraint settings constructors may acquire Jolt's internal body locks.
			// This protected backend boundary already serializes lifecycle calls and
			// Step joins every Jolt job before returning, so the no-lock lookup is safe
			// here and avoids recursively acquiring PhysicsLock in the wrong order.
			const JPH::BodyLockInterfaceNoLock& bodyLookup = physicsSystem->GetBodyLockInterfaceNoLock();
			if(recordA != nullptr) {
				bodyA = bodyLookup.TryGetBody(recordA->bodyId);
				if(bodyA == nullptr) return Rejected(PhysicsBackendErrorCode::StaleHandle, "Jolt no longer owns constraint body A.");
			}
			if(recordB != nullptr) {
				bodyB = bodyLookup.TryGetBody(recordB->bodyId);
				if(bodyB == nullptr) return Rejected(PhysicsBackendErrorCode::StaleHandle, "Jolt no longer owns constraint body B.");
			}

			const JPH::RVec3 pointA = ToJoltPosition(request.localFrameA.position);
			const JPH::RVec3 pointB = ToJoltPosition(request.localFrameB.position);
			const JPH::Vec3 axisA = ToNormalizedJoltAxis(request.axisA);
			const JPH::Vec3 axisB = ToNormalizedJoltAxis(request.axisB);
			const JPH::Vec3 normalA = ToNormalizedJoltAxis(request.normalAxisA);
			const JPH::Vec3 normalB = ToNormalizedJoltAxis(request.normalAxisB);
			const PhysicsConstraintAxisSettings& primary = request.axes[0U];
			try {
				switch(request.kind) {
				case PhysicsConstraintKind::Fixed: {
					JPH::FixedConstraintSettings settings;
					settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = pointA; settings.mPoint2 = pointB;
					settings.mAxisX1 = ToJoltRotation(request.localFrameA.rotation) * JPH::Vec3::sAxisX();
					settings.mAxisY1 = ToJoltRotation(request.localFrameA.rotation) * JPH::Vec3::sAxisY();
					settings.mAxisX2 = ToJoltRotation(request.localFrameB.rotation) * JPH::Vec3::sAxisX();
					settings.mAxisY2 = ToJoltRotation(request.localFrameB.rotation) * JPH::Vec3::sAxisY();
					output = settings.Create(*bodyA, *bodyB); break;
				}
				case PhysicsConstraintKind::Point: {
					JPH::PointConstraintSettings settings; settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = pointA; settings.mPoint2 = pointB; output = settings.Create(*bodyA, *bodyB); break;
				}
				case PhysicsConstraintKind::Distance: {
					JPH::DistanceConstraintSettings settings; settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = pointA; settings.mPoint2 = pointB;
					if(primary.limitEnabled) { settings.mMinDistance = primary.minimum; settings.mMaxDistance = primary.maximum; }
					settings.mLimitsSpringSettings = ToJoltSpring(primary); output = settings.Create(*bodyA, *bodyB); break;
				}
				case PhysicsConstraintKind::Hinge: {
					JPH::HingeConstraintSettings settings; settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = pointA; settings.mPoint2 = pointB; settings.mHingeAxis1 = axisA; settings.mHingeAxis2 = axisB;
					settings.mNormalAxis1 = normalA; settings.mNormalAxis2 = normalB;
					if(primary.limitEnabled) { settings.mLimitsMin = primary.minimum; settings.mLimitsMax = primary.maximum; }
					settings.mLimitsSpringSettings = ToJoltSpring(primary); ConfigureJoltMotor(settings.mMotorSettings, primary, true);
					auto* hinge = static_cast<JPH::HingeConstraint*>(settings.Create(*bodyA, *bodyB));
					hinge->SetMotorState(ToJoltMotorState(primary.motorMode)); hinge->SetTargetAngle(primary.motorTargetPosition); hinge->SetTargetAngularVelocity(primary.motorTargetVelocity); output = hinge; break;
				}
				case PhysicsConstraintKind::Slider: {
					JPH::SliderConstraintSettings settings; settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = pointA; settings.mPoint2 = pointB; settings.mSliderAxis1 = axisA; settings.mSliderAxis2 = axisB;
					settings.mNormalAxis1 = normalA; settings.mNormalAxis2 = normalB;
					if(primary.limitEnabled) { settings.mLimitsMin = primary.minimum; settings.mLimitsMax = primary.maximum; }
					settings.mLimitsSpringSettings = ToJoltSpring(primary); ConfigureJoltMotor(settings.mMotorSettings, primary, false);
					auto* slider = static_cast<JPH::SliderConstraint*>(settings.Create(*bodyA, *bodyB));
					slider->SetMotorState(ToJoltMotorState(primary.motorMode)); slider->SetTargetPosition(primary.motorTargetPosition); slider->SetTargetVelocity(primary.motorTargetVelocity); output = slider; break;
				}
				case PhysicsConstraintKind::Cone: {
					JPH::ConeConstraintSettings settings; settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = pointA; settings.mPoint2 = pointB; settings.mTwistAxis1 = axisA; settings.mTwistAxis2 = axisB;
					settings.mHalfConeAngle = primary.limitEnabled ? primary.maximum : std::numbers::pi_v<float>; output = settings.Create(*bodyA, *bodyB); break;
				}
				case PhysicsConstraintKind::SwingTwist: {
					JPH::SwingTwistConstraintSettings settings; settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPosition1 = pointA; settings.mPosition2 = pointB; settings.mTwistAxis1 = axisA; settings.mTwistAxis2 = axisB;
					settings.mPlaneAxis1 = normalA; settings.mPlaneAxis2 = normalB;
					settings.mNormalHalfConeAngle = request.axes[0].limitEnabled ? request.axes[0].maximum : std::numbers::pi_v<float>;
					settings.mPlaneHalfConeAngle = request.axes[1].limitEnabled ? request.axes[1].maximum : std::numbers::pi_v<float>;
					settings.mTwistMinAngle = request.axes[2].limitEnabled ? request.axes[2].minimum : -std::numbers::pi_v<float>;
					settings.mTwistMaxAngle = request.axes[2].limitEnabled ? request.axes[2].maximum : std::numbers::pi_v<float>;
					ConfigureJoltMotor(settings.mSwingMotorSettings, request.axes[0], true); ConfigureJoltMotor(settings.mTwistMotorSettings, request.axes[2], true);
					auto* swing = static_cast<JPH::SwingTwistConstraint*>(settings.Create(*bodyA, *bodyB));
					swing->SetSwingMotorState(ToJoltMotorState(request.axes[0].motorMode)); swing->SetTwistMotorState(ToJoltMotorState(request.axes[2].motorMode));
					swing->SetTargetAngularVelocityCS(JPH::Vec3(request.axes[2].motorTargetVelocity, request.axes[0].motorTargetVelocity, request.axes[1].motorTargetVelocity)); output = swing; break;
				}
				case PhysicsConstraintKind::SixDegreesOfFreedom: {
					JPH::SixDOFConstraintSettings settings; settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPosition1 = pointA; settings.mPosition2 = pointB;
					settings.mAxisX1 = axisA; settings.mAxisY1 = normalA; settings.mAxisX2 = axisB; settings.mAxisY2 = normalB;
					for(int index = 0; index < JPH::SixDOFConstraintSettings::EAxis::Num; ++index) {
						const auto axis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(index); const auto& source = request.axes[static_cast<std::size_t>(index)];
						if(source.limitEnabled) settings.SetLimitedAxis(axis, source.minimum, source.maximum); else settings.MakeFreeAxis(axis);
						ConfigureJoltMotor(settings.mMotorSettings[index], source, index >= 3);
						if(index < 3) settings.mLimitsSpringSettings[index] = ToJoltSpring(source);
					}
					auto* six = static_cast<JPH::SixDOFConstraint*>(settings.Create(*bodyA, *bodyB));
					for(int index = 0; index < JPH::SixDOFConstraintSettings::EAxis::Num; ++index) six->SetMotorState(static_cast<JPH::SixDOFConstraint::EAxis>(index), ToJoltMotorState(request.axes[static_cast<std::size_t>(index)].motorMode));
					six->SetTargetPositionCS(JPH::Vec3(request.axes[0].motorTargetPosition, request.axes[1].motorTargetPosition, request.axes[2].motorTargetPosition));
					six->SetTargetVelocityCS(JPH::Vec3(request.axes[0].motorTargetVelocity, request.axes[1].motorTargetVelocity, request.axes[2].motorTargetVelocity));
					six->SetTargetAngularVelocityCS(JPH::Vec3(request.axes[3].motorTargetVelocity, request.axes[4].motorTargetVelocity, request.axes[5].motorTargetVelocity)); output = six; break;
				}
				case PhysicsConstraintKind::Gear: {
					ConstraintRecord* first = FindConstraint(request.gearConstraintA); ConstraintRecord* second = FindConstraint(request.gearConstraintB);
					if(first == nullptr || second == nullptr || first->kind != PhysicsConstraintKind::Hinge || second->kind != PhysicsConstraintKind::Hinge) return Rejected(PhysicsBackendErrorCode::Unsupported, "A Jolt gear requires two live hinge constraints.");
					JPH::GearConstraintSettings settings; settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM; settings.mHingeAxis1 = axisA; settings.mHingeAxis2 = axisB; settings.mRatio = request.gearRatio;
					auto* gear = static_cast<JPH::GearConstraint*>(settings.Create(*bodyA, *bodyB)); gear->SetConstraints(first->constraint, second->constraint); output = gear; break;
				}
				}
			} catch(const std::exception& exception) {
				return ProviderFailure(std::string("Jolt constraint creation failed: ") + exception.what());
			}
			return output != nullptr ? Success() : ProviderFailure("Jolt did not create the requested constraint.");
		}

		PhysicsEventParticipant MakeEventParticipant(
			const BodyRecord& body,
			JPH::SubShapeID subShape) const {
			PhysicsEventParticipant participant;
			participant.objectId = body.objectId;
			participant.objectGeneration = body.objectGeneration;
			participant.componentGeneration = body.componentGeneration;
			// The initial body registry has one stable collider-set owner per object.
			// SubshapeId distinguishes records inside compounds until the richer
			// collider-record registry exposes its authored UUID directly.
			participant.colliderRecordId = body.objectId;
			participant.recordGeneration = body.componentGeneration;
			participant.body = body.handle;
			participant.subshapeId = subShape.GetValue();
			participant.collisionLayer = body.collisionLayer;
			return participant;
		}

		void ConvertRawEvents(std::vector<RawPhysicsEvent> raw, std::uint64_t dropped) {
			droppedEvents += dropped;
			const std::size_t maximumPending = std::max<std::size_t>(
				4096U, static_cast<std::size_t>(worldConfiguration.maximumContactConstraints) * 4U);
			for(const RawPhysicsEvent& source : raw) {
				const BodyRecord* first = FindBodyByJoltId(source.firstBody);
				if(first == nullptr || source.tick == 0U) {
					++droppedEvents;
					continue;
				}
				PhysicsEvent event;
				event.world = worldConfiguration.world;
				event.tick = source.tick;
				event.first = MakeEventParticipant(*first, source.firstSubShape);
				event.point = FromJoltPosition(source.point);
				event.normal = FromJoltVector(source.normal);
				event.relativeVelocity = FromJoltVector(source.relativeVelocity);
				event.penetration = source.penetration;

				if(source.kind == RawPhysicsEventKind::Activated
					|| source.kind == RawPhysicsEventKind::Deactivated) {
					event.type = source.kind == RawPhysicsEventKind::Activated
						? PhysicsEventType::Activated : PhysicsEventType::Slept;
					event.hasSecondParticipant = false;
					if(event.IsFinite() && pendingEvents.size() < maximumPending) {
						pendingEvents.push_back(std::move(event));
					} else {
						++droppedEvents;
					}
					continue;
				}

				const BodyRecord* second = FindBodyByJoltId(source.secondBody);
				if(second == nullptr) {
					++droppedEvents;
					continue;
				}
				event.second = MakeEventParticipant(*second, source.secondSubShape);
				const bool sensor = source.sensor || first->sensor || second->sensor;
				switch(source.kind) {
				case RawPhysicsEventKind::Added:
					event.type = sensor ? PhysicsEventType::TriggerEnter : PhysicsEventType::CollisionEnter;
					break;
				case RawPhysicsEventKind::Persisted:
					event.type = sensor ? PhysicsEventType::TriggerStay : PhysicsEventType::CollisionStay;
					break;
				case RawPhysicsEventKind::Removed:
					event.type = sensor ? PhysicsEventType::TriggerExit : PhysicsEventType::CollisionExit;
					break;
				default:
					continue;
				}
				if(event.IsFinite() && pendingEvents.size() < maximumPending) {
					pendingEvents.push_back(event);
				} else {
					++droppedEvents;
					continue;
				}
				// A contact summary accompanies physical enter/stay records. Triggers
				// intentionally publish only their transition because they do not solve.
				if(!sensor && source.kind != RawPhysicsEventKind::Removed) {
					event.type = PhysicsEventType::Contact;
					if(pendingEvents.size() < maximumPending) pendingEvents.push_back(std::move(event));
					else ++droppedEvents;
				}
			}
		}

		void PublishCommandTransitions(PhysicsTick tick, std::size_t firstNewEvent) {
			const std::size_t maximumPending = std::max<std::size_t>(
				4096U, static_cast<std::size_t>(worldConfiguration.maximumContactConstraints) * 4U);
			for(const auto& [bodyKey, type] : pendingCommandTransitions) {
				const auto body = bodies.find(bodyKey);
				if(body == bodies.end() || !body->second.added) continue;
				const bool providerAlreadyReported = std::any_of(
					pendingEvents.begin() + std::min(firstNewEvent, pendingEvents.size()),
					pendingEvents.end(), [&](const PhysicsEvent& event) {
						return event.type == type && event.first.body == body->second.handle;
					});
				if(providerAlreadyReported) continue;
				PhysicsEvent event;
				event.type = type;
				event.world = worldConfiguration.world;
				event.tick = tick;
				event.first = MakeEventParticipant(body->second, {});
				event.hasSecondParticipant = false;
				if(event.IsFinite() && pendingEvents.size() < maximumPending) {
					pendingEvents.push_back(std::move(event));
				} else {
					++droppedEvents;
				}
			}
			pendingCommandTransitions.clear();
		}

		ThreadPool& sharedPool;
		JoltPhysicsBackendConfiguration configuration;
		std::unique_ptr<PhysicsAllocatorAdapter> allocator;
		std::unique_ptr<PhysicsJobAdapter> jobAdapter;
		std::unique_ptr<JPH::TempAllocatorImpl> temporaryAllocator;
		std::unique_ptr<JPH::PhysicsSystem> physicsSystem;
		TerraBroadPhaseLayerInterface broadPhaseLayers;
		TerraObjectLayerPairFilter objectPairFilter;
		TerraObjectVsBroadPhaseFilter objectVsBroadPhaseFilter;
		PhysicsBackendWorldConfiguration worldConfiguration;
		std::unordered_map<std::uint64_t, ShapeRecord> shapes;
		// Recipes with the same nonzero stable hash share one immutable provider
		// shape. The snapshot hash covers every child and local transform.
		std::unordered_map<std::uint64_t, JPH::Ref<JPH::Shape>> shapeCache;
		std::unordered_map<std::uint64_t, BodyRecord> bodies;
		// Membership changes rebuild this compact deterministic order once. Fixed
		// steps reuse it directly and never sort the complete world per extraction.
		std::vector<std::uint64_t> bodyOrder;
		std::unordered_map<std::uint64_t, std::uint32_t> bodyOrderIndices;
		std::uint64_t bodyOrderGeneration = 1U;
		// Only bodies changed outside ordinary active simulation need an engine-owned
		// membership set. Jolt already owns the authoritative active-body list, so an
		// incremental extraction visits active plus dirty bodies without scanning the
		// complete stable body order.
		std::unordered_set<std::uint64_t> dirtyBodyKeys;
		JPH::BodyIDVector activeBodiesScratch;
		std::vector<PhysicsBodyHandle> pendingRemovedBodies;
		std::unordered_map<std::uint64_t, ConstraintRecord> constraints;
		std::unordered_map<std::uint64_t, CharacterRecord> characters;
		// Provider maps retain O(1) handle lookup, while these compact membership
		// orders make fixed-step extraction deterministic without sorting every tick.
		std::vector<std::uint64_t> constraintOrder;
		std::vector<std::uint64_t> characterOrder;
		// Query filters and callback publication resolve provider IDs frequently.
		// This O(1) index avoids an O(body-count) scan for every hit/contact.
		std::unordered_map<std::uint32_t, std::uint64_t> bodyIdToKey;
		std::unordered_map<TerraEngine::UUID, std::uint64_t> bodyObjectToKey;
		std::unordered_map<std::uint64_t, StagingRecord> staging;
		std::uint64_t nextStagingToken = 1U;
		std::uint64_t assertionBaseline = 0U;
		std::uint64_t allocationFailureBaseline = 0U;
		TerraContactListener contactListener;
		std::vector<PhysicsEvent> pendingEvents;
		// Jolt does not promise activation-listener callbacks for an explicit
		// ActivateBody/DeactivateBody call. Retain the command transition until the
		// next step, then publish it only if the provider did not report the same edge.
		std::unordered_map<std::uint64_t, PhysicsEventType> pendingCommandTransitions;
		std::uint64_t droppedEvents = 0U;
		PhysicsTick lastEventDrainTick = 0U;
		// A lease owns only stable engine keys and copied event values. Provider
		// membership stays live until the protected publisher acknowledges this
		// exact token, which makes a rejected publication safe to retry.
		struct PendingExtractionLease {
			PhysicsBackendExtractionToken token;
			std::vector<std::uint64_t> bodyKeys;
			bool includesRemovedBodies = false;
			bool includesEvents = false;
			std::size_t removedBodyCount = 0U;
			std::size_t eventCount = 0U;
		};
		std::unordered_map<std::uint64_t, PendingExtractionLease> extractionLeases;
		std::uint64_t nextExtractionToken = 1U;
		bool typesRegistered = false;
		bool initialized = false;
		mutable std::atomic<bool> healthy{true};
		mutable std::shared_mutex mutex;
	};

	JoltPhysicsBackend::JoltPhysicsBackend(
		ThreadPool& sharedPool,
		JoltPhysicsBackendConfiguration configuration)
		: impl(new Impl(sharedPool, configuration)) {
	}

	JoltPhysicsBackend::~JoltPhysicsBackend() {
		(void)Shutdown();
		delete impl;
	}

	PhysicsBackendResult JoltPhysicsBackend::Initialize() {
		std::lock_guard lock(impl->mutex);
		if(impl->initialized) {
			return Success("The Jolt backend was already initialized.");
		}
		try {
			impl->allocator = std::make_unique<PhysicsAllocatorAdapter>();
			if(!impl->allocator->IsInstalled()) {
				impl->allocator.reset();
				return ProviderFailure("TerraEngine could not install the Jolt allocation callbacks.", true);
			}
			std::string registrationError;
			if(!AcquireJoltTypes(registrationError)) {
				impl->allocator.reset();
				return ProviderFailure(std::move(registrationError), true);
			}
			impl->typesRegistered = true;
			if(impl->configuration.temporaryAllocatorBytes < 1024U * 1024U) {
				impl->configuration.temporaryAllocatorBytes = 1024U * 1024U;
			}
			impl->temporaryAllocator = std::make_unique<JPH::TempAllocatorImpl>(
				impl->configuration.temporaryAllocatorBytes);
			impl->jobAdapter = std::make_unique<PhysicsJobAdapter>(
				impl->sharedPool,
				PhysicsJobAdapterConfig{
					impl->configuration.maximumPhysicsWorkers,
					impl->configuration.maximumBarriers,
					impl->configuration.maximumJobs});
			const PhysicsAllocationDiagnostics diagnostics = impl->allocator->Diagnostics();
			impl->assertionBaseline = diagnostics.assertionFailures;
			impl->allocationFailureBaseline = diagnostics.allocationFailures;
			impl->healthy.store(true, std::memory_order_release);
			impl->initialized = true;
			return Success("Jolt Physics 5.6 initialized through TerraEngine adapters.");
		} catch(const std::exception& exception) {
			impl->healthy.store(false, std::memory_order_release);
			impl->jobAdapter.reset();
			impl->temporaryAllocator.reset();
			if(impl->typesRegistered) {
				ReleaseJoltTypes();
				impl->typesRegistered = false;
			}
			impl->allocator.reset();
			return ProviderFailure(std::string("Jolt initialization failed: ") + exception.what(), true);
		} catch(...) {
			impl->healthy.store(false, std::memory_order_release);
			impl->jobAdapter.reset();
			impl->temporaryAllocator.reset();
			if(impl->typesRegistered) {
				ReleaseJoltTypes();
				impl->typesRegistered = false;
			}
			impl->allocator.reset();
			return ProviderFailure("Jolt initialization failed with an unknown exception.", true);
		}
	}

	PhysicsBackendResult JoltPhysicsBackend::Shutdown() {
		std::lock_guard lock(impl->mutex);
		if(!impl->initialized && impl->allocator == nullptr) {
			return Success("The Jolt backend was already shut down.");
		}

		if(impl->physicsSystem != nullptr) {
			// Listeners are detached before provider records are destroyed. Update has
			// already joined through the shared job adapter, so no late callback can
			// append to the bounded collector during teardown.
			impl->physicsSystem->SetContactListener(nullptr);
			impl->physicsSystem->SetBodyActivationListener(nullptr);
			// Constraints retain body references, so remove them before bodies. Virtual
			// characters retain PhysicsSystem state and must die before that system.
			for(auto& [key, constraint] : impl->constraints) {
				(void)key;
				if(constraint.added) impl->physicsSystem->RemoveConstraint(constraint.constraint);
			}
			impl->characters.clear();
			impl->constraints.clear();
			for(auto& [key, body] : impl->bodies) {
				(void)key;
				if(body.added) impl->physicsSystem->GetBodyInterface().RemoveBody(body.bodyId);
				impl->physicsSystem->GetBodyInterface().DestroyBody(body.bodyId);
			}
		}
		impl->bodies.clear();
		impl->bodyOrder.clear();
		impl->bodyOrderIndices.clear();
		impl->dirtyBodyKeys.clear();
		impl->activeBodiesScratch.clear();
		// JPH::Array frees through Jolt's allocator callback. Release its retained
		// capacity while that callback and the backend allocator are still alive.
		impl->activeBodiesScratch.shrink_to_fit();
		impl->pendingRemovedBodies.clear();
		impl->constraints.clear();
		impl->characters.clear();
		impl->constraintOrder.clear();
		impl->characterOrder.clear();
		impl->bodyIdToKey.clear();
		impl->bodyObjectToKey.clear();
		impl->shapes.clear();
		impl->shapeCache.clear();
		impl->staging.clear();
		impl->pendingEvents.clear();
		impl->pendingCommandTransitions.clear();
		impl->droppedEvents = 0U;
		impl->lastEventDrainTick = 0U;
		impl->extractionLeases.clear();
		impl->contactListener.Clear();
		impl->physicsSystem.reset();
		if(impl->jobAdapter != nullptr) {
			impl->jobAdapter->RequestStop();
			impl->jobAdapter->WaitForIdle();
		}
		impl->jobAdapter.reset();
		impl->temporaryAllocator.reset();
		if(impl->typesRegistered) {
			ReleaseJoltTypes();
			impl->typesRegistered = false;
		}
		// The allocator callback adapter is deliberately released last, after the
		// final provider reference and global RTTI object have been destroyed.
		impl->allocator.reset();
		impl->worldConfiguration = {};
		impl->initialized = false;
		impl->healthy.store(true, std::memory_order_release);
		return Success("Jolt backend shutdown completed in provider-safe order.");
	}

	PhysicsBackendResult JoltPhysicsBackend::CreateWorld(
		const PhysicsBackendWorldConfiguration& configuration) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem != nullptr) {
			return Rejected(PhysicsBackendErrorCode::InvalidState, "A Jolt world is already active.");
		}
		if(!configuration.world.IsValid() || !configuration.settings.Validate().accepted
			|| configuration.maximumBodies == 0U) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The backend world configuration is invalid.");
		}

		try {
			impl->broadPhaseLayers.Configure(configuration.settings);
			impl->objectPairFilter.Configure(configuration.settings);
			impl->objectVsBroadPhaseFilter.Configure(configuration.settings, impl->objectPairFilter);
			impl->physicsSystem = std::make_unique<JPH::PhysicsSystem>();
			impl->physicsSystem->Init(
				configuration.maximumBodies,
				0U,
				configuration.maximumBodyPairs,
				configuration.maximumContactConstraints,
				impl->broadPhaseLayers,
				impl->objectVsBroadPhaseFilter,
				impl->objectPairFilter);
			// Project quality is provider-neutral authoring data. Apply it before any
			// body or constraint can enter this generation so diagnostics and results
			// describe one coherent solver profile for the world's whole lifetime.
			JPH::PhysicsSettings providerSettings = impl->physicsSystem->GetPhysicsSettings();
			providerSettings.mNumVelocitySteps = configuration.settings.velocityIterations;
			providerSettings.mNumPositionSteps = configuration.settings.positionIterations;
			// Avoid replacing Jolt's already-matching default record. This preserves the
			// established default-world bitwise trajectory while non-default profiles
			// still apply their explicitly authored solver quality before admission.
			const JPH::PhysicsSettings& currentSettings = impl->physicsSystem->GetPhysicsSettings();
			if(currentSettings.mNumVelocitySteps != providerSettings.mNumVelocitySteps
				|| currentSettings.mNumPositionSteps != providerSettings.mNumPositionSteps) {
				impl->physicsSystem->SetPhysicsSettings(providerSettings);
			}
			impl->physicsSystem->SetContactListener(&impl->contactListener);
			impl->physicsSystem->SetBodyActivationListener(&impl->contactListener);
			impl->contactListener.Configure(std::max<std::size_t>(
				4096U, static_cast<std::size_t>(configuration.maximumContactConstraints) * 4U));
			impl->physicsSystem->SetGravity(ToJoltVector(configuration.settings.gravity));
			impl->worldConfiguration = configuration;
			impl->bodyOrderGeneration = 1U;
			impl->lastEventDrainTick = 0U;
			return impl->CheckProviderHealth()
				? Success("The Jolt world was created.")
				: ProviderFailure("Jolt reported a fatal failure while creating the world.", true);
		} catch(const std::exception& exception) {
			impl->physicsSystem.reset();
			impl->healthy.store(false, std::memory_order_release);
			return ProviderFailure(std::string("Jolt world creation failed: ") + exception.what(), true);
		} catch(...) {
			impl->physicsSystem.reset();
			impl->healthy.store(false, std::memory_order_release);
			return ProviderFailure("Jolt world creation failed with an unknown exception.", true);
		}
	}

	PhysicsBackendResult JoltPhysicsBackend::DestroyWorld(PhysicsWorldHandle world) {
		std::lock_guard lock(impl->mutex);
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(world, impl->worldConfiguration.world)) {
			return Rejected(PhysicsBackendErrorCode::StaleWorld, "The requested Jolt world is absent or stale.");
		}
		impl->physicsSystem->SetContactListener(nullptr);
		impl->physicsSystem->SetBodyActivationListener(nullptr);
		for(auto& [key, constraint] : impl->constraints) {
			(void)key;
			if(constraint.added) impl->physicsSystem->RemoveConstraint(constraint.constraint);
		}
		impl->characters.clear();
		impl->constraints.clear();
		for(auto& [key, body] : impl->bodies) {
			(void)key;
			if(body.added) impl->physicsSystem->GetBodyInterface().RemoveBody(body.bodyId);
			impl->physicsSystem->GetBodyInterface().DestroyBody(body.bodyId);
		}
		impl->bodies.clear();
		impl->bodyOrder.clear();
		impl->bodyOrderIndices.clear();
		impl->dirtyBodyKeys.clear();
		impl->activeBodiesScratch.clear();
		impl->activeBodiesScratch.shrink_to_fit();
		impl->pendingRemovedBodies.clear();
		impl->constraints.clear();
		impl->characters.clear();
		impl->constraintOrder.clear();
		impl->characterOrder.clear();
		impl->bodyIdToKey.clear();
		impl->bodyObjectToKey.clear();
		impl->shapes.clear();
		impl->shapeCache.clear();
		impl->staging.clear();
		impl->pendingEvents.clear();
		impl->pendingCommandTransitions.clear();
		impl->droppedEvents = 0U;
		impl->lastEventDrainTick = 0U;
		impl->extractionLeases.clear();
		impl->contactListener.Clear();
		impl->physicsSystem.reset();
		impl->worldConfiguration = {};
		return Success("The Jolt world and all provider records were destroyed.");
	}

	PhysicsBackendResult JoltPhysicsBackend::ReplaceWorld(
		const PhysicsBackendWorldConfiguration&,
		PhysicsBackendStagingToken&) {
		return Rejected(PhysicsBackendErrorCode::Unsupported,
			"Atomic whole-world replacement is reserved for the lifecycle integration slice.");
	}

	PhysicsCookResult JoltPhysicsBackend::CookShape(const PhysicsCookRequest& request) {
		std::lock_guard lock(impl->mutex);
		PhysicsCookResult result;
		result.recipeHash = request.recipe.stableRecipeHash;
		result.formatFamily = std::string(kJoltCookFormatFamily);
		result.provider = std::string(kJoltProviderName);
		result.providerVersion = std::string(kJoltProviderVersion);
		result.providerRevision = std::string(kJoltProviderRevision);
		result.formatVersion = kJoltCookFormatVersion;
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) {
			result.result = ready;
			return result;
		}
		if(request.recipe.stableRecipeHash == 0U
			|| (request.recipe.cookedPayload != nullptr && !request.recipe.cookedPayload->empty())) {
			result.result = Rejected(PhysicsBackendErrorCode::InvalidArgument,
				"Cooking requires a nonzero normalized recipe hash and uncooked source geometry.");
			return result;
		}
		if(request.world.IsValid() && (impl->physicsSystem == nullptr
			|| !HandleMatchesWorld(request.world, impl->worldConfiguration.world))) {
			result.result = Rejected(PhysicsBackendErrorCode::StaleWorld,
				"The optional cooking world handle is stale.");
			return result;
		}
		const float maximumExtent = impl->physicsSystem != nullptr
			? impl->worldConfiguration.settings.maxShapeExtent
			: kPhysicsMaximumShapeExtent;
		if(!ValidShapeDimensions(request.recipe, maximumExtent)) {
			result.result = Rejected(PhysicsBackendErrorCode::InvalidArgument,
				"The collider recipe is invalid, degenerate, oversized, or exceeds cooking limits.");
			return result;
		}

		try {
			JPH::ShapeSettings::ShapeResult shapeResult = CreateRecipeShape(
				request.recipe, maximumExtent, 0U);
			if(!shapeResult.IsValid()) {
				result.result = ProviderFailure(shapeResult.HasError()
					? std::string(shapeResult.GetError().c_str())
					: "Jolt returned an empty cooked shape result.");
				return result;
			}

			std::ostringstream bytes(std::ios::binary | std::ios::out);
			JPH::StreamOutWrapper stream(bytes);
			JPH::Shape::ShapeToIDMap shapes;
			JPH::Shape::MaterialToIDMap materials;
			shapeResult.Get()->SaveWithChildren(stream, shapes, materials);
			if(stream.IsFailed()) {
				result.result = ProviderFailure("Jolt failed while serializing the cooked shape.");
				return result;
			}
			const std::string serialized = bytes.str();
			if(serialized.empty() || serialized.size() > kMaximumCookedPayloadBytes) {
				result.result = Rejected(PhysicsBackendErrorCode::CapacityExceeded,
					"The cooked shape payload is empty or exceeds the configured payload bound.");
				return result;
			}
			result.payload.resize(serialized.size());
			std::memcpy(result.payload.data(), serialized.data(), serialized.size());
			result.payloadChecksum = CookedPayloadChecksum(result.payload);
			result.result = Success("The collider recipe was cooked with Jolt Physics 5.6.");
		} catch(const std::exception& exception) {
			result.result = ProviderFailure(std::string("Jolt collider cooking failed: ") + exception.what());
		} catch(...) {
			result.result = ProviderFailure("Jolt collider cooking failed with an unknown exception.");
		}
		return result;
	}

	PhysicsBackendResult JoltPhysicsBackend::CreateShape(
		PhysicsWorldHandle world,
		PhysicsShapeHandle handle,
		const PhysicsShapeRecipe& recipe,
		PhysicsBackendStagingToken& staging) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(world, impl->worldConfiguration.world)
			|| !HandleMatchesWorld(handle, world)
			|| !ValidShapeDimensions(recipe, impl->worldConfiguration.settings.maxShapeExtent)) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The shape request contains a stale handle or invalid dimensions.");
		}
		const std::uint64_t key = HandleKey(handle);
		if(impl->shapes.contains(key)) {
			return Rejected(PhysicsBackendErrorCode::InvalidState, "The shape handle is already populated.");
		}
		PhysicsBackendResult stagingResult = impl->EnsureStaging(staging, world);
		if(!stagingResult.Succeeded()) return stagingResult;

		try {
			JPH::ShapeSettings::ShapeResult shapeResult;
			if(recipe.stableRecipeHash != 0U) {
				const auto cached = impl->shapeCache.find(recipe.stableRecipeHash);
				if(cached != impl->shapeCache.end()) shapeResult.Set(cached->second);
			}
			if(!shapeResult.IsValid()) {
				// The same recursive path handles source recipes and opaque cooked
				// children, so compounds may freely contain several cooked assets.
				shapeResult = CreateRecipeShape(
					recipe, impl->worldConfiguration.settings.maxShapeExtent, 0U);
			}
			if(!shapeResult.IsValid()) {
				return ProviderFailure(shapeResult.HasError()
					? std::string(shapeResult.GetError().c_str())
					: "Jolt returned an empty shape result.");
			}
			if(recipe.stableRecipeHash != 0U) {
				impl->shapeCache.try_emplace(recipe.stableRecipeHash, shapeResult.Get());
			}
			impl->shapes.emplace(key, Impl::ShapeRecord{handle, shapeResult.Get(), staging.value});
			impl->staging.at(staging.value).shapeKeys.push_back(key);
			return Success("The Jolt shape was prepared off-world.");
		} catch(const std::exception& exception) {
			return ProviderFailure(std::string("Jolt shape creation failed: ") + exception.what());
		} catch(...) {
			return ProviderFailure("Jolt shape creation failed with an unknown exception.");
		}
	}

	PhysicsBackendResult JoltPhysicsBackend::DestroyShape(PhysicsShapeHandle handle) {
		std::lock_guard lock(impl->mutex);
		if(!HandleMatchesWorld(handle, impl->worldConfiguration.world)) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The shape handle is stale.");
		}
		return impl->shapes.erase(HandleKey(handle)) == 1U
			? Success("The engine released its Jolt shape reference.")
			: Rejected(PhysicsBackendErrorCode::StaleHandle, "The shape handle was not found.");
	}

	PhysicsBackendResult JoltPhysicsBackend::CreateBody(
		const PhysicsBodyCreateRequest& request,
		PhysicsBackendStagingToken& staging) {
		const bool insideBatchAdmission = batchAdmissionOwner == this;
		std::unique_lock<std::shared_mutex> lock(impl->mutex, std::defer_lock);
		if(!insideBatchAdmission) {
			lock.lock();
			const PhysicsBackendResult ready = impl->CheckReady();
			if(!ready.Succeeded()) return ready;
		}
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(request.handle, impl->worldConfiguration.world)
			|| !HandleMatchesWorld(request.shape, impl->worldConfiguration.world)
			|| request.objectId == TerraEngine::EMPTY_UUID
			|| request.objectGeneration == 0U || request.componentGeneration == 0U
			|| !request.pose.IsFinite() || !IsUsableRotation(request.pose.rotation)
			|| !request.linearVelocity.IsFinite()
			|| !request.angularVelocity.IsFinite() || request.collisionLayer >= kJoltObjectLayerCount
			|| !impl->worldConfiguration.settings.layers[request.collisionLayer].enabled
			|| !std::isfinite(request.mass) || request.mass <= 0.0F
			|| !std::isfinite(request.friction) || request.friction < 0.0F
			|| !std::isfinite(request.restitution) || request.restitution < 0.0F
			|| !std::isfinite(request.linearDamping) || request.linearDamping < 0.0F
			|| !std::isfinite(request.angularDamping) || request.angularDamping < 0.0F
			|| !std::isfinite(request.maximumLinearVelocity) || request.maximumLinearVelocity <= 0.0F
			|| !std::isfinite(request.maximumAngularVelocity) || request.maximumAngularVelocity <= 0.0F
			|| !std::isfinite(request.gravityFactor) || request.gravityFactor < 0.0F) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The body request is invalid or stale.");
		}
		const std::uint64_t bodyKey = HandleKey(request.handle);
		const auto shape = impl->shapes.find(HandleKey(request.shape));
		if(impl->bodies.contains(bodyKey) || shape == impl->shapes.end()) {
			return Rejected(PhysicsBackendErrorCode::InvalidState,
				"The body handle already exists or its shape is missing.");
		}
		if(request.motionType != PhysicsMotionType::Static && shape->second.shape->MustBeStatic()) {
			return Rejected(PhysicsBackendErrorCode::Unsupported,
				"This cooked shape contains triangle-mesh geometry and can only be used by a static body.");
		}
		if(!insideBatchAdmission) {
			PhysicsBackendResult stagingResult = impl->EnsureStaging(staging, impl->worldConfiguration.world);
			if(!stagingResult.Succeeded()) return stagingResult;
			// A later single-body mutation changes the packet's authored ordering. Abort
			// any earlier background preparation and let CommitStaging rebuild the one
			// canonical partition stream instead of finalizing stale shuffled ID arrays.
			impl->AbortPreparedActivationPartitions(impl->staging.at(staging.value));
		}

		try {
			const std::uint32_t allowedDegreesOfFreedom = request.allowedDegreesOfFreedom & 0x3FU;
			const bool fullyLocked = request.motionType == PhysicsMotionType::Dynamic
				&& allowedDegreesOfFreedom == 0U;
			// Jolt explicitly disallows EAllowedDOFs::None. Represent an authored
			// fully locked dynamic body as an immovable provider body while retaining
			// its engine motion intent for serialization and diagnostics.
			const PhysicsMotionType providerMotionType = fullyLocked
				? PhysicsMotionType::Static
				: request.motionType;
			JPH::BodyCreationSettings settings(
				shape->second.shape,
				ToJoltPosition(request.pose.position),
				ToJoltRotation(request.pose.rotation),
				ToJoltMotionType(providerMotionType),
				static_cast<JPH::ObjectLayer>(request.collisionLayer));
			settings.mLinearVelocity = ToJoltVector(request.linearVelocity);
			settings.mAngularVelocity = ToJoltVector(request.angularVelocity);
			settings.mMotionQuality = ToJoltMotionQuality(request.motionQuality);
			settings.mFriction = request.friction;
			settings.mRestitution = request.restitution;
			settings.mLinearDamping = request.linearDamping;
			settings.mAngularDamping = request.angularDamping;
			settings.mMaxLinearVelocity = request.maximumLinearVelocity;
			settings.mMaxAngularVelocity = request.maximumAngularVelocity;
			if(providerMotionType == PhysicsMotionType::Dynamic) {
				settings.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(allowedDegreesOfFreedom);
			}
			settings.mIsSensor = request.sensor;
			settings.mAllowSleeping = request.allowSleeping;
			settings.mGravityFactor = request.useGravity ? request.gravityFactor : 0.0F;
			settings.mUserData = EncodeCollisionMetadata(
				request.collisionLayer,
				request.collisionMask,
				request.frictionCombine,
				request.restitutionCombine);
			if(request.motionType == PhysicsMotionType::Dynamic && request.mass > 0.0F) {
				settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
				settings.mMassPropertiesOverride.mMass = request.mass;
			}
			JPH::Body* body = impl->physicsSystem->GetBodyInterface().CreateBody(settings);
			if(body == nullptr) {
				return Rejected(PhysicsBackendErrorCode::CapacityExceeded, "Jolt body capacity is exhausted.");
			}
			const JPH::BodyID providerBodyId = body->GetID();
			auto [bodyRecord, inserted] = impl->bodies.emplace(bodyKey, Impl::BodyRecord{
				request.handle, request.objectId, request.objectGeneration,
				request.componentGeneration, providerBodyId, request.mass,
				request.motionType, request.collisionLayer, request.collisionMask,
				request.frictionCombine, request.restitutionCombine,
				request.sensor, fullyLocked, request.startAwake, request.enabled, false, staging.value});
			(void)inserted;
			bodyRecord->second.retainedState.handle = request.handle;
			bodyRecord->second.retainedState.pose = request.pose;
			bodyRecord->second.retainedState.linearVelocity = request.linearVelocity;
			bodyRecord->second.retainedState.angularVelocity = request.angularVelocity;
			bodyRecord->second.retainedState.mass = request.mass;
			bodyRecord->second.retainedState.awake = request.startAwake;
			bodyRecord->second.retainedState.objectId = request.objectId;
			bodyRecord->second.retainedState.objectGeneration = request.objectGeneration;
			bodyRecord->second.retainedState.componentGeneration = request.componentGeneration;
			bodyRecord->second.retainedState.fields = PhysicsBodyStateFieldMask::BodyState;
			bodyRecord->second.retainedState.change = PhysicsBodyStateChangeMask::Created;
			bodyRecord->second.retainedState.motionType = request.motionType;
			bodyRecord->second.hasRetainedState = true;
			impl->bodyIdToKey[providerBodyId.GetIndexAndSequenceNumber()] = bodyKey;
			// Do not update the object lookup while staging. A replacement body can
			// share the authored identity with a live body; callers must keep seeing
			// that live record until the protected commit boundary swaps the index.
			impl->staging.at(staging.value).bodyKeys.push_back(bodyKey);
			return Success("The Jolt body was staged without broad-phase visibility.");
		} catch(const std::exception& exception) {
			return ProviderFailure(std::string("Jolt body creation failed: ") + exception.what());
		} catch(...) {
			return ProviderFailure("Jolt body creation failed with an unknown exception.");
		}
	}

	PhysicsBackendResult JoltPhysicsBackend::CreateBodies(
		std::span<const PhysicsBodyCreateRequest> requests,
		PhysicsBackendStagingToken& staging) {
		if(requests.empty()) return Success("The empty body packet required no provider admission work.");
		std::lock_guard lock(impl->mutex);
		if(batchAdmissionOwner != nullptr) {
			return Rejected(PhysicsBackendErrorCode::InvalidState,
				"A nested provider body-admission packet is not supported.");
		}
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		const PhysicsBackendResult stagingResult = impl->EnsureStaging(staging, impl->worldConfiguration.world);
		if(!stagingResult.Succeeded()) return stagingResult;

		struct PendingBody final {
			const PhysicsBodyCreateRequest* request = nullptr;
			JPH::BodyCreationSettings settings;
			bool fullyLocked = false;
			JPH::Body* allocatedBody = nullptr;
		};
		std::vector<PendingBody> pending;
		pending.reserve(requests.size());
		std::unordered_set<std::uint64_t> pendingKeys;
		pendingKeys.reserve(requests.size());
		try {
			// Validate and materialize immutable provider settings before scheduling any
			// allocation. Worker completion order can therefore never alter admission
			// order, error precedence, or the authoring record that owns each body.
			for(const PhysicsBodyCreateRequest& request : requests) {
				if(!HandleMatchesWorld(request.handle, impl->worldConfiguration.world)
					|| !HandleMatchesWorld(request.shape, impl->worldConfiguration.world)
					|| request.objectId == TerraEngine::EMPTY_UUID
					|| request.objectGeneration == 0U || request.componentGeneration == 0U
					|| !request.pose.IsFinite() || !IsUsableRotation(request.pose.rotation)
					|| !request.linearVelocity.IsFinite() || !request.angularVelocity.IsFinite()
					|| request.collisionLayer >= kJoltObjectLayerCount
					|| !impl->worldConfiguration.settings.layers[request.collisionLayer].enabled
					|| !std::isfinite(request.mass) || request.mass <= 0.0F
					|| !std::isfinite(request.friction) || request.friction < 0.0F
					|| !std::isfinite(request.restitution) || request.restitution < 0.0F
					|| !std::isfinite(request.linearDamping) || request.linearDamping < 0.0F
					|| !std::isfinite(request.angularDamping) || request.angularDamping < 0.0F
					|| !std::isfinite(request.maximumLinearVelocity) || request.maximumLinearVelocity <= 0.0F
					|| !std::isfinite(request.maximumAngularVelocity) || request.maximumAngularVelocity <= 0.0F
					|| !std::isfinite(request.gravityFactor) || request.gravityFactor < 0.0F) {
					return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The body packet contains an invalid or stale request.");
				}
				const std::uint64_t bodyKey = HandleKey(request.handle);
				const auto shape = impl->shapes.find(HandleKey(request.shape));
				if(impl->bodies.contains(bodyKey) || !pendingKeys.insert(bodyKey).second || shape == impl->shapes.end()) {
					return Rejected(PhysicsBackendErrorCode::InvalidState,
						"The body packet reuses a handle or references a missing shape.");
				}
				if(request.motionType != PhysicsMotionType::Static && shape->second.shape->MustBeStatic()) {
					return Rejected(PhysicsBackendErrorCode::Unsupported,
						"This cooked shape contains triangle-mesh geometry and can only be used by a static body.");
				}
				const std::uint32_t allowedDegreesOfFreedom = request.allowedDegreesOfFreedom & 0x3FU;
				const bool fullyLocked = request.motionType == PhysicsMotionType::Dynamic
					&& allowedDegreesOfFreedom == 0U;
				const PhysicsMotionType providerMotionType = fullyLocked
					? PhysicsMotionType::Static
					: request.motionType;
				JPH::BodyCreationSettings settings(
					shape->second.shape,
					ToJoltPosition(request.pose.position),
					ToJoltRotation(request.pose.rotation),
					ToJoltMotionType(providerMotionType),
					static_cast<JPH::ObjectLayer>(request.collisionLayer));
				settings.mLinearVelocity = ToJoltVector(request.linearVelocity);
				settings.mAngularVelocity = ToJoltVector(request.angularVelocity);
				settings.mMotionQuality = ToJoltMotionQuality(request.motionQuality);
				settings.mFriction = request.friction;
				settings.mRestitution = request.restitution;
				settings.mLinearDamping = request.linearDamping;
				settings.mAngularDamping = request.angularDamping;
				settings.mMaxLinearVelocity = request.maximumLinearVelocity;
				settings.mMaxAngularVelocity = request.maximumAngularVelocity;
				if(providerMotionType == PhysicsMotionType::Dynamic) {
					settings.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(allowedDegreesOfFreedom);
				}
				settings.mIsSensor = request.sensor;
				settings.mAllowSleeping = request.allowSleeping;
				settings.mGravityFactor = request.useGravity ? request.gravityFactor : 0.0F;
				settings.mUserData = EncodeCollisionMetadata(
					request.collisionLayer,
					request.collisionMask,
					request.frictionCombine,
					request.restitutionCombine);
				if(request.motionType == PhysicsMotionType::Dynamic && request.mass > 0.0F) {
					settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
					settings.mMassPropertiesOverride.mMass = request.mass;
				}
				pending.push_back({&request, std::move(settings), fullyLocked, nullptr});
			}
		} catch(const std::exception& exception) {
			return ProviderFailure(std::string("Jolt body packet preparation failed: ") + exception.what());
		} catch(...) {
			return ProviderFailure("Jolt body packet preparation failed with an unknown exception.");
		}

		// Jolt explicitly permits CreateBodyWithoutID followed by AssignBodyID to
		// decouple independent construction from its lock-protected ID registry.
		// The slots below are disjoint and every worker joins before the protected
		// assignment loop begins, so no task can outlive this staging transaction.
		constexpr std::size_t kAllocationRangeSize = 64U;
		const std::size_t rangeCount = (pending.size() + kAllocationRangeSize - 1U) / kAllocationRangeSize;
		const std::size_t laneCount = std::min(rangeCount, impl->sharedPool.getThreadCount());
		std::atomic<std::size_t> nextRange{0U};
		std::atomic<bool> allocationFailed{false};
		JPH::BodyInterface& bodies = impl->physicsSystem->GetBodyInterface();
		auto allocateRanges = [&]() {
			for(;;) {
				const std::size_t rangeIndex = nextRange.fetch_add(1U, std::memory_order_relaxed);
				if(rangeIndex >= rangeCount) return;
				const std::size_t begin = rangeIndex * kAllocationRangeSize;
				const std::size_t end = std::min(begin + kAllocationRangeSize, pending.size());
				for(std::size_t index = begin; index < end; ++index) {
					try {
						pending[index].allocatedBody = bodies.CreateBodyWithoutID(pending[index].settings);
						if(pending[index].allocatedBody == nullptr) allocationFailed.store(true, std::memory_order_release);
					} catch(...) {
						allocationFailed.store(true, std::memory_order_release);
					}
				}
			}
		};
		std::vector<std::future<void>> futures;
		try {
			futures.reserve(laneCount > 0U ? laneCount - 1U : 0U);
			for(std::size_t lane = 1U; lane < laneCount; ++lane) {
				auto task = impl->sharedPool.createTask(allocateRanges);
				futures.push_back(task->getFuture());
				impl->sharedPool.enqueue(std::move(task), kAllocationRangeSize);
			}
		} catch(...) {
			allocationFailed.store(true, std::memory_order_release);
		}
		allocateRanges();
		for(std::future<void>& future : futures) {
			try {
				future.get();
			} catch(...) {
				allocationFailed.store(true, std::memory_order_release);
			}
		}
		if(allocationFailed.load(std::memory_order_acquire)) {
			for(PendingBody& entry : pending) {
				if(entry.allocatedBody != nullptr) bodies.DestroyBodyWithoutID(entry.allocatedBody);
			}
			return Rejected(PhysicsBackendErrorCode::CapacityExceeded,
				"Jolt could not allocate every member of the body packet.");
		}

		struct AssignedBody final {
			std::uint64_t key = 0U;
			JPH::BodyID id;
		};
		std::vector<AssignedBody> assignedBodies;
		assignedBodies.reserve(pending.size());
		auto discardAssigned = [&]() noexcept {
			// Assigned bodies are still outside Jolt's broad phase. Remove them in
			// reverse request order and restore the staging token to its pre-packet
			// state, leaving the caller authority to roll back earlier staged records.
			for(auto assigned = assignedBodies.rbegin(); assigned != assignedBodies.rend(); ++assigned) {
				auto found = impl->bodies.find(assigned->key);
				if(found != impl->bodies.end()) {
					bodies.DestroyBody(found->second.bodyId);
					impl->bodyIdToKey.erase(found->second.bodyId.GetIndexAndSequenceNumber());
					impl->bodies.erase(found);
				} else {
					// A later map insertion can throw after AssignBodyID. Reclaim that
					// ID directly instead of leaving an unreachable provider allocation.
					JPH::Body* unassigned = bodies.UnassignBodyID(assigned->id);
					if(unassigned != nullptr) bodies.DestroyBodyWithoutID(unassigned);
				}
			}
			auto stagingRecord = impl->staging.find(staging.value);
			if(stagingRecord != impl->staging.end()) {
				stagingRecord->second.bodyKeys.erase(std::remove_if(
					stagingRecord->second.bodyKeys.begin(), stagingRecord->second.bodyKeys.end(),
					[&assignedBodies](std::uint64_t key) {
						return std::find_if(assignedBodies.begin(), assignedBodies.end(),
							[key](const AssignedBody& assigned) { return assigned.key == key; }) != assignedBodies.end();
					}), stagingRecord->second.bodyKeys.end());
			}
		};
		// Sequential ID assignment fixes the provider's observable identity order
		// before any worker starts broad-phase preparation. Preparation may shuffle
		// only its private per-partition ID array, never this engine-owned sequence.
		try {
			for(PendingBody& entry : pending) {
				if(!bodies.AssignBodyID(entry.allocatedBody)) {
					for(PendingBody& unassigned : pending) {
						if(unassigned.allocatedBody != nullptr) bodies.DestroyBodyWithoutID(unassigned.allocatedBody);
					}
					discardAssigned();
					return Rejected(PhysicsBackendErrorCode::CapacityExceeded,
						"Jolt body capacity is exhausted while assigning the body packet.");
				}
				const PhysicsBodyCreateRequest& request = *entry.request;
				const std::uint64_t bodyKey = HandleKey(request.handle);
				const JPH::BodyID providerBodyId = entry.allocatedBody->GetID();
				entry.allocatedBody = nullptr; // BodyInterface now owns the assigned body.
				assignedBodies.push_back({bodyKey, providerBodyId});
			}
		} catch(...) {
			for(PendingBody& entry : pending) {
				if(entry.allocatedBody != nullptr && entry.allocatedBody->GetID().IsInvalid()) {
					bodies.DestroyBodyWithoutID(entry.allocatedBody);
				}
			}
			discardAssigned();
			return ProviderFailure("Jolt body packet ID assignment failed and was rolled back before publication.");
		}

		auto& stagingRecord = impl->staging.at(staging.value);
		impl->AbortPreparedActivationPartitions(stagingRecord);
		if(stagingRecord.constraintKeys.empty()) {
			for(std::size_t index = 0U; index < pending.size(); ++index) {
				const PhysicsBodyCreateRequest& request = *pending[index].request;
				if(!request.enabled) continue;
				const JPH::EActivation activation = request.startAwake && request.motionType != PhysicsMotionType::Static
					? JPH::EActivation::Activate
					: JPH::EActivation::DontActivate;
				if(stagingRecord.preparedActivationPartitions.empty()
					|| stagingRecord.preparedActivationPartitions.back().activation != activation) {
					stagingRecord.preparedActivationPartitions.push_back({activation, {}, nullptr, false});
				}
				stagingRecord.preparedActivationPartitions.back().bodyIds.push_back(assignedBodies[index].id);
			}
		}

		std::atomic<std::size_t> nextPreparedPartition{0U};
		std::atomic<bool> preparationFailed{false};
		auto preparePartitions = [&]() {
			for(;;) {
				const std::size_t index = nextPreparedPartition.fetch_add(1U, std::memory_order_relaxed);
				if(index >= stagingRecord.preparedActivationPartitions.size()) return;
				auto& partition = stagingRecord.preparedActivationPartitions[index];
				try {
					partition.addState = bodies.AddBodiesPrepare(partition.bodyIds.data(),
						static_cast<int>(partition.bodyIds.size()));
					partition.prepared = true;
				} catch(...) {
					preparationFailed.store(true, std::memory_order_release);
				}
			}
		};
		std::vector<std::future<void>> preparationFutures;
		try {
			const std::size_t workerCount = impl->sharedPool.getThreadCount();
			const std::size_t laneCount = workerCount > 1U
				? std::min(stagingRecord.preparedActivationPartitions.size(), workerCount)
				: 0U;
			preparationFutures.reserve(laneCount);
			for(std::size_t lane = 0U; lane < laneCount; ++lane) {
				auto task = impl->sharedPool.createTask(preparePartitions);
				preparationFutures.push_back(task->getFuture());
				impl->sharedPool.enqueue(std::move(task), 1U);
			}
		} catch(...) {
			preparationFailed.store(true, std::memory_order_release);
		}

		bool publicationFailed = false;
		try {
			// While Jolt prepares broad-phase state on worker lanes, publish the copied
			// engine records in canonical request order. These maps remain staging-only
			// until CommitStaging finalizes every prepared provider partition.
			for(std::size_t index = 0U; index < pending.size(); ++index) {
				const PendingBody& entry = pending[index];
				const PhysicsBodyCreateRequest& request = *entry.request;
				const std::uint64_t bodyKey = assignedBodies[index].key;
				const JPH::BodyID providerBodyId = assignedBodies[index].id;
				auto [bodyRecord, inserted] = impl->bodies.emplace(bodyKey, Impl::BodyRecord{
					request.handle, request.objectId, request.objectGeneration,
					request.componentGeneration, providerBodyId, request.mass,
					request.motionType, request.collisionLayer, request.collisionMask,
					request.frictionCombine, request.restitutionCombine,
					request.sensor, entry.fullyLocked, request.startAwake, request.enabled, false, staging.value});
				if(!inserted) {
					publicationFailed = true;
					break;
				}
				bodyRecord->second.retainedState.handle = request.handle;
				bodyRecord->second.retainedState.pose = request.pose;
				bodyRecord->second.retainedState.linearVelocity = request.linearVelocity;
				bodyRecord->second.retainedState.angularVelocity = request.angularVelocity;
				bodyRecord->second.retainedState.mass = request.mass;
				bodyRecord->second.retainedState.awake = request.startAwake;
				bodyRecord->second.retainedState.objectId = request.objectId;
				bodyRecord->second.retainedState.objectGeneration = request.objectGeneration;
				bodyRecord->second.retainedState.componentGeneration = request.componentGeneration;
				bodyRecord->second.retainedState.fields = PhysicsBodyStateFieldMask::BodyState;
				bodyRecord->second.retainedState.change = PhysicsBodyStateChangeMask::Created;
				bodyRecord->second.retainedState.motionType = request.motionType;
				bodyRecord->second.hasRetainedState = true;
				impl->bodyIdToKey[providerBodyId.GetIndexAndSequenceNumber()] = bodyKey;
				stagingRecord.bodyKeys.push_back(bodyKey);
			}
		} catch(...) {
			publicationFailed = true;
		}
		for(std::future<void>& future : preparationFutures) {
			try {
				future.get();
			} catch(...) {
				preparationFailed.store(true, std::memory_order_release);
			}
		}
		if(preparationFutures.empty() && !stagingRecord.preparedActivationPartitions.empty()) {
			// A stopped or zero-worker pool cannot run a task. Keep the exact same
			// documented prepare operation correct on the protected caller lane.
			preparePartitions();
		}
		if(publicationFailed || preparationFailed.load(std::memory_order_acquire)) {
			impl->AbortPreparedActivationPartitions(stagingRecord);
			discardAssigned();
			return ProviderFailure("Jolt body packet preparation or engine publication failed before visibility.");
		}
		return Success("The complete provider body packet was allocated in worker ranges and staged in request order.");
	}

	PhysicsBackendResult JoltPhysicsBackend::UpdateBody(const PhysicsBodyUpdateRequest& request) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(request.handle, impl->worldConfiguration.world)
			|| !request.pose.IsFinite() || !IsUsableRotation(request.pose.rotation)
			|| !request.linearVelocity.IsFinite()
			|| !request.angularVelocity.IsFinite() || request.collisionLayer >= kJoltObjectLayerCount
			|| !impl->worldConfiguration.settings.layers[request.collisionLayer].enabled) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The body handle is stale.");
		}
		auto body = impl->bodies.find(HandleKey(request.handle));
		if(body == impl->bodies.end()) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The body handle was not found.");
		}
		JPH::BodyInterface& bodies = impl->physicsSystem->GetBodyInterface();
		if(request.enabled && !body->second.added) {
			bodies.AddBody(body->second.bodyId, request.wake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
			body->second.added = true;
		} else if(!request.enabled && body->second.added) {
			bodies.RemoveBody(body->second.bodyId);
			body->second.added = false;
		}
		bodies.SetPositionAndRotation(body->second.bodyId,
			ToJoltPosition(request.pose.position), ToJoltRotation(request.pose.rotation),
			request.wake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
		bodies.SetLinearAndAngularVelocity(body->second.bodyId,
			ToJoltVector(request.linearVelocity), ToJoltVector(request.angularVelocity));
		bodies.SetObjectLayer(body->second.bodyId, static_cast<JPH::ObjectLayer>(request.collisionLayer));
		body->second.collisionLayer = request.collisionLayer;
		body->second.collisionMask = request.collisionMask;
		body->second.enabled = request.enabled;
		body->second.retainedState.pose = request.pose;
		body->second.retainedState.linearVelocity = request.linearVelocity;
		body->second.retainedState.angularVelocity = request.angularVelocity;
		body->second.retainedState.awake = request.wake || body->second.retainedState.awake;
		body->second.pendingChange |= PhysicsBodyStateChangeMask::Updated;
		if(request.wake) body->second.pendingChange |= PhysicsBodyStateChangeMask::Awakened;
		impl->dirtyBodyKeys.insert(HandleKey(request.handle));
		bodies.SetUserData(body->second.bodyId, EncodeCollisionMetadata(
			request.collisionLayer,
			request.collisionMask,
			body->second.frictionCombine,
			body->second.restitutionCombine));
		return Success("The Jolt body was updated.");
	}

	PhysicsBackendResult JoltPhysicsBackend::DestroyBody(PhysicsBodyHandle handle) {
		std::lock_guard lock(impl->mutex);
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(handle, impl->worldConfiguration.world)) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The body handle is stale.");
		}
		auto body = impl->bodies.find(HandleKey(handle));
		if(body == impl->bodies.end()) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The body handle was not found.");
		}
		for(const auto& [key, constraint] : impl->constraints) {
			(void)key;
			if(constraint.bodyA == handle || constraint.bodyB == handle) {
				return Rejected(PhysicsBackendErrorCode::InvalidState, "Destroy the body's constraints transactionally before releasing its Jolt body.");
			}
		}
		if(body->second.added) impl->physicsSystem->GetBodyInterface().RemoveBody(body->second.bodyId);
		impl->physicsSystem->GetBodyInterface().DestroyBody(body->second.bodyId);
		impl->bodyIdToKey.erase(body->second.bodyId.GetIndexAndSequenceNumber());
		if(const auto indexed = impl->bodyObjectToKey.find(body->second.objectId);
			indexed != impl->bodyObjectToKey.end() && indexed->second == HandleKey(handle)) {
			impl->bodyObjectToKey.erase(indexed);
		}
		impl->pendingRemovedBodies.push_back(handle);
		impl->dirtyBodyKeys.erase(HandleKey(handle));
		impl->pendingCommandTransitions.erase(HandleKey(handle));
		impl->bodies.erase(body);
		impl->RebuildBodyOrder();
		return Success("The Jolt body was removed and destroyed.");
	}

	PhysicsBackendResult JoltPhysicsBackend::CreateConstraint(
		const PhysicsConstraintCreateRequest& request,
		PhysicsBackendStagingToken& staging) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(request.handle, impl->worldConfiguration.world)) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The constraint handle targets a stale Jolt world.");
		}
		const std::uint64_t key = HandleKey(request.handle);
		if(impl->constraints.contains(key)) return Rejected(PhysicsBackendErrorCode::InvalidState, "The constraint handle is already populated.");
		PhysicsBackendResult staged = impl->EnsureStaging(staging, impl->worldConfiguration.world);
		if(!staged.Succeeded()) return staged;
		// Constraints require the established sequential body insertion order. A
		// packet may have prepared a broad-phase batch before its dependent records
		// arrived, so release that private Jolt preparation before staging the link.
		impl->AbortPreparedActivationPartitions(impl->staging.at(staging.value));
		JPH::Ref<JPH::Constraint> providerConstraint;
		PhysicsBackendResult built = impl->BuildConstraint(request, providerConstraint);
		if(!built.Succeeded()) return built;
		impl->constraints.emplace(key, Impl::ConstraintRecord{
			request.handle, request.ownerObjectId, request.ownerObjectGeneration, request.componentGeneration,
			request.recordId, request.recordGeneration, request.kind, request.bodyA, request.bodyB,
			request.gearConstraintA, request.gearConstraintB,
			std::move(providerConstraint), request.resetGeneration, request.enabled, false, staging.value});
		impl->staging.at(staging.value).constraintKeys.push_back(key);
		return Success("The Jolt constraint was staged without solver visibility.");
	}

	PhysicsBackendResult JoltPhysicsBackend::UpdateConstraint(const PhysicsConstraintCreateRequest& request) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(request.handle, impl->worldConfiguration.world)) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The constraint handle targets a stale Jolt world.");
		}
		Impl::ConstraintRecord* existing = impl->FindConstraint(request.handle);
		if(existing == nullptr || existing->stagingToken != 0U) return Rejected(PhysicsBackendErrorCode::StaleHandle, "The constraint is missing or not committed.");
		// A gear retains references to its hinges. Rejecting a destructive hinge update
		// keeps that dependent graph atomic; the transaction path can recreate it.
		if(existing->kind == PhysicsConstraintKind::Hinge) {
			for(const auto& [key, candidate] : impl->constraints) {
				(void)key;
				if(candidate.kind == PhysicsConstraintKind::Gear
					&& (candidate.gearConstraintA == existing->handle || candidate.gearConstraintB == existing->handle)) {
					return Rejected(PhysicsBackendErrorCode::InvalidState, "A hinge referenced by a gear must be recreated transactionally.");
				}
			}
		}
		JPH::Ref<JPH::Constraint> replacement;
		PhysicsBackendResult built = impl->BuildConstraint(request, replacement);
		if(!built.Succeeded()) return built;
		const bool wasAdded = existing->added;
		if(wasAdded) impl->physicsSystem->RemoveConstraint(existing->constraint);
		JPH::Ref<JPH::Constraint> previous = std::move(existing->constraint);
		existing->constraint = std::move(replacement);
		existing->ownerObjectId = request.ownerObjectId;
		existing->ownerObjectGeneration = request.ownerObjectGeneration;
		existing->componentGeneration = request.componentGeneration;
		existing->recordId = request.recordId;
		existing->recordGeneration = request.recordGeneration;
		existing->kind = request.kind;
		existing->bodyA = request.bodyA;
		existing->bodyB = request.bodyB;
		existing->gearConstraintA = request.gearConstraintA;
		existing->gearConstraintB = request.gearConstraintB;
		existing->resetGeneration = request.resetGeneration;
		existing->enabled = request.enabled;
		// Updates run only for committed records, so an enabled replacement must
		// immediately join Jolt's solver even when the previous record was disabled.
		existing->added = request.enabled;
		if(existing->added) impl->physicsSystem->AddConstraint(existing->constraint);
		return Success("The Jolt constraint was atomically replaced at the protected boundary.");
	}

	PhysicsBackendResult JoltPhysicsBackend::DestroyConstraint(PhysicsConstraintHandle handle) {
		std::lock_guard lock(impl->mutex);
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(handle, impl->worldConfiguration.world)) return Rejected(PhysicsBackendErrorCode::StaleHandle, "The constraint handle is stale.");
		auto found = impl->constraints.find(HandleKey(handle));
		if(found == impl->constraints.end() || found->second.handle != handle) return Rejected(PhysicsBackendErrorCode::StaleHandle, "The constraint handle was not found.");
		if(found->second.kind == PhysicsConstraintKind::Hinge) {
			for(const auto& [key, candidate] : impl->constraints) {
				(void)key;
				if(candidate.kind == PhysicsConstraintKind::Gear
					&& (candidate.gearConstraintA == found->second.handle || candidate.gearConstraintB == found->second.handle)) {
					return Rejected(PhysicsBackendErrorCode::InvalidState, "A hinge referenced by a gear must be destroyed transactionally.");
				}
			}
		}
		if(found->second.added) impl->physicsSystem->RemoveConstraint(found->second.constraint);
		impl->constraints.erase(found);
		impl->RebuildConstraintOrder();
		return Success("The Jolt constraint was removed before its provider reference was released.");
	}

	PhysicsBackendResult JoltPhysicsBackend::CreateCharacter(
		const PhysicsCharacterCreateRequest& request,
		PhysicsBackendStagingToken& staging) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(request.handle, impl->worldConfiguration.world)
			|| request.objectId == TerraEngine::EMPTY_UUID || request.objectGeneration == 0U || request.componentGeneration == 0U
			|| !request.pose.IsFinite() || !IsUsableRotation(request.pose.rotation) || !request.localCenter.IsFinite()
			|| !IsUsableAxis(request.upDirection) || !std::isfinite(request.radius) || !std::isfinite(request.height)
			|| request.radius <= 0.0F || request.height <= 0.0F || request.height < 2.0F * request.radius
			|| !std::isfinite(request.stepHeight) || request.stepHeight < 0.0F || !std::isfinite(request.contactPadding) || request.contactPadding < 0.0F
			|| !std::isfinite(request.maximumSlopeAngleRadians) || request.maximumSlopeAngleRadians <= 0.0F
			|| !std::isfinite(request.mass) || request.mass <= 0.0F || !std::isfinite(request.maximumStrength) || request.maximumStrength <= 0.0F
			|| !std::isfinite(request.penetrationRecoverySpeed) || request.penetrationRecoverySpeed < 0.0F
			|| !std::isfinite(request.groundAdhesion) || request.groundAdhesion < 0.0F
			|| !std::isfinite(request.gravityScale) || !std::isfinite(request.jumpSpeed) || request.jumpSpeed < 0.0F
			|| request.collisionLayer >= kJoltObjectLayerCount) return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The virtual-character request is invalid or stale.");
		const std::uint64_t key = HandleKey(request.handle);
		if(impl->characters.contains(key)) return Rejected(PhysicsBackendErrorCode::InvalidState, "The character handle is already populated.");
		PhysicsBackendResult staged = impl->EnsureStaging(staging, impl->worldConfiguration.world);
		if(!staged.Succeeded()) return staged;
		try {
			JPH::ShapeSettings::ShapeResult shape = request.shapeKind == PhysicsShapeKind::Capsule
				? JPH::CapsuleShapeSettings(0.5F * (request.height - 2.0F * request.radius), request.radius).Create()
				: request.shapeKind == PhysicsShapeKind::Cylinder
					? JPH::CylinderShapeSettings(0.5F * request.height, request.radius).Create()
					: JPH::ShapeSettings::ShapeResult{};
			if(!shape.IsValid()) return Rejected(PhysicsBackendErrorCode::Unsupported, "A virtual character requires a valid capsule or cylinder shape.");
			JPH::CharacterVirtualSettings settings;
			settings.mShape = shape.Get(); settings.mShapeOffset = ToJoltVector(request.localCenter); settings.mUp = ToNormalizedJoltAxis(request.upDirection);
			// CharacterVirtual models its support region as a plane, not a finite
			// cylinder. Preserve the authored radius in the provider record while the
			// height maps to Jolt's plane offset; a later finite-support provider can
			// consume the radius without changing this engine packet.
			settings.mSupportingVolume = JPH::Plane(settings.mUp, -std::max(0.0F, request.supportVolumeHeight));
			settings.mMaxSlopeAngle = request.maximumSlopeAngleRadians; settings.mCharacterPadding = request.contactPadding;
			settings.mMass = request.mass; settings.mMaxStrength = request.maximumStrength; settings.mPenetrationRecoverySpeed = request.penetrationRecoverySpeed;
			auto character = std::make_unique<JPH::CharacterVirtual>(&settings, ToJoltPosition(request.pose.position), ToJoltRotation(request.pose.rotation), impl->physicsSystem.get());
			impl->characters.emplace(key, Impl::CharacterRecord{request.handle, request.objectId, request.objectGeneration, request.componentGeneration,
				request.shapeKind, request.radius, request.height, request, std::move(character), request.enabled, false, staging.value});
			impl->staging.at(staging.value).characterKeys.push_back(key);
			return Success("The Jolt virtual character was staged without update visibility.");
		} catch(const std::exception& exception) {
			return ProviderFailure(std::string("Jolt virtual-character creation failed: ") + exception.what());
		}
	}

	PhysicsBackendResult JoltPhysicsBackend::UpdateCharacter(const PhysicsCharacterUpdateRequest& request) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(request.handle, impl->worldConfiguration.world)
			|| !request.desiredVelocity.IsFinite() || !request.pose.IsFinite() || !IsUsableRotation(request.pose.rotation)
			|| !std::isfinite(request.fixedDeltaTime) || request.fixedDeltaTime <= 0.0F) return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The virtual-character update is invalid or stale.");
		auto found = impl->characters.find(HandleKey(request.handle));
		if(found == impl->characters.end() || found->second.handle != request.handle || found->second.stagingToken != 0U
			|| found->second.objectId != request.objectId || found->second.objectGeneration != request.objectGeneration || found->second.componentGeneration != request.componentGeneration) return Rejected(PhysicsBackendErrorCode::StaleHandle, "The virtual character identity is stale.");
		auto& record = found->second;
		record.enabled = request.enabled;
		if(!record.enabled) return Success("The disabled virtual character retained its last published state.");
		// Jolt documents character-vs-character callbacks as non-thread-safe. This
		// provider boundary is therefore serialized under the backend mutex; Jolt's
		// rigid-body solver still uses the engine-owned ThreadPool job adapter.
		JPH::DefaultBroadPhaseLayerFilter broadPhase(impl->objectVsBroadPhaseFilter, static_cast<JPH::ObjectLayer>(record.settings.collisionLayer));
		class CharacterObjectLayerFilter final : public JPH::ObjectLayerFilter {
		public:
			CharacterObjectLayerFilter(const TerraObjectLayerPairFilter& pairs, JPH::ObjectLayer source, std::uint32_t mask)
				: pairs(pairs), source(source), mask(mask) { }
			bool ShouldCollide(JPH::ObjectLayer candidate) const override {
				return candidate < 32U && (mask & (1U << candidate)) != 0U && pairs.ShouldCollide(source, candidate);
			}
		private:
			const TerraObjectLayerPairFilter& pairs;
			JPH::ObjectLayer source;
			std::uint32_t mask;
		} objectLayer(impl->objectPairFilter, static_cast<JPH::ObjectLayer>(record.settings.collisionLayer), record.settings.collisionMask);
		JPH::BodyFilter bodyFilter;
		JPH::ShapeFilter shapeFilter;
		if(request.teleportRequested) {
			record.character->SetPosition(ToJoltPosition(request.pose.position)); record.character->SetRotation(ToJoltRotation(request.pose.rotation));
			record.character->RefreshContacts(broadPhase, objectLayer, bodyFilter, shapeFilter, *impl->temporaryAllocator);
		}
		if(request.resizeRequested) {
			if(!std::isfinite(request.resizeHeight) || request.resizeHeight < 2.0F * record.radius) return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The requested character height is invalid.");
			JPH::ShapeSettings::ShapeResult shape = record.shapeKind == PhysicsShapeKind::Capsule
				? JPH::CapsuleShapeSettings(0.5F * (request.resizeHeight - 2.0F * record.radius), record.radius).Create()
				: JPH::CylinderShapeSettings(0.5F * request.resizeHeight, record.radius).Create();
			if(!shape.IsValid() || !record.character->SetShape(shape.Get(), request.recoverPenetration ? FLT_MAX : 0.0F, broadPhase, objectLayer, bodyFilter, shapeFilter, *impl->temporaryAllocator)) return Rejected(PhysicsBackendErrorCode::ProviderFailure, "Jolt rejected the requested character resize.");
			record.height = request.resizeHeight;
		}
		const JPH::Vec3 up = ToNormalizedJoltAxis(record.settings.upDirection);
		JPH::Vec3 velocity = ToJoltVector(request.desiredVelocity);
		if(record.settings.useGravity) velocity += ToJoltVector(impl->worldConfiguration.settings.gravity) * record.settings.gravityScale * request.fixedDeltaTime;
		if(record.character->IsSupported()) velocity += record.character->GetGroundVelocity();
		if(request.jumpRequested) velocity += up * (request.jumpSpeed > 0.0F ? request.jumpSpeed : record.settings.jumpSpeed);
		record.character->SetLinearVelocity(velocity);
		JPH::CharacterVirtual::ExtendedUpdateSettings settings;
		settings.mStickToFloorStepDown = -up * record.settings.groundAdhesion;
		settings.mWalkStairsStepUp = record.settings.allowStairWalking ? up * record.settings.stepHeight : JPH::Vec3::sZero();
		record.character->ExtendedUpdate(request.fixedDeltaTime, ToJoltVector(impl->worldConfiguration.settings.gravity) * record.settings.gravityScale,
			settings, broadPhase, objectLayer, bodyFilter, shapeFilter, *impl->temporaryAllocator);
		return Success("The serialized Jolt virtual-character update completed.");
	}

	PhysicsBackendResult JoltPhysicsBackend::DestroyCharacter(PhysicsCharacterHandle handle) {
		std::lock_guard lock(impl->mutex);
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(handle, impl->worldConfiguration.world)) return Rejected(PhysicsBackendErrorCode::StaleHandle, "The character handle is stale.");
		auto found = impl->characters.find(HandleKey(handle));
		if(found == impl->characters.end() || found->second.handle != handle) return Rejected(PhysicsBackendErrorCode::StaleHandle, "The character handle was not found.");
		impl->characters.erase(found);
		impl->RebuildCharacterOrder();
		return Success("The virtual character was destroyed before provider-world teardown.");
	}

	PhysicsBackendResult JoltPhysicsBackend::ApplyCommand(const PhysicsCommand& command) {
		std::unique_lock lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || command.worldId != impl->worldConfiguration.world.worldId
			|| command.worldGeneration != impl->worldConfiguration.world.worldGeneration) {
			return Rejected(PhysicsBackendErrorCode::StaleWorld, "The command targets a stale Jolt world.");
		}
		if(command.type == PhysicsCommandType::ResetConstraint) {
			const PhysicsConstraintResetPayload* reset = std::get_if<PhysicsConstraintResetPayload>(&command.payload);
			Impl::ConstraintRecord* constraint = impl->FindConstraint(command.constraint);
			if(reset == nullptr || constraint == nullptr || command.recordId != constraint->recordId
				|| command.recordGeneration != constraint->recordGeneration || command.targetObjectId != constraint->ownerObjectId
				|| command.objectGeneration != constraint->ownerObjectGeneration || command.componentGeneration != constraint->componentGeneration
				|| reset->resetGeneration <= constraint->resetGeneration) {
				return Rejected(PhysicsBackendErrorCode::StaleHandle, "The constraint reset identity or monotonic generation is stale.");
			}
			constraint->resetGeneration = reset->resetGeneration;
			constraint->enabled = true;
			if(!constraint->added) { impl->physicsSystem->AddConstraint(constraint->constraint); constraint->added = true; }
			return Success("The newer constraint reset generation re-enabled its Jolt solver record.");
		}
		if(command.type == PhysicsCommandType::MoveCharacter || command.type == PhysicsCommandType::JumpCharacter
			|| command.type == PhysicsCommandType::ResizeCharacter
			|| (command.type == PhysicsCommandType::Teleport && command.character.IsValid())) {
			const PhysicsCharacterMotionPayload* payload = std::get_if<PhysicsCharacterMotionPayload>(&command.payload);
			if(payload == nullptr) return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The character command payload is missing.");
			PhysicsCharacterUpdateRequest update;
			update.handle = command.character; update.objectId = command.targetObjectId; update.objectGeneration = command.objectGeneration;
			update.componentGeneration = command.componentGeneration; update.desiredVelocity = payload->desiredVelocity; update.pose = payload->pose;
			update.fixedDeltaTime = impl->worldConfiguration.settings.fixedTimeStep;
			update.jumpRequested = command.type == PhysicsCommandType::JumpCharacter; update.jumpSpeed = payload->jumpSpeed;
			update.resizeRequested = command.type == PhysicsCommandType::ResizeCharacter; update.resizeHeight = payload->resizeHeight;
			update.teleportRequested = command.type == PhysicsCommandType::Teleport;
			// UpdateCharacter owns the serial character boundary. Drop this outer lock
			// before delegating so the worker command never falls through body lookup.
			lock.unlock();
			return UpdateCharacter(update);
		}
		Impl::BodyRecord* record = impl->FindBodyByObject(command.targetObjectId);
		// Validate both object and component generations before touching provider state so a
		// recycled UUID slot cannot accept a delayed command from an earlier live object.
		if(record == nullptr || (command.objectGeneration != 0U
			&& command.objectGeneration != record->objectGeneration)
			|| (command.componentGeneration != 0U
			&& command.componentGeneration != record->componentGeneration)) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The command targets an absent or stale Jolt body.");
		}
		if(command.effectiveTick != 0U) impl->contactListener.BeginTick(command.effectiveTick);
		JPH::BodyInterface& bodies = impl->physicsSystem->GetBodyInterface();
		switch(command.type) {
		case PhysicsCommandType::WakeBody:
			if(bodies.IsActive(record->bodyId)) return Success("Redundant wake command was suppressed.");
			bodies.ActivateBody(record->bodyId);
			break;
		case PhysicsCommandType::SleepBody:
			if(!bodies.IsActive(record->bodyId)) return Success("Redundant sleep command was suppressed.");
			bodies.DeactivateBody(record->bodyId);
			break;
		case PhysicsCommandType::ApplyForce:
		case PhysicsCommandType::ApplyAcceleration:
		case PhysicsCommandType::ApplyImpulse:
		case PhysicsCommandType::ApplyVelocityChange: {
			if(record->motionType != PhysicsMotionType::Dynamic || record->fullyLocked) {
				return Rejected(PhysicsBackendErrorCode::InvalidState,
					"Forces and impulses require a dynamic body with at least one unlocked degree of freedom.");
			}
			const PhysicsForcePayload* force = std::get_if<PhysicsForcePayload>(&command.payload);
			if(force == nullptr || !force->value.IsFinite() || !force->point.IsFinite()) {
				return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The force command payload is invalid.");
			}
			const bool impulse = command.type == PhysicsCommandType::ApplyImpulse
				|| command.type == PhysicsCommandType::ApplyVelocityChange;
			const bool massIndependent = command.type == PhysicsCommandType::ApplyAcceleration
				|| command.type == PhysicsCommandType::ApplyVelocityChange;
			const JPH::Vec3 value = ToJoltVector(force->value)
				* (massIndependent ? record->mass : 1.0F);
			if(impulse) {
				if(force->applyAtPoint) bodies.AddImpulse(record->bodyId, value, ToJoltPosition(force->point));
				else bodies.AddImpulse(record->bodyId, value);
			} else {
				if(force->applyAtPoint) bodies.AddForce(record->bodyId, value, ToJoltPosition(force->point));
				else bodies.AddForce(record->bodyId, value);
			}
			break;
		}
		case PhysicsCommandType::SetVelocity:
		case PhysicsCommandType::Teleport:
		case PhysicsCommandType::SetKinematicTarget: {
			const PhysicsMotionPayload* motion = std::get_if<PhysicsMotionPayload>(&command.payload);
			if(motion == nullptr || !motion->pose.IsFinite()
				|| !IsUsableRotation(motion->pose.rotation)
				|| !motion->linearVelocity.IsFinite() || !motion->angularVelocity.IsFinite()) {
				return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The motion command payload is invalid.");
			}
			if(command.type == PhysicsCommandType::SetKinematicTarget) {
				if(record->motionType != PhysicsMotionType::Kinematic
					|| !std::isfinite(motion->duration) || motion->duration <= 0.0F) {
					return Rejected(PhysicsBackendErrorCode::InvalidState,
						"Kinematic targets require a kinematic body and a positive fixed duration.");
				}
				JPH::RVec3 currentPosition;
				JPH::Quat currentRotation;
				bodies.GetPositionAndRotation(record->bodyId, currentPosition, currentRotation);
				const PhysicsVector3 currentLinearVelocity = FromJoltVector(bodies.GetLinearVelocity(record->bodyId));
				if(impl->NearlySame(PhysicsPose{FromJoltPosition(currentPosition), FromJoltRotation(currentRotation)}, motion->pose)
					&& impl->NearlySame(currentLinearVelocity, PhysicsVector3{})) {
					return Success("Unchanged kinematic target was suppressed.");
				}
				bodies.MoveKinematic(
					record->bodyId,
					ToJoltPosition(motion->pose.position),
					ToJoltRotation(motion->pose.rotation),
					motion->duration);
				break;
			}
			if(command.type == PhysicsCommandType::SetVelocity) {
				JPH::Vec3 currentLinear;
				JPH::Vec3 currentAngular;
				bodies.GetLinearAndAngularVelocity(record->bodyId, currentLinear, currentAngular);
				if(impl->NearlySame(FromJoltVector(currentLinear), motion->linearVelocity)
					&& impl->NearlySame(FromJoltVector(currentAngular), motion->angularVelocity)) {
					return Success("Unchanged velocity command was suppressed.");
				}
			}
			if(command.type == PhysicsCommandType::Teleport) {
				bodies.SetPositionAndRotation(record->bodyId,
					ToJoltPosition(motion->pose.position), ToJoltRotation(motion->pose.rotation),
					JPH::EActivation::Activate);
			}
			bodies.SetLinearAndAngularVelocity(record->bodyId,
				ToJoltVector(motion->linearVelocity), ToJoltVector(motion->angularVelocity));
			break;
		}
		default:
			return Rejected(PhysicsBackendErrorCode::Unsupported,
				"This typed command is handled by a later backend story or a coarse lifecycle API.");
		}
		// Command-side changes must remain publishable even when the body is static or
		// goes to sleep before extraction. Active simulation membership is supplied by
		// Jolt; this compact set covers the complementary changed-body membership.
		record->pendingChange |= PhysicsBodyStateChangeMask::Updated;
		if(command.type == PhysicsCommandType::WakeBody) {
			record->pendingChange |= PhysicsBodyStateChangeMask::Awakened;
			impl->pendingCommandTransitions[HandleKey(record->handle)] = PhysicsEventType::Activated;
		} else if(command.type == PhysicsCommandType::SleepBody) {
			record->pendingChange |= PhysicsBodyStateChangeMask::Slept;
			impl->pendingCommandTransitions[HandleKey(record->handle)] = PhysicsEventType::Slept;
		}
		impl->dirtyBodyKeys.insert(HandleKey(record->handle));
		return impl->CheckProviderHealth()
			? Success("The typed command was applied at the protected boundary.")
			: ProviderFailure("Jolt reported a fatal failure while applying a command.", true);
	}

	PhysicsBackendResult JoltPhysicsBackend::Step(const PhysicsBackendStepRequest& request) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(request.world, impl->worldConfiguration.world)
			|| request.tick == 0U || !std::isfinite(request.deltaTime) || request.deltaTime <= 0.0F
			|| request.collisionSteps == 0U) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The fixed-step request is invalid or stale.");
		}
		try {
			PhysicsBackendProfileScope profile("Physics: Jolt Fixed Step");
			const std::size_t firstNewEvent = impl->pendingEvents.size();
			impl->contactListener.BeginTick(request.tick);
			const JPH::EPhysicsUpdateError errors = impl->physicsSystem->Update(
				request.deltaTime,
				static_cast<int>(request.collisionSteps),
				impl->temporaryAllocator.get(),
				impl->jobAdapter.get());
			impl->jobAdapter->WaitForIdle();
			std::uint64_t dropped = 0U;
			impl->ConvertRawEvents(impl->contactListener.Drain(dropped), dropped);
			impl->PublishCommandTransitions(request.tick, firstNewEvent);
			for(std::size_t index = firstNewEvent; index < impl->pendingEvents.size(); ++index) {
				const PhysicsEvent& event = impl->pendingEvents[index];
				Impl::BodyRecord* body = impl->FindBody(event.first.body);
				if(body == nullptr) continue;
				if(event.type == PhysicsEventType::Activated) {
					body->pendingChange |= PhysicsBodyStateChangeMask::Awakened;
				} else if(event.type == PhysicsEventType::Slept) {
					body->pendingChange |= PhysicsBodyStateChangeMask::Slept;
				}
				impl->dirtyBodyKeys.insert(HandleKey(body->handle));
			}
			if(errors != JPH::EPhysicsUpdateError::None) {
				return ProviderFailure("Jolt exhausted one or more configured contact/body-pair capacities.");
			}
			return impl->CheckProviderHealth()
				? Success("The Jolt fixed step completed and all jobs joined.")
				: ProviderFailure("Jolt reported a fatal failure during simulation.", true);
		} catch(const std::exception& exception) {
			impl->healthy.store(false, std::memory_order_release);
			return ProviderFailure(std::string("Jolt simulation failed: ") + exception.what(), true);
		} catch(...) {
			impl->healthy.store(false, std::memory_order_release);
			return ProviderFailure("Jolt simulation failed with an unknown exception.", true);
		}
	}

	PhysicsBackendQueryResult JoltPhysicsBackend::Query(const PhysicsBackendQueryRequest& request) const {
		std::shared_lock lock(impl->mutex);
		return QueryLocked(request);
	}

	PhysicsBackendResult JoltPhysicsBackend::QueryBatch(
		std::span<const PhysicsBackendQueryRequest> requests,
		std::span<PhysicsBackendQueryResult> results) const {
		if(requests.size() != results.size()) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument,
				"The Jolt query batch has mismatched request and result spans.");
		}
		// Simulation has already joined before query publication. Holding one shared
		// lock across this immutable packet removes per-query lock traffic without
		// allowing lifecycle mutation to interleave between members.
		std::shared_lock lock(impl->mutex);
		for(std::size_t index = 0U; index < requests.size(); ++index) {
			results[index] = QueryLocked(requests[index]);
		}
		return Success("The bounded Jolt query batch completed.");
	}

	PhysicsBackendQueryResult JoltPhysicsBackend::QueryLocked(
		const PhysicsBackendQueryRequest& request) const {
		PhysicsBackendQueryResult result;
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) {
			result.result = ready;
			return result;
		}
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(request.world, impl->worldConfiguration.world)
			|| !request.pose.IsFinite() || !request.direction.IsFinite()
			|| !IsUsableRotation(request.pose.rotation)
			|| !std::isfinite(request.distance) || request.distance < 0.0F
			|| request.maximumHits == 0U
			|| request.maximumHits > impl->worldConfiguration.settings.maxHitsPerQuery
			|| request.filter.bodyTypes == PhysicsQueryBodyTypeMask::None) {
			result.result = Rejected(PhysicsBackendErrorCode::InvalidArgument, "The query request is invalid or stale.");
			return result;
		}

		const float directionLength = std::sqrt(
			request.direction.x * request.direction.x
			+ request.direction.y * request.direction.y
			+ request.direction.z * request.direction.z);
		const bool cast = request.kind == PhysicsQueryKind::RayCast
			|| request.kind == PhysicsQueryKind::ShapeCast;
		const bool shaped = request.kind == PhysicsQueryKind::ShapeCast
			|| request.kind == PhysicsQueryKind::Overlap;
		if((cast && (directionLength <= 0.0F || request.distance <= 0.0F
				|| request.distance > impl->worldConfiguration.settings.maxCastDistance))
			|| (shaped && (!request.shapeExtent.IsFinite()
				|| request.shapeExtent.x <= 0.0F || request.shapeExtent.y <= 0.0F || request.shapeExtent.z <= 0.0F
				|| request.shapeExtent.x > impl->worldConfiguration.settings.maxShapeExtent
				|| request.shapeExtent.y > impl->worldConfiguration.settings.maxShapeExtent
				|| request.shapeExtent.z > impl->worldConfiguration.settings.maxShapeExtent))) {
			result.result = Rejected(PhysicsBackendErrorCode::InvalidArgument,
				"The query direction, distance, or shape extent is outside project limits.");
			return result;
		}

		class TerraQueryBodyFilter final : public JPH::BodyFilter {
		public:
			TerraQueryBodyFilter(const Impl& owner, const PhysicsQueryFilter& filter)
				: owner(owner), filter(filter) {
			}

			bool ShouldCollide(const JPH::BodyID& bodyId) const override {
				const Impl::BodyRecord* body = owner.FindBodyByJoltId(bodyId);
				if(body == nullptr || (!filter.includeSensors && body->sensor)) return false;
				if(filter.excludedBody.IsValid() && body->handle == filter.excludedBody) return false;
				if(filter.excludedObjectId != TerraEngine::EMPTY_UUID
					&& body->objectId == filter.excludedObjectId
					&& (filter.excludedObjectGeneration == 0U
						|| filter.excludedObjectGeneration == body->objectGeneration)) return false;
				switch(body->motionType) {
				case PhysicsMotionType::Static:
					return HasFlag(filter.bodyTypes, PhysicsQueryBodyTypeMask::Static);
				case PhysicsMotionType::Dynamic:
					return HasFlag(filter.bodyTypes, PhysicsQueryBodyTypeMask::Dynamic);
				case PhysicsMotionType::Kinematic:
					return HasFlag(filter.bodyTypes, PhysicsQueryBodyTypeMask::Kinematic);
				}
				return false;
			}

			bool ShouldCollideLocked(const JPH::Body& body) const override {
				return ShouldCollide(body.GetID());
			}

		private:
			const Impl& owner;
			const PhysicsQueryFilter& filter;
		};

		const QueryObjectLayerFilter objectFilter(request.filter.collisionMask);
		const TerraQueryBodyFilter bodyFilter(*impl, request.filter);
		const JPH::NarrowPhaseQuery& query = impl->physicsSystem->GetNarrowPhaseQuery();
		const JPH::BodyInterface& bodies = impl->physicsSystem->GetBodyInterface();
		const std::uint32_t providerLimit = request.hitMode == PhysicsQueryHitMode::All
			? request.maximumHits : 1U;
		result.hits.reserve(providerLimit);

		const auto appendHit = [&](
			JPH::BodyID bodyId,
			JPH::SubShapeID subShape,
			PhysicsVector3 position,
			PhysicsVector3 normal,
			float distance,
			float fraction) {
			const Impl::BodyRecord* body = impl->FindBodyByJoltId(bodyId);
			if(body == nullptr) return;
			const std::uint64_t subShapeData = bodies.GetShape(bodyId)->GetSubShapeUserData(subShape);
			const bool sensor = body->sensor || (subShapeData & (1ULL << 63U)) != 0ULL;
			if(sensor && !request.filter.includeSensors) return;
			PhysicsBackendQueryHit converted;
			converted.body = body->handle;
			converted.objectId = body->objectId;
			converted.objectGeneration = body->objectGeneration;
			converted.componentGeneration = body->componentGeneration;
			converted.position = position;
			converted.normal = normal;
			converted.distance = std::max(0.0F, distance);
			converted.fraction = std::max(0.0F, fraction);
			converted.subShapeKey = subShape.GetValue();
			converted.collisionLayer = body->collisionLayer;
			converted.motionType = body->motionType;
			converted.sensor = sensor;
			result.hits.push_back(std::move(converted));
		};

		const auto surfaceNormal = [&](JPH::BodyID bodyId, JPH::SubShapeID subShape, JPH::RVec3Arg position) {
			return FromJoltVector(bodies.GetTransformedShape(bodyId)
				.GetWorldSpaceSurfaceNormal(subShape, position));
		};

		if(request.kind == PhysicsQueryKind::RayCast) {
			const PhysicsVector3 normalized{
				request.direction.x / directionLength,
				request.direction.y / directionLength,
				request.direction.z / directionLength};
			const JPH::RRayCast ray(
				ToJoltPosition(request.pose.position), ToJoltVector(normalized) * request.distance);
			JPH::RayCastSettings settings;
			if(request.hitMode == PhysicsQueryHitMode::Any) {
				JPH::AnyHitCollisionCollector<JPH::CastRayCollector> collector;
				query.CastRay(ray, settings, collector, {}, objectFilter, bodyFilter);
				if(collector.HadHit()) {
					const JPH::RVec3 position = ray.GetPointOnRay(collector.mHit.mFraction);
					appendHit(collector.mHit.mBodyID, collector.mHit.mSubShapeID2,
						FromJoltPosition(position), surfaceNormal(collector.mHit.mBodyID, collector.mHit.mSubShapeID2, position),
						collector.mHit.mFraction * request.distance, collector.mHit.mFraction);
				}
			} else if(request.hitMode == PhysicsQueryHitMode::Closest) {
				JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
				query.CastRay(ray, settings, collector, {}, objectFilter, bodyFilter);
				if(collector.HadHit()) {
					const JPH::RVec3 position = ray.GetPointOnRay(collector.mHit.mFraction);
					appendHit(collector.mHit.mBodyID, collector.mHit.mSubShapeID2,
						FromJoltPosition(position), surfaceNormal(collector.mHit.mBodyID, collector.mHit.mSubShapeID2, position),
						collector.mHit.mFraction * request.distance, collector.mHit.mFraction);
				}
			} else {
				BoundedHitCollector<JPH::CastRayCollector> collector(providerLimit);
				query.CastRay(ray, settings, collector, {}, objectFilter, bodyFilter);
				result.truncated = collector.truncated;
				for(const JPH::RayCastResult& hit : collector.hits) {
					const JPH::RVec3 position = ray.GetPointOnRay(hit.mFraction);
					appendHit(hit.mBodyID, hit.mSubShapeID2, FromJoltPosition(position),
						surfaceNormal(hit.mBodyID, hit.mSubShapeID2, position),
						hit.mFraction * request.distance, hit.mFraction);
				}
			}
		} else if(request.kind == PhysicsQueryKind::ShapeCast || request.kind == PhysicsQueryKind::Overlap) {
			JPH::ShapeSettings::ShapeResult shapeResult = JPH::BoxShapeSettings(ToJoltVector(request.shapeExtent)).Create();
			if(!shapeResult.IsValid()) {
				result.result = ProviderFailure("Jolt rejected the bounded query box shape.");
				return result;
			}
			const JPH::RefConst<JPH::Shape> shape = shapeResult.Get();
			const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
				ToJoltRotation(request.pose.rotation), ToJoltPosition(request.pose.position));
			if(request.kind == PhysicsQueryKind::ShapeCast) {
				const PhysicsVector3 normalized{
					request.direction.x / directionLength,
					request.direction.y / directionLength,
					request.direction.z / directionLength};
				const JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
					shape, JPH::Vec3::sOne(), transform, ToJoltVector(normalized) * request.distance);
				JPH::ShapeCastSettings settings;
				BoundedHitCollector<JPH::CastShapeCollector> collector(providerLimit);
				query.CastShape(shapeCast, settings, JPH::RVec3::sZero(), collector, {}, objectFilter, bodyFilter);
				result.truncated = collector.truncated;
				for(const JPH::ShapeCastResult& hit : collector.hits) {
					JPH::Vec3 normal = -hit.mPenetrationAxis;
					if(normal.LengthSq() > 1.0e-12F) normal = normal.Normalized();
					else normal = ToJoltVector(surfaceNormal(hit.mBodyID2, hit.mSubShapeID2, JPH::RVec3(hit.mContactPointOn2)));
					appendHit(hit.mBodyID2, hit.mSubShapeID2, FromJoltVector(hit.mContactPointOn2),
						FromJoltVector(normal), hit.mFraction * request.distance, hit.mFraction);
				}
			} else {
				JPH::CollideShapeSettings settings;
				BoundedHitCollector<JPH::CollideShapeCollector> collector(providerLimit);
				query.CollideShape(shape, JPH::Vec3::sOne(), transform, settings,
					JPH::RVec3::sZero(), collector, {}, objectFilter, bodyFilter);
				result.truncated = collector.truncated;
				for(const JPH::CollideShapeResult& hit : collector.hits) {
					JPH::Vec3 normal = -hit.mPenetrationAxis;
					if(normal.LengthSq() > 1.0e-12F) normal = normal.Normalized();
					else normal = ToJoltVector(surfaceNormal(hit.mBodyID2, hit.mSubShapeID2, JPH::RVec3(hit.mContactPointOn2)));
					appendHit(hit.mBodyID2, hit.mSubShapeID2, FromJoltVector(hit.mContactPointOn2),
						FromJoltVector(normal), 0.0F, 0.0F);
				}
			}
		} else if(request.kind == PhysicsQueryKind::Point) {
			BoundedHitCollector<JPH::CollidePointCollector> collector(providerLimit);
			const JPH::RVec3 point = ToJoltPosition(request.pose.position);
			query.CollidePoint(point, collector, {}, objectFilter, bodyFilter);
			result.truncated = collector.truncated;
			for(const JPH::CollidePointResult& hit : collector.hits) {
				appendHit(hit.mBodyID, hit.mSubShapeID2, request.pose.position,
					surfaceNormal(hit.mBodyID, hit.mSubShapeID2, point), 0.0F, 0.0F);
			}
		}

		std::sort(result.hits.begin(), result.hits.end(), [](const PhysicsBackendQueryHit& left, const PhysicsBackendQueryHit& right) {
			if(left.distance != right.distance) return left.distance < right.distance;
			if(left.objectId != right.objectId) return left.objectId < right.objectId;
			return left.subShapeKey < right.subShapeKey;
		});
		result.result = Success("The bounded Jolt 5.6 query completed.");
		return result;
	}

	PhysicsBackendResult JoltPhysicsBackend::Extract(PhysicsBackendExtraction& extraction) const {
		PhysicsBackendExtractionRequest request;
		request.world = extraction.world;
		request.tick = extraction.tick;
		request.selection = PhysicsBackendExtractionSelection::FullState;
		request.fields = PhysicsBodyStateFieldMask::All;
		request.fullState = true;
		return Extract(request, extraction);
	}

	PhysicsBackendResult JoltPhysicsBackend::ReserveAdmission(
		const PhysicsBackendAdmissionReservation& reservation,
		PhysicsBackendPreparedAdmission& prepared) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(!reservation.IsValid() || reservation.world != impl->worldConfiguration.world) {
			return Rejected(PhysicsBackendErrorCode::StaleWorld,
				"The provider admission reservation targets an invalid or stale world.");
		}
		// Reject impossible body capacity before creating a staging token or growing
		// any container. This makes reservation failure atomic and prevents a hostile
		// or accidental oversized batch from allocating memory it can never commit.
		const std::uint64_t requestedBodies = static_cast<std::uint64_t>(impl->bodies.size())
			+ reservation.bodyCount;
		if(requestedBodies > impl->worldConfiguration.maximumBodies) {
			return Rejected(PhysicsBackendErrorCode::CapacityExceeded,
				"The provider admission reservation exceeds the active world's body capacity.");
		}
		PhysicsBackendStagingToken staging;
		const PhysicsBackendResult staged = impl->EnsureStaging(staging, reservation.world);
		if(!staged.Succeeded()) return staged;
		auto found = impl->staging.find(staging.value);
		if(found == impl->staging.end()) return ProviderFailure("The reserved staging record was not created.");
		found->second.shapeKeys.reserve(reservation.shapeCount);
		found->second.bodyKeys.reserve(reservation.bodyCount);
		found->second.constraintKeys.reserve(reservation.constraintCount);
		found->second.characterKeys.reserve(reservation.characterCount);
		impl->shapes.reserve(impl->shapes.size() + reservation.shapeCount);
		impl->bodies.reserve(impl->bodies.size() + reservation.bodyCount);
		// These indexes become visible only after CommitStaging, but reserving their
		// exact copied admission capacity here prevents repeated rehashes while a
		// large transaction transfers its already-staged body identities.
		impl->bodyIdToKey.reserve(impl->bodyIdToKey.size() + reservation.bodyCount);
		impl->bodyObjectToKey.reserve(impl->bodyObjectToKey.size() + reservation.bodyCount);
		impl->dirtyBodyKeys.reserve(impl->dirtyBodyKeys.size() + reservation.bodyCount);
		impl->pendingCommandTransitions.reserve(
			impl->pendingCommandTransitions.size() + reservation.bodyCount);
		impl->bodyOrder.reserve(impl->bodyOrder.size() + reservation.bodyCount);
		impl->bodyOrderIndices.reserve(impl->bodyOrderIndices.size() + reservation.bodyCount);
		impl->constraints.reserve(impl->constraints.size() + reservation.constraintCount);
		impl->characters.reserve(impl->characters.size() + reservation.characterCount);
		impl->constraintOrder.reserve(impl->constraintOrder.size() + reservation.constraintCount);
		impl->characterOrder.reserve(impl->characterOrder.size() + reservation.characterCount);
		prepared.staging = staging;
		prepared.reservedShapes = reservation.shapeCount;
		prepared.reservedBodies = reservation.bodyCount;
		prepared.reservedConstraints = reservation.constraintCount;
		prepared.reservedCharacters = reservation.characterCount;
		// Four staging vectors, four primary maps, six body publication indexes, and
		// two stable secondary orders are the engine-owned reservation sites.
		prepared.stagingAllocationCount = 16U;
		return Success("Provider admission storage was reserved before staging dependencies.");
	}

	PhysicsBackendResult JoltPhysicsBackend::Extract(
		const PhysicsBackendFixedStepWorkProfile& profile,
		PhysicsBackendExtraction& extraction,
		PhysicsBackendExtractionToken& token) const {
		if(!profile.world.IsValid() || profile.tick == 0U
			|| profile.fields == PhysicsBodyStateFieldMask::None || profile.rangeSize == 0U) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument,
				"The fixed-step extraction profile is invalid.");
		}

		PhysicsBackendExtractionRequest request;
		std::vector<std::uint32_t> selectedIndices;
		std::unique_lock lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded() || impl->physicsSystem == nullptr
			|| !HandleMatchesWorld(profile.world, impl->worldConfiguration.world)
			|| (profile.providerOrderGeneration != 0U
				&& profile.providerOrderGeneration != impl->bodyOrderGeneration)) {
			return Rejected(PhysicsBackendErrorCode::StaleWorld,
				"The extraction profile no longer matches the current provider order.");
		}
		selectedIndices = profile.activeStableIndices;
		selectedIndices.insert(selectedIndices.end(), profile.dirtyStableIndices.begin(), profile.dirtyStableIndices.end());
		// Jolt exposes its active membership directly. Combining that compact list
		// with the provider's dirty-key index removes the former whole-world scan
		// while retaining sleeping/static changes and caller-required bodies.
		impl->physicsSystem->GetActiveBodies(JPH::EBodyType::RigidBody, impl->activeBodiesScratch);
		for(const JPH::BodyID bodyId : impl->activeBodiesScratch) {
			if(const Impl::BodyRecord* body = impl->FindBodyByJoltId(bodyId); body != nullptr) {
				if(const auto found = impl->bodyOrderIndices.find(HandleKey(body->handle));
					found != impl->bodyOrderIndices.end()) selectedIndices.push_back(found->second);
			}
		}
		for(const std::uint64_t bodyKey : impl->dirtyBodyKeys) {
			if(const auto found = impl->bodyOrderIndices.find(bodyKey);
				found != impl->bodyOrderIndices.end()) selectedIndices.push_back(found->second);
		}
		for(const PhysicsBodyHandle handle : profile.explicitRequiredHandles) {
			if(const auto found = impl->bodyOrderIndices.find(HandleKey(handle));
				HandleMatchesWorld(handle, impl->worldConfiguration.world)
				&& found != impl->bodyOrderIndices.end()) {
				selectedIndices.push_back(found->second);
			}
		}
		std::sort(selectedIndices.begin(), selectedIndices.end());
		selectedIndices.erase(std::unique(selectedIndices.begin(), selectedIndices.end()), selectedIndices.end());
		if(std::any_of(selectedIndices.begin(), selectedIndices.end(),
			[this](std::uint32_t index) { return index >= impl->bodyOrder.size(); })) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument,
				"The extraction profile contains an out-of-range stable index.");
		}

		request.world = profile.world;
		request.tick = profile.tick;
		// ActiveBodies has the same non-consuming retained-state behavior needed by
		// a lease; the prepared stable span below is the actual authoritative selection.
		request.selection = PhysicsBackendExtractionSelection::ActiveBodies;
		request.fields = profile.fields;
		request.batchSize = profile.rangeSize;
		request.parallelThreshold = std::max<std::uint32_t>(1U, profile.rangeSize);
		// The retryable lease copies pending events below and erases them only after
		// acknowledgement. The nested body read must not consume that queue first.
		request.drainEvents = false;
		const PhysicsBackendResult extracted = ExtractLocked(
			request, extraction, selectedIndices, true, true);
		if(!extracted.Succeeded()) return extracted;
		std::uint64_t value = impl->nextExtractionToken++;
		if(value == 0U) value = impl->nextExtractionToken++;
		token = {profile.world, profile.tick, value};
		Impl::PendingExtractionLease lease;
		lease.token = token;
		lease.includesRemovedBodies = true;
		lease.includesEvents = HasPhysicsBodyStateFields(profile.fields, PhysicsBodyStateFieldMask::Events);
		lease.removedBodyCount = impl->pendingRemovedBodies.size();
		lease.eventCount = lease.includesEvents ? impl->pendingEvents.size() : 0U;
		for(const std::uint32_t index : selectedIndices) lease.bodyKeys.push_back(impl->bodyOrder[index]);
		extraction.removedBodies = impl->pendingRemovedBodies;
		if(lease.includesEvents) {
			extraction.events = impl->pendingEvents;
			extraction.droppedEventCount = impl->droppedEvents;
			extraction.eventDrainPerformed = true;
		}
		extraction.fullState = profile.fullStateReason != PhysicsBackendFullStateReason::None;
		extraction.providerOrderGeneration = impl->bodyOrderGeneration;
		extraction.totalBodyCount = impl->bodyOrder.size();
		extraction.examinedBodyCount = selectedIndices.size();
		impl->extractionLeases.emplace(value, std::move(lease));
		return Success("Jolt state was prepared in a retryable generation-bound extraction lease.");
	}

	PhysicsBackendResult JoltPhysicsBackend::AcknowledgeExtraction(PhysicsBackendExtractionToken token) {
		std::unique_lock lock(impl->mutex);
		const auto found = impl->extractionLeases.find(token.value);
		if(!token.IsValid() || found == impl->extractionLeases.end() || found->second.token.world != token.world
			|| found->second.token.tick != token.tick) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The extraction lease is stale or already resolved.");
		}
		for(const std::uint64_t key : found->second.bodyKeys) {
			if(const auto body = impl->bodies.find(key); body != impl->bodies.end()) {
				body->second.pendingChange = PhysicsBodyStateChangeMask::None;
				impl->dirtyBodyKeys.erase(key);
			}
		}
		if(found->second.includesRemovedBodies) {
			impl->pendingRemovedBodies.erase(impl->pendingRemovedBodies.begin(),
				impl->pendingRemovedBodies.begin() + std::min(found->second.removedBodyCount, impl->pendingRemovedBodies.size()));
		}
		if(found->second.includesEvents) {
			impl->pendingEvents.erase(impl->pendingEvents.begin(),
				impl->pendingEvents.begin() + std::min(found->second.eventCount, impl->pendingEvents.size()));
			if(impl->pendingEvents.empty()) impl->droppedEvents = 0U;
			impl->lastEventDrainTick = token.tick;
		}
		impl->extractionLeases.erase(found);
		return Success("The extraction lease was acknowledged after protected publication.");
	}

	PhysicsBackendResult JoltPhysicsBackend::AbortExtraction(PhysicsBackendExtractionToken token) {
		std::unique_lock lock(impl->mutex);
		const auto found = impl->extractionLeases.find(token.value);
		if(!token.IsValid() || found == impl->extractionLeases.end() || found->second.token.world != token.world
			|| found->second.token.tick != token.tick) {
			return Rejected(PhysicsBackendErrorCode::StaleHandle, "The extraction lease is stale or already resolved.");
		}
		// No provider-owned pending state changed while this token was prepared.
		impl->extractionLeases.erase(found);
		return Success("The extraction lease was aborted and remains available for retry.");
	}

	PhysicsBackendExtractionCapabilities JoltPhysicsBackend::ExtractionCapabilities() const noexcept {
		return {true, true, true, true, 8192U};
	}

	PhysicsBackendResult JoltPhysicsBackend::Extract(
		const PhysicsBackendExtractionRequest& request,
		PhysicsBackendExtraction& extraction) const {
		return ExtractLocked(request, extraction, {}, false, false);
	}

	PhysicsBackendResult JoltPhysicsBackend::ExtractLocked(
		const PhysicsBackendExtractionRequest& request,
		PhysicsBackendExtraction& extraction,
		std::span<const std::uint32_t> preparedStableIndices,
		bool usePreparedSelection,
		bool membershipLockHeld) const {
		// Retained states and event ownership are updated by this operation, so the
		// backend takes exclusive membership ownership. Independent Jolt getter calls
		// still run through bounded worker lanes while membership remains immutable.
		std::unique_lock lock(impl->mutex, std::defer_lock);
		if(!membershipLockHeld) lock.lock();
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(!request.IsValid() || impl->physicsSystem == nullptr
			|| !HandleMatchesWorld(request.world, impl->worldConfiguration.world)) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument,
				"Extraction targets an invalid request or stale Jolt world.");
		}

		extraction = {};
		extraction.world = request.world;
		extraction.tick = request.tick;
		extraction.providerOrderGeneration = impl->bodyOrderGeneration;
		extraction.totalBodyCount = impl->bodyOrder.size();
		extraction.extractedFields = request.fields;
		extraction.fullState = request.fullState;
		const JPH::BodyInterface& bodies = impl->physicsSystem->GetBodyInterface();
		std::vector<std::uint32_t> selectedIndices;
		if(usePreparedSelection) selectedIndices.assign(
			preparedStableIndices.begin(), preparedStableIndices.end());

		auto appendStableRange = [&](PhysicsBackendStableRange range) -> bool {
			if(range.EndIndex() > impl->bodyOrder.size()) return false;
			for(std::uint64_t index = range.firstIndex; index < range.EndIndex(); ++index) {
				selectedIndices.push_back(static_cast<std::uint32_t>(index));
			}
			return true;
		};

		if(!usePreparedSelection) switch(request.selection) {
		case PhysicsBackendExtractionSelection::FullState:
			selectedIndices.resize(impl->bodyOrder.size());
			std::iota(selectedIndices.begin(), selectedIndices.end(), 0U);
			extraction.examinedBodyCount = impl->bodyOrder.size();
			break;
		case PhysicsBackendExtractionSelection::StableRanges:
			for(const PhysicsBackendStableRange range : request.stableRanges) {
				if(!appendStableRange(range)) {
					return Rejected(PhysicsBackendErrorCode::InvalidArgument,
						"A requested stable range exceeds the current provider order.");
				}
			}
			extraction.examinedBodyCount = selectedIndices.size();
			break;
		case PhysicsBackendExtractionSelection::ExplicitHandles:
			for(const PhysicsBodyHandle handle : request.explicitHandles) {
				if(!HandleMatchesWorld(handle, impl->worldConfiguration.world)) continue;
				const auto found = impl->bodyOrderIndices.find(HandleKey(handle));
				if(found != impl->bodyOrderIndices.end()) selectedIndices.push_back(found->second);
			}
			extraction.examinedBodyCount = request.explicitHandles.size();
			break;
		case PhysicsBackendExtractionSelection::ActiveBodies:
		case PhysicsBackendExtractionSelection::PublicationRelevant:
			impl->physicsSystem->GetActiveBodies(JPH::EBodyType::RigidBody, impl->activeBodiesScratch);
			for(const JPH::BodyID bodyId : impl->activeBodiesScratch) {
				if(const Impl::BodyRecord* body = impl->FindBodyByJoltId(bodyId); body != nullptr) {
					if(const auto found = impl->bodyOrderIndices.find(HandleKey(body->handle));
						found != impl->bodyOrderIndices.end()) selectedIndices.push_back(found->second);
				}
			}
			if(request.selection == PhysicsBackendExtractionSelection::PublicationRelevant) {
				for(const std::uint64_t bodyKey : impl->dirtyBodyKeys) {
					if(const auto found = impl->bodyOrderIndices.find(bodyKey);
						found != impl->bodyOrderIndices.end()) selectedIndices.push_back(found->second);
				}
			}
			// Explicit consumers such as ragdolls need sleeping mapped bodies, but
			// they must join the incremental active/dirty selection rather than force a
			// provider-wide state read. The stable lookup keeps this addition bounded.
			for(const PhysicsBodyHandle handle : request.explicitHandles) {
				if(!HandleMatchesWorld(handle, impl->worldConfiguration.world)) continue;
				const auto found = impl->bodyOrderIndices.find(HandleKey(handle));
				if(found != impl->bodyOrderIndices.end()) selectedIndices.push_back(found->second);
			}
			break;
		}
		if(!usePreparedSelection) {
			std::sort(selectedIndices.begin(), selectedIndices.end());
			selectedIndices.erase(std::unique(selectedIndices.begin(), selectedIndices.end()), selectedIndices.end());
		}
		extraction.examinedBodyCount = selectedIndices.size();
		struct ExtractionRecordSnapshot {
			std::uint32_t stableIndex = 0U;
			PhysicsBodyHandle handle;
			TerraEngine::UUID objectId{};
			PhysicsGeneration objectGeneration = 0U;
			PhysicsGeneration componentGeneration = 0U;
			JPH::BodyID bodyId;
			PhysicsBodyStateChangeMask pendingChange = PhysicsBodyStateChangeMask::None;
			PhysicsMotionType motionType = PhysicsMotionType::Static;
			float mass = 0.0F;
			std::uint32_t contactCount = 0U;
			bool retainedAwake = false;
		};
		std::vector<ExtractionRecordSnapshot> immutableRecords;
		immutableRecords.reserve(selectedIndices.size());
		for(const std::uint32_t stableIndex : selectedIndices) {
			auto found = impl->bodies.find(impl->bodyOrder[stableIndex]);
			if(found == impl->bodies.end()) {
				return ProviderFailure("Stable Jolt body order changed while preparing extraction records.");
			}
			const Impl::BodyRecord& record = found->second;
			immutableRecords.push_back({stableIndex, record.handle, record.objectId,
				record.objectGeneration, record.componentGeneration, record.bodyId,
				record.pendingChange, record.motionType, record.mass,
				record.retainedState.contactCount, record.retainedState.awake});
		}
		extraction.bodies.resize(immutableRecords.size());
		if(request.selection == PhysicsBackendExtractionSelection::PublicationRelevant
			|| request.selection == PhysicsBackendExtractionSelection::FullState) {
			extraction.removedBodies = std::move(impl->pendingRemovedBodies);
			impl->pendingRemovedBodies.clear();
		}

		const auto readRange = [&](std::size_t begin, std::size_t end) {
			for(std::size_t outputIndex = begin; outputIndex < end; ++outputIndex) {
				// Worker ranges receive only copied metadata plus provider getter access.
				// Retained-state, pending-change, and event ownership is committed below
				// after every range joins under this protected provider boundary.
				const ExtractionRecordSnapshot& record = immutableRecords[outputIndex];
				PhysicsBackendBodyState state;
				state.handle = record.handle;
				state.stableIndex = record.stableIndex;
				state.objectId = record.objectId;
				state.objectGeneration = record.objectGeneration;
				state.componentGeneration = record.componentGeneration;
				state.fields = static_cast<PhysicsBodyStateFieldMask>(
					static_cast<std::uint32_t>(request.fields)
					& static_cast<std::uint32_t>(PhysicsBodyStateFieldMask::BodyState));
				state.change = record.pendingChange;
				state.motionType = record.motionType;
				if(request.selection == PhysicsBackendExtractionSelection::ExplicitHandles) {
					state.change |= PhysicsBodyStateChangeMask::ExplicitlyRequested;
				}

				if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::Pose)) {
					JPH::RVec3 position;
					JPH::Quat rotation;
					bodies.GetPositionAndRotation(record.bodyId, position, rotation);
					state.pose = {FromJoltPosition(position), FromJoltRotation(rotation)};
				}
				if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::LinearVelocity)
					|| HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::AngularVelocity)) {
					JPH::Vec3 linearVelocity;
					JPH::Vec3 angularVelocity;
					bodies.GetLinearAndAngularVelocity(record.bodyId, linearVelocity, angularVelocity);
					if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::LinearVelocity)) {
						state.linearVelocity = FromJoltVector(linearVelocity);
					}
					if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::AngularVelocity)) {
						state.angularVelocity = FromJoltVector(angularVelocity);
					}
				}
				if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::Mass)) {
					state.mass = record.mass;
				}
				if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::ContactCount)) {
					state.contactCount = record.contactCount;
				}
				if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::Awake)) {
					state.awake = bodies.IsActive(record.bodyId);
					if(state.awake != record.retainedAwake) {
						state.change |= state.awake
							? PhysicsBodyStateChangeMask::Awakened
							: PhysicsBodyStateChangeMask::Slept;
					}
				}
				extraction.bodies[outputIndex] = state;
			}
		};

		// The adapter owns the safety fallback: one worker, a small selection,
		// disabled provider capability, or an explicit request all execute the same
		// immutable reader inline. Only the range reads parallelize; provider event
		// draining and retained-state acknowledgement remain protected below.
		const PhysicsIndependentRangeExecution execution = impl->jobAdapter->ExecuteIndependentRanges(
			selectedIndices.size(), request.batchSize, request.parallelThreshold,
			request.allowParallelReads && ExtractionCapabilities().independentBodyReadsSafe,
			readRange);
		if(!execution.succeeded) {
			extraction.bodies.clear();
			return ProviderFailure("A bounded Jolt extraction range failed.");
		}
		for(std::size_t index = 0U; index < immutableRecords.size(); ++index) {
			const ExtractionRecordSnapshot& source = immutableRecords[index];
			auto found = impl->bodies.find(impl->bodyOrder[source.stableIndex]);
			if(found == impl->bodies.end() || found->second.handle != source.handle) {
				extraction.bodies.clear();
				return ProviderFailure("Provider membership changed before extraction state could commit.");
			}
			Impl::BodyRecord& retained = found->second;
			const PhysicsBackendBodyState& state = extraction.bodies[index];
			if(HasPhysicsBodyStateFields(state.fields, PhysicsBodyStateFieldMask::Pose)) retained.retainedState.pose = state.pose;
			if(HasPhysicsBodyStateFields(state.fields, PhysicsBodyStateFieldMask::LinearVelocity)) retained.retainedState.linearVelocity = state.linearVelocity;
			if(HasPhysicsBodyStateFields(state.fields, PhysicsBodyStateFieldMask::AngularVelocity)) retained.retainedState.angularVelocity = state.angularVelocity;
			if(HasPhysicsBodyStateFields(state.fields, PhysicsBodyStateFieldMask::Awake)) retained.retainedState.awake = state.awake;
			retained.retainedState.fields |= state.fields;
			retained.retainedState.change = state.change;
			if(request.selection == PhysicsBackendExtractionSelection::PublicationRelevant
				|| request.selection == PhysicsBackendExtractionSelection::FullState) {
				retained.pendingChange = PhysicsBodyStateChangeMask::None;
				impl->dirtyBodyKeys.erase(HandleKey(retained.handle));
			}
		}
		extraction.extractedBodyCount = extraction.bodies.size();
		extraction.extractionRangeCount = execution.rangeCount;
		extraction.usedParallelReads = execution.parallel;
		extraction.usedSerialFallback = execution.serialFallback;
		extraction.activeBodyCount = static_cast<std::uint64_t>(
			impl->physicsSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody));
		extraction.contactCount = static_cast<std::uint64_t>(std::count_if(
			impl->pendingEvents.begin(), impl->pendingEvents.end(), [](const PhysicsEvent& event) {
				return event.type == PhysicsEventType::Contact;
			}));

		if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::Constraints)) {
			extraction.constraints.reserve(impl->constraintOrder.size());
			for(const std::uint64_t key : impl->constraintOrder) {
				const auto found = impl->constraints.find(key);
				if(found == impl->constraints.end()) continue;
				const Impl::ConstraintRecord& record = found->second;
				if(!record.added || record.constraint == nullptr) continue;
				PhysicsBackendConstraintState state;
				state.handle = record.handle; state.ownerObjectId = record.ownerObjectId;
				state.ownerObjectGeneration = record.ownerObjectGeneration; state.componentGeneration = record.componentGeneration;
				state.recordId = record.recordId; state.recordGeneration = record.recordGeneration; state.enabled = record.enabled;
				switch(record.kind) {
				case PhysicsConstraintKind::Fixed: {
					const auto& value = static_cast<const JPH::FixedConstraint&>(*record.constraint);
					state.linearImpulseMagnitude = Magnitude(value.GetTotalLambdaPosition()); state.angularImpulseMagnitude = Magnitude(value.GetTotalLambdaRotation()); break;
				}
				case PhysicsConstraintKind::Point: state.linearImpulseMagnitude = Magnitude(static_cast<const JPH::PointConstraint&>(*record.constraint).GetTotalLambdaPosition()); break;
				case PhysicsConstraintKind::Distance: state.linearImpulseMagnitude = std::abs(static_cast<const JPH::DistanceConstraint&>(*record.constraint).GetTotalLambdaPosition()); break;
				case PhysicsConstraintKind::Hinge: {
					const auto& value = static_cast<const JPH::HingeConstraint&>(*record.constraint);
					state.linearImpulseMagnitude = Magnitude(value.GetTotalLambdaPosition()); state.angularImpulseMagnitude = Magnitude(value.GetTotalLambdaRotation()) + std::abs(value.GetTotalLambdaRotationLimits()) + std::abs(value.GetTotalLambdaMotor()); break;
				}
				case PhysicsConstraintKind::Slider: {
					const auto& value = static_cast<const JPH::SliderConstraint&>(*record.constraint);
					state.linearImpulseMagnitude = Magnitude(value.GetTotalLambdaPosition()) + std::abs(value.GetTotalLambdaPositionLimits()) + std::abs(value.GetTotalLambdaMotor()); state.angularImpulseMagnitude = Magnitude(value.GetTotalLambdaRotation()); break;
				}
				case PhysicsConstraintKind::Cone: {
					const auto& value = static_cast<const JPH::ConeConstraint&>(*record.constraint);
					state.linearImpulseMagnitude = Magnitude(value.GetTotalLambdaPosition()); state.angularImpulseMagnitude = std::abs(value.GetTotalLambdaRotation()); break;
				}
				case PhysicsConstraintKind::SwingTwist: {
					const auto& value = static_cast<const JPH::SwingTwistConstraint&>(*record.constraint);
					state.linearImpulseMagnitude = Magnitude(value.GetTotalLambdaPosition()); state.angularImpulseMagnitude = std::abs(value.GetTotalLambdaTwist()) + std::abs(value.GetTotalLambdaSwingY()) + std::abs(value.GetTotalLambdaSwingZ()) + Magnitude(value.GetTotalLambdaMotor()); break;
				}
				case PhysicsConstraintKind::SixDegreesOfFreedom: {
					const auto& value = static_cast<const JPH::SixDOFConstraint&>(*record.constraint);
					state.linearImpulseMagnitude = Magnitude(value.GetTotalLambdaPosition()) + Magnitude(value.GetTotalLambdaMotorTranslation()); state.angularImpulseMagnitude = Magnitude(value.GetTotalLambdaRotation()) + Magnitude(value.GetTotalLambdaMotorRotation()); break;
				}
				case PhysicsConstraintKind::Gear: state.angularImpulseMagnitude = std::abs(static_cast<const JPH::GearConstraint&>(*record.constraint).GetTotalLambda()); break;
				}
				extraction.constraints.push_back(state);
			}
		}

		if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::Characters)) {
			extraction.characters.reserve(impl->characterOrder.size());
			for(const std::uint64_t key : impl->characterOrder) {
				const auto found = impl->characters.find(key);
				if(found == impl->characters.end()) continue;
				const Impl::CharacterRecord& record = found->second;
				if(!record.committed || record.character == nullptr) continue;
				const JPH::CharacterVirtual& character = *record.character;
				PhysicsBackendCharacterState state;
				state.handle = record.handle; state.objectId = record.objectId; state.objectGeneration = record.objectGeneration; state.componentGeneration = record.componentGeneration;
				state.pose = {FromJoltPosition(character.GetPosition()), FromJoltRotation(character.GetRotation())}; state.linearVelocity = FromJoltVector(character.GetLinearVelocity());
				state.groundPosition = FromJoltPosition(character.GetGroundPosition()); state.groundNormal = FromJoltVector(character.GetGroundNormal()); state.supportVelocity = FromJoltVector(character.GetGroundVelocity()); state.supported = character.IsSupported();
				switch(character.GetGroundState()) {
				case JPH::CharacterBase::EGroundState::OnGround: state.groundState = PhysicsCharacterGroundState::OnGround; break;
				case JPH::CharacterBase::EGroundState::OnSteepGround: state.groundState = PhysicsCharacterGroundState::OnSteepGround; break;
				case JPH::CharacterBase::EGroundState::NotSupported: state.groundState = PhysicsCharacterGroundState::NotSupported; break;
				case JPH::CharacterBase::EGroundState::InAir: state.groundState = PhysicsCharacterGroundState::InAir; break;
				}
				if(const Impl::BodyRecord* support = impl->FindBodyByJoltId(character.GetGroundBodyID())) state.supportBody = support->handle;
				state.contactCount = static_cast<std::uint32_t>(std::count_if(character.GetActiveContacts().begin(), character.GetActiveContacts().end(), [](const JPH::CharacterContact& contact) { return contact.mHadCollision; }));
				extraction.characters.push_back(state);
			}
		}
		extraction.constraintCount = extraction.constraints.size();

		if(HasPhysicsBodyStateFields(request.fields, PhysicsBodyStateFieldMask::Events)
			&& request.drainEvents && (request.tick == 0U || request.tick > impl->lastEventDrainTick)) {
			extraction.events = std::move(impl->pendingEvents);
			impl->pendingEvents.clear();
			extraction.droppedEventCount = impl->droppedEvents;
			impl->droppedEvents = 0U;
			impl->lastEventDrainTick = request.tick;
			extraction.eventDrainPerformed = true;
		}
		return Success("Jolt state was collected through deterministic bounded provider ranges.");
	}

	PhysicsBackendResult JoltPhysicsBackend::CommitStaging(PhysicsBackendStagingToken staging) {
		std::lock_guard lock(impl->mutex);
		const PhysicsBackendResult ready = impl->CheckReady();
		if(!ready.Succeeded()) return ready;
		if(impl->physicsSystem == nullptr || !HandleMatchesWorld(staging.world, impl->worldConfiguration.world)) {
			return Rejected(PhysicsBackendErrorCode::StaleWorld, "The staging token targets a stale world.");
		}
		auto record = impl->staging.find(staging.value);
		if(record == impl->staging.end()) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The staging token was not found.");
		}
		// Do not expose any staged record once the provider has entered a contained
		// failure state. Keeping the staging record intact lets the caller execute
		// the normal inverse path without touching the last known-good world.
		if(!impl->CheckProviderHealth()) {
			return ProviderFailure("Jolt was unhealthy before staging publication.", true);
		}
		auto& bodyInterface = impl->physicsSystem->GetBodyInterface();
		if(!record->second.constraintKeys.empty()) {
			// A constraint added after packet staging deliberately cancels background
			// preparation. Its solver-visible insertion order remains the established
			// sequential authored order below.
			impl->AbortPreparedActivationPartitions(record->second);
			// Constraint solvers are sensitive to broadphase/body insertion order.
			// Preserve the established one-at-a-time order for dependent admissions;
			// unconstrained bulk scenes still use Jolt's faster prepared partitions.
			for(std::uint64_t bodyKey : record->second.bodyKeys) {
				auto body = impl->bodies.find(bodyKey);
				if(body == impl->bodies.end() || body->second.added || !body->second.enabled) continue;
				bodyInterface.AddBody(body->second.bodyId,
					body->second.startAwake && body->second.motionType != PhysicsMotionType::Static
						? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
			}
		} else if(!record->second.preparedActivationPartitions.empty()) {
			// CreateBodies already performed the documented background-safe prepare
			// work while its caller lane built engine records. Finalize consumes those
			// same (possibly shuffled) private ID arrays without another provider pass.
			for(auto& partition : record->second.preparedActivationPartitions) {
				if(!partition.prepared || partition.bodyIds.empty()) continue;
				bodyInterface.AddBodiesFinalize(partition.bodyIds.data(),
					static_cast<int>(partition.bodyIds.size()), partition.addState, partition.activation);
				partition.prepared = false;
			}
			record->second.preparedActivationPartitions.clear();
		} else {
			struct ActivationPartition {
				JPH::EActivation activation = JPH::EActivation::DontActivate;
				std::vector<JPH::BodyID> bodies;
			};
			std::vector<ActivationPartition> activationPartitions;
			activationPartitions.reserve(2U);
			for(std::uint64_t bodyKey : record->second.bodyKeys) {
				auto body = impl->bodies.find(bodyKey);
				if(body == impl->bodies.end() || body->second.added || !body->second.enabled) continue;
				const bool activate = body->second.startAwake
					&& body->second.motionType != PhysicsMotionType::Static;
				const JPH::EActivation activation = activate
					? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
				// Keep contiguous activation runs in authored order. Reordering every
				// dormant body ahead of every active body changes Jolt's broadphase insertion
				// order and can change deterministic constrained poses even though the set of
				// admitted bodies is identical.
				if(activationPartitions.empty() || activationPartitions.back().activation != activation) {
					activationPartitions.push_back({activation, {}});
				}
				activationPartitions.back().bodies.push_back(body->second.bodyId);
			}
			const auto finalizePartition = [&bodyInterface](ActivationPartition& partition) -> bool {
				std::vector<JPH::BodyID>& bodies = partition.bodies;
				if(bodies.empty()) return true;
				if(bodies.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
					return false;
				}
				// Jolt may reorder the copied IDs during preparation. No engine identity
				// table observes that temporary provider order; visibility begins only
				// after the entire staging record commits below.
				const int count = static_cast<int>(bodies.size());
				const JPH::BodyInterface::AddState addState = bodyInterface.AddBodiesPrepare(bodies.data(), count);
				bodyInterface.AddBodiesFinalize(bodies.data(), count, addState, partition.activation);
				return true;
			};
			for(ActivationPartition& partition : activationPartitions) {
				if(!finalizePartition(partition)) {
					return Rejected(PhysicsBackendErrorCode::CapacityExceeded,
						"The provider admission batch exceeds Jolt's bounded bulk-add range.");
				}
			}
		}
		for(std::uint64_t bodyKey : record->second.bodyKeys) {
			auto body = impl->bodies.find(bodyKey);
			if(body != impl->bodies.end() && body->second.enabled) body->second.added = true;
		}
		for(std::uint64_t constraintKey : record->second.constraintKeys) {
			auto constraint = impl->constraints.find(constraintKey);
			if(constraint != impl->constraints.end()) {
				if(constraint->second.enabled) {
					impl->physicsSystem->AddConstraint(constraint->second.constraint);
					constraint->second.added = true;
				}
			}
		}
		for(std::uint64_t characterKey : record->second.characterKeys) {
			auto character = impl->characters.find(characterKey);
			if(character != impl->characters.end()) {
				character->second.committed = true;
			}
		}

		if(!impl->CheckProviderHealth()) {
			// Jolt publication operations are void, so assertions and allocation
			// diagnostics are the only late failure channel. Reverse visibility in
			// strict dependency order and leave token ownership unchanged; the caller
			// can then invoke RollbackStaging to destroy every provider record.
			for(auto constraintKey = record->second.constraintKeys.rbegin();
				constraintKey != record->second.constraintKeys.rend(); ++constraintKey) {
				auto constraint = impl->constraints.find(*constraintKey);
				if(constraint != impl->constraints.end() && constraint->second.added) {
					impl->physicsSystem->RemoveConstraint(constraint->second.constraint);
					constraint->second.added = false;
				}
			}
			for(auto characterKey = record->second.characterKeys.rbegin();
				characterKey != record->second.characterKeys.rend(); ++characterKey) {
				auto character = impl->characters.find(*characterKey);
				if(character != impl->characters.end()) character->second.committed = false;
			}
			for(auto bodyKey = record->second.bodyKeys.rbegin();
				bodyKey != record->second.bodyKeys.rend(); ++bodyKey) {
				auto body = impl->bodies.find(*bodyKey);
				if(body != impl->bodies.end() && body->second.added) {
					impl->physicsSystem->GetBodyInterface().RemoveBody(body->second.bodyId);
					body->second.added = false;
				}
			}
			return ProviderFailure("Jolt reported a fatal failure while committing staging.", true);
		}

		// Publication is now known-good. Only this point changes object lookup and
		// clears staging ownership, so staged replacements never hide the live body
		// before the transaction has succeeded as a whole.
		for(std::uint64_t bodyKey : record->second.bodyKeys) {
			auto body = impl->bodies.find(bodyKey);
			if(body == impl->bodies.end()) continue;
			body->second.stagingToken = 0U;
			impl->bodyObjectToKey[body->second.objectId] = bodyKey;
			impl->dirtyBodyKeys.insert(bodyKey);
		}
		if(!record->second.bodyKeys.empty()) impl->RebuildBodyOrder();
		for(std::uint64_t constraintKey : record->second.constraintKeys) {
			auto constraint = impl->constraints.find(constraintKey);
			if(constraint != impl->constraints.end()) constraint->second.stagingToken = 0U;
		}
		if(!record->second.constraintKeys.empty()) impl->RebuildConstraintOrder();
		for(std::uint64_t characterKey : record->second.characterKeys) {
			auto character = impl->characters.find(characterKey);
			if(character != impl->characters.end()) character->second.stagingToken = 0U;
		}
		if(!record->second.characterKeys.empty()) impl->RebuildCharacterOrder();
		for(std::uint64_t shapeKey : record->second.shapeKeys) {
			auto shape = impl->shapes.find(shapeKey);
			if(shape != impl->shapes.end()) shape->second.stagingToken = 0U;
		}
		impl->staging.erase(record);
		return Success("The staged provider records are now visible.");
	}

	PhysicsBackendResult JoltPhysicsBackend::RollbackStaging(PhysicsBackendStagingToken staging) {
		std::lock_guard lock(impl->mutex);
		auto record = impl->staging.find(staging.value);
		if(record == impl->staging.end() || record->second.world != staging.world) {
			return Rejected(PhysicsBackendErrorCode::InvalidArgument, "The staging token was not found.");
		}
		// Jolt requires every background AddBodiesPrepare handle to be consumed by a
		// matching finalize or abort call before its uncommitted body IDs are freed.
		impl->AbortPreparedActivationPartitions(record->second);
		for(auto constraintKey = record->second.constraintKeys.rbegin(); constraintKey != record->second.constraintKeys.rend(); ++constraintKey) {
			auto constraint = impl->constraints.find(*constraintKey);
			if(constraint != impl->constraints.end()) {
				if(constraint->second.added) impl->physicsSystem->RemoveConstraint(constraint->second.constraint);
				impl->constraints.erase(constraint);
			}
		}
		for(auto characterKey = record->second.characterKeys.rbegin(); characterKey != record->second.characterKeys.rend(); ++characterKey) {
			impl->characters.erase(*characterKey);
		}
		for(auto bodyKey = record->second.bodyKeys.rbegin(); bodyKey != record->second.bodyKeys.rend(); ++bodyKey) {
			auto body = impl->bodies.find(*bodyKey);
			if(body != impl->bodies.end()) {
				if(body->second.added) impl->physicsSystem->GetBodyInterface().RemoveBody(body->second.bodyId);
				impl->physicsSystem->GetBodyInterface().DestroyBody(body->second.bodyId);
				impl->bodyIdToKey.erase(body->second.bodyId.GetIndexAndSequenceNumber());
				if(const auto indexed = impl->bodyObjectToKey.find(body->second.objectId);
					indexed != impl->bodyObjectToKey.end() && indexed->second == *bodyKey) {
					impl->bodyObjectToKey.erase(indexed);
				}
				impl->dirtyBodyKeys.erase(*bodyKey);
				impl->bodies.erase(body);
			}
		}
		for(auto shapeKey = record->second.shapeKeys.rbegin(); shapeKey != record->second.shapeKeys.rend(); ++shapeKey) {
			impl->shapes.erase(*shapeKey);
		}
		impl->staging.erase(record);
		return Success("The staged provider records were completely discarded.");
	}

	bool JoltPhysicsBackend::IsHealthy() const noexcept {
		return impl->CheckProviderHealth();
	}

	PhysicsAllocationDiagnostics JoltPhysicsBackend::AllocationDiagnostics() const noexcept {
		return impl->allocator != nullptr ? impl->allocator->Diagnostics() : PhysicsAllocationDiagnostics{};
	}

	PhysicsBackendJobDrainDiagnostics JoltPhysicsBackend::JobDiagnostics() const noexcept {
		const PhysicsBackendJobTelemetry diagnostics = JobTelemetry();
		return {diagnostics.queued, diagnostics.activePumps};
	}

	PhysicsBackendJobTelemetry JoltPhysicsBackend::JobTelemetry() const noexcept {
		if(impl->jobAdapter == nullptr) return {};
		const PhysicsJobAdapterDiagnostics source = impl->jobAdapter->Diagnostics();
		PhysicsBackendJobTelemetry result;
		result.submitted = source.submitted;
		result.completed = source.completed;
		result.rejected = source.rejected;
		result.batchCount = source.batchCount;
		result.lockAcquisitionCount = source.lockAcquisitionCount;
		result.pumpSubmissionCount = source.pumpSubmissionCount;
		result.barrierWaitCount = source.barrierWaitCount;
		result.barrierWaitMicroseconds = source.barrierWaitMicroseconds;
		result.maximumBarrierWaitMicroseconds = source.maximumBarrierWaitMicroseconds;
		result.jobExecutionMicroseconds = source.jobExecutionMicroseconds;
		result.queued = source.queued;
		result.activePumps = source.activePumps;
		result.maximumActivePumps = source.maxActivePumps;
		result.workerLimit = source.workerLimit;
		result.observedWorkerThreads = source.observedWorkerThreads;
		result.available = true;
		result.idle = source.queued == 0U && source.activePumps == 0U;
		return result;
	}

	PhysicsBackendSimulationQuality JoltPhysicsBackend::AppliedSimulationQuality() const noexcept {
		std::shared_lock lock(impl->mutex);
		PhysicsBackendSimulationQuality result;
		if(impl->physicsSystem == nullptr || !impl->worldConfiguration.world.IsValid()) return result;
		const JPH::PhysicsSettings& settings = impl->physicsSystem->GetPhysicsSettings();
		result.world = impl->worldConfiguration.world;
		result.velocityIterations = settings.mNumVelocitySteps;
		result.positionIterations = settings.mNumPositionSteps;
		result.applied = true;
		return result;
	}

	PhysicsProviderError JoltPhysicsBackend::LastProviderError() const {
		return impl->allocator != nullptr ? impl->allocator->LastError() : PhysicsProviderError{};
	}

	void JoltPhysicsBackend::ReportFatalProviderFailureForValidation() noexcept {
		std::lock_guard lock(impl->mutex);
		if(impl->allocator == nullptr) return;
		// Recording a translated failure keeps this validation representative of an
		// actual callback failure while avoiding undefined behavior from a real OOM.
		impl->allocator->ReportAllocationFailureForValidation(4096U);
		(void)impl->CheckProviderHealth();
	}
}
