#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <span>
#include <stack>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container {

template<typename T, typename Allocator = std::allocator<T>>
using Vector = std::vector<T, Allocator>;

template<typename Char,
         typename Traits = std::char_traits<Char>,
         typename Allocator = std::allocator<Char>>
using BasicString = std::basic_string<Char, Traits, Allocator>;

using String = BasicString<char>;
using WString = BasicString<wchar_t>;
using U8String = BasicString<char8_t>;
using U16String = BasicString<char16_t>;
using U32String = BasicString<char32_t>;

using StringView = std::string_view;
using WStringView = std::wstring_view;
using U8StringView = std::u8string_view;
using U16StringView = std::u16string_view;
using U32StringView = std::u32string_view;

template<typename Key,
         typename Value,
         typename Compare = std::less<Key>,
         typename Allocator = std::allocator<std::pair<const Key, Value>>>
using TreeMap = std::map<Key, Value, Compare, Allocator>;

template<typename Key,
         typename Compare = std::less<Key>,
         typename Allocator = std::allocator<Key>>
using TreeSet = std::set<Key, Compare, Allocator>;

template<typename T, typename Allocator = std::allocator<T>>
using Deque = std::deque<T, Allocator>;

template<typename T, typename Allocator = std::allocator<T>>
using List = std::list<T, Allocator>;

template<typename T, typename Container = Deque<T>>
using Queue = std::queue<T, Container>;

template<typename T, typename Container = Deque<T>>
using Stack = std::stack<T, Container>;

template<typename T,
         typename Container = Vector<T>,
         typename Compare = std::less<typename Container::value_type>>
using PriorityQueue = std::priority_queue<T, Container, Compare>;

template<typename T, typename Deleter = std::default_delete<T>>
using UniquePtr = std::unique_ptr<T, Deleter>;

template<typename T>
using SharedPtr = std::shared_ptr<T>;

template<typename T>
using WeakPtr = std::weak_ptr<T>;

template<typename T, std::size_t Size>
using Array = std::array<T, Size>;

template<typename T, std::size_t Extent = std::dynamic_extent>
using Span = std::span<T, Extent>;

} // namespace container
