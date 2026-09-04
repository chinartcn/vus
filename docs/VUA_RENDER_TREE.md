> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# VUA 渲染树格式（native → Java）

> **当前状态：已实现（v3.0.20260904150204（正式版））**
> 本文档定义 native 侧 `.vua` 运行时（`rt/vua.c`）解析 `.vua` 后，输出给 Java 侧（APK shell 的 `VuaRenderer.java`）去重建 Android View 的那份**规范化 JSON 渲染树**。
> 对应源码契约：native `vua_screen_dump_rendertree()` 的返回值（另有 64 位指纹 `vua_screen_rendertree_hash()` 供 Java 做"是否重建"判定）。`.vua` 源语言本身见 `VUA_REFERENCE.md`。

## 一、为什么需要这份格式

- `.vua` 是**中文、需词典翻译、需严格校验**的源文件。
- Java 侧不应碰词典、不碰校验。所以 native 先做完「解析 + 校验 + 原语键归一」，再输出一份**可直接渲染**的 JSON。
- Java 的唯一职责：**按 `type` 建 View → 套属性 → 记录事件/变量 → 收触摸回传 native**。

## 二、核心原则

1. **原语键统一定为英文**：`type` / `id` / `children` / `variable` / `event`（native 归一：`变量`→`variable`、`子组件`→`children`、`事件`/`点击`/`变化`→`event`）；**控件属性键原样透传**（`.vua` 写中文键，渲染树里仍是中文键，如 `内容`/`文字`/`标签`），由该 `type` 的控件实现自行读取。
2. **`type` 保留中文**（`界面` / `按钮` / `文本` / `迷你图` …），Java 用它定位控件实现。
3. **布局节点带 `children`** 数组；无子节点的控件不带。
4. **每个节点都尽量带 `id`**（可空）；`id` 既是渲染/调试标识，也是"按 id 取事件"的键。
5. **事件统一为 `event` 对象**，支持按 `name` 或按 `id` 触发。
6. **变量统一为 `variable`**（输入控件），是 Java 写回状态、事件取值的关键。

## 三、完整示例

```json
{
  "type": "界面",
  "id": "scr_main",
  "主题": "light",
  "children": [
    {
      "type": "表单",
      "id": "form1",
      "children": [
        { "type": "输入框", "id": "in金额", "label": "金额", "variable": "金额" },
        { "type": "输入框", "id": "in备注", "label": "备注", "variable": "备注" }
      ]
    },
    {
      "type": "按钮",
      "id": "btn保存",
      "text": "保存",
      "event": { "name": "保存账单", "collect": ["金额", "备注"] }
    }
  ]
}
```

## 四、节点通用字段

渲染树的键分**两类**，语义截然不同：

| 类别 | 键 | 谁读 |
|------|----|------|
| **内部原语**（固定，Java 用固定名读） | `type` `id` `children` `variable` `event` | Java / 渲染器专用，不查词典 |
| **控件属性**（此 type 控件的键，交给该 type 实现读） | `text` `label` `value` `color` `数据` … | 按 `type` 分发给对应控件实现 |

> **Java 永不猜键归属**：它只认那 5 个内部原语键，除此之外的键一律**原样透传**给"这个 `type` 的控件实现"去自行读取。所以中英混排不影响 Java —— 中英键的差异只在"控件属性"这一类里，由各控件实现负责，不进入 Java 的取键逻辑。

节点通用字段明细：

| 键 | 类别 | 说明 | 必备 |
|----|------|------|:----:|
| `type` | 原语 | 控件类型（中文，经控件表与词典归一后的权威 type） | ✅ |
| `id` | 原语 | 控件标识，同屏内应唯一；也是"按 id 取事件"的键 | 建议 |
| `variable` | 原语 | （输入控件）此控件把当前值写入的变量名 | 输入用 |
| `event` | 原语 | （交互控件）事件描述，见第五节 | 交互用 |
| `children` | 原语 | 子节点数组 | 布局用 |
| 其余 | 属性 | 此 type 控件的属性（键名由该控件自带词典决定，中英皆可） | —— |

**控件自定义键的约定**：一个控件的属性键名，由它自己的控件表 `字段` 词典决定（可为中文，也可为英文）；Java 不关心具体键名，只按 `type` 把整块属性原样交给该控件实现。

## 五、事件（`event` 对象）

交互控件的事件统一描述为：

```json
"event": {
  "name":    "保存账单",
  "collect": ["金额", "备注"]
}
```

| 字段 | 说明 |
|------|------|
| `name` | 事件名，对应 `.vus` 里登记的处理函数（`vua_on`） |
| `collect` | 触发时要把哪些 `variable` 的当前值一并带给逻辑；为空表示不携带 |

### 按 `id` 取事件

除了按 `name` 触发，事件也可**按控件的 `id` 直接触发**：

- 每个事件节点保留其 `id`（上例 `btn保存`）。
- 需要从一个控件触发**另一个控件**（或自己）的事件时，可用 **`triggerId`** 指向目标节点 id：

```json
{
  "type": "列表项",
  "id": "list_item_3",
  "children": [],
  "onSel": { "triggerId": "btn保存" }
}
```

- **索引归属（定死）：`id → 事件` 索引由 native 侧维护，随渲染树在下发时一并放在树顶层，Java 只读。** Java 不自己从树里重建索引。
- 触发时 Java 用 `triggerId` 到树顶的索引里查，取该节点的事件（`name` + `collect`）；查不到即报错（不静默）。
- 对同一事件，`name` 与 `triggerId` 两种写法**二选一**：`name` 直接指处理函数，`triggerId` 先解析到节点再取其 `name`/`collect`。

#### 树顶层携带的事件索引

```json
{
  "type": "界面",
  "id": "scr_main",
  "主题": "light",
  "children": [ /* ... */ ],
  "eventIndex": {
    "btn保存":      { "name": "保存账单", "collect": ["金额", "备注"] },
    "list_item_3": { "name": "选择列表项", "collect": [] }
  }
}
```

`eventIndex` 是**内部原语**键，Java 在其上做 `triggerId` 查询；其余节点属性照常透传。

## 六、变量与事件的数据流

```
Java 建 View 时（只读，不自建索引）：
   输入控件  → 记住 { id, variable }（输入值变更 → 写入该 variable）
   交互控件  → 记住 event（name / triggerId / collect）

用户输入  → 写入 native 侧 VusDict 的对应 variable

用户点按钮 → Java 取 event：
     name      直接触发
     triggerId  → 到树顶层 eventIndex 查 → 取 name + collect
   → 收集 collect 里各 variable 的当前值 → JNI 调 {native} vua_触发事件(name, dict)
```

> **索引唯一所有者是 native**：`id → 事件` 索引在 native 侧（`.vus` 编译 / `vua_on` 登记）建好，随树下发，Java 只消费、绝不重建。
> Java 侧"零逻辑"边界：它不解释事件语义，只负责「读 event → 组装 (name, 变量字典) → 调 native」。事件到底调哪个 `.vus` 函数、要不要换界面，全在 native / `.vus` 侧决定。

## 七、扩展控件的属性透传

未内置的自定义控件（如 `迷你图`），native 把经控件自带词典翻译后的**原样属性**透传进节点：

```json
{
  "type": "迷你图",
  "id": "spark_本月",
  "数据": "本月",          // 控件自带词典翻译后的中文键，说明本控件认这个中文键
  "color": "#4caf50"      // 部分键仍用翻译后的内部名
}
```

Java 侧对这种节点：**以 `type` 查已注册的对应控件实现**处理；未注册 → 按 `VUA_REFERENCE` 的严格原则报错（不静默降级）。

## 八、错误

native 解析/校验失败时，不产出渲染树，而是返回错误信息；Java 直接展示或提示，不半渲染。
（详细错误清单与原则见 `VUA_REFERENCE.md` 第十节。）