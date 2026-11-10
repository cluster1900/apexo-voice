# Apexo (0.2.0)

Apexo 是本地实时语音转写与文字总结 SDK。支持四类核心场景：

1. **纯语音转文字（ASR-only）**：实时推入 16 kHz 单声道 float32 PCM（Web 支持任意采样率输入并自动流式重采样），实时获取 interim/final 转写与事件回调，无需加载总结模型。
2. **纯文字总结（Summary-only）**：直接对外部传入文本调用 `apexo_session_summarize_text` / `summarize(text)` 生成总结，无需语音模型或麦克风，且不修改会话底稿与摘要状态。
3. **录音进行中的滚动阶段摘要（Stage Summary）**：边录边调 `apexo_session_summarize_new` / `summarizeNew()`，仅针对已完成定稿（final）的新增快照追加阶段摘要，不打断麦克风采集、不阻塞 `push_pcm`、不强行将未说完的 interim 定稿或重置识别器。
4. **录音结束后的完整总结（Full Summary）**：停止采集后先调用 `endSegment()`（通过 `InputFinished` 刷出识别器尾帧，使最后一个词进入底稿），再调用 `summarize()` 对完整底稿执行分块与跨块汇总归纳。

## 仓库目录结构

| 路径 | 内容 |
| --- | --- |
| [`native/`](native/) | C 接口（[`apexo_session.h`](native/include/apexo_session.h)）、C++ 会话与引擎适配层、Android JNI 与 [`ApexoSession.java`](native/java/com/apexo/sdk/ApexoSession.java) |
| [`web/`](web/) | npm 包 `@apexo/sdk`（`0.2.0`）、运行时资源复制脚本 [`copy-runtime.mjs`](web/scripts/copy-runtime.mjs)、浏览器演示 [`web/demo/`](web/demo/) 及 Vite 接入示例 [`web/examples/vite/`](web/examples/vite/) |
| [`ios-demo/`](ios-demo/) | iOS 麦克风实时转写、阶段摘要、完整总结与外部文本总结演示工程 |
| [`examples/`](examples/) | 原生平台最小可运行接入示例（[`examples/macos/`](examples/macos/)、[`examples/android/`](examples/android/)、[`examples/windows/`](examples/windows/)） |
| [`pack.sh`](pack.sh) | 构建并打包五个平台交付物到 `dist/`（含配套运行库与许可证） |
| [`run-tests.sh`](run-tests.sh) | 自动重编本机动态库并运行全部原生测试与 Web 测试 |
| [`docs/`](docs/) | [会话约定](docs/会话.md)、[平台包与运行时矩阵](docs/平台包.md)、[网页接入与演示](docs/网页.md)、[0.2.0 SDK 改进开发与验收说明](docs/SDK改进开发说明.md) |

## 会话核心行为

宿主创建会话并推入 PCM。会话输出 `interim`（草稿）与 `final`（定稿）：

- 相邻 `final` 间隔小于 2000 毫秒时合并在同一显示行，达到 2000 毫秒则另起一行；界面滚动显示最近 4 行、200 个词，完整底稿（`full_transcript` / `fullTranscript`）完整保留不裁剪。
- 可通过 `apexo_session_set_transcript_callback`（Java `setTranscriptListener`、Web `onTranscript`）在会话锁外接收结构化转写事件（`text`、`isFinal`、`startMs`、`endMs`）。
- 一段录音结束时调用 `apexo_session_end_segment` / `await session.endSegment()`，识别器通过 `InputFinished` 补齐尾帧解码，把最后一句完整定稿并重建下一条流，无需人为追加数秒静音。
- 总结按需加载 Gemma 4 E2B `.litertlm`，生成完毕立即卸载。总结运行期间 `push_pcm` / `pushPcm` **继续正常接收音频（返回 `0`）**；同一会话同一时刻只允许一个总结任务运行，并发提交第二个总结任务立即返回 `APEXO_ERR_BUSY`（`-6`）。
- 清空当前会话调用 `apexo_session_reset` / `reset()`（`release()` 作为兼容别名保留）；彻底销毁会话调用 `apexo_session_destroy` / `close()` / `await dispose()`。

详细契约、提示词与返回码见 [docs/会话.md](docs/会话.md)。

## 五个平台交付包

执行 `./pack.sh` 在 `dist/` 下生成五个平台包及配套运行库：

- `dist/macos/`：`libapexo_session.dylib`、`apexo_session.h`、`libsherpa-onnx-c-api.dylib`、`libsherpa-onnx-cxx-api.dylib`、`libonnxruntime.dylib`、`liblitert-lm.dylib`
- `dist/ios/`：`ApexoSession.xcframework`、`SherpaOnnxC.xcframework`、`CLiteRTLM.xcframework`（均含 `ios-arm64` 真机与 `ios-arm64-simulator` 模拟器切片）
- `dist/android/`：`apexo-session.aar`（含 `arm64-v8a` 与 `x86_64` 双架构 JNI 与引擎 `.so`）
- `dist/windows/`：`apexo_session.dll`、`apexo_session.lib`、`apexo_session.def`、`apexo_session.h`、`sherpa-onnx-c-api.dll`、`onnxruntime.dll`、`litert-lm.dll`
- `dist/web/`：`apexo-sdk-0.2.0.tgz`

各平台接入方式、运行库版本与迁移指南见 [docs/平台包.md](docs/平台包.md)。

## 网页演示

```bash
cd web
npm ci
cd demo
node server.js
```

浏览器打开 `http://127.0.0.1:4173/`。页面提供「开始录音」「结束」「阶段摘要」「总结」「回听刚才这一段」「下载 Gemma 4 E2B」以及可选的「启用浏览器自带识别」开关（默认关闭，不会静默将音频发往云端）。详见 [docs/网页.md](docs/网页.md)。

## 测试

运行完整原生与网页测试套件（自动重编 `dist/macos/libapexo_session.dylib`）：

```bash
./run-tests.sh
```

附带真实流式语音模型与原生 sherpa-onnx C 动态库运行全套测试：

```bash
APEXO_TEST_ASR_DIR=/abs/path/to/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23 \
APEXO_SHERPA_LIBRARY=/abs/path/to/libsherpa-onnx-c-api.dylib \
./run-tests.sh
```

构建并验证原生三端（macOS、Android、Windows x64）接入示例：

```bash
./examples/macos/build_and_run.sh
./examples/android/build.sh
./examples/windows/build.sh
```
