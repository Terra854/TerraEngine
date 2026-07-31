#include "Renderer/VirtualGeometry/Diagnostics/VirtualGeometryEvidence.h"

#include <sstream>

namespace TerraEngine::Renderer
{
	namespace
	{
		// JSON escaping is kept local because benchmark captures need only strings,
		// integers, booleans, objects, and arrays; no runtime JSON dependency is
		// appropriate for this small deterministic evidence writer.
		void AppendJsonString(std::ostringstream &output, std::string_view value)
		{
			output << '"';
			for (const char character : value)
			{
				switch (character)
				{
				case '"':
					output << "\\\"";
					break;
				case '\\':
					output << "\\\\";
					break;
				case '\n':
					output << "\\n";
					break;
				case '\r':
					output << "\\r";
					break;
				case '\t':
					output << "\\t";
					break;
				default:
					if (static_cast<unsigned char>(character) < 0x20u)
					{
						output << "\\u00";
						constexpr char hexadecimal[] = "0123456789abcdef";
						output << hexadecimal[(static_cast<unsigned char>(character) >> 4u) & 0x0fu];
						output << hexadecimal[static_cast<unsigned char>(character) & 0x0fu];
					}
					else
					{
						output << character;
					}
					break;
				}
			}
			output << '"';
		}

		void AppendStringMap(std::ostringstream &output, const std::map<std::string, std::string> &values)
		{
			output << '{';
			bool first = true;
			for (const auto &[key, value] : values)
			{
				if (!first)
				{
					output << ',';
				}
				AppendJsonString(output, key);
				output << ':';
				AppendJsonString(output, value);
				first = false;
			}
			output << '}';
		}

		void AppendOptionalNumber(std::ostringstream &output, const std::optional<std::uint64_t> &value)
		{
			if (value.has_value())
			{
				output << *value;
			}
			else
			{
				output << "null";
			}
		}

		const char *ToString(VirtualGeometryMeasurementAvailability availability)
		{
			switch (availability)
			{
			case VirtualGeometryMeasurementAvailability::Pending:
				return "pending";
			case VirtualGeometryMeasurementAvailability::Unavailable:
				return "unavailable";
			case VirtualGeometryMeasurementAvailability::Invalid:
				return "invalid";
			case VirtualGeometryMeasurementAvailability::Measured:
				return "measured";
			default:
				return "invalid";
			}
		}
	} // namespace

	std::string SerializeVirtualGeometryBenchmarkCapture(const VirtualGeometryBenchmarkCapture &capture)
	{
		std::ostringstream output;
		output << '{';
		output << "\"identity\":{";
		output << "\"schemaVersion\":";
		AppendJsonString(output, capture.identity.schemaVersion);
		output << ",\"workloadVersion\":";
		AppendJsonString(output, capture.identity.workloadVersion);
		output << ",\"captureId\":";
		AppendJsonString(output, capture.identity.captureId);
		output << ",\"runId\":";
		AppendJsonString(output, capture.identity.runId);
		output << ",\"engineHash\":";
		AppendJsonString(output, capture.identity.engineHash);
		output << ",\"sourceHash\":";
		AppendJsonString(output, capture.identity.sourceHash);
		output << ",\"diffHash\":";
		AppendJsonString(output, capture.identity.diffHash);
		output << ",\"executableHash\":";
		AppendJsonString(output, capture.identity.executableHash);
		output << ",\"shaderHash\":";
		AppendJsonString(output, capture.identity.shaderHash);
		output << ",\"assetHash\":";
		AppendJsonString(output, capture.identity.assetHash);
		output << ",\"buildRecipeHash\":";
		AppendJsonString(output, capture.identity.buildRecipeHash);
		output << '}';

		output << ",\"environment\":{";
		output << "\"adapter\":";
		AppendJsonString(output, capture.environment.adapter);
		output << ",\"driver\":";
		AppendJsonString(output, capture.environment.driver);
		output << ",\"backend\":";
		AppendJsonString(output, capture.environment.backend);
		output << ",\"operatingSystem\":";
		AppendJsonString(output, capture.environment.operatingSystem);
		output << ",\"route\":";
		AppendJsonString(output, capture.environment.route);
		output << ",\"resolution\":";
		AppendJsonString(output, capture.environment.resolution);
		output << ",\"quality\":";
		AppendJsonString(output, capture.environment.quality);
		output << ",\"viewCount\":" << capture.environment.viewCount << '}';

		output << ",\"workload\":{";
		output << "\"name\":";
		AppendJsonString(output, capture.workload.name);
		output << ",\"sceneHash\":";
		AppendJsonString(output, capture.workload.sceneHash);
		output << ",\"workloadHash\":";
		AppendJsonString(output, capture.workload.workloadHash);
		output << '}';

		output << ",\"warmupAndSampling\":{";
		output << "\"warmupSamples\":" << capture.warmupAndSampling.warmupSamples;
		output << ",\"requestedSamples\":" << capture.warmupAndSampling.requestedSamples;
		output << ",\"randomizedOrder\":" << (capture.warmupAndSampling.randomizedOrder ? "true" : "false") << '}';

		output << ",\"capabilities\":[";
		for (std::size_t index = 0u; index < capture.capabilities.size(); ++index)
		{
			if (index != 0u)
			{
				output << ',';
			}
			const VirtualGeometryCapabilityEvidence &capability = capture.capabilities[index];
			output << '{';
			output << "\"deviceIdentity\":";
			AppendJsonString(output, capability.key.deviceIdentity);
			output << ",\"planned\":" << (capability.planned ? "true" : "false");
			output << ",\"supported\":" << (capability.supported ? "true" : "false");
			output << ",\"executed\":" << (capability.CanReportExecuted() ? "true" : "false");
			output << ",\"validated\":" << (capability.CanReportValidated() ? "true" : "false");
			output << '}';
		}
		output << ']';

		output << ",\"rawSamples\":[";
		for (std::size_t index = 0u; index < capture.rawSamples.size(); ++index)
		{
			if (index != 0u)
			{
				output << ',';
			}
			const VirtualGeometryMeasurementRecord &sample = capture.rawSamples[index];
			output << "{\"availability\":";
			AppendJsonString(output, ToString(sample.availability));
			output << ",\"rawTicks\":";
			AppendOptionalNumber(output, sample.rawTicks);
			output << ",\"durationNanoseconds\":";
			if (sample.HasAuthoritativeDuration())
			{
				AppendOptionalNumber(output, sample.durationNanoseconds);
			}
			else
			{
				output << "null";
			}
			output << ",\"rejectionReason\":";
			AppendJsonString(output, sample.rejectionReason);
			output << '}';
		}
		output << ']';

		output << ",\"statistics\":{";
		output << "\"medianNanoseconds\":";
		AppendOptionalNumber(output, capture.statistics.medianNanoseconds);
		output << ",\"p95Nanoseconds\":";
		AppendOptionalNumber(output, capture.statistics.p95Nanoseconds);
		output << ",\"p99Nanoseconds\":";
		AppendOptionalNumber(output, capture.statistics.p99Nanoseconds);
		output << ",\"varianceNanosecondsSquared\":";
		AppendOptionalNumber(output, capture.statistics.varianceNanosecondsSquared);
		output << ",\"worstNanoseconds\":";
		AppendOptionalNumber(output, capture.statistics.worstNanoseconds);
		output << ",\"sampleCount\":" << capture.statistics.sampleCount;
		output << ",\"rejectedCount\":" << capture.statistics.rejectedCount;
		output << ",\"rejectionReasons\":[";
		for (std::size_t index = 0u; index < capture.statistics.rejectionReasons.size(); ++index)
		{
			if (index != 0u)
			{
				output << ',';
			}
			AppendJsonString(output, capture.statistics.rejectionReasons[index]);
		}
		output << "]}";

		output << ",\"geometry\":";
		AppendStringMap(output, capture.sections.geometry);
		output << ",\"residencyAndStreaming\":";
		AppendStringMap(output, capture.sections.residencyAndStreaming);
		output << ",\"visibilityAndShading\":";
		AppendStringMap(output, capture.sections.visibilityAndShading);
		output << ",\"deformation\":";
		AppendStringMap(output, capture.sections.deformation);
		output << ",\"auxiliaryViews\":";
		AppendStringMap(output, capture.sections.auxiliaryViews);
		output << ",\"fallbacksAndFailures\":";
		AppendStringMap(output, capture.sections.fallbacksAndFailures);
		output << ",\"baselineAndComparability\":";
		AppendStringMap(output, capture.sections.baselineAndComparability);
		output << ",\"artifacts\":";
		AppendStringMap(output, capture.sections.artifacts);
		output << ",\"integrity\":{";
		output << "\"hash\":";
		AppendJsonString(output, capture.integrityHash);
		output << ",\"diagnostics\":";
		AppendStringMap(output, capture.diagnostics);
		output << "}}";
		return output.str();
	}
} // namespace TerraEngine::Renderer
