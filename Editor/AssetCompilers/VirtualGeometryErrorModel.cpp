#include "VirtualGeometryErrorModel.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Editor
{
	bool CanSimplifyVirtualGeometryAttributesTogether(const VirtualGeometryAttributeSignature &first, const VirtualGeometryAttributeSignature &second) noexcept
	{
		return first == second;
	}

	bool VirtualGeometryCoverageProof::Valid() const noexcept
	{
		return completeCoverage && std::isfinite(objectError) && std::isfinite(descendantError) && std::isfinite(screenError) && objectError >= 0.0f &&
			   descendantError >= objectError && screenError >= 0.0f;
	}

	VirtualGeometryCoverageProof BuildVirtualGeometryCoverageProof(
		float objectError, const std::vector<float> &descendantErrors, float projectionScale, float distance)
	{
		VirtualGeometryCoverageProof result{};
		result.objectError = objectError;
		result.descendantError = objectError;
		if (!std::isfinite(objectError) || objectError < 0.0f || descendantErrors.empty() || !std::isfinite(projectionScale) || projectionScale < 0.0f ||
			!std::isfinite(distance) || distance <= 0.0f)
		{
			return result;
		}

		for (const float descendantError : descendantErrors)
		{
			if (!std::isfinite(descendantError) || descendantError < 0.0f)
			{
				return result;
			}
			result.descendantError = std::max(result.descendantError, descendantError);
		}
		result.screenError = result.descendantError * projectionScale / distance;
		result.completeCoverage = true;
		return result;
	}

	bool VirtualGeometryAccumulatedDetailError::Valid() const noexcept
	{
		return monotonic && std::isfinite(localError) && std::isfinite(quantizationHalfStepError) && std::isfinite(accumulatedError) &&
			localError >= 0.0f && quantizationHalfStepError >= 0.0f && accumulatedError >= localError &&
			accumulatedError >= quantizationHalfStepError;
	}

	VirtualGeometryAccumulatedDetailError BuildVirtualGeometryAccumulatedDetailError(
		float localError, float quantizationHalfStepError, const std::vector<float> &childAccumulatedErrors)
	{
		VirtualGeometryAccumulatedDetailError result{};
		result.localError = localError;
		result.quantizationHalfStepError = quantizationHalfStepError;
		if (!std::isfinite(localError) || localError < 0.0f || !std::isfinite(quantizationHalfStepError) || quantizationHalfStepError < 0.0f)
		{
			return result;
		}

		result.accumulatedError = std::max(localError, quantizationHalfStepError);
		for (const float childError : childAccumulatedErrors)
		{
			if (!std::isfinite(childError) || childError < 0.0f)
			{
				return VirtualGeometryAccumulatedDetailError{};
			}
			result.accumulatedError = std::max(result.accumulatedError, childError);
		}
		result.monotonic = true;
		return result;
	}
} // namespace TerraEngine::Editor
