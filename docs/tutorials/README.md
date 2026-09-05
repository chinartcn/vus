# VUS 教程分册

> 本目录是 VUS 官方教程的**实战分册**，与 [TUTORIAL.md](../TUTORIAL.md)（语言完整教程）互补：
> [TUTORIAL.md](../TUTORIAL.md) 讲语言语法与桌面能力，本目录讲**场景化实战**，重点覆盖 Android 组件流（VUA）与网络/构建链的完整链路。

---

## 学习路径

**零基础（没写过 VUS）**：先读 [TUTORIAL.md](../TUTORIAL.md) 第 1~6 章，
再按下面的路径走通实战。

**会写 VUS、第一次做 Android 界面**：直接读 [01-VUA-组件流-从零到高级](01-VUA-组件流-从零到高级.md)，
从 `.vua` 页面到长列表/富文本/远程图片/动态渲染树一步到位。

**需要联网能力**：读 [02-网络与文件实战](02-网络与文件实战.md)，
覆盖 token 认证、超时/重试、multipart 文件上传、下载与桌面/APK 差异。

**要出 APK 安装包**：读 [03-APK-构建与发布](03-APK-构建与发布.md)，
讲清楚 `build_apk.sh` 每一步在干什么、常见坑怎么绕。

---

## 分册一览

| 分册 | 主题 | 学完能做什么 |
|------|------|--------------|
| [01-VUA-组件流-从零到高级](01-VUA-组件流-从零到高级.md) | Android 组件流（VUA）：`.vua` 页面、`界面_*` 驱动、事件/变量链路、多屏导航、**列表虚拟化**、**网页富文本**、**远程图片缓存**、**动态渲染树**、课表 | 用纯 VUS 写出不卡的列表、富文本、带缓存图片的 Android App |
| [02-网络与文件实战](02-网络与文件实战.md) | `网络_GET/POST/请求/下载`、`文件_上传`（multipart）、文件读写全家桶、桌面(cos libcurl)与 APK(Java 平台桥) 的差异 | 登录、带 token 拉数据、下载、上传文件，一套代码两端跑 |
| [03-APK-构建与发布](03-APK-构建与发布.md) | `build_apk.sh` 完整构建链：资产同步、`.vaz` 展开、`vus_app.c` 生成、JNI 桥、native 交叉编译、JNI 符号校验、javac + R8 dex、打包签名 | 修改即出可安装的 `dist/VUS.apk`，并能排查构建故障 |
| [04-.vus-组件流脚本编写](04-.vus-组件流脚本编写.md) | `.vus` 逻辑脚本：事件三要素（声明→注册→处理）、参数约定、状态读写（`界面_设置/取`）、网络+JSON 数据流、加载更多/分页、网页 JS 回传、换屏生命周期、调试技巧 | 写出"界面骨肉分离、逻辑干净"的完整交互逻辑 |

---

## 对照地图

| 你手里的素材 | 在代码里的位置 |
|---|---|
| 示例工程 | `examples/vua-android/`（源码 + `scripts/build_apk.sh` + `dist/VUS.apk`） |
| 页面与控件数据（单一真相源） | `testdata/`（`vua_*.vua` 页面 + `vua_controls.json` 控件表） |
| 逻辑脚本（事件处理） | `testdata/vua_test.vus` |
| 渲染器（JSON → Android View） | `examples/vua-android/app/src/main/java/com/vus/android/VuaRenderer.java` |
| 平台能力桥（网络/文件 Java 实现） | `.../VuaBridge.java`（`callJava` 分发 `http.*`/`file.*`/`ext.*`） |
| 远程图片加载器 | `.../ImageLoader.java` |
| VUA 规范 | [VUA_REFERENCE.md](../VUA_REFERENCE.md) / [VUA_RENDER_TREE.md](../VUA_RENDER_TREE.md) |