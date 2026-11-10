# Apexo

Apexo 是本地实时转写和文字总结。麦克风或宿主送来的 PCM 进会话，转写一边出字，用户点总结时才把整份底稿交给 Gemma 4 E2B。音频不进总结模型。

本仓库是 Apexo 自己的实现。协议是 [MIT](LICENSE)。语音模型和 Gemma 权重不打进包里，由宿主按下面的文件名自己准备。sherpa-onnx 和 LiteRT-LM 各自带自己的许可证。

## 仓库里有什么

| 路径 | 内容 |
| --- | --- |
| `native/` | C 接口和 C++ 会话。Android、iOS、macOS、Windows 都链这一份 |
| `web/` | npm 包 `@apexo/sdk`，以及浏览器演示 |
| `ios-demo/` | iOS 麦克风演示 |
| `pack.sh` | 打出五个平台的包到 `dist/` |
| `run-tests.sh` | 跑原生会话测试和网页测试 |
| `docs/` | 会话约定、平台包、网页演示 |

## 会话在做什么

宿主创建会话，持续推入 16 kHz、单声道、float32 的 PCM。会话给出 interim 和 final。相邻 final 如果间隔小于 2 秒就合成一行显示；满 2 秒则另起一行。界面最多 4 行、200 个词，完整底稿不裁。一段结束时，还没定稿的 interim 会收成 final。

总结只在调用总结时加载 Gemma，生成完就卸载。Gemma 还在内存里时，再推 PCM 会拒绝。提示词固定为只根据给定转写写，转写里没有的内容不补。底稿按最多 1500 个字符分块。

细节和返回码见 [docs/会话.md](docs/会话.md)。

## 五个包

Android AAR、iOS xcframework、macOS dylib、Windows x64 DLL，加网页 npm 包。调用方式同一套。录音、权限和界面由宿主负责。见 [docs/平台包.md](docs/平台包.md)。

原生总结用 Gemma 4 E2B 的 `.litertlm`，文件名不要带 `-web`。网页总结用带 WebGPU 产物的文件。演示页上的「下载 Gemma 4 E2B」拉取的是 `gemma-4-E2B-it-gpu.litertlm`。

## 网页演示

```bash
cd web
npm ci
cd demo
node server.js
```

浏览器打开 `http://127.0.0.1:4173/`。页面有开始录音、结束、总结、回听刚才这一段，以及下载 Gemma 4 E2B。用法见 [docs/网页.md](docs/网页.md)。

## 测试

网页会话和采集测试不需要先打包：

```bash
cd web
npm ci
node --import ./test/register-fakes.js ./test/adapter.test.js
node ./test/session.test.js
node ./test/capture.test.js
node ./test/asr-presence.test.js
node ./test/demo-flow.test.js
```

`demo-flow.test.js` 使用开发依赖里的 Playwright。第一次跑之前执行 `npx playwright install chromium`。原生库测试要先有 `dist/macos/libapexo_session.dylib`，再跑仓库根目录的 `./run-tests.sh`。
