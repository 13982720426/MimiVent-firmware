# MimiVent 固件签名

无线 OTA 只接受签名的 `.mvota` v2 完整包或 `.mvdelta` v2 差分包。签名算法为 RSA-3072、RSA-PSS、SHA-256，固定 32 字节盐。ESP32-C3 固件在校验固件镜像后、写入启动分区前验证签名。App 校验包结构并传输；设备内置公钥是最终信任边界。

## 完整固件

编译得到 `.bin` 后运行：

```sh
python3 scripts/build_ota_package.py build/firmware.bin \
  --key ~/.config/mimivent/firmware-signing/private.pem
```

输出同名 `.mvota` 文件，在 App 中选择该文件升级。App 不接受原始 `.bin`。

## 差分固件

提供当前设备基线 `.bin` 和目标 `.bin`：

```sh
python3 scripts/build_ota_delta.py base.bin target.bin \
  --key ~/.config/mimivent/firmware-signing/private.pem
```

输出 `.mvdelta`。如果希望 App 在设备不支持差分包时回退，需要把目标版本的签名 `.mvota` 放在同目录并使用相同文件名。

## 密钥管理

私钥保存在仓库外的 `~/.config/mimivent/firmware-signing/private.pem`，文件权限为 `600`。它不能提交到 Git、发送到设备或放入 App。保留至少一份离线加密备份；私钥丢失后，已烧录的公钥无法验证新密钥签发的固件，必须通过 USB 刷入更新公钥的固件。

设备公钥位于 `OtaManager.cpp`。轮换密钥时，先发布包含新公钥的固件，并以旧密钥签名完成升级；设备确认运行新固件后，再使用新密钥签署后续固件。当前尚未启用 ESP-IDF 硬件 Secure Boot，因此签名防止未经授权的无线固件安装，不防止可接触 Flash 的攻击者替换整个固件和公钥。
