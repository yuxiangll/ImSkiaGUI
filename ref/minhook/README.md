# ref/minhook — 钩子库（MinHook）依赖

本目录提供 `HooksManager` 需要的 MinHook（x64）。

## 目录内容

```
include/MinHook.h         MinHook 1.3.3 公开头文件（BSD-2-Clause）
lib/x64/buffer.obj        ┐
lib/x64/hook.obj          │ 用 clang-cl 23.1.0 / /MT /O2 编译好的 MinHook 目标文件
lib/x64/trampoline.obj    │ （源文件缺失时的回退方案）
lib/x64/hde64.obj         ┘
```

## 为什么是 .obj 而不是 .c

原本项目里是 `ref/minhook-master/`（带 `src/*.c` 源码），但该目录在本次改造前
已经不在工作区里了（`ref/` 只剩 `skia-canvas-3.0.8/`），而本机无外网、也无法
从其它位置找到 MinHook 源码。为了让工程仍然能一键编译，这里把**用同一套
clang-cl 23.1.0 + `/MT` 编译出来的目标文件**连同头文件一起纳入版本控制。

实测（`tests/minhook_probe.cpp`）：MH_Initialize / MH_CreateHook / MH_EnableHook /
调用被钩函数 / 调用 trampoline / MH_DisableHook / MH_RemoveHook / MH_Uninitialize
全部正常，钩子命中且 trampoline 返回值正确。

## 恢复源码版本（可选）

把 MinHook 源码放回 `ref/minhook-master/`（结构为 `include/MinHook.h` +
`src/{buffer,hook,trampoline}.c` + `src/hde/hde64.c`），构建脚本会**自动优先
编译源码**，不再使用这里的 .obj：

```
ref/minhook-master/include/MinHook.h
ref/minhook-master/src/buffer.c
ref/minhook-master/src/hook.c
ref/minhook-master/src/trampoline.c
ref/minhook-master/src/hde/hde64.c
```

`build_overlay.bat` / `build_canvas.bat` / `src/CMakeLists.txt` 都做了这个
"有源码就编译、没有就链接 .obj" 的判断。

## 许可证

MinHook: Copyright (C) 2009-2017 Tsuda Kageyu, BSD-2-Clause（见 `include/MinHook.h` 头部）。
