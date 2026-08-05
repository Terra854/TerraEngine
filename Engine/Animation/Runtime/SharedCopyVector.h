// SharedCopyVector.h - Copy-on-write storage for immutable runtime arrays.
//
// Animation bindings are copied into frame work items, but most of their arrays
// describe immutable asset metadata. Sharing those arrays makes a binding copy
// constant-time; the first later mutation receives a private std::vector copy.

#pragma once

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace TerraEngine::Animation {
	template<typename T>
	class SharedCopyVector final {
	public:
		using value_type = T;
		using size_type = typename std::vector<T>::size_type;
		using difference_type = typename std::vector<T>::difference_type;
		using const_reference = typename std::vector<T>::const_reference;
		using const_iterator = typename std::vector<T>::const_iterator;

		SharedCopyVector() = default;
		SharedCopyVector(std::initializer_list<T> values)
			: values_(std::make_shared<std::vector<T>>(values))
		{
		}
		SharedCopyVector(const std::vector<T>& values)
			: values_(values.empty()
				? nullptr : std::make_shared<std::vector<T>>(values))
		{
		}
		SharedCopyVector(std::vector<T>&& values)
			: values_(values.empty()
				? nullptr : std::make_shared<std::vector<T>>(std::move(values)))
		{
		}

		SharedCopyVector& operator=(std::initializer_list<T> values)
		{
			Mutable().assign(values.begin(), values.end());
			return *this;
		}
		SharedCopyVector& operator=(const std::vector<T>& values)
		{
			if(values.empty()) {
				values_.reset();
			} else {
				Mutable() = values;
			}
			return *this;
		}
		SharedCopyVector& operator=(std::vector<T>&& values)
		{
			if(values.empty()) {
				values_.reset();
			} else {
				values_ = std::make_shared<std::vector<T>>(std::move(values));
			}
			return *this;
		}

		[[nodiscard]] bool empty() const noexcept { return Read().empty(); }
		[[nodiscard]] size_type size() const noexcept { return Read().size(); }
		[[nodiscard]] size_type capacity() const noexcept { return Read().capacity(); }
		[[nodiscard]] const T* data() const noexcept { return Read().data(); }
		[[nodiscard]] const_reference operator[](size_type index) const noexcept
		{
			return Read()[index];
		}
		// Non-const reads intentionally remain immutable. Callers that need to edit
		// an element use Mutable(), making the potentially expensive detach explicit.
		[[nodiscard]] const_reference operator[](size_type index) noexcept
		{
			return Read()[index];
		}
		[[nodiscard]] const_iterator begin() const noexcept { return Read().begin(); }
		[[nodiscard]] const_iterator end() const noexcept { return Read().end(); }
		[[nodiscard]] const_iterator begin() noexcept { return Read().begin(); }
		[[nodiscard]] const_iterator end() noexcept { return Read().end(); }

		void reserve(size_type capacity) { Mutable().reserve(capacity); }
		void clear() noexcept { values_.reset(); }
		void push_back(const T& value) { Mutable().push_back(value); }
		void push_back(T&& value) { Mutable().push_back(std::move(value)); }
		template<typename Iterator>
		void assign(Iterator first, Iterator last) { Mutable().assign(first, last); }
		void assign(size_type count, const T& value) { Mutable().assign(count, value); }
		template<typename Iterator>
		const_iterator insert(const_iterator position, Iterator first, Iterator last)
		{
			const difference_type offset = position - Read().begin();
			std::vector<T>& values = Mutable();
			return values.insert(values.begin() + offset, first, last);
		}
		const_iterator erase(const_iterator first, const_iterator last)
		{
			const difference_type firstOffset = first - Read().begin();
			const difference_type lastOffset = last - Read().begin();
			std::vector<T>& values = Mutable();
			return values.erase(
				values.begin() + firstOffset, values.begin() + lastOffset);
		}

		// Mutation is intentionally visible at the call site. If another binding or
		// frame item shares the array, this method first preserves its old snapshot.
		[[nodiscard]] std::vector<T>& Mutable()
		{
			if(!values_) {
				values_ = std::make_shared<std::vector<T>>();
			} else if(values_.use_count() != 1L) {
				values_ = std::make_shared<std::vector<T>>(*values_);
			}
			return *values_;
		}
		[[nodiscard]] const std::vector<T>& Vector() const noexcept { return Read(); }
		operator const std::vector<T>&() const noexcept { return Read(); }

		friend bool operator==(
			const SharedCopyVector& left, const SharedCopyVector& right) noexcept
		{
			return left.values_ == right.values_ || left.Read() == right.Read();
		}
		friend bool operator==(
			const SharedCopyVector& left, const std::vector<T>& right) noexcept
		{
			return left.Read() == right;
		}
		friend bool operator==(
			const std::vector<T>& left, const SharedCopyVector& right) noexcept
		{
			return left == right.Read();
		}

	private:
		[[nodiscard]] static const std::vector<T>& Empty() noexcept
		{
			static const std::vector<T> empty;
			return empty;
		}
		[[nodiscard]] const std::vector<T>& Read() const noexcept
		{
			return values_ ? *values_ : Empty();
		}

		std::shared_ptr<std::vector<T>> values_;
	};
}
