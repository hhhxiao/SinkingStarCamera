# SinkingStarCamera

《Order of the Sinking Star（沉星之序）》相机 Mod：地图缩放、视场角调整、纯俯视视角。

不修改游戏目录里的任何文件——通过注入在运行时生效，关掉游戏即恢复原样。

## 功能

| 按键 | 作用 |
|---|---|
| **F9** | 缩小（相机拉远） |
| **F10** | 放大（相机拉近） |
| **F7** | 视场角变宽 |
| **F8** | 视场角变窄 |
| **F6** | 纯俯视开关（相机移到目标正上方、去掉倾斜；保留约 0.3° 微顷，让屏幕保持北朝上、WASD 与画面方向一致） |
| **F4** | 一键复位（高度、视场角、俯视全部还原） |
| **F11** | 显示/隐藏游戏内叠加层（网格和涂鸦一起） |
| **F3** | 涂鸦模式开关 |
| **Shift+F3** | 清空涂鸦 |

**想要"工程图"效果**：按 **F6** 打开纯俯视 → 用 **F9/F10** 选高度 → 按 **F8** 收窄视场角。

相机部分标记为“实验性，只有部分关卡生效”：不同关卡的相机控制方式不完全一致，若某个关卡里
无效，请按 **F4** 复位并保留日志。

## 使用

1. 运行 `SinkingStarCameraInjector.exe`（同目录下需有 `sinkingstar_camera.dll` 与 `config.json`）。
   它会通过 Steam 启动游戏并自动注入；若游戏已在运行，则直接注入。
2. 进入游戏后使用上面的热键。

## 配置（`config.json`）

```json
{
    "target_module": "sinking_star.exe",
    "debug_mode": false,
    "log_level": "info",
    "poll_hz": 60,
    "rescan_interval_ms": 3000,
    "zoom": { "scale": 1.0, "step": 1.25, "min": 0.05, "max": 50.0 },
    "fov":  { "step": 1.15, "min": 0.1, "max": 3.0 },
    "grid": {
        "enabled": true,
        "cell": 1.0,
        "range": 20,
        "offset": 0.5,
        "width": 2,
        "color_r": 0,
        "color_g": 0,
        "color_b": 0
    },
    "d3d12": { "enabled": true, "test": false },
    "panel": { "visible": false },
    "doodle": { "enabled": false, "width": 2, "color_r": 0, "color_g": 0, "color_b": 0 }
}
```

| 字段 | 说明 |
|---|---|
| `zoom.scale` | 启动时的缩放（1.0 = 游戏原始距离） |
| `zoom.step` | 每次 F9/F10 的倍率，想更细腻可改成 1.1 |
| `zoom.min` / `zoom.max` | 缩放范围 |
| `fov.step` | 每次 F7/F8 的倍率，想更细腻可改成 1.05 |
| `fov.min` / `fov.max` | 视场角相对原始值的范围 |
| `grid.*` | 网格开关、格子尺寸、范围、偏移、线宽和线色 |
| `d3d12.enabled` | 使用游戏内 D3D12 叠加层；网格和涂鸦需要开启 |
| `doodle.width` | 涂鸦线宽（像素），默认 2 |
| `doodle.color_r/g/b` | 涂鸦线色，默认黑色 `0,0,0` |
| `poll_hz` | 轮询频率（默认 60） |
| `debug_mode` | `true` 时额外打开控制台窗口 |

日志写在 dll 旁边的 `sinkingstar_camera.log`。

涂鸦设置可以在 F2 面板里实时调整；勾选“直线模式”后拖动会画直线，也可以在普通涂鸦模式下按住
**Shift** 临时画直线。

## 原理

游戏由 Jai 编译，可执行文件里保留了完整的类型反射数据。Mod 在**运行时**按类型名定位
`Camera_Control` 组件（因此游戏更新挪动内部地址也不会失效），在进程内存里找到活的相机实体，
每帧写入距离 / 视场角字段，引擎自己据这些值重建视图。网格和涂鸦走独立的 D3D12 叠加层，
通过 Present 和窗口消息钩子绘制，不修改游戏文件；相机部分不依赖这些钩子。

## 注意

- 关卡内的相机同时受关卡逻辑驱动，Mod 的写入会与它共同作用；镜头出现异常时按 **F4** 复位。
- 纯俯视会改变方向感，依赖镜头朝向的交互场景建议用完按 **F4** 或 **F6** 还原。
- 若游戏更新后 Mod 失效（日志出现 `accepted 0`），需要适配新版本。

## 卸载

关闭游戏，删除本文件夹即可。游戏目录从未被修改。

## 许可证

MIT，见 [LICENSE](LICENSE)。
