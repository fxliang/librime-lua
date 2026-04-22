#include "lib/lua_templates.h"
#include "lua_gears.h"
#include "path_compat.h"
#include <vector>
#include <sstream>
#include <typeinfo>
#include <rime/schema.h>
#ifdef __GNUG__
#include <cxxabi.h>
#include <cstdlib>
#include <memory>
#endif

namespace rime {

namespace {

static bool file_exists(const std::string& file_path) {
  std::error_code ec;
  return fs::exists(file_path, ec);
}

static std::string get_package_path(lua_State* L) {
  std::string package_path;
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "path");
  if (lua_isstring(L, -1)) {
    package_path = lua_tostring(L, -1);
  }
  lua_pop(L, 2);
  return package_path;
}

static void set_package_path(lua_State* L, const std::string& package_path) {
  lua_getglobal(L, "package");
  lua_pushlstring(L, package_path.c_str(), package_path.size());
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);
}

static std::string get_schema_namespace(const Ticket& ticket) {
  if (!ticket.schema) {
    return "";
  }
  auto schema_namespace = fs::path(ticket.schema->schema_id()).parent_path();
  return schema_namespace.empty() ? "" : schema_namespace.generic_u8string();
}

static std::string get_lua_module_root(const Ticket& ticket) {
  if (ticket.klass.empty() || ticket.klass[0] != '*') {
    return "";
  }
  auto module = ticket.klass.substr(1);
  auto separator = module.find('*');
  if (separator != std::string::npos) {
    module = module.substr(0, separator);
  }
  return module;
}

static std::string make_namespace_lua_path_prefix(
    const std::string& name_space,
    const std::string& module_root) {
  if (name_space.empty()) {
    return "";
  }
  const auto user_dir = lua_path_compat::GetUserDataDir();
  const auto shared_dir = lua_path_compat::GetSharedDataDir();
  const auto user_ns_lua_dir = (fs::path(user_dir) / name_space / "lua").string();
  const auto shared_ns_lua_dir =
      (fs::path(shared_dir) / name_space / "lua").string();
  std::string prefix = user_ns_lua_dir + LUA_DIRSEP "?.lua;" +
                       user_ns_lua_dir + LUA_DIRSEP "?" LUA_DIRSEP "init.lua;" +
                       shared_ns_lua_dir + LUA_DIRSEP "?.lua;" +
                       shared_ns_lua_dir + LUA_DIRSEP "?" LUA_DIRSEP "init.lua;";
  if (!module_root.empty()) {
    const auto user_ns_module_dir =
        (fs::path(user_ns_lua_dir) / module_root).string();
    const auto shared_ns_module_dir =
        (fs::path(shared_ns_lua_dir) / module_root).string();
    prefix = user_ns_module_dir + LUA_DIRSEP "?.lua;" +
             user_ns_module_dir + LUA_DIRSEP "?" LUA_DIRSEP "init.lua;" +
             shared_ns_module_dir + LUA_DIRSEP "?.lua;" +
             shared_ns_module_dir + LUA_DIRSEP "?" LUA_DIRSEP "init.lua;" +
             prefix;
  }
  return prefix;
}

static bool namespace_resources_only(const Ticket& ticket) {
  if (!ticket.schema) {
    return false;
  }
  auto* config = ticket.schema->config();
  if (!config) {
    return false;
  }
  bool value = false;
  return config->GetBool("schema/namespace_resources_only", &value) && value;
}

static std::vector<std::string> split_package_path(const std::string& package_path) {
  std::vector<std::string> entries;
  std::string::size_type start = 0;
  while (start <= package_path.size()) {
    const auto end = package_path.find(';', start);
    if (end == std::string::npos) {
      entries.push_back(package_path.substr(start));
      break;
    }
    entries.push_back(package_path.substr(start, end - start));
    start = end + 1;
  }
  return entries;
}

static std::string join_package_path(const std::vector<std::string>& entries) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& entry : entries) {
    if (entry.empty()) {
      continue;
    }
    if (!first) {
      oss << ';';
    }
    first = false;
    oss << entry;
  }
  return oss.str();
}

static std::vector<std::string> default_lua_data_paths() {
  const auto user_dir = lua_path_compat::GetUserDataDir();
  const auto shared_dir = lua_path_compat::GetSharedDataDir();
  return {
      (fs::path(user_dir) / "lua" / "?.lua").string(),
      (fs::path(user_dir) / "lua" / "?" / "init.lua").string(),
      (fs::path(shared_dir) / "lua" / "?.lua").string(),
      (fs::path(shared_dir) / "lua" / "?" / "init.lua").string(),
  };
}

static std::string remove_default_lua_data_paths(
    const std::string& package_path) {
  const auto blocked = default_lua_data_paths();
  auto entries = split_package_path(package_path);
  std::vector<std::string> filtered;
  for (const auto& entry : entries) {
    bool keep = true;
    for (const auto& blocked_entry : blocked) {
      if (entry == blocked_entry) {
        keep = false;
        break;
      }
    }
    if (keep) {
      filtered.push_back(entry);
    }
  }
  return join_package_path(filtered);
}

static void load_namespace_rime_lua(lua_State* L, const std::string& name_space) {
  if (name_space.empty()) {
    return;
  }
  const std::string loaded_key = "__rime_ns_bootstrap__/" + name_space;
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "loaded");
  lua_getfield(L, -1, loaded_key.c_str());
  const bool already_loaded = lua_toboolean(L, -1);
  lua_pop(L, 1);
  if (already_loaded) {
    lua_pop(L, 2);
    return;
  }
  lua_pop(L, 2);

  const auto user_dir = lua_path_compat::GetUserDataDir();
  const auto shared_dir = lua_path_compat::GetSharedDataDir();
  const auto user_file = (fs::path(user_dir) / name_space / "rime.lua").string();
  const auto shared_file =
      (fs::path(shared_dir) / name_space / "rime.lua").string();
  const std::string file_path =
      file_exists(user_file) ? user_file : (file_exists(shared_file) ? shared_file
                                                                      : "");
  if (file_path.empty()) {
    return;
  }
  if (luaL_dofile(L, file_path.c_str())) {
    const char* e = lua_tostring(L, -1);
    LOG(ERROR) << "namespace rime.lua error (" << name_space << "): " << e;
    lua_pop(L, 1);
    return;
  }
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "loaded");
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, loaded_key.c_str());
  lua_pop(L, 2);
}

}  // namespace

static std::string demangle(const char* name) {
#ifdef __GNUG__
  int status = -1;
  std::unique_ptr<char, void(*)(void*)> res {
    abi::__cxa_demangle(name, NULL, NULL, &status),
    std::free
  };
  std::string demangled_name = (status == 0) ? res.get() : name;
#else
  std::string demangled_name = name;
#endif
  auto pos = demangled_name.rfind("::");
  if (pos != std::string::npos) {
    return demangled_name.substr(pos + 2);
  }
  return demangled_name;
}

//--- LuaTranslation
bool LuaTranslation::Next() {
  if (exhausted()) {
    return false;
  }
  auto r = lua_->resume<an<Candidate>>(f_);
  if (!r.ok()) {
    LuaErr e = r.get_err();
    if (e.e != "")
      LOG(ERROR) << "LuaTranslation::Next error(" << e.status << "): " << e.e;
    set_exhausted(true);
    return false;
  } else {
    c_ = r.get();
    return true;
  }
}

LuaTranslation::~LuaTranslation() {
  lua_->gc();
}

static std::vector<std::string> split_string(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> result;
    size_t pos = 0;
    size_t found;
    while ((found = str.find(delimiter, pos)) != std::string::npos) {
        result.push_back(str.substr(pos, found - pos));
        pos = found + delimiter.length();
    }
    result.push_back(str.substr(pos));
    return result;
}
static bool sub_module_init(lua_State *L, const Ticket &t,
                            const std::vector<std::string>& vec_klass) {
  size_t vec_klass_sz= vec_klass.size();
  for (size_t index=1 ;index < vec_klass_sz; index++) {
    lua_getfield(L, -1, vec_klass.at(index).c_str() );
    if ( index < vec_klass_sz-1 && lua_type(L, -1) != LUA_TTABLE ) {
      std::ostringstream ostr;
      ostr << "Lua Compoment of initialize  error:("
        << " klass: " << t.klass
        << " module: "<< vec_klass.at(0)
        << ", name_space: " << t.name_space
        << ", sub-table(" <<index << ") "
        << "\"" << vec_klass.at(index) << "\" type: " << luaL_typename(L, -1)
        << " ): " << "type error expect table ";
      LOG(ERROR) << ostr.str();

      LuaType<string>::pushdata(L, ostr.str());
      return false;
    }
  }
  return true;
}
//---
static void raw_init(lua_State *L, const Ticket &t,
                     an<LuaObj> *env, an<LuaObj> *func, an<LuaObj> *fini, an<LuaObj> *tags_match= NULL, string* file_path = NULL, fs::file_time_type* last_write_time = NULL) {
  const auto schema_namespace = get_schema_namespace(t);
  const auto module_root = get_lua_module_root(t);
  const auto package_path_backup = get_package_path(L);
  const auto namespace_path_prefix =
      make_namespace_lua_path_prefix(schema_namespace, module_root);
  auto package_path_fallback = package_path_backup;
  if (!schema_namespace.empty() && namespace_resources_only(t)) {
    package_path_fallback = remove_default_lua_data_paths(package_path_backup);
  }
  if (!namespace_path_prefix.empty()) {
    set_package_path(L, namespace_path_prefix + package_path_fallback);
  }
  load_namespace_rime_lua(L, schema_namespace);

  lua_newtable(L);
  Engine *e = t.engine;
  LuaType<Engine *>::pushdata(L, e);
  lua_setfield(L, -2, "engine");
  LuaType<const string &>::pushdata(L, t.name_space);
  lua_setfield(L, -2, "name_space");
  *env = LuaObj::todata(L, -1);
  lua_pop(L, 1);

  std::vector<std::string> _vec_klass = (t.klass[0] == '*') ?
    split_string(t.klass.substr(1), "*") : split_string(t.klass, "*");
  if (t.klass.size() > 0 && t.klass[0] == '*') {
    if (file_path) {
      lua_getglobal(L, "package");
      lua_getfield(L, -1, "searchpath");
      lua_pushstring(L, _vec_klass.at(0).c_str());
      lua_getfield(L, -3, "path");
      if (lua_pcall(L, 2, 1, 0) == LUA_OK && lua_isstring(L, -1)) {
        *file_path = lua_tostring(L, -1);
        if (last_write_time) {
          std::error_code ec;
          *last_write_time = fs::last_write_time(*file_path, ec);
        }
      }
      lua_pop(L, 2);
    }

    lua_getglobal(L, "require");
    lua_pushstring(L, _vec_klass.at(0).c_str());
    int status = lua_pcall(L, 1, 1, 0);
    if (status != LUA_OK) {
      const char *e = lua_tostring(L, -1);
      LOG(ERROR) << "Lua Compoment of autoload error:("
                 << " module: "<< t.klass
                 << " name_space: " << t.name_space
                 << " status: " << status
                 << " ): " << e;
    }
  } else {
    lua_getglobal(L, _vec_klass.at(0).c_str());
  }

  if (_vec_klass.size() > 1) {
    sub_module_init(L, t, _vec_klass);
  }

  if (lua_type(L, -1) == LUA_TTABLE) {
    lua_getfield(L, -1, "init");
    if (lua_type(L, -1) == LUA_TFUNCTION) {
      LuaObj::pushdata(L, *env);
      int status = lua_pcall(L, 1, 1, 0);
      if (status != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        LOG(ERROR) << "Lua Compoment of initialize  error:("
          << " module: "<< t.klass
          << " name_space: " << t.name_space
          << " status: " << status
          << " ): " << e;
      }
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "fini");
    if (lua_type(L, -1) == LUA_TFUNCTION) {
      *fini = LuaObj::todata(L, -1);
    }
    lua_pop(L, 1);

    if (tags_match) {
      lua_getfield(L, -1, "tags_match");
      if (lua_type(L, -1) == LUA_TFUNCTION) {
        *tags_match = LuaObj::todata(L, -1);
      }
      lua_pop(L, 1);
    }

    lua_getfield(L, -1, "func");
  }

  if (lua_type(L, -1) != LUA_TFUNCTION) {
    LOG(ERROR) << "Lua Compoment of initialize  error:("
      << " module: "<< t.klass
      << " name_space: " << t.name_space
      << " func type: " << luaL_typename(L, -1)
      << " ): " << "func type error expect function ";
  }
  *func = LuaObj::todata(L, -1);
  lua_pop(L, 1);
  if (!namespace_path_prefix.empty()) {
    set_package_path(L, package_path_backup);
  }
}

LuaGearImpl::LuaGearImpl(const Ticket& ticket, Lua* lua, const std::string& impl_name)
  : lua_(lua), ticket_(ticket), impl_name_(demangle(impl_name.c_str())) {
  lua->to_state([&](lua_State *L) {
    raw_init(L, ticket, &env_, &func_, &fini_, &tags_match_, &file_path_, &last_write_time_);
  });
}

LuaGearImpl::~LuaGearImpl() {
  if (fini_) {
    auto r = lua_->void_call<an<LuaObj>, an<LuaObj>>(fini_, env_);
    if (!r.ok()) {
      auto e = r.get_err();
      LOG(ERROR) << impl_name_ << "::~" << impl_name_ << " of "<< ticket_.name_space << " error(" << e.status << "): " << e.e;
    }
  }
}

void LuaGearImpl::ReloadIfModified() {
  if (file_path_.empty()) return;
  time_t now = time(NULL);
  if (now - last_check_time_ < 1) return;
  last_check_time_ = now;

  std::error_code ec;
  auto current_time = fs::last_write_time(file_path_, ec);
  if (!ec && current_time > last_write_time_) {
    last_write_time_ = current_time;
    LOG(INFO) << "Reloading Lua module: " << file_path_;
    lua_->to_state([&](lua_State *L) {
      if (ticket_.klass.size() > 0 && ticket_.klass[0] == '*') {
        std::string module_name = ticket_.klass.substr(1);
        size_t pos = module_name.find('*');
        if (pos != std::string::npos) {
          module_name = module_name.substr(0, pos);
        }
        lua_getglobal(L, "package");
        lua_getfield(L, -1, "loaded");
        lua_pushnil(L);
        lua_setfield(L, -2, module_name.c_str());
        lua_pop(L, 2);
      }
      raw_init(L, ticket_, &env_, &func_, &fini_, &tags_match_, &file_path_);
    });
  }
}

//--- LuaFilter
LuaFilter::LuaFilter(const Ticket& ticket, Lua* lua)
  : Filter(ticket), TagMatching(ticket), LuaGear(ticket, lua) {
}

an<Translation> LuaFilter::Apply(
  an<Translation> translation, CandidateList* candidates) {
  ReloadIfModified();
  auto f = lua_->newthread<an<LuaObj>, an<Translation>,
                           an<LuaObj>, CandidateList *>(func_, translation, env_, candidates);
  return New<LuaTranslation>(lua_, f);
}


//--- LuaTranslator
LuaTranslator::LuaTranslator(const Ticket& ticket, Lua* lua)
  : Translator(ticket), LuaGear(ticket, lua) {
}

an<Translation> LuaTranslator::Query(const string& input,
                                     const Segment& segment) {
  ReloadIfModified();
  auto f = lua_->newthread<an<LuaObj>, const string &, const Segment &,
                           an<LuaObj>>(func_, input, segment, env_);
  an<Translation> t = New<LuaTranslation>(lua_, f);
  if (t->exhausted())
    return an<Translation>();
  else
    return t;
}

//--- LuaSegmentor
LuaSegmentor::LuaSegmentor(const Ticket& ticket, Lua *lua)
  : Segmentor(ticket), LuaGear(ticket, lua) {
}

bool LuaSegmentor::Proceed(Segmentation* segmentation) {
  ReloadIfModified();
  auto r = lua_->call<bool, an<LuaObj>, Segmentation &,
                      an<LuaObj>>(func_, *segmentation, env_);
  if (!r.ok()) {
    auto e = r.get_err();
    LOG(ERROR) << "LuaSegmentor::Proceed of "<< name_space_ << " error(" << e.status << "): " << e.e;
    return true;
  } else
    return r.get();
}

//--- LuaProcessor
LuaProcessor::LuaProcessor(const Ticket& ticket, Lua* lua)
  : Processor(ticket), LuaGear(ticket, lua) {
}

ProcessResult LuaProcessor::ProcessKeyEvent(const KeyEvent& key_event) {
  ReloadIfModified();
  auto r = lua_->call<int, an<LuaObj>, const KeyEvent&,
                      an<LuaObj>>(func_, key_event, env_);
  if (!r.ok()) {
    auto e = r.get_err();
    LOG(ERROR) << "LuaProcessor::ProcessKeyEvent of "<< name_space_ << " error(" << e.status << "): " << e.e;
    return kNoop;
  } else
    switch (r.get()) {
    case 0: return kRejected;
    case 1: return kAccepted;
    default: return kNoop;
    }
}

}  // namespace rime
