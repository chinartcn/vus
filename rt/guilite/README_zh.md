> 文档版本：v1.0_apk（APK 功能时代）
> 最后更新时间：2026-09-04


# 歡迎蒞臨 GUI-lite
<p align="center">
    <img src="documents/logo.png" alt="Logo"/>
</p>
<p align="center">
  <img src="https://img.shields.io/badge/build-passing-brightgreen.svg">
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20iOS%20%7C%20Android%20%7C%20MCU-brightgreen.svg">
  <img src="https://img.shields.io/badge/architecture-amd64%20%7C%20arm%20%7C%20arm64-blue.svg">
  <img src="https://img.shields.io/badge/license-Apache%202-blue.svg">
</p>
<p align="center"> 大道至簡 - 4千行/仅头文件/全平台GUI库 </p>
<p align="center">
    <img src="documents/WhyGuiLite-cn.png" alt="Why GuiLite"/>
</p>

## 輕-快-容
- ✂️轻量: 4千行C++代码，0依赖，单一头文件库（GuiLite.h）
- ⚡超快: 高效渲染，单片机上也能流畅运行
- 👫🏻兼容: 完美兼容第3方GUI框架(Qt/MFC/Winform/Cocoa/Web)，现有Qt/MFC代码可以继续使用
- ⚙️️最低硬件要求:

  | Processor | Disk/ROM space | Memory |
  | --- | --- | --- |
  | 24 MHZ | 29 KB | 9 KB |

## 跨平臺
- 支持的操作系统：iOS/macOS/WatchOS，Android，Linux（ARM/x86-64），Windows（包含VR），RTOS... 甚至**无操作系统的单片机**
- 支持的开发语言： C/C++, Swift, Java, Javascript, C#, Golang...
- 支持的第3方库：Qt, MFC, Winforms, CoCoa...

## 實用的功能
- ☁️完美的“云” + “物联网”解决方案：让你轻松驾驭全球IoT业务
- 🔣支持多语言,采用 UTF-8 编码；📀支持视频播放
- 🔨[资源制作工具](https://github.com/idea4good/GuiLiteToolkit)为你定制自己的字体/图片资源
- 📐[所见即所得的GUI布局工具](https://github.com/idea4good/GuiLitePreviewer)
- 📊编译活跃度统计，及实时分析
- 📦支持3D & [Web](https://github.com/idea4good/GuiLiteWeb)
- 🐋支持Docker，一条命令启动Docker: `sudo docker run -it --privileged -v /dev:/dev-share idea4good/gui-lite:latest bash /run.sh`

## 易學習和全面的技術支持
即使是C语言的初学者，也可以很快掌握GUI-lite；代码只使用C++的基本特性（类和虚函数），选择C++语言，能让代码体积更小，更易阅读。
- 📚开发文档
    - [如何使用GUI-lite?](documents/HowToUse.md)
    - [软件设计说明](documents/HowToWork-cn.md)
    - [如何布局UI?](documents/HowLayoutWork.md)
    - [如何制作多种文字/位图资源?](https://github.com/idea4good/GuiLiteToolkit)
    - [如何“换肤”？](https://github.com/idea4good/GuiLiteSamples/blob/master/HostMonitor/UIcode/source/resource/resource.cpp)
    - [如何传递消息?](documents/HowMessageWork.md)
    - [核心UML示意图](documents/UML.md)
- 📈学习方法
    1. 编译GUI-lite库
    2. 编译、调试：HelloXXX实例程序
    3. 研读、修改100+行的`HelloXXX/UIcode/UIcode.cpp`源代码
    4. 研读`widgets`目录下的代码
    5. 研读`core`目录下的代码
    6. 扩展/开发自己的GUI库
- 📞遇到任何问题，都可以[联系我们](#社区交流)，也欢迎加入我们的开发者大家庭
- 🌐[码云Gitee快速链接](https://gitee.com/idea4good/GuiLite)

## Demo展示墻
- 0依赖，100%编译通过，可直接运行
- 点击你喜欢的实例，并运行在你的硬件上面
- 大部分demo的UI代码只有100行左右，代碼庫在[GuiLiteSamples](https://github.com/idea4good/GuiLiteSamples)
---
### 單片機平臺
---
<table>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/Hello3D"><img src="documents/Hello3D.gif" width="200px;"/><br /><sub><b>3D on STM32</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloWave"><img src="documents/HelloWave.gif" width="200px;"/><br /><sub><b>Wave on STM32</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloParticle"><img src="documents/HelloParticle.gif" width="200px;"/><br /><sub><b>Particle on STM32</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloStar"><img src="documents/HelloStar.gif" width="200px;"/><br /><sub><b>Star on STM32</b></sub></a>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/Hello3Dwave"><img src="documents/Hello3Dwave.gif" width="200px;"/><br /><sub><b>3D wave on STM32</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloNoTouch"><img src="documents/HelloNoTouch.gif" width="200px;"/><br /><sub><b>Keyboard on STM32</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloMario"><img src="documents/HelloMario.gif" width="200px;"/><br /><sub><b>Mario on STM32</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloCircle"><img src="documents/HelloCircle.gif" width="200px;"/><br /><sub><b>3D circle on STM32</b></sub></a>
  </tr>
  <tr>
  <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/Hello3Ddonut"><img src="documents/Hello3Ddonut.gif" width="200px;"/><br /><sub><b>3D donut on STM32</b></sub></a>
  <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloTimer"><img src="documents/HelloTimer.gif" width="200px;"/><br /><sub><b>Timer</b></sub></a>
  <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloMolecule"><img src="documents/HelloMolecule.gif" width="200px;"/><br /><sub><b>Molecule move</b></sub></a>
  <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloPendulum"><img src="documents/HelloPendulum.gif" width="200px;"/><br /><sub><b>Pendulum effect</b></sub></a>
  </tr>
</table>

---
### IoT 功能
---
<table>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HostMonitor"><img src="documents/data_on_cloud.png" width="600px;"/><br /><sub><b>Monitor IoT device on cloud</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HostMonitor"><img src="documents/BuildInfo.png" width="600px;"/><br /><sub><b>Code Telemetry & Analysis in real time</b></sub></a>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloAzureIoT"><img src="documents/HelloAzureIoT.jpg" width="600px;"/><br /><sub><b>Track IoT device over the world</b></sub></a>
  </tr>
</table>

---
### 多語言，設計工具及視頻
---
<table>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloFont"><img src="documents/HelloFont.gif" width="600px;"/><br /><sub><b>Lattice Font</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloFreetype"><img src="documents/HelloFreetype.gif" width="600px;"/><br /><sub><b>Freetype Font</b></sub></a>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLitePreviewer"><img src="documents/Previewer.gif" width="600px;"/><br /><sub><b>Layout GUI</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloJPG"><img src="documents/HelloJPG.gif" width="600px;"/><br /><sub><b>Render JPG file quickly</b></sub></a>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloFFmpeg"><img src="documents/HelloFFmpeg.jpg" width="600px;"/><br /><sub><b>Play video with FFmpeg</b></sub></a>
  </tr>
</table>

---
### UI 控件實作
---
<table>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloGuiLite"><img src="documents/HelloGuiLite.gif" width="600px;"/><br /><sub><b>Emulate  Windows UI</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloScroll"><img src="documents/HelloScroll.gif" width="600px;"/><br /><sub><b>Scroll widget</b></sub></a>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloWidgets"><img src="documents/HelloWidgets.gif" width="600px;"/><br /><sub><b>How to use widgets</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloNets"><img src="documents/HelloNets-pc.gif" width="600px;"/><br /><sub><b>3D Nets on Windows/Linux</b></sub></a>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloTransparent"><img src="documents/HelloTransparent.gif" width="600px;"/><br /><sub><b>Transparent dialog</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloSlide"><img src="documents/HelloSlide.gif" width="600px;"/><br /><sub><b>Swipe view</b></sub></a>
  </tr>
</table>

---
### 跨平臺實作
---
<table>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HostMonitor"><img src="documents/HostMonitor.gif" width="300px;"/><br /><sub><b>HostMonitor on Windows</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HostMonitor"><img src="documents/Mac.gif" width="300px;"/><br /><sub><b>HostMonitor on Mac</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HostMonitor"><img src="documents/Android.gif" width="300px;"/><br /><sub><b>HostMonitor on Android</b></sub></a>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HostMonitor"><img src="documents/WinMR.gif" width="300px;"/><br /><sub><b>HostMonitor on Windows Mixed Reality</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HostMonitor"><img src="documents/Linux.gif" width="300px;"/><br /><sub><b>HostMonitor on Linux</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteWeb"><img src="documents/GuiLite3D.gif" width="300px;"/><br /><sub><b>3D on Web</b></sub></a>
  </tr>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteWeb"><img src="documents/GuiLiteCube.gif" width="300px;"/><br /><sub><b>3D on Web</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HostMonitor"><img src="documents/iOS.gif" width="300px;"/><br /><sub><b>HostMonitor on iPhone</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteWeb"><img src="documents/GuiLiteGraphic.gif" width="300px;"/><br /><sub><b>How display work</b></sub></a>
  </tr>
</table>

---
### 蘋果平臺
---
<table>
  <tr>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/Hello3D"><img src="documents/iWatch.hello3D.gif" width="300px;"/><br /><sub><b>3D on Apple Watch</b></sub></a>
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloWave"><img src="documents/iWatch.helloWave.gif" width="300px;"/><br /><sub><b>Wave on Apple Watch</b></sub></a>    
    <td align="center"><a href="https://github.com/idea4good/GuiLiteSamples/tree/master/HelloParticle"><img src="documents/iOS.particle.gif" width="300px;"/><br /><sub><b>Particle on iPhone</b></sub></a>
  </tr>
</table>

## 📞社區交流
- 感谢开发者群的所有同学，是你们塑造了今天的GUI-lite！也欢迎新的大神/小白加入我们。
- 开发者QQ
<table>
  <tr>
    <td align="center"><img src="documents/qq.group-7.png" width="240px;"/><br /><sub><b>開發群🔑：875721211</b></sub></a>
  </tr>
  