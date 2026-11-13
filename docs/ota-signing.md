# MimiVent 固件签名

无线 OTA 只接受签名的 `.mvota` v2 完整包或 `.mvdelta` v2 差分包。原始 `.bin` 是 ESP 镜像，只能作为打包输入，App 不接受它。

签名算法为 RSA-3072、RSA-PSS、SHA-256，固定 32 字节盐。签名覆盖完整目标镜像；设备在校验镜像及签名后，才允许切换启动分区。App 校验包结构并传输；设备内置公钥是最终信任边界。

## 固定格式约定

当前完整包和差分包的分块大小都是 **4096 字节（4 KiB）**。App、打包器和设备接收端必须一致；只改其中一端会使设备在传输开始后返回 `ERROR:PACKAGE_BLOCK` 或对应的块格式错误。

| 包格式 | App 解析器 | 打包器 | 固件接收端 | 固定分块 |
| --- | --- | --- | --- | ---: |
| `.mvota` v2 完整包 | `MimiVent-app/lib/features/firmware_update/ota_package.dart` 的 `OtaPackage.blockSize` | `scripts/build_ota_package.py` 的 `BLOCK_SIZE` | `OtaManager.cpp` 的 `kOtaPackageBlockBytes` | 4096 B |
| `.mvdelta` v2 差分包 | `MimiVent-app/lib/features/firmware_update/ota_delta_package.dart` 的 `OtaDeltaPackage.blockSize` | `scripts/build_ota_delta.py` 的 `BLOCK_SIZE` | `OtaManager.cpp` 的 `kOtaDeltaBlockBytes` | 4096 B |

修改任一分块常量时，必须同步检查同一行的另外两个实现，以及对应的解析/打包测试：完整包是 `MimiVent-app/test/ota_package_test.dart` 和 `tests/test_ota_package_builder.py`；差分包是 `MimiVent-app/test/ota_delta_package_test.dart` 和 `tests/test_ota_delta_builder.py`。`.mvota` v2 的分块大小写在 512 字节固定头的偏移 20（小端 uint32）；`.mvdelta` v2 写在 576 字节固定头的偏移 156（小端 uint32）。打包完成后应核对头字段为 4096，并确认包的签名针对同一份 `.bin` 镜像。

## 完整固件

先构建当前完整 WiFi/MQTT 固件，使用 `read4k` 模式：

```sh
bash scripts/build_firmware.sh read4k
```

这会生成 `build/firmware-wifi-read4k/MimiVent.ino.bin`。不要用 `off` 或 `on` 模式的镜像替代完整 WiFi/MQTT 发布镜像。

再把 `.bin` 打成 App 可选的签名 `.mvota`：

```sh
python3 scripts/build_ota_package.py \
  build/firmware-wifi-read4k/MimiVent.ino.bin \
  --key ~/.config/mimivent/firmware-signing/private.pem \
  --output build/MimiVent-0.7.0.mvota
```

在 App 的“固件升级”页面选择生成的 `.mvota`。文件名应与镜像中嵌入的固件版本一致；后续发布需相应更新命令中的版本号。发布时同时记录固件版本、镜像 SHA-256、包 SHA-256、包格式和 4096 字节分块值；不要把 `.bin` 改扩展名冒充 `.mvota`。

## 差分固件

提供当前设备基线 `.bin` 和目标 `.bin`：

```sh
python3 scripts/build_ota_delta.py base.bin target.bin \
  --key ~/.config/mimivent/firmware-signing/private.pem
```

输出 `.mvdelta`。目标与基线必须是分区配置相同、设备当前运行的 `.bin` 镜像。如果希望 App 在设备不支持差分包时回退，需要把目标版本的签名 `.mvota` 放在同目录并使用相同文件名。

## 密钥管理

私钥保存在仓库外的 `~/.config/mimivent/firmware-signing/private.pem`，文件权限为 `600`。它不能提交到 Git、发送到设备或放入 App。保留至少一份离线加密备份；私钥丢失后，已烧录的公钥无法验证新密钥签发的固件，必须通过 USB 刷入更新公钥的固件。

设备公钥位于 `OtaManager.cpp`。轮换密钥时，先发布包含新公钥的固件，并以旧密钥签名完成升级；设备确认运行新固件后，再使用新密钥签署后续固件。当前尚未启用 ESP-IDF 硬件 Secure Boot，因此签名防止未经授权的无线固件安装，不防止可接触 Flash 的攻击者替换整个固件和公钥。
