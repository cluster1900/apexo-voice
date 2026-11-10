# Apexo SDK 改进开发说明

日期：2026-11-10。评审基线：`110c960`，目标版本：`0.2.0`（已完成实现与全平台验证）。

**状态：已完成实施并通过五平台验证（`0.2.0`）。** 本文定义的 D01～D08 全部接口、并发契约、尾帧收尾、跨块归纳、资源打包与平台示例均已落地并同步至 [会话约定](会话.md)、[平台包说明](平台包.md) 和 [网页说明](网页.md)。

本文面向负责 C++ 核心、Android JNI/Java、Apple 示例、Windows/macOS 包和 Web SDK 的开发者。按“目标行为 → 修改文件与函数 → 实现步骤 → 验收”组织，可以直接按任务编号拆分开发工作。

## 1. 本轮目标与范围

完成以下四种正常使用流程：

| 使用场景 | 完成后的行为 |
| --- | --- |
| 只做实时语音转文字 | 配置 ASR 即可；不需要总结模型；持续得到 interim/final |
| 只总结已有文字 | 配置总结模型即可；直接传文字；不需要 ASR 模型和麦克风 |
| 录音中查看阶段摘要 | 总结已定稿文字的快照，音频继续进入识别器，正在识别的句子继续修正 |
| 结束录音后生成完整总结 | 先等待 ASR 收尾，再读取完整底稿并生成整体总结 |

继续使用现有的 C++ 核心、Java 包装和 JavaScript 实现。第一轮继续按需加载 Gemma、一次请求结束后卸载；不引入模型池、常驻策略管理器或自动任务调度。

本轮不增加知识库、向量数据库、RAG、Agent、自动问答、说话人分离、自动选模型、云端重试链路。麦克风权限、录音设备和应用 UI 仍由宿主负责。保留现有 16 kHz/单声道/float32 的原生音频契约和 Web 重采样能力。

### 1.1 与 Foresight 的关系

Google 官方介绍的 Foresight 是 Mac 上的本地会议应用，具有麦克风/系统音频接入、结合当前会议增强笔记、历史内容检索等产品能力。Apexo 本轮借鉴其中“持续转写、基于会议上下文处理文字”的使用流程，不把整个会议应用塞进 SDK。[Google 官方介绍](https://developers.googleblog.com/google-ai-edge-with-embeddinggemma-2/)

该对比来自公开产品资料，不据此推断 Foresight 的内部线程、ASR 分块算法或模型驻留策略，也不作为两者准确率、速度相当的证明。

### 1.2 开发任务与顺序

| 编号 | 优先级 | 工作内容 | 依赖 |
| --- | --- | --- | --- |
| [D01](#d01) | P1 | 总结读取快照，修复重复转写与录音被拒绝 | 无 |
| [D02](#d02) | P1 | 增加独立文字总结入口 | 复用 D01 的总结执行路径 |
| [D03](#d03) | P1 | 完整实现 ASR 收尾，Web 可以等待结束 | 与 D01 一起验收 |
| [D04](#d04) | P1 | 最终总结增加跨段归纳，明确滚动摘要含义 | D01、D02 |
| [D05](#d05) | P1 | Web 资源路径可配置，脱离 Demo 接入 | 无；最终与 D02、D03 联调 |
| [D06](#d06) | P1 | 补齐四个原生平台的运行时依赖和可运行示例 | D01～D04 的最终接口 |
| [D07](#d07) | P2 | 输出转写事件，明确显示、重置和错误契约 | D01、D03 |
| [D08](#d08) | P1 | 更新测试、迁移说明与发布验收 | 覆盖以上任务 |

P1 是下一版正式交付前要完成的内容。D07 可以在核心行为稳定后单独提交；不能为了 D07 延迟修复重复底稿、总结干扰识别等问题。

## 2. 先统一接口和数据含义

### 2.1 三种总结入口

保留一个会话对象，复用同一套总结引擎，不新增独立的“总结 SDK”。

| 用途 | C API | Java | Web |
| --- | --- | --- | --- |
| 总结当前已定稿底稿 | 现有 `apexo_session_summarize` | 现有 `summarize()` | 现有 `summarize()` |
| 总结外部文字 | 新增 `apexo_session_summarize_text` | 新增重载 `summarize(String text)` | `summarize(text)` |
| 追加新增底稿的阶段摘要 | 现有 `apexo_session_summarize_new` | 现有 `summarizeNew()` | 现有 `summarizeNew()` |

Web 的 `summarize()` 或 `summarize(undefined)` 表示会话总结；传入字符串表示外部文字总结；`null`、数字、对象属于参数错误。空字符串是合法输入，直接返回空字符串，不加载模型。

目标 C 签名：

```c
APEXO_API int apexo_session_summarize_text(
    ApexoSession* session,
    const char* text,
    char* out,
    int32_t out_cap);
```

`text == NULL` 返回参数错误；`text` 指向 UTF-8 字符串。输出缓冲规则沿用现有 C API：成功返回 `0`，空间不够返回 `-1`，不能静默截断。Java/Swift 包装继续负责字符串转换和输出缓冲。

### 2.2 状态写入规则

| 调用 | 读取什么 | 成功后修改什么 | 不允许修改什么 |
| --- | --- | --- | --- |
| `summarize()` | 本次快照中的全部 final | 替换 `summaryText`；增量游标移到快照结尾 | interim、ASR 状态、音频输入、底稿 |
| `summarize(text)` | 调用方传入的文字副本 | 只返回结果 | 底稿、`summaryText`、增量游标、ASR 状态 |
| `summarizeNew()` | 增量游标之后的 final 快照 | 追加到 `summaryText`；游标移到本次快照结尾 | interim、ASR 状态、音频输入、已有 final |

约定：

- `fullTranscript` 只保存 final。interim 用于实时预览，不进入总结输入。
- `summarizeNew()` 没有新 final 时直接返回已有 `summaryText`，不加载模型。
- 总结失败时保留旧 `summaryText` 和旧游标，不留下半份摘要。
- 同一会话同一时刻只执行一个总结任务，三个入口共用这个限制。不排队、不自动重试。
- 总结期间继续收到的 final 留给下一次调用，本次结果只对应启动时取得的快照。
- 全量总结成功后再调用增量总结，只处理全量快照之后的新 final。
- 调用外部文字总结后，下一次增量总结仍从原来的会话游标开始。

原生游标继续按 UTF-8 字节数记录，JS 游标继续按字符串索引记录。它们只在各自进程内使用，不能把原生字节偏移直接当成 JS 索引互传。

### 2.3 异步和错误

Web 对外的 `summarize()`、`summarize(text)`、`summarizeNew()` 一律返回 `Promise<string>`；`endSegment()` 改为 `Promise<void>`。不再让调用方判断返回的是数字、字符串还是 Promise。真实引擎和注入测试引擎使用相同的公开契约。

原生和 Java 的总结仍是阻塞调用，由宿主放到后台线程。`pushPcm` 继续返回整数状态，避免为音频热路径引入新的异步机制。

保留已有 `-1`～`-5` 的主要用途，增加 `APEXO_ERR_BUSY = -6`，专门表示同一会话已经有总结在执行。`-5` 用于运行时加载、生成或识别失败，不再代表“Gemma 在内存中，因此拒绝录音”。Web 错误对象带整数 `code` 和可读 `message`；不新增复杂错误层级。

Java 本轮保留总结失败时抛 `IllegalStateException` 的形式，错误消息包含返回码和具体原因。`-6` 的测试同时覆盖 Java 包装。成功调用应清理对应的旧错误，避免显示上一次失败原因。

<a id="d01"></a>

## 3. D01：让总结不改变识别状态

### 3.1 当前问题和定位

| 文件 | 函数/位置 | 当前问题 |
| --- | --- | --- |
| [native/src/session.cpp](../native/src/session.cpp) | `apexo_session_summarize` | 把 interim 写成 final；开始前清空旧摘要；设置驻留后拒绝 PCM |
| 同上 | `apexo_session_summarize_new` | 调用 `end_segment_locked`，总结会重置正在识别的句子 |
| 同上 | `apexo_session_push_pcm` | 依据 `gemma_resident` 返回 `-5` |
| [web/src/session.js](../web/src/session.js) | `summarize`、`summarizeNew`、`runChunks` | 与原生相同；`blockPcm` 还让驻留状态含义不一致 |
| [web/src/index.js](../web/src/index.js) | `summarizeWithGemma` | 增量总结在调用核心前执行 `api.endSegment()` |
| 同上 | `pushPcm` | 核心检查之前还有一层驻留拦截 |

已复现：先收到 `interim="明天交付"`，调用全量总结，再收到 `final="明天交付新版"`，底稿变成两行。修复不能只增加字符串去重，因为真正重复说出的两句话仍应保留。

### 3.2 实现步骤

1. 删除所有总结入口中把 interim 转 final、关闭段落、重置 ASR 的代码。Web 包装中的 `api.endSegment()` 也必须删除。
2. 在原生会话锁内复制 `full`、当前 `epoch`、快照结束位置；增量调用另外读取旧游标。复制完成后释放会话锁。
3. 在会话锁外完成模型加载、分块/归纳、卸载。不能把整个总结过程放在 `session->mu` 内，否则音频线程仍会被阻塞。
4. JS 使用同样的数据流。总结方法可以统一为 `async`，用 `await` 同时接收同步和异步引擎返回值，删除重复的同步/Promise 分支。
5. 原生、Web 核心以及 Web 包装都移除“Gemma 驻留就拒绝 PCM/ASR 事件”的条件。继续保留真实的识别器缺失、失败和音频格式检查。
6. `gemmaResident` 仅表示加载开始至卸载完成这一段生命周期；全量、增量、外部文字总结都应正确设置。它不再是录音许可开关。
7. 引擎成功生成结果后，再检查 epoch 并一次性写入摘要和游标。发生错误不清空旧结果；C 输出缓冲不足时也不提交摘要/游标，避免重试跳过这一段。
8. 保留现有 epoch 机制。总结过程中发生 `reset/release`，旧任务完成后返回空字符串，不向新会话写入结果；模型仍由旧任务负责卸载一次。
9. 三个总结入口共用 busy 标志。Web 在异步导入运行时、等待 Worker、探测模型之前就登记任务，防止两个调用在准备阶段同时进入并互相覆盖 `callModel` 等状态。

空输入判断应早于模型文件探测和运行时导入；否则纯文字空串仍可能因为缺权重、路径错误或浏览器运行环境而失败。基本参数错误和会话已关闭仍正常返回错误。

推荐的数据流如下，伪代码中的 busy/finally 覆盖准备、生成和卸载整个过程：

```text
申请本会话的总结执行权，已经 busy 则返回 -6
try:
    在短锁内取得输入快照、结束位置、epoch
    空输入按入口的空值约定直接返回
    在锁外加载模型、生成结果、卸载模型
    在短锁内检查 epoch
    epoch 已变化：返回空字符串，不回写
    检查 C 输出容量
    按入口规则提交摘要和游标，返回结果
finally:
    释放总结执行权
```

Web 会话总结可以先 `await drain()` 等待已经发送的音频被识别，再取 final 快照。`drain()` 只是消息屏障，不关闭句子；屏障之后到达的音频仍继续处理。外部文字总结不等待 ASR。

### 3.3 录音和内存策略

Demo 的“结束并总结”由宿主执行：停止采集 → 等待 ASR 收尾 → 总结。若增加“阶段摘要”按钮，则直接调用 `summarizeNew()`，不停止采集。

本轮维持 ASR 已加载、Gemma 按需加载/卸载的方式。停止送音频不等于释放 ASR 权重，使用文档必须如实说明两类模型可能同时占用内存。是否对某款手机开放边录边总结，依赖该设备上的实际测试；不在 SDK 中加入自动暂停录音或基于猜测的内存判断。

### 3.4 验收

- 只有 interim 时调用两种会话总结：不加载 Gemma，不修改 interim，不调用 `asr_reset`。
- interim 后调用总结，再收到更完整的 final：底稿只出现完整句一次。
- 用可控的未完成 Promise/线程屏障暂停总结；同时推 PCM，必须成功进入 ASR。
- 总结时新到达的 final 不进入本次 prompt，但下一次增量调用必须包含它。
- 生成失败、模型缺失、输出缓冲不足：旧摘要与增量游标保持不变。
- 总结过程中 reset：旧任务不会污染新会话，也不会重复卸载引擎。

<a id="d02"></a>

## 4. D02：支持直接总结一段文字

### 4.1 修改位置

- [native/include/apexo_session.h](../native/include/apexo_session.h)：增加 `apexo_session_summarize_text` 声明和副作用说明。
- [native/src/session.cpp](../native/src/session.cpp)：提取共用总结执行函数，接收文字副本及“是否写入会话摘要”的内部参数。
- [native/src/jni_session.cpp](../native/src/jni_session.cpp)：增加 JNI 入口，复用当前输出缓冲和失败处理。
- [ApexoSession.java](../native/java/com/apexo/sdk/ApexoSession.java)：增加 `summarize(String text)` 重载，沿用 `enter()/exit()` 生命周期保护。
- [web/src/session.js](../web/src/session.js)、[web/src/index.js](../web/src/index.js)：实现公开重载并将外部文字传入共用执行函数。
- [ios-demo/ApexoDemo/MicCapture.swift](../ios-demo/ApexoDemo/MicCapture.swift)：在薄包装中提供已有文字总结的示例调用。

### 4.2 实现要求

ASR 路径为空时允许创建会话，创建和纯文字总结都不加载 ASR。不能通过调用 `pushAsrEvent`、拼造 final 或创建假音频来实现文字总结。

复用既有模型路径、下载权重和 runtime 配置。外部文字在调用开始时复制，后台任务使用这份副本；不要保存调用方临时 C 字符串的裸指针。

原生 `apexo_engine_state_create(NULL, model_path)` 可以用于纯文字场景；但仍需绑定总结引擎。Java 使用 `new ApexoSession(null, modelPath)`，Web 省略 `asrModelPath`。这里只表示 ASR 可选，不改变“真正调用 ASR 时必须有模型”的规则。

目标 Web 用法：

```js
const session = createSession({
  summaryModelPath: "/models/gemma-4-E2B-it-gpu.litertlm",
  litertWasmUrl: "/assets/apexo/litert/",
});

const result = await session.summarize("这里放已有文字或用户编辑过的转写。");
// 此时 session.fullTranscript 和 session.summaryText 仍为空。
await session.dispose();
```

目标 Java 用法：

```java
// 这段代码由宿主放到后台线程执行。
try (ApexoSession session = new ApexoSession(null, summaryModelPath)) {
  String result = session.summarize(editedTranscript);
  // 宿主把 result 派发到 UI 线程显示。
}
```

### 4.3 验收

- 没有任何 ASR 文件，仍可用真实总结模型处理外部文字。
- 空字符串返回空串；不加载模型、不请求麦克风、不探测 ASR 文件。
- 会话已转写 A，外部总结 B，随后增量总结：仍处理 A，底稿中没有 B。
- 三个总结入口并发调用，后到者得到 busy；不会各自创建第二个 Gemma 引擎。
- 外部文字的归纳逻辑与全量会话总结相同，包括长文处理。

<a id="d03"></a>

## 5. D03：结束录音必须真正收尾

### 5.1 当前问题

当前原生 `end_segment_locked()` 先把已有 interim 写为 final，再调用 `asr_reset`；Web Worker 收到 `end` 也只执行 `pipeline.reset()`。这只能保存已经解码出来的文字，没有显式要求识别器处理剩余输入。

这是已确认的代码路径缺口。具体丢失多少尾音尚未通过真实模型复现，不能把风险描述成已有准确率测量。现有真实音频测试会在音频之后补 4.8 秒静音，可能掩盖“用户说完立即停止”的问题。

### 5.2 目标契约

- `endSegment()` 处理调用前已送入的所有音频，并输出当前段的最终识别结果。
- 操作完成后才允许 UI 显示“这一段已结束”；这时立即读底稿即可得到收尾结果。
- 它不负责关闭宿主麦克风；宿主先停止送新音频。
- 收尾只完成当前段，识别模型仍可以用于下一段；不清除已有底稿。
- `reset()` 表示主动丢弃当前识别状态并清空会话，不能用收尾操作代替。

### 5.3 原生实现

在 `ApexoEngines` 尾部增加可选回调：

```c
int (*asr_finish)(void* user, ApexoAsrEvent* out);
```

回调返回 `0` 表示收尾成功，`out->is_final` 为 `1` 或 `-1`；`-1` 表示当前段没有文字。失败返回负状态码。内置适配器必须实现；没有该回调的自定义引擎保留“提交已有 interim 后 reset”的兼容行为，并明确它不提供引擎尾帧冲刷保证。

修改 [native/src/engine_adapter.cpp](../native/src/engine_adapter.cpp)：

1. 动态绑定和 `APEXO_STATIC_ENGINES` 两条路径都接入 sherpa 的 `SherpaOnnxOnlineStreamInputFinished`。
2. 内置 `asr_finish` 对当前 stream 标记输入结束，执行剩余可解码步骤，取得最终文字。
3. 如所选流式 transducer 需要尾部 padding，在适配器内按该模型官方示例固定处理；所选模型和 padding 参数记录在发布说明中，并用立即停止的音频用例验证。不要把测试里的 4.8 秒静音当成 SDK 默认参数。
4. padding 只供识别器内部使用，不添加到用户录音、不延长真实音频时间轴。返回事件的 `end_ms` 以真实输入音频结尾为上限。
5. InputFinished 后，销毁当前 stream 并创建新 stream，保留 recognizer。不要假设一次普通 reset 就能恢复已结束的输入对象。
6. 若没有有效 ASR 输入，收尾直接成功且不创建识别模型；重复收尾不产生重复 final。

修改 `end_segment_locked()`：先调用 `asr_finish`，成功后只接受这一次最终结果，再关闭显示段。不能先把旧 interim 入稿、再追加引擎的新 final。成功返回无文字事件时清理 interim，不额外制造 final。真实收尾失败时保留已有文字并返回错误，不把失败包装成成功。`apexo_engine_state_bind` 必须填入新回调；静态声明、动态符号表及两套假引擎也一起更新。

**ABI 说明：** 当前 `ApexoEngines` 没有长度字段，直接追加成员会改变二进制布局。本轮统一按 `0.2.0` 重建头文件、四个原生包、JNI 和自定义引擎调用方；旧二进制不能与新库混用。所有初始化处必须零初始化该结构。不为这次升级新增多代 ABI 自动探测框架。

### 5.4 Web 实现

| 文件 | 修改 |
| --- | --- |
| [web/src/asr-pipeline.js](../web/src/asr-pipeline.js) | 增加 `finish()`：结束输入、解码尾帧、返回最终事件并创建下一条 stream；stream 改为可替换引用 |
| [web/src/asr-worker.js](../web/src/asr-worker.js) | `end` 消息调用 `finish()`，先发最终事件，再发完成通知 |
| [web/src/index.js](../web/src/index.js) | `backend.end()` 返回可等待的 Promise；`endSegment()` 等待该通知后再 resolve |
| [web/src/session.js](../web/src/session.js) | 区分“接收收尾结果并关闭段”与“重置引擎”，防止 onWorkerEnd 再触发一次 finish/reset |

结束操作先 flush 重采样器，将它的尾部音频送入 Worker，再发送 `end`。用现有 generation 标记过滤旧结果，并给结束请求一个 id，以便对应 Promise。Worker 出错、被关闭或会话 reset 时，未完成的结束请求必须明确结束/报错，不能永久 pending。

`drain()` 继续只表示之前消息处理完毕；`finish()` 才表示结束 ASR 输入。不能把两者当成同一个操作。Node 的同步识别后端和自定义引擎也从公开接口返回 `Promise<void>`。

Web SDK 的正常结束顺序：

```js
// stopCaptureAndDeliverTail 是宿主负责的采集逻辑：
// 停止麦克风，并把已采集但尚未转交 SDK 的最后一个音频块送完。
await stopCaptureAndDeliverTail();
await session.endSegment();
const transcript = session.fullTranscript;
const summary = await session.summarize();
```

Demo 的采集关闭也要检查：`web/demo/index.html` 当前在关闭时先设置 `recording=false`，迟到的 Worklet 消息会被忽略。停止时应先确定采集边界、交付已采集尾块，再关闭识别。无需长时间等待或额外补几秒用户静音。

### 5.5 验收

- 音频最后一个词后立即结束，不人为追加 4.8 秒静音；最后一个词进入底稿。
- 比较同一音频“正常静音结束”和“显式结束”的末句，收尾路径不能额外漏字或重复。
- 48 kHz 输入验证重采样尾块和识别器尾帧都得到处理。
- `await endSegment()` 后立即读底稿，结果完整；再次结束不增加文字。
- 结束后继续送下一段音频，模型可复用，上一段结果不重复。
- 用需要 `inputFinished` 才产生最终文字的假识别器测试调用链；真实音频用例验证实际模型，不能只测试 reset 计数。

<a id="d04"></a>

## 6. D04：区分阶段摘要和最终总结

### 6.1 当前行为与修改位置

当前 `generate_chunks()` / `runChunks()` 将每块最多 1500 个 Unicode 字符的摘要直接以换行拼接；每块使用新对话。这可以提供分段概览，但无法稳定归纳跨段的决策变化。

修改 [native/src/session.cpp](../native/src/session.cpp) 的 `generate_chunks` 和 [web/src/session.js](../web/src/session.js) 的 `runChunks`。保留 [native/src/engine_adapter.cpp](../native/src/engine_adapter.cpp) 和 Web 默认引擎“每次生成使用新对话”的原则，显式把需要的上下文放进 prompt。

### 6.2 实现策略

本轮沿用现有 `summary_chunk_chars`（默认 1500）作为保守的文字切块上限，不直接根据 Gemma 宣传的上下文长度扩大输入。字符数不等于 token 数；提高默认值前，必须用本轮实际 runtime 和权重验证完整 prompt 的可用输入预算。

对于全量会话总结和外部文字总结：

1. 输入不超过切块上限时，一次生成整体摘要。
2. 超过上限时，沿用句末/换行优先的分块方式，按顺序生成各块的简洁事实摘要。
3. 把中间摘要按原始时间顺序组合，再调用一次整体归纳，返回归纳结果，不直接返回中间摘要拼接。
4. 若中间摘要总量仍超过同一输入上限，复用现有切块函数分组归纳，直到可做最后一次整体归纳。普通会议只需要“分块 + 汇总”；长输入只增加这个顺序循环，不引入任务图或并行调度器。
5. 分组归纳要缩短输入；若一轮不能缩短到继续收敛，返回明确的生成错误并保留旧摘要。不能无限重试，也不能静默截断后半场会议。
6. 一次总结请求只加载/卸载 Gemma 一次；块与块、归纳步骤之间重建 conversation，不能反复重载权重。

`summarizeNew()` 仍是阶段摘要：只处理新增 final，把这次新增范围的简洁摘要追加到旧结果，不重写前面的历史摘要。UI 标记“阶段摘要”。会议结束时调用 `summarize()`，替换为完整总结。

### 6.3 提示词要求

保留“仅依据提供内容”的约束。原生和 Web 使用相同文本，不各自发明不同默认模板。

分块阶段建议文本：

```text
仅依据以下转写提取简洁事实摘要。
保留决定、待办、明确的负责人和时间，以及对先前决定的修改或撤销。
没有明确给出的信息不要补充；尚未确定的事项保持未确定。
按原文发生顺序概括，保留“取消”“改为”“最终决定”等关系。
转写：
{chunk}
```

整体归纳阶段建议文本：

```text
以下内容按会议发生顺序排列。请归纳成一份简洁的会议总结。
合并重复事项；对于明确被后续内容修改或撤销的决定，以最后明确决定为准。
保留仍未确定或互相矛盾且没有结论的事项，不自行猜测。
只写材料中有依据的要点、最终决定和待办，不补负责人或日期。
材料：
{ordered_notes}
```

短文直接总结时采用同样的事实和最终决定规则，把原文作为材料。暂不增加格式枚举、可编程模板系统或 JSON schema 输出要求。

### 6.4 验收

- 构造跨块材料：“原定周五发布” → “取消周五，最终改到下周三”；最终结果应表达下周三，并明确周五已被替代。
- 同一事项跨块重复讨论，最终总结合并它；阶段摘要允许按时间保留讨论过程。
- 原文没有负责人或截止时间时，不要求模型填充这些字段。
- 空输入、单块、多块及中间结果仍很长的输入都覆盖；不能丢弃末尾材料。
- 假引擎测试验证归纳步骤确实拿到了各块输出、顺序正确、一个请求只加载一次模型。
- 真实模型使用少量固定会议材料人工检查事实和决策变化；不以逐字字符串相等判断生成质量。

<a id="d05"></a>

## 7. D05：Web SDK 脱离 Demo 目录约定

### 7.1 路径配置

保留现有 `sherpaUrl`，增加一个 `litertWasmUrl`，不另建资源管理类：

```js
const session = createSession({
  asrModelPath: "/meeting/models/asr/",
  summaryModelPath: "/meeting/models/gemma-4-E2B-it-gpu.litertlm",
  sherpaUrl: "/meeting/assets/apexo/sherpa/",
  litertWasmUrl: "/meeting/assets/apexo/litert/",
  onChange: () => render(session.displayText),
});
await session.loadRecognizer();
```

修改 `prepareWebGemmaRuntime(litert)`，显式接收该会话的 `litertWasmUrl` 并传给 `getOrLoadGlobalLiteRtLm`。相对路径统一相对当前页面 URL 解析；调用方也可传绝对 URL。

为迁移保留当前两个 `/vendor/.../` 默认值，但 Demo 和新示例都显式配置路径。配置了自定义路径时，运行期间不能再请求默认 `/vendor` 路径。LiteRT 的底层加载器具有全局初始化入口，同一页面多会话共用同一个 runtime 路径；本轮不支持同页混装多个 runtime 版本。

### 7.2 资源交付和普通服务器

新增一个小的资源复制脚本，建议路径 `web/scripts/copy-runtime.mjs`：

- 从当前安装的 `sherpa-onnx` 复制 Worker 实际使用的两个 JS 文件和对应 WASM。
- 从当前安装的 `@litert-lm/core` 复制完整 WASM 资源目录，保留其相对目录结构。
- 接收目标目录参数，便于复制到 Demo 或应用的 `public/assets/apexo/`。
- 在 `package.json` 中交付该脚本和使用说明；来源版本仍以锁文件为准。
- 不从网络另下载一个未锁定版本，不把模型权重打进 npm 包。

补充一个最小 Vite 接入示例，建议放在 `web/examples/vite/`，直接使用打出的 npm 包和复制后的资源。现有 Demo 继续承担普通页面示例，无需重新做一套 UI。

模型文件探测不能强制服务器识别 `X-Apexo-Probe` 自定义头。`probeAsrFile` 改用普通 HEAD；服务器不支持 HEAD 时交给实际 GET 加载判断。404 归类为模型缺失；网络或运行时加载失败保留具体错误。测试不能仅因为 Demo 能返回 204 就认为通用静态托管已成立。

文档列出实际需要的 COOP/COEP、WASM MIME 和同源资源要求；不要在本轮实现通用代理服务器或 CORS 自动修复。当前 Node shim 保留，后续是否替换为上游浏览器专用构建另行决定。

### 7.3 Demo 的本地/浏览器识别选择

`web/demo/index.html` 当前在识别器不是 `live` 时会尝试 `SpeechRecognition`，包含“模型存在但加载失败”的情况；现有文字只强调了模型缺失。

默认演示本地模式。缺模型时显示如何准备模型，加载失败时显示具体原因；只有用户主动选择“浏览器识别”后才调用 `SpeechRecognition`，并在选择处说明浏览器可能把音频交给在线服务。这个选择只放在 Demo，SDK 核心不增加云端 fallback 管理。

下载权重仍可由宿主交给 `setSummaryModel`。本轮无需做下载中心、断点续传或持久化缓存框架；文档须说明当前内存权重会随页面关闭失效。

### 7.4 验收

- 从打出的 `.tgz` 安装到最小示例，使用自定义 `/meeting/assets/` 路径完成加载。
- 默认 `/vendor/...` 故意返回 404，实际 ASR/总结仍从配置位置加载。
- 不支持自定义探测头的普通静态服务器能工作。
- 只有总结模型的页面不请求 ASR 脚本或四个 ASR 文件。
- 本地模型缺失/加载失败不会自动启动 `SpeechRecognition`；显式选择后才启动。
- 使用缺失权重与注入错误验证提示；用真实模型另验至少一次从 npm 包到生成结果的完整流程。

<a id="d06"></a>

## 8. D06：把原生包交付做到可直接接入

### 8.1 本轮选择一种主交付方式

继续交付 Apexo 适配层，并随发布提供版本匹配的 runtime 文件及清单。模型权重由宿主单独准备。调用方应能按说明复制、链接并运行，不必先研究 sherpa/LiteRT-LM 的构建系统。

当前代码里手写的 sherpa 配置布局注明匹配 `2.0.1`；LiteRT-LM 文档只写 C API `1.0.0 或更新`，尚不足以指定可用二进制。发布时必须记录实际验证的库 tag/commit、架构、产物名称和模型来源；不能把 Web npm 的 `0.19.0` 直接当成所有原生 C 库版本。

在 `docs/平台包.md` 中增加这份已验证组合表，版本值由真实构建和设备运行结果填写。若某平台拿不到可用引擎产物，应标记该平台未完成；仅打出 Apexo 空壳不能计为真实推理验收通过。

使用锁定版本的官方 C 头文件核对符号和结构布局。优先让适配器引用对应声明，避免长期维护一套手抄 ABI。保持单一已验证版本，暂不实现跨版本运行时适配。

### 8.2 平台具体修改

| 平台 | 修改位置 | 明确要交付的内容 |
| --- | --- | --- |
| Android | `pack.sh`、JNI/Java、一个最小应用示例 | AAR 中放入各 ABI 的匹配引擎 `.so` 及其实际依赖；示例演示模型文件复制到可访问目录、后台推理和关闭会话 |
| iOS | `pack.sh`、`ios-demo` 工程、`DemoSupport.c` | 主流程使用已嵌入、签名的引擎 framework；从应用 bundle 取得其实际二进制路径并调用现有 `apexo_engine_state_set_libraries`；示例工程包含相应链接/嵌入配置 |
| macOS | `pack.sh`、最小 C 示例 | Apexo dylib、匹配引擎 dylib、头文件、必要依赖及可用的 rpath/显式加载路径 |
| Windows x64 | `pack.sh`、最小 C 示例 | Apexo DLL、可用于示例工具链的 import library、匹配引擎 DLL 和实际依赖；说明 DLL 搜索路径与模型路径 |

Android 首轮只支持当前已打包的 `arm64-v8a`、`x86_64`；macOS 当前构建是 arm64；iOS 当前是 arm64 真机和模拟器；Windows 当前是 x64。不要扩展未经验证的架构矩阵。

iOS 的静态链接能力可以继续存在，但本轮接入文档只把一种经过真实验证的方式作为主流程。当前 iOS 示例未把两套引擎集成进工程，不能仅修改文档就宣称完成。

若引擎的默认加载名称与随包产物不一致，优先在打包时统一名称，或由平台示例传入明确路径；避免让 Android 用户通过环境变量配置应用。对于原生第三方产物，附上其已有的许可证/NOTICE 文件。

### 8.3 录音线程示例

[MicCapture.swift](../ios-demo/ApexoDemo/MicCapture.swift) 当前在音频 tap 中直接调用 `apexo_session_push_pcm`；而原生适配器可能在第一次 push 中完成模型加载和解码。

修改示例：tap 只复制音频并交给一个串行处理队列；队列执行重采样、`push_pcm` 和收尾，结果再派发到主线程。停止时确保队列中已采集的音频先处理完，再执行 `end_segment`。Java 示例采用相同原则，总结在另一个后台任务运行。

不在 C 核心内新增录音线程池或任务系统。先用已有的宿主线程能力解决示例中的实际阻塞；若目标设备处理速度跟不上采集，应在验收中记录，不能靠无限缓存声称实时。

### 8.4 最小接入文档必须包含

每个平台各给一段从创建到销毁的完整例子，说明：运行库放哪里、模型放哪里、PCM 格式、在哪个线程调用、如何等待结束、如何总结已有文字、如何读取错误、如何释放 session 和 engine state。

示例覆盖“ASR only”“summary only”“结束后总结”三条路径。边录边总结在真实设备通过后列为该设备已验证能力，不要求首轮为所有设备设定统一的速度或内存承诺。

验收环境中移除开发者临时设置的引擎路径，仅使用发布包、模型与文档即可运行。四个原生平台分别在实际系统执行，不用 macOS 上的假引擎测试代替 Android/iOS/Windows 验收。

<a id="d07"></a>

## 9. D07：收敛事件、显示与生命周期接口

### 9.1 对外提供现有转写事件

新增一个可选的事件回调即可，不做事件总线或订阅框架：

```c
typedef void (*ApexoTranscriptCallback)(void* user, const ApexoAsrEvent* event);
APEXO_API void apexo_session_set_transcript_callback(
    ApexoSession* session, ApexoTranscriptCallback callback, void* user);
```

Web 增加 `onTranscript(event)`，事件字段使用现有的 `text/isFinal/startMs/endMs`；Java 增加一个 listener 接口传递相同内容。C 回调中的指针仅在回调期间有效，需异步处理时由调用方复制；Java/JS 得到独立的事件值。

回调在会话确认接受 interim/final 和显式收尾结果后触发；现有去重规则丢弃的重叠重复 final、`isFinal=-1` 不通知。显示段合并不改变这些事件，不把多个 final 合成一次通知。它在会话锁外执行，避免回调读会话时死锁。单一音频生产者按音频顺序调用 push/end；回调不能在同一调用栈递归执行 push/end/reset/destroy。注册/撤销 listener 在开始/停止采集的边界执行。

原生回调位于推理调用线程，宿主负责派发到 UI；JNI 不共享跨线程 `JNIEnv*`，listener 引用由现有 Boxed 生命周期管理。Web Worker 消息回到主线程后再回调；直接 `pushAsrEvent` 的宿主注入路径也遵守相同契约。保留 `onChange` 供现有 Demo 迁移。

时间戳仍是当前适配器估算的分段时间，不新增逐词精确对齐承诺，也不增加说话人字段。

### 9.2 显示规则

保留 `displayText` 和默认 4 行/200 词作为便利输出，避免为本次修改大规模搬迁代码。新调用示例优先展示如何用 `onTranscript` 渲染 interim 和保存 final。

完整底稿、总结和原始事件不受显示行数影响。将现有文档中的“SDK 必须显示 4 行”改成“便利显示默认值”；本轮不扩展一组 UI 布局配置。

### 9.3 重置和释放

新增明确名称，同时保留旧别名：

| 操作 | 目标名称 | 行为 |
| --- | --- | --- |
| 清空当前会话 | C `apexo_session_reset`；Java/Web `reset()` | 清底稿、摘要、游标和 interim，重置 ASR；保留已加载 recognizer |
| 原有清空调用 | `release()` / `apexo_session_release` | 调用同一 reset 实现，不复制逻辑 |
| 不再使用会话 | C `destroy`；Java `close()`；Web `dispose()` | 完成已有任务的安全收尾后释放引擎，禁止新调用 |

`reset()` 同时把本次会话的音频时间轴归零；普通 endpoint 和 `endSegment()` 只结束当前段，时间轴继续累计。原生的 `samples`、Web 流水线的 `fed` 分别按这两种操作处理，不能每个句子都归零，也不能新录音继续沿用上次的时间。

Web `dispose()` 返回 `Promise<void>`：先标记关闭，等待当前总结自行完成卸载，再释放 recognizer。不能像当前实现一样在生成期间额外执行 `sumUnload()`。它不要求底层提供即时取消生成能力。

原生 `destroy()` 不代替调用方持有的 engine state 销毁。宿主必须在后台总结返回后再销毁对应 engine state；Java 继续用现有 active-call 计数，Swift 示例继续用串行 teardown，避免增加另一套所有权框架。

### 9.4 验收

- 事件包含 interim 的修正及唯一 final；显示合并不吞掉原始 final 通知。
- listener 能把内容复制后送 UI，不在 SDK 锁内执行应用代码。
- `reset()` 与 `release()` 行为相同，下一段不混入上一段结果。
- 生成期间 `dispose/close` 不提前销毁模型，最终只释放一次。
- 对外示例不依赖 `gemmaResident` 判断是否可以送音频。

<a id="d08"></a>

## 10. D08：测试、兼容和交付清单

### 10.1 必须修改的旧断言

以下测试目前把旧行为当成正确行为，不能原样保留：

| 文件 | 旧断言/测试描述 | 新断言 |
| --- | --- | --- |
| `native/tests/session_test.cpp`、`web/test/session.test.js` | `summarize promotes an unfinished interim line` | 总结不修改 interim/final；之后完整 final 只入稿一次 |
| 同上 | 增量总结包含未完成行并 reset | 增量只处理新 final，reset 计数不变 |
| `web/test/session.test.js` | 总结加载/卸载期间 PCM 被拒绝 | 加载、生成、卸载三个阶段 PCM 均可进入 ASR |
| 同上及原生会话测试 | 第二个总结返回 `-5` | busy 使用 `-6`；Web 公开接口 Promise rejection |
| `web/test/adapter.test.js` | 总结失败通过 `result.summary === -3` 判断 | 使用异常 `code === -3`；不返回负数充当摘要 |
| `web/test/demo-flow.test.js` | 点击总结隐式结束录音 | 保留“结束并总结”的产品流程，等待真正收尾；阶段摘要另测持续录音 |
| `native/tests/real_engine_test.cpp`、`web/test/browser-asr.test.js` | 喂入长静音后才检查结果 | 增加无长静音的显式结束用例，验证最后一个词 |
| ASR fake、静态引擎测试 | 只有 reset，没有 finish | 增加 InputFinished 及最终尾帧结果，覆盖静态/动态绑定 |

去重、中文/英文拼接、重采样、显示裁剪、完整底稿保留和模型缺失等现有有效测试继续保留。测试重点是调用者能观察到的行为，不增加只检查私有函数名称或函数调用次数的测试。

### 10.2 测试矩阵

| 编号 | 场景 | 关键通过条件 |
| --- | --- | --- |
| T01 | interim → 总结 → 更完整 final | 不提前定稿、不重复、不重置识别 |
| T02 | 总结期间连续推 PCM | 仍进入 ASR；新 final 由下次摘要处理 |
| T03 | 全量与增量连续交替 | 游标准确；不漏、不重复总结已覆盖范围 |
| T04 | 已有摘要后生成失败 | 旧摘要和游标保留 |
| T05 | 外部文字总结 | ASR 不加载，不污染会话状态 |
| T06 | 空文本、无新 final、重复提交总结 | 空任务不加载；忙任务明确失败 |
| T07 | 16 kHz/48 kHz 最后一个词后立即停止 | ASR 收尾完整；等待后可读最终底稿 |
| T08 | reset/close 与总结交叠 | 不回写旧状态、不提前释放模型 |
| T09 | 跨段决策被修改或撤销 | 最终总结表达最后明确结论，阶段摘要保留历史 |
| T10 | 自定义 Web 资源路径和子目录部署 | 不依赖 Demo 特殊路由 |
| T11 | 原生四平台新工程接入 | 仅凭交付物和说明完成真实转写、纯文本总结 |
| T12 | 本地识别加载失败 | 默认不转用浏览器识别；错误原因可见 |

T01～T06、T08 的控制流用假引擎稳定验证；T07、T09、T11 必须包含真实模型结果。T10 同时覆盖假引擎路径检查和至少一次实际模型加载。没有权重时跳过真实测试要明确记录为未验证，不能算作通过。

### 10.3 运行入口与测试脚本

当前常用命令如下，修改后沿用入口：

```sh
# 仓库根目录
./run-tests.sh

# Web 单独验证
cd web
node --import ./test/register-fakes.js ./test/adapter.test.js
node ./test/session.test.js
node ./test/capture.test.js
node ./test/asr-presence.test.js
node ./test/asr-pipeline.test.js
node ./test/demo-flow.test.js
APEXO_TEST_ASR_DIR=/absolute/path/to/asr-model node ./test/browser-asr.test.js
```

调整 `run-tests.sh`：当前只有 dylib 不存在时才重编核心，会导致修改了源码却测试旧库。本轮直接在测试开始前重新编译本机测试库，或使用独立的测试构建目录；选择简单的前者即可。不必为这个小仓库引入新构建系统。

测试前按 README 安装 npm 依赖和 Playwright Chromium。真实原生 ASR 测试继续使用 `APEXO_TEST_ASR_DIR` 与 `APEXO_SHERPA_LIBRARY`。补充真实总结验证入口，记录实际 LiteRT 库、模型路径和固定输入材料；当前仓库没有可代替它的真实总结质量测试。

### 10.4 迁移与文档同步

本轮按 `0.2.0` 统一发布，Web 包和锁文件同步更新。原生新头文件、新库以及使用 `ApexoEngines` 的调用方一起重编。

迁移说明至少列出：

1. `summarize()` 默认只读 final，调用者需要最后一句时先完成 `endSegment()`。
2. Web 所有总结调用改为 `await`，错误改用 `try/catch`；`endSegment()` 也必须等待。
3. 总结不再以 Gemma 驻留为由拒绝 PCM。是否停止录音由应用决定。
4. `summarize(text)` 是外部文字总结，不会把文字存入会话。
5. `summarizeNew()` 是追加阶段摘要，最终会议总结调用 `summarize()`。
6. `release()` 作为 reset 的迁移别名；新代码使用 `reset()`。
7. `summarizeWithGemma()` 不再作为推荐入口，Demo 改用 `summarize()`；为已有调用保留成功时 `{ litert, summary }` 的包装形式，但内部复用同一路径，失败同样抛出错误。
8. 自定义引擎重新编译并零初始化扩展后的 `ApexoEngines`，实现 finish 后才能承诺尾帧收尾。

同步修改 `README.md`、`docs/会话.md`、`docs/平台包.md`、`docs/网页.md`、C 头文件注释、Java Javadoc 和示例提示语。删除“所有总结期间拒绝 PCM”“增量总结把 interim 强制定稿”等过时描述。

### 10.5 建议提交拆分

| 提交 | 内容 | 提交时应完成的验证 |
| --- | --- | --- |
| 1 | D01 快照与独立录音；D02 纯文字总结；核心错误约定 | T01～T06、T08，原生/Web 对齐 |
| 2 | D03 ASR finish 与宿主结束流程 | T07，动态/静态引擎绑定，Web/Node 后端 |
| 3 | D04 分段归纳与最终总结 | T09，真实材料检查及失败保留旧结果 |
| 4 | D05 Web 资源与 Demo 接入 | T10、T12，npm 打包后的最小应用 |
| 5 | D06 原生运行时和平台例子 | T11，记录各平台实际结果 |
| 6 | D07 接口便利性与 D08 发布说明 | listener/reset/dispose 回归、文档与版本一致 |

每个提交同时更新它影响的使用说明和测试，不把全部文档修改拖到最后。最终交付物包含 SDK 包、匹配运行库、模型获取说明、完整接入示例、迁移说明，以及注明真实验证环境的验收结果。

## 11. 0.2.0 实施与真模型验收记录（2026-11-10）

| 平台 / 环境 | 运行时与模型资产 | 验证范围（T01～T12） | 结果 |
| --- | --- | --- | --- |
| **macOS (`arm64`)** | `dist/macos/libapexo_session.dylib` + `libsherpa-onnx-c-api.dylib` (`v2.0.1`) + `liblitert-lm.dylib` (`0.10.1`) + `sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23` + `gemma-4-E2B-it.litertlm` | `./run-tests.sh`（含 `session_test`、`adapter_test`、`static_engines_test`、`real_engine_test`）、`examples/macos/minimal_example`、`native_full`（18 项真模型断言，含 T01/T02/T05/T06/T07/T09/D07） | 全部通过（`failures = 0`） |
| **iOS Simulator (`arm64`)** | `iPhone 17 Pro`（iOS 26.3 Simulator）+ `dist/ios/ApexoSession.xcframework` + `SherpaOnnxC.xcframework` + `CLiteRTLM.xcframework` + 真模型权重 | `ios-demo/ApexoDemo.xcodeproj` 编译签名校验、`ApexoRealTest.app` 18 项核心真模型断言 + `MicCapture` 串行 `audioQueue` 48 kHz → 16 kHz 转换、无尾静音 `endSegment`、阶段摘要、完整总结与外部文本总结 | 全部通过（`failures = 0`） |
| **Android Emulator (`arm64-v8a`)** | `ApexoReal35`（Android 15 / API 35 `arm64-v8a`）+ `dist/android/apexo-session.aar`（含 `arm64-v8a` 与 `x86_64` 双架构 `.so`）+ 真模型权重 | `examples/android/MinimalExample.java` 编译验证、`ApexoRealTest.apk` 16 项真模型断言（含 `summarize(text)`、T09 跨块归纳、T07 无静音尾帧收尾、`setTranscriptListener`、T02 总结期 `pushPcm == 0`、T06 `ERR_BUSY (-6)`、`summarizeNew()`、`reset()`） | 全部通过（`failures = 0`） |
| **Web (Chrome WebGPU + Node)** | Chromium WebGPU + `@apexo/sdk@0.2.0` + `sherpa-onnx@2.0.1` Wasm Worker + `@litert-lm/core@0.1.0` + `gemma-4-E2B-it-gpu.litertlm` / `gemma-4-E2B-it-web.litertlm` | `web/test/*.test.js`（7 个测试套件，含 `browser-asr.test.js` 与 `demo-flow.test.js`）、`copy-runtime.mjs` 自定义路径校验、`web_full.mjs`（17 项真模型 WebGPU 断言，含 Demo 阶段摘要/总结、零 `/vendor/` 请求、T01/T02/T05/T06/T07/T09/D07） | 全部通过（`failures = 0`） |
| **Windows (`x86_64`)** | `dist/windows/apexo_session.dll` + `apexo_session.lib` + `sherpa-onnx-c-api.dll` (`v2.0.1`) + `litert-lm.dll` (`0.10.1`) | `examples/windows/build.sh`：交叉编译 `minimal_example.exe` 与 `windows_test.exe`、19 个 C API 符号 PE/COFF 导出与导入表校验、`SherpaOnnxOnlineStreamInputFinished` 与 `litert_lm_engine_create` 导出校验、`windows_host_verify`（ASCII/UTF-8/8.3 短路径与 CP1252 中文路径防有损替换验证）及完整会话并发契约测试 | 全部通过（`failures = 0`） |

