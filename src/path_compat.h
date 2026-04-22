#ifndef RIME_LUA_PATH_COMPAT_H_
#define RIME_LUA_PATH_COMPAT_H_

#include <string>
#include <utility>
#include <rime/service.h>
#include <rime_api.h>

namespace rime {
namespace lua_path_compat {

template <typename>
using void_t = void;

template <typename T, typename = void>
struct DataDirProvider {
  static std::string GetSharedDataDir() {
    return std::string(rime_get_api()->get_shared_data_dir());
  }

  static std::string GetUserDataDir() {
    return std::string(rime_get_api()->get_user_data_dir());
  }
};

template <typename T>
struct DataDirProvider<T, void_t<decltype(std::declval<T>().user_data_dir.string())>> {
  static std::string GetSharedDataDir() {
    // path::string() returns native encoding on Windows.
    T& deployer = Service::instance().deployer();
    return deployer.shared_data_dir.string();
  }

  static std::string GetUserDataDir() {
    T& deployer = Service::instance().deployer();
    return deployer.user_data_dir.string();
  }
};

inline std::string GetSharedDataDir() {
  return DataDirProvider<Deployer>::GetSharedDataDir();
}

inline std::string GetUserDataDir() {
  return DataDirProvider<Deployer>::GetUserDataDir();
}

}  // namespace lua_path_compat
}  // namespace rime

#endif  // RIME_LUA_PATH_COMPAT_H_
