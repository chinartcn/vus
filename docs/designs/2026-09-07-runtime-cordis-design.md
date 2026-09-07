# Cordis 式上下文元框架（运行时服务层）设计

> 日期：2026-09-07 · 状态：设计定稿 · 配套：rt/vus_cordis.c + vus_cordis.h（路线 A）、src/generator.c 内建分发（路线 B）、tests/test_cordis.vus

## 一、目标

仿 DSH 底层 Cordis 的"一切皆插件"思想，为 VUS 增加一层**运行期上下文元框架**：

- **路线 A（C 元框架）**：`rt/vus_cordis.c` 提供上下文、服务容器、依赖注入、effect 栈、事件四模式，统一收编现有 FFI 桥域、VUA 事件、vusx 对象。
- **路线 B（VUS 语言）**：在 .vus 语法层暴露同一批能力（`注册服务`/`服务获取`/`监听事件`/`挂载效果`），让用户用 VUS 本身写插件，不碰 C。

两条路线共用同一运行时（路线 B 是 A 的语法糖）。

## 二、五个核心概念 → 实现映射

| Cordis 概念 | 本设计对应 | 落点 |
|---|---|---|
| 插件 = Service | 服务 = 命名记录 + 方法回调表 | VusService（C）/ `注册服务`（VUS） |
| Context = 服务容器 | 上下文 = 按 命名空间+键 查找服务的容器 | VusCtx |
| inject = 依赖声明 | 依赖表 → 加载器拓扑排序，逆序卸载 | VusService.deps / `依赖` |
| 类型化事件 | emit / waterfall / parallel / serial 四模式分发 | VusEmitter |
| 可逆副作用 | 所有注册返回 disposer，fiber 卸载逆序回退 | VusCtx.effect 栈 |

**路径无关性**：最终系统状态只取决于注册了哪些服务；加载按依赖 DAG 拓扑序，卸载按逆拓扑序，与书写顺序无关。

## 三、运行时协议（路线 A，rt/vus_cordis.h 已实现）

```c
/* 事件分发模式（每个事件一次声明一个模式，公开契约） */
enum { VUS_EV_EMIT = 0, VUS_EV_PARALLEL, VUS_EV_SERIAL, VUS_EV_WATERFALL };

typedef struct VusCtx VusCtx;              /* 上下文（不透明） */

/* 服务描述符 */
typedef struct VusCtxService {
    const char *key;                  /* 服务键（ctx 内唯一） */
    const char **deps;                /* 依赖的服务键列表，NULL 结尾；驱动 DI 拓扑激活 */
    void (*init)(void *impl);         /* 激活（依赖就绪后调用，可为 NULL） */
    void (*dispose)(void *impl);      /* 逆序卸载（可为 NULL） */
    void *impl;                       /* 实现数据 */
} VusCtxService;

/* 上下文生命周期 */
VusCtx *vus_ctx_create(void);
void    vus_ctx_dispose(VusCtx *ctx);       /* 逆序跑服务 dispose + effect 栈，再释放 */

/* 服务容器 + DI */
void *vus_ctx_register(VusCtx *ctx, VusCtxService *svc); /* 返回 disposer；环/缺依赖拒绝 */
void *vus_ctx_get(VusCtx *ctx, const char *key);          /* 取已激活服务 impl（borrow） */
void  vus_ctx_unregister(VusCtx *ctx, void *disposer);    /* 单服务提前撤销 */

/* effect 栈（可逆副作用） */
void *vus_ctx_effect(VusCtx *ctx, void (*undo)(void *data), void *data); /* 登记，栈序回退 */
void  vus_ctx_cancel_effect(VusCtx *ctx, void *disposer);

/* 事件四模式 */
void *vus_ctx_on(VusCtx *ctx, const char *ev, int mode, VusClosure *cb); /* 返回 disposer */
void  vus_ctx_fire(VusCtx *ctx, const char *ev, void *args);             /* EMIT/PARALLEL 广播 */
void  vus_ctx_pipeline(VusCtx *ctx, const char *ev, void *args, int mode); /* SERIAL/WATERFALL 管线 */
void  vus_ctx_set_stop(VusCtx *ctx, int stop);   /* 管线回调内短路后续 */
int   vus_ctx_stopped(VusCtx *ctx);
int   vus_ev_mode(const char *name);             /* "观察|中间件|并行|顺序" → 枚举 */
const char *vus_ctx_last_error(VusCtx *ctx);     /* 诊断 */
```

**DI 语义**：`vus_ctx_register` 注册即推进拓扑激活——依赖全部就绪的服务立即 `init`（依赖先于依赖者）；每轮无新进展即停，若新服务本身未被激活则本轮注册被拒绝（环依赖/缺依赖）。`vus_ctx_dispose` 或显式注销时按激活序**逆序**调用 `dispose`，随后逆序跑 effect 栈。

## 四、事件四模式语义

| 模式 | 等待 | 顺序 | 返回值 | 语义 |
|---|---|---|---|---|
| EMIT | 否 | 注册顺序 | 无 | 观察者广播（日志/状态通知） |
| WATERFALL | 否 | 链式，须调 `next()` | 有 | 中间件流水线（改写/拦截/短路） |
| PARALLEL | 是 | 并行 | 无 | 多观察者并发响应 |
| SERIAL | 是 | 注册顺序 | 有 | 首个非"未命中"返回值即短路 |

事件在 `ns` 下命名（`ns/ev` 等价路径），与 FFI 桥域同一命名空间策略，避免污染全局键表。

## 五、VUS 语法（路线 B）

仿现有 `导入外部`/`导出变量` 关键字族，新增四个顶层语句，全部映射到路线 A 的 C API：

```vus
# 1. 注册服务：deps 为依赖键列表（可选，驱动 DI 加载顺序）
注册服务 (日志) {
    依赖: ["存储"]
    方法: {
        写: 函数 (消息) { 存储.追加(日志文件, 消息) }
    }
    初始化: function (ctx, svc) { ... }   # 可选，依赖就绪后调用
}

# 2. 服务获取：ctx.<别名>.<键>（等价 vus_ctx_get）
_存储 = 服务获取 (存储)

# 3. 监听事件：模式 ∈ 观察|中间件|并行|顺序
监听事件 (消息到达, 处理消息, "观察")

# 4. 挂载效果：注册可逆副作用（进程退出/服务卸载逆序回退）
挂载效果 (撤销回调, 数据)
```

为保持第一版小巧，路线 B 首批实现以下六个内建调用（不新增 AST 节点类型，复用函数调用表/字典/定义语法）：

- `注册服务(字符串键, {依赖: [...], 初始化: 函数, 拆卸: 函数, 数据: 表达式})` → `vus_ctx_register`
  - `依赖`：字符串/标识符列表（可选，驱动 DI 拓扑）；
  - `初始化`/`拆卸`：用户 .vus 函数，回调时收到 `数据` 作为第一参数；
  - `数据`：任意表达式，作为服务 impl（缺省空串）。
- `服务获取(字符串键)` → `vus_ctx_get`（返回 impl 数据，borrow 转出生份；未激活返回空串）
- `监听事件(事件名, 回调函数, "观察|中间件|并行|顺序")` → `vus_ctx_on`（回调收到事件参数）
- `发射事件(事件名, 参数)` → `vus_ctx_fire`（EMIT 广播）
- `管线事件(事件名, 参数, "中间件|顺序")` → `vus_ctx_pipeline`（WATERFALL/SERIAL）
- `挂载效果(撤销函数, 参数)` → `vus_ctx_effect`（进程退出时逆序回退）

示例见 `tests/test_cordis.vus`（DI 拓扑/服务获取/事件 EMIT/WATERFALL/effect 回退/缺依赖拒绝）。

> 版本二候选（未实现）：服务 `方法` 表（取到方法对象后 `._方法名()` 调用）、
> 事件 `stop` 短路内建、`并行` 模式的真实并发。

## 六、与既有系统接线

- **FFI 桥域**（未接线，版本二）：`导入外部` 的域注册改为同时登记进上下文（`vus_ctx_*`），别名即 ns——外部服务与 VUS 服务可互相依赖。
- **VUA 事件**（未接线，版本二）：`界面_绑定` 底层改用 EMIT 模式事件，保留 `vua_on/off` 兼容壳。
- **vusx 对象**（未接线，版本二）：加载的 vusx 对象按 key 挂入上下文，`服务获取` 可直接取。
- **生命周期**（已接线）：`vus_ctx_dispose` 挂在生成代码的 atexit 清理路径（`_vus_ctx()` 首次创建时注册），进程退出跑 effect 栈保证"拔得干净"。

## 七、实现记录

- 实现文件：`rt/vus_cordis.c`、`rt/vus_cordis.h`（并入 libvus_rt.a / .so）
- 语言层：`src/generator.c` 内建分发（`注册服务/服务获取/监听事件/发射事件/管线事件/挂载效果`），生成 `vus_ctx_*` 调用与惰性单例 infra
- 测试：`tests/test_cordis.vus`（DI 拓扑/DATA 获取/EMIT 顺序/WATERFALL 顺序/effect 逆序回退/缺依赖拒绝）
- 示例：待补 `examples/cordis/`（VUS 版"服务插件"）

## 八、实现要点（踩坑记录）

- **服务描述符存储**：`注册服务` 生成 `static VusCtxService`（.bss 零初始化）再逐字段赋值——不能把 `vus_string_new`/字面量缓存等运行期表达式放进静态初始化，也不可依赖语句表达式栈变量跨表达式存活（优化器可复用栈槽导致两个注册互相覆盖）。
- **VUS 回调包装**：调用用户函数须传 2 槽参数数组 `{NULL, 首参}`（槽 0 为返回值槽），只传 1 槽会让函数内 `_vus_params[1]` 读到越界垃圾导致 vus_ref 崩溃。
- **事件顺序**：监听器链表尾插，保证 EMIT/SERIAL/WATERFALL 均按注册顺序分发（头插会反转顺序）。