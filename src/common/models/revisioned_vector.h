#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace uam
{
	/// <summary>
	/// A vector whose content carries a revision stamp, so derived values (a transcript digest, serialized
	/// tool calls) can be reused while the stamp is unchanged instead of being recomputed from the content.
	///
	/// Invariant: two instances with the same non-zero stamp hold equal content. Every way to reach the
	/// elements for writing (non-const element access, iterators, data, the mutating members, conversion to a
	/// mutable vector) first takes a fresh process-wide stamp, so a writer cannot change content without changing
	/// the stamp. Reading through a non-const instance takes a fresh stamp too: that only costs a recomputation.
	/// Copies keep the stamp, as their content is equal; a moved-from instance is empty with stamp zero.
	///
	/// What the type cannot see: a reference, pointer or iterator kept from an earlier non-const access and written
	/// through after the stamp was read. Readers that cache by stamp therefore keep a periodic full comparison.
	/// </summary>
	template <class T>
	class RevisionedVector
	{
	  public:
		using Items = std::vector<T>;
		using value_type = typename Items::value_type;
		using size_type = typename Items::size_type;
		using difference_type = typename Items::difference_type;
		using reference = typename Items::reference;
		using const_reference = typename Items::const_reference;
		using iterator = typename Items::iterator;
		using const_iterator = typename Items::const_iterator;
		using reverse_iterator = typename Items::reverse_iterator;
		using const_reverse_iterator = typename Items::const_reverse_iterator;

		RevisionedVector() = default;
		RevisionedVector(Items items) : m_items(std::move(items)), m_revision(m_items.empty() ? 0 : NextRevision()) {}
		RevisionedVector(std::initializer_list<T> items) : RevisionedVector(Items(items)) {}
		RevisionedVector(const RevisionedVector&) = default;
		RevisionedVector& operator=(const RevisionedVector&) = default;
		RevisionedVector(RevisionedVector&& other) noexcept : m_items(std::move(other.m_items)), m_revision(other.m_revision) { other.Reset(); }
		RevisionedVector& operator=(RevisionedVector&& other) noexcept
		{
			if (this != &other)
			{
				m_items = std::move(other.m_items);
				m_revision = other.m_revision;
				other.Reset();
			}
			return *this;
		}
		RevisionedVector& operator=(std::initializer_list<T> items) { return *this = RevisionedVector(Items(items)); }

		/// <summary>Stamp of the current content; zero only for content that was never written.</summary>
		std::uint64_t revision() const { return m_revision; }

		// Reading.
		const Items& items() const { return m_items; }
		operator const Items&() const { return m_items; }
		size_type size() const { return m_items.size(); }
		bool empty() const { return m_items.empty(); }
		size_type capacity() const { return m_items.capacity(); }
		size_type max_size() const { return m_items.max_size(); }
		const_reference operator[](size_type index) const { return m_items[index]; }
		const_reference at(size_type index) const { return m_items.at(index); }
		const_reference front() const { return m_items.front(); }
		const_reference back() const { return m_items.back(); }
		const T* data() const { return m_items.data(); }
		const_iterator begin() const { return m_items.begin(); }
		const_iterator end() const { return m_items.end(); }
		const_iterator cbegin() const { return m_items.cbegin(); }
		const_iterator cend() const { return m_items.cend(); }
		const_reverse_iterator rbegin() const { return m_items.rbegin(); }
		const_reverse_iterator rend() const { return m_items.rend(); }
		const_reverse_iterator crbegin() const { return m_items.crbegin(); }
		const_reverse_iterator crend() const { return m_items.crend(); }

		// Writing: every entry point goes through Mutable().
		operator Items&() & { return Mutable(); }
		operator Items() && { return take(); }
		/// <summary>
		/// Moves the content out as a plain vector and leaves this empty. Assigning `std::move(revisioned)` to an
		/// existing plain vector is ambiguous between its copy and move assignment; this is the spelling for that.
		/// </summary>
		Items take() { Items items = std::move(m_items); Reset(); return items; }
		reference operator[](size_type index) { return Mutable()[index]; }
		reference at(size_type index) { return Mutable().at(index); }
		reference front() { return Mutable().front(); }
		reference back() { return Mutable().back(); }
		T* data() { return Mutable().data(); }
		iterator begin() { return Mutable().begin(); }
		iterator end() { return Mutable().end(); }
		reverse_iterator rbegin() { return Mutable().rbegin(); }
		reverse_iterator rend() { return Mutable().rend(); }
		void push_back(const T& item) { Mutable().push_back(item); }
		void push_back(T&& item) { Mutable().push_back(std::move(item)); }
		template <class... Arguments> reference emplace_back(Arguments&&... arguments) { return Mutable().emplace_back(std::forward<Arguments>(arguments)...); }
		void pop_back() { Mutable().pop_back(); }
		void clear() { Mutable().clear(); }
		void resize(size_type count) { Mutable().resize(count); }
		void reserve(size_type count) { Mutable().reserve(count); }
		void shrink_to_fit() { Mutable().shrink_to_fit(); }
		template <class... Arguments> void assign(Arguments&&... arguments) { Mutable().assign(std::forward<Arguments>(arguments)...); }
		iterator insert(const_iterator position, const T& item) { return Mutable().insert(position, item); }
		iterator insert(const_iterator position, T&& item) { return Mutable().insert(position, std::move(item)); }
		iterator insert(const_iterator position, size_type count, const T& item) { return Mutable().insert(position, count, item); }
		iterator insert(const_iterator position, std::initializer_list<T> items) { return Mutable().insert(position, items); }
		template <class Iterator> iterator insert(const_iterator position, Iterator first, Iterator last) { return Mutable().insert(position, first, last); }
		template <class... Arguments> iterator emplace(const_iterator position, Arguments&&... arguments) { return Mutable().emplace(position, std::forward<Arguments>(arguments)...); }
		iterator erase(const_iterator position) { return Mutable().erase(position); }
		iterator erase(const_iterator first, const_iterator last) { return Mutable().erase(first, last); }
		void swap(RevisionedVector& other) noexcept
		{
			m_items.swap(other.m_items);
			std::swap(m_revision, other.m_revision);
		}
		void swap(Items& other)
		{
			Mutable().swap(other);
		}

		friend bool operator==(const RevisionedVector& left, const RevisionedVector& right) { return left.m_items == right.m_items; }
		friend bool operator==(const RevisionedVector& left, const Items& right) { return left.m_items == right; }

	  private:
		static std::uint64_t NextRevision()
		{
			static std::atomic<std::uint64_t> next{0};
			return ++next;
		}

		Items& Mutable()
		{
			m_revision = NextRevision();
			return m_items;
		}

		void Reset()
		{
			m_items.clear();
			m_revision = 0;
		}

		Items m_items;
		std::uint64_t m_revision = 0;
	};
} // namespace uam
