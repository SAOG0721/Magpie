#pragma once
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace Magpie {

// Dynamic resource names may expire after Get returns. Own the keys and keep
// returned text references valid when later lookups add entries.
class LocalizedStringCache {
public:
    template<class Load>
    const std::string& Get(std::wstring_view key, Load&& load) {
        if (const auto it = _values.find(key); it != _values.end()) return it->second;
        return _values.emplace(std::wstring(key), std::forward<Load>(load)(key)).first->second;
    }

private:
    std::map<std::wstring, std::string, std::less<>> _values;
};

}
