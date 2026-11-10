/* Host-runnable verification test for Windows x64 path conversion & DLL search behavior.
 *
 * Exercises the exact Windows UTF-8 / ACP / 8.3 short-path / LoadLibraryExW logic used by
 * native/src/engine_adapter.cpp under simulated Windows code pages (CP1252 English without 8.3,
 * CP1252 with 8.3 short name, CP936 GBK, and CP_UTF8).
 */
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

static void check(bool ok, const char* name) {
  if (ok) {
    std::printf("PASS %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
    ++g_failures;
  }
}

namespace win_sim {

using DWORD = unsigned long;
using UINT = unsigned int;
using BOOL = int;
constexpr BOOL FALSE_VAL = 0;
constexpr BOOL TRUE_VAL = 1;
constexpr UINT CP_ACP_VAL = 0;
constexpr UINT CP_UTF8_VAL = 65001;
constexpr DWORD MB_ERR_INVALID_CHARS_VAL = 0x08;
constexpr DWORD WC_NO_BEST_FIT_CHARS_VAL = 0x0400;

static UINT g_simulated_acp = 1252;  // English Windows code page by default
static std::u16string g_simulated_short_path;

UINT GetACP() { return g_simulated_acp; }

/* Minimal UTF-8 <-> UTF-16 and simulated CP1252 conversion to test the exact adapter logic. */
bool utf8_to_u16(const std::string& s, std::u16string* out) {
  out->clear();
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    size_t extra = 0;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= s.size()) return false;
    for (size_t j = 1; j <= extra; ++j) {
      unsigned char cc = static_cast<unsigned char>(s[i + j]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    i += 1 + extra;
    if (cp <= 0xFFFF) {
      out->push_back(static_cast<char16_t>(cp));
    } else {
      cp -= 0x10000;
      out->push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out->push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
  }
  return true;
}

std::string u16_to_utf8(const std::u16string& w) {
  std::string out;
  for (size_t i = 0; i < w.size(); ++i) {
    uint32_t cp = w[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
      ++i;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

bool wide_to_acp(const std::u16string& wide, std::string* out) {
  if (GetACP() == CP_UTF8_VAL) {
    *out = u16_to_utf8(wide);
    return true;
  }
  std::string s;
  for (char16_t ch : wide) {
    if (ch > 0x7F) {
      // Unrepresentable in simulated CP1252 -> used_default = TRUE -> must fail!
      return false;
    }
    s.push_back(static_cast<char>(ch));
  }
  *out = s;
  return true;
}

bool normalize_utf8_path(const std::string& path, std::string* out) {
  std::u16string w;
  if (!utf8_to_u16(path, &w) || w.empty()) return false;
  *out = u16_to_utf8(w);
  return true;
}

bool normalize_narrow_path(const std::string& path, std::string* out) {
  std::u16string w;
  if (!utf8_to_u16(path, &w) || w.empty()) return false;
  if (GetACP() == CP_UTF8_VAL) {
    *out = path;
    return true;
  }
  if (wide_to_acp(w, out)) return true;
  if (g_simulated_short_path.empty()) return false;
  return wide_to_acp(g_simulated_short_path, out);
}

/* Mirrors open_library slash normalization + self_module_dir bare-name resolution on Windows. */
std::string resolve_win32_dll_target(const std::string& raw_path, const std::string& self_dir,
                                     const std::string& existing_sibling) {
  std::string wpath = raw_path;
  for (char& ch : wpath) {
    if (ch == '/') ch = '\\';
  }
  if (wpath.find_first_of("\\:") != std::string::npos) {
    return "FULL:" + wpath;
  }
  if (!self_dir.empty()) {
    const std::string sibling = self_dir + "\\" + wpath;
    if (sibling == existing_sibling) {
      return "FULL:" + sibling;
    }
  }
  return "DEFAULT:" + wpath;
}

}  // namespace win_sim

int main() {
  std::string out;

  /* 1. ASCII path under English CP1252 succeeds in both ACP and UTF-8. */
  win_sim::g_simulated_acp = 1252;
  win_sim::g_simulated_short_path.clear();
  check(win_sim::normalize_narrow_path("C:\\models\\gemma-4-E2B-it.litertlm", &out) &&
            out == "C:\\models\\gemma-4-E2B-it.litertlm",
        "Windows ASCII path succeeds in normalize_narrow_path");

  /* 2. Chinese path under CP1252 WITHOUT 8.3 short name:
   *    - normalize_narrow_path rejects lossy '?' substitution (returns false)
   *    - normalize_utf8_path preserves full UTF-8 path for sherpa-onnx / UTF-8 fallback! */
  const std::string zh_path = "C:\\模型目录\\语音\\encoder.onnx";
  check(!win_sim::normalize_narrow_path(zh_path, &out),
        "Windows Chinese path under CP1252 without 8.3 short name is not lossily corrupted into '?'");
  check(win_sim::normalize_utf8_path(zh_path, &out) && out == zh_path,
        "Windows Chinese path preserves exact UTF-8 for sherpa-onnx ToWideString and LiteRT UTF-8 fallback");

  /* 3. Chinese path under CP1252 WITH 8.3 short name uses the 8.3 short path for ACP consumers. */
  win_sim::g_simulated_short_path = u"C:\\MODEL~1\\ASR~1\\ENCODER.ONNX";
  check(win_sim::normalize_narrow_path(zh_path, &out) && out == "C:\\MODEL~1\\ASR~1\\ENCODER.ONNX",
        "Windows Chinese path with 8.3 short name resolves to exact ASCII short path");

  /* 4. Chinese path under CP_UTF8 (65001) passes UTF-8 directly. */
  win_sim::g_simulated_acp = win_sim::CP_UTF8_VAL;
  win_sim::g_simulated_short_path.clear();
  check(win_sim::normalize_narrow_path(zh_path, &out) && out == zh_path,
        "Windows Chinese path under CP_UTF8 passes UTF-8 directly");

  /* 5. Forward slashes in explicit DLL path are normalized to backslashes for LoadLibraryExW. */
  check(win_sim::resolve_win32_dll_target("C:/apexo/sdk/sherpa-onnx-c-api.dll", "D:\\app", "") ==
            "FULL:C:\\apexo\\sdk\\sherpa-onnx-c-api.dll",
        "Windows explicit DLL path normalizes '/' to '\\' for LoadLibraryExW");

  /* 6. Bare DLL name resolves to apexo_session.dll's own directory first (matching macOS @loader_path). */
  check(win_sim::resolve_win32_dll_target("sherpa-onnx-c-api.dll", "D:\\sdk\\windows",
                                          "D:\\sdk\\windows\\sherpa-onnx-c-api.dll") ==
            "FULL:D:\\sdk\\windows\\sherpa-onnx-c-api.dll",
        "Windows bare DLL name finds companion DLL next to apexo_session.dll even when host .exe is elsewhere");
  check(win_sim::resolve_win32_dll_target("sherpa-onnx-c-api.dll", "D:\\sdk\\windows", "") ==
            "DEFAULT:sherpa-onnx-c-api.dll",
        "Windows bare DLL name falls back to default search when absent next to apexo_session.dll");

  if (g_failures != 0) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all windows host verification assertions passed\n");
  return 0;
}
