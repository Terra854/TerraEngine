#pragma once

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

	class TestContext {
	public:
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
			const char* filterValue = std::getenv("TERRAENGINE_TEST_FILTER");
			const std::string filter = filterValue != nullptr ? filterValue : "";

			auto matchesFilter = [&filter](const std::string& testName) {
				if(filter.empty()) {
					return true;
				}
				std::size_t begin = 0;
				while(begin <= filter.size()) {
					const std::size_t end = filter.find_first_of(",;", begin);
					const std::string token = filter.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
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
				if(!matchesFilter(test.name)) {
					continue;
				}
				try {
					test.body(context);
					output << "[PASS] " << test.name << '\n';
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

			output << tests_.size() << " tests, " << failedCount << " failures\n";
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
