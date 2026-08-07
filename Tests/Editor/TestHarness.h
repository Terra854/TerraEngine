#pragma once

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests {
	class TestFailure final : public std::runtime_error {
	public:
		explicit TestFailure(const std::string& message)
			: std::runtime_error(message) {}
	};

	class TestSkip final : public std::runtime_error {
	public:
		explicit TestSkip(const std::string& message)
			: std::runtime_error(message) {}
	};

	class TestContext {
	public:
		[[noreturn]] void Skip(std::string_view message) const {
			throw TestSkip(std::string(message));
		}

		void Expect(bool condition, std::string_view message) const {
			if(!condition) {
				throw TestFailure(std::string(message));
			}
		}

		template<typename Expected, typename Actual>
		void ExpectEqual(const Expected& expected, const Actual& actual, std::string_view message) const {
			if(!(expected == actual)) {
				std::ostringstream stream;
				stream << message << " (expected '" << expected << "', got '" << actual << "')";
				throw TestFailure(stream.str());
			}
		}
	};

	class TestRegistry {
	public:
		using TestBody = std::function<void(TestContext&)>;

		void Add(std::string name, TestBody body) {
			tests_.push_back({std::move(name), std::move(body)});
		}

		int RunAll(std::ostream& output) const {
			TestContext context;
			int failedCount = 0;
			int skippedCount = 0;
			std::size_t selectedCount = 0;
			const char* filterValue = std::getenv("TERRAENGINE_TEST_FILTER");
			const std::string filter = filterValue != nullptr ? filterValue : "";
			const char* excludeFilterValue = std::getenv("TERRAENGINE_TEST_EXCLUDE_FILTER");
			const std::string excludeFilter = excludeFilterValue != nullptr ? excludeFilterValue : "";

			// Inclusion and exclusion intentionally share one parser so CTest can
			// retain a broad registry while an intermediate gate omits measured cases.
			auto containsAnyToken = [](const std::string& testName, const std::string& tokenList) {
				std::size_t begin = 0;
				while(begin <= tokenList.size()) {
					const std::size_t end = tokenList.find_first_of(",;", begin);
					const std::string token = tokenList.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
					if(!token.empty() && testName.find(token) != std::string::npos) {
						return true;
					}
					if(end == std::string::npos) {
						break;
					}
					begin = end + 1;
				}
				return false;
			};

			// Run every registered test even after one failure so CTest output
			// identifies the full set of broken editor behaviors in one pass.
			for(const TestCase& test : tests_) {
				const bool included = filter.empty() || containsAnyToken(test.name, filter);
				const bool excluded = !excludeFilter.empty() && containsAnyToken(test.name, excludeFilter);
				if(!included || excluded) {
					continue;
				}
				++selectedCount;
				try {
					test.body(context);
					output << "[PASS] " << test.name << '\n';
				} catch(const TestSkip& skip) {
					++skippedCount;
					output << "[SKIP] " << test.name << ": " << skip.what() << '\n';
				} catch(const TestFailure& failure) {
					++failedCount;
					output << "[FAIL] " << test.name << ": " << failure.what() << '\n';
				} catch(const std::exception& exception) {
					++failedCount;
					output << "[ERROR] " << test.name << ": " << exception.what() << '\n';
				} catch(...) {
					++failedCount;
					output << "[ERROR] " << test.name << ": unknown exception\n";
				}
			}

			// Keep focused evidence honest: registry size describes discovery,
			// while selectedCount describes work that actually ran.
			output << selectedCount << " selected tests (" << tests_.size()
				<< " registered), " << failedCount << " failures, " << skippedCount << " skipped\n";
			return failedCount == 0 ? 0 : 1;
		}

	private:
		struct TestCase {
			std::string name;
			TestBody body;
		};

		std::vector<TestCase> tests_;
	};
}
