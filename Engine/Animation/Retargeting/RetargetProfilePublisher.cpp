// RetargetProfilePublisher.cpp - Generation-safe asynchronous profile publish.

#include "Animation/Retargeting/RetargetProfilePublisher.h"

#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		class ThreadPoolRetargetProfileCompileExecutor final
			: public RetargetProfileCompileExecutor {
		public:
			explicit ThreadPoolRetargetProfileCompileExecutor(ThreadPool& pool)
				: pool_(pool) {}

			void Enqueue(std::function<void()> work) override {
				// The task owns the callable and the publisher's shared state; it never
				// retains a raw publisher or caller-owned asset pointer.
				auto task = pool_.createTask(std::move(work));
				pool_.enqueue(task);
			}

		private:
			ThreadPool& pool_;
		};

		AnimationStatus ValidateRequestBounds(
			const RetargetProfileCompileRequest& request) noexcept {
			if(!request.profile.assetId.IsValid() || request.sourceGeneration == 0U
				|| !request.sourceSkeleton.assetId.IsValid()
				|| !request.targetSkeleton.assetId.IsValid()
				|| request.sourceSkeleton.runtimeGeneration == 0U
				|| request.targetSkeleton.runtimeGeneration == 0U) {
				return AnimationStatus::InvalidArgument;
			}
			if(request.sourceSkeleton.joints.size() > kRetargetProfileMaximumMappings
				|| request.targetSkeleton.joints.size() > kRetargetProfileMaximumMappings
				|| request.profile.semanticMappings.size()
					> kRetargetProfileMaximumMappings
				|| request.profile.explicitMappings.size()
					> kRetargetProfileMaximumMappings
				|| request.profile.chainMappings.size() > kRetargetProfileMaximumChains) {
				return AnimationStatus::HardLimitExceeded;
			}
			return AnimationStatus::Ok;
		}

		RetargetProfileGenerationSnapshot SnapshotFor(
			const RetargetProfileCompileRequest& request) noexcept {
			RetargetProfileGenerationSnapshot snapshot;
			snapshot.profileAssetId = request.profile.assetId;
			snapshot.sourceGeneration = request.sourceGeneration;
			snapshot.sourceSkeleton = {request.sourceSkeleton.assetId,
				request.sourceSkeleton.runtimeGeneration};
			snapshot.targetSkeleton = {request.targetSkeleton.assetId,
				request.targetSkeleton.runtimeGeneration};
			snapshot.sourceSkeletonFingerprint =
				request.sourceSkeleton.compatibilityFingerprint;
			snapshot.targetSkeletonFingerprint =
				request.targetSkeleton.compatibilityFingerprint;
			snapshot.sourceReferencePoseHash = request.sourceSkeleton.referencePoseHash;
			snapshot.targetReferencePoseHash = request.targetSkeleton.referencePoseHash;
			return snapshot;
		}
	}

	struct RetargetProfilePublisher::SharedState {
		struct Job {
			std::uint64_t requestSequence = 0U;
			RetargetProfileCompileRequest request{};
			bool cancelled = false;
			bool completed = false;
			RetargetValidationResult validation{};
			RetargetCompiledProfile compiled{};
			AnimationStatus status = AnimationStatus::NotReady;
		};

		struct RetiredRecord {
			RetargetProfilePublishRecord record{};
		};

		mutable std::mutex mutex;
		bool shuttingDown = false;
		std::size_t maxPendingJobs = 0U;
		std::size_t maxRetiredGenerations = 0U;
		AnimationGuid profileAssetId{};
		std::uint64_t newestRequestSequence = 0U;
		AnimationGeneration nextProfileGeneration = 1U;
		AnimationGeneration nextDependentInvalidationGeneration = 1U;
		std::vector<std::shared_ptr<Job>> jobs;
		std::optional<RetargetProfilePublishRecord> published;
		std::vector<RetiredRecord> retired;
	};

	std::shared_ptr<RetargetProfileCompileExecutor>
	CreateThreadPoolRetargetProfileCompileExecutor(ThreadPool& pool) {
		return std::make_shared<ThreadPoolRetargetProfileCompileExecutor>(pool);
	}

	RetargetProfilePublisher::RetargetProfilePublisher(
		std::shared_ptr<RetargetProfileCompileExecutor> executor,
		std::size_t maxPendingJobs,
		std::size_t maxRetiredGenerations)
		: state_(std::make_shared<SharedState>()), executor_(std::move(executor)) {
		state_->maxPendingJobs = std::min(maxPendingJobs,
			kRetargetPublisherMaximumPendingJobs);
		state_->maxRetiredGenerations = std::min(maxRetiredGenerations,
			kRetargetPublisherMaximumRetiredGenerations);
	}

	RetargetProfilePublisher::~RetargetProfilePublisher() {
		CancelPending();
		std::scoped_lock lock(state_->mutex);
		state_->shuttingDown = true;
	}

	AnimationStatus RetargetProfilePublisher::RequestPublication(
		const RetargetProfileCompileRequest& input) {
		if(executor_ == nullptr || state_->maxPendingJobs == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus bounds = ValidateRequestBounds(input);
		if(bounds != AnimationStatus::Ok) {
			return bounds;
		}
		std::shared_ptr<SharedState::Job> job;
		try {
			job = std::make_shared<SharedState::Job>();
			job->request = input;
			job->request.profile.runtimeGeneration = 0U;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}

		{
			std::scoped_lock lock(state_->mutex);
			if(state_->shuttingDown) {
				return AnimationStatus::Cancelled;
			}
			if(state_->jobs.size() >= state_->maxPendingJobs) {
				return AnimationStatus::Rejected;
			}
			if(state_->profileAssetId.IsValid()
				&& state_->profileAssetId != job->request.profile.assetId) {
				return AnimationStatus::InvalidArgument;
			}
			if(state_->newestRequestSequence == std::numeric_limits<std::uint64_t>::max()) {
				return AnimationStatus::RangeOverflow;
			}
			job->requestSequence = state_->newestRequestSequence + 1U;
			try {
				state_->jobs.push_back(job);
			} catch(...) {
				return AnimationStatus::InternalError;
			}
			state_->profileAssetId = job->request.profile.assetId;
			state_->newestRequestSequence = job->requestSequence;
		}

		const std::shared_ptr<SharedState> state = state_;
		try {
			executor_->Enqueue([state, job]() {
				RetargetValidationResult validation;
				RetargetCompiledProfile compiled;
				const AnimationStatus status = CompileRetargetProfile(
					job->request.profile, job->request.sourceSkeleton,
					job->request.targetSkeleton, validation, compiled);
				std::scoped_lock lock(state->mutex);
				if(state->shuttingDown || job->cancelled) {
					return;
				}
				job->status = status;
				job->validation = std::move(validation);
				if(status == AnimationStatus::Ok) {
					job->compiled = std::move(compiled);
				}
				job->completed = true;
			});
		} catch(...) {
			std::scoped_lock lock(state_->mutex);
			job->status = AnimationStatus::InternalError;
			job->completed = true;
		}
		return AnimationStatus::Queued;
	}

	AnimationStatus RetargetProfilePublisher::RequestSerializedPublication(
		std::span<const std::uint8_t> bytes,
		AnimationCompatibilityFingerprint expectedProfileFingerprint,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		AnimationGeneration sourceGeneration) {
		RetargetProfileAsset profile;
		const AnimationStatus status = DeserializeRetargetProfileAsset(bytes,
			expectedProfileFingerprint, profile);
		if(status != AnimationStatus::Ok) {
			return status;
		}
		RetargetProfileCompileRequest request;
		request.profile = std::move(profile);
		request.sourceSkeleton = sourceSkeleton;
		request.targetSkeleton = targetSkeleton;
		request.sourceGeneration = sourceGeneration;
		return RequestPublication(request);
	}

	void RetargetProfilePublisher::CancelPending() noexcept {
		if(state_ == nullptr) {
			return;
		}
		std::scoped_lock lock(state_->mutex);
		for(const std::shared_ptr<SharedState::Job>& job : state_->jobs) {
			job->cancelled = true;
		}
	}

	RetargetProfilePublishPollResult RetargetProfilePublisher::PollAndPublish(
		const RetargetProfileGenerationSnapshot& current) {
		RetargetProfilePublishPollResult result;
		std::scoped_lock lock(state_->mutex);
		for(auto iterator = state_->jobs.begin(); iterator != state_->jobs.end();) {
			const std::shared_ptr<SharedState::Job>& job = *iterator;
			if(!job->completed && !job->cancelled) {
				++iterator;
				continue;
			}

			if(job->cancelled) {
				result.status = AnimationStatus::Cancelled;
			} else if(job->requestSequence != state_->newestRequestSequence) {
				result.status = AnimationStatus::StaleHandle;
			} else if(SnapshotFor(job->request) != current) {
				result.status = AnimationStatus::StaleHandle;
			} else if(job->status != AnimationStatus::Ok) {
				result.status = job->status;
			} else if(state_->nextProfileGeneration == 0U
				|| state_->nextProfileGeneration
					== std::numeric_limits<AnimationGeneration>::max()
				|| state_->nextDependentInvalidationGeneration == 0U
				|| state_->nextDependentInvalidationGeneration
					== std::numeric_limits<AnimationGeneration>::max()) {
				result.status = AnimationStatus::InternalError;
			} else {
				try {
					// Stage the new record and retirement list in temporary storage. The
					// assignments below are the only atomic last-good replacement point.
					RetargetCompiledProfile compiled = std::move(job->compiled);
					compiled.profile.runtimeGeneration = state_->nextProfileGeneration;
					RetargetProfilePublishRecord stagedRecord;
					stagedRecord.profileAssetId = job->request.profile.assetId;
					stagedRecord.sourceGeneration = job->request.sourceGeneration;
					stagedRecord.profileGeneration = state_->nextProfileGeneration;
					stagedRecord.dependentInvalidationGeneration =
						state_->nextDependentInvalidationGeneration;
					stagedRecord.sourceSkeleton = {job->request.sourceSkeleton.assetId,
						job->request.sourceSkeleton.runtimeGeneration};
					stagedRecord.targetSkeleton = {job->request.targetSkeleton.assetId,
						job->request.targetSkeleton.runtimeGeneration};
					stagedRecord.stableHash = compiled.stableHash;
					stagedRecord.compiledProfile =
						std::make_shared<const RetargetCompiledProfile>(std::move(compiled));

					std::vector<SharedState::RetiredRecord> stagedRetired = state_->retired;
					if(state_->published.has_value()
						&& state_->maxRetiredGenerations != 0U) {
						stagedRetired.push_back({*state_->published});
						if(stagedRetired.size() > state_->maxRetiredGenerations) {
							const std::size_t removeCount = stagedRetired.size()
								- state_->maxRetiredGenerations;
							stagedRetired.erase(stagedRetired.begin(),
								stagedRetired.begin()
									+ static_cast<std::ptrdiff_t>(removeCount));
						}
					}
					state_->retired = std::move(stagedRetired);
					state_->published = std::move(stagedRecord);
					++state_->nextProfileGeneration;
					++state_->nextDependentInvalidationGeneration;
					result.status = AnimationStatus::Ok;
					result.published = true;
					result.dependentInvalidationGeneration =
						state_->published->dependentInvalidationGeneration;
				} catch(const std::bad_alloc&) {
					result.status = AnimationStatus::InternalError;
				} catch(...) {
					result.status = AnimationStatus::InternalError;
				}
			}
			iterator = state_->jobs.erase(iterator);
		}
		result.retainedOldGenerationCount = state_->retired.size();
		return result;
	}

	std::optional<RetargetProfilePublishRecord>
	RetargetProfilePublisher::GetPublishedRecord() const {
		std::scoped_lock lock(state_->mutex);
		return state_->published;
	}

	std::vector<RetargetRetiredGeneration>
	RetargetProfilePublisher::GetRetiredGenerations() const {
		std::scoped_lock lock(state_->mutex);
		std::vector<RetargetRetiredGeneration> result;
		try {
			result.reserve(state_->retired.size());
			for(const SharedState::RetiredRecord& retired : state_->retired) {
				result.push_back({retired.record.profileGeneration,
					retired.record.stableHash});
			}
		} catch(...) {
			result.clear();
		}
		return result;
	}

	void RetargetProfilePublisher::RetireThrough(
		AnimationGeneration completedGeneration) noexcept {
		std::scoped_lock lock(state_->mutex);
		state_->retired.erase(std::remove_if(state_->retired.begin(),
			state_->retired.end(), [completedGeneration](const SharedState::RetiredRecord& retired) {
				return retired.record.profileGeneration <= completedGeneration;
			}), state_->retired.end());
	}
}
