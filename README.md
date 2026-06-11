> [!WARNING]
> **This demo project is for the deprecated Xsolla UE4 SDK and is no longer actively maintained.**
>
> If you arrived here from a web search, this is a legacy sample project for an older Xsolla Unreal Engine integration. For current Unreal Engine integration, use the **Xsolla Game Commerce SDK** instead — it supports Unreal Engine 5 and covers login, store, payments, and subscriptions in a single unified plugin.
>
> - 🔗 **[Game Commerce SDK](https://github.com/xsolla/store-ue4-sdk)** on GitHub — the actively maintained UE replacement
> - 📖 **[Unreal Engine SDK Documentation](https://developers.xsolla.com/sdk/unreal-engine/)** — get started with the current integration
> - 🔭 **[SDK Explorer](https://developers.xsolla.com/sdk/demo/)** — try the SDK interactively before writing any code

# xsolla-ue4-demo

## Overview

The Xsolla UE4 Demo is an Unreal Engine sample project that demonstrates integration of the Xsolla SDK for Unreal Engine. It provides a working example game showing Xsolla features — including user authentication, in-game store, and payment flows — implemented using the Xsolla Unreal Engine plugin.

The project uses git submodules and includes build automation scripts (`ue_build_automation.py`, `ue_run_tests.py`) for CI/CD workflows.

## Requirements

- **Unreal Engine:** See `MyXsolla.uproject` → `EngineAssociation` for the exact pinned version
- **Xsolla store-ue4-sdk plugin:** Included as a git submodule in `Plugins/`
- **Visual Studio 2019+** (Windows) or **Xcode** (macOS) for C++ compilation
- **Python 3** (for build automation scripts)
- **Xsolla Publisher Account:** Required — [sign up here](https://publisher.xsolla.com/)

## Install

> **Note:** This repo uses git submodules. Always clone with `--recurse-submodules`.

```bash
git clone --recurse-submodules https://github.com/xsolla/xsolla-ue4-demo.git
cd xsolla-ue4-demo
```

If you already cloned without `--recurse-submodules`, initialize submodules:

```bash
git submodule update --init --recursive
```

Then:

1. Right-click `MyXsolla.uproject` → **Generate Visual Studio project files** (Windows) or open with Unreal Editor.
2. Build the project in Unreal Engine.

## Usage

1. Open `MyXsolla.uproject` in the Unreal Editor.
2. Configure your Xsolla project credentials (project ID, login ID) in the plugin settings under **Project Settings > Xsolla SDK**.
3. Open the main demo level from the `Content/` browser.
4. Click **Play** to run the demo.

For automated builds and tests, use the included scripts:

```bash
python ue_build_automation.py
python ue_run_tests.py
```

## Documentation

- [Xsolla SDK for Unreal Engine](https://developers.xsolla.com/sdk/unreal-engine/)
- [Xsolla Developer Portal](https://developers.xsolla.com/)

## Support

- [GitHub Issues](https://github.com/xsolla/xsolla-ue4-demo/issues)
- [Xsolla Developer Portal](https://developers.xsolla.com/)

## License

MIT License. See [LICENSE](./LICENSE).
