# AutoICON

A Win32 application that automatically hides your Windows desktop icons, with Microsoft Store packaging support.

## 项目结构 (Project Structure)

- **`AutoICON/`**: 核心 Win32 应用程序工程（C++），集成 C++/WinRT 支持 Windows 应用商店沙箱自启动机制。
- **`AutoICON(Package)/`**: Windows 应用打包工程（WAP / MSIX），用于 Microsoft Store 发布和打包。
- **`AutoICON.slnx`**: Visual Studio 统一解决方案文件。
- **`OldVersions/`**: 历史单文件版本归档。
- **`ShowPage/`**: 赞助与支持展示页面。

## 构建与运行 (Build & Run)

1. 使用 **Visual Studio 2022**（建议 17.10 及以上版本）打开根目录下的 `AutoICON.slnx`。
2. 确保已安装工作负荷：
   - **使用 C++ 的桌面开发** (Desktop development with C++)
   - **通用 Windows 平台开发** (Universal Windows Platform development)
3. 还原 NuGet 程序包（包含 `Microsoft.Windows.CppWinRT` 和 `Microsoft.Windows.SDK.BuildTools`）。
4. 选择 `Release | x64` 或 `Release | x86` 即可进行编译：
   - 若要运行桌面独立程序，将 `AutoICON` 设为启动项目。
   - 若要调试或生成商店安装包，将 `AutoICON(Package)` 设为启动项目，通过 **项目 -> 发布 -> 创建应用程序包** 生成 MSIX 包。

## 许可证 (License)

本项目采用 [MIT License](LICENSE) 许可证。
