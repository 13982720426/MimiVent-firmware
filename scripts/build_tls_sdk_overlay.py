#!/usr/bin/env python3
"""仅在项目 build 中生成同版本 TLS SDK 副本；不覆盖安装的 SDK。

锁定 IDF b774170ff46 / mbedTLS 9d669ead；只改变固定 TLS 记录缓冲，
不启用会改变公共结构的 VARIABLE_BUFFER_LENGTH，不改变认证及密码套件配置。
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

SOURCE_COMMIT = '9d669eadb1955d348986b9280156710aaaadf79f'
SOURCE_SHA256 = '45f5e3ca387dfc1dbc41bd221f56971a6d254eb1a88d8a0faadfa9e0d193719d'
ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(arg) for arg in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=ROOT / 'build' / ('mbedtls-' + SOURCE_COMMIT))
    parser.add_argument('--validate-only', action='store_true')
    parser.add_argument('--content-bytes', type=int, choices=(4096, 8192), default=4096)
    args = parser.parse_args()
    original = args.sdk.resolve()
    source = args.source.resolve()
    out = ROOT / 'build' / ('sdk-tls-' + str(args.content_bytes))
    objects = ROOT / 'build' / ('tls-objects-' + str(args.content_bytes))
    archive = ROOT / 'build' / 'mbedtls-9d669ead.tar.gz'
    if sha(archive) != SOURCE_SHA256:
        raise SystemExit('mbedTLS 官方源码压缩包校验失败')
    source_files_verified = 0
    with tarfile.open(archive, 'r:gz') as official:
        for member in official.getmembers():
            relative = Path(*Path(member.name).parts[1:])
            if not member.isfile() or not relative.parts or relative.parts[0] not in ('include', 'library'):
                continue
            if '..' in relative.parts:
                raise SystemExit('源码压缩包路径越界')
            current = source / relative
            if not current.is_file() or current.read_bytes() != official.extractfile(member).read():
                raise SystemExit('已解压源码被修改: ' + str(relative))
            source_files_verified += 1
    if 'esp-idf: v5.5.5 b774170ff46'  not in (original / 'versions.txt').read_text():
        raise SystemExit('SDK 版本与锁定版本不符；禁止混合构建')
    checks = [original / 'lib/libmbedtls_2.a', original / 'sdkconfig',
              original / 'dio_qspi/include/sdkconfig.h', original / 'qio_qspi/include/sdkconfig.h']
    original_hashes = {str(path): sha(path) for path in checks}
    checked_headers = 0
    for header in (source / 'include').rglob('*.h'):
        installed = original / 'include/mbedtls/mbedtls/include' / header.relative_to(source / 'include')
        if installed.exists():
            if sha(header) != sha(installed):
                raise SystemExit('源码与 SDK 头文件不匹配: ' + str(header))
            checked_headers += 1
    if checked_headers < 90:
        raise SystemExit('SDK 头文件校验覆盖不足')
    if args.validate_only:
        print(f'source files verified={source_files_verified}; SDK headers matched={checked_headers}')
        return
    if original == out or out in original.parents or original in out.parents:
        raise SystemExit('SDK 副本不能与输入 SDK 路径重叠')
    if out.exists(): shutil.rmtree(out)
    shutil.copytree(original, out)
    replacements = []
    for relative in ('sdkconfig', 'dio_qspi/include/sdkconfig.h', 'qio_qspi/include/sdkconfig.h'):
        path = out / relative
        text = path.read_text()
        old = 'CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=' if relative == 'sdkconfig' else '#define CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN '
        target = old + '16384'
        if text.count(target) != 1: raise SystemExit('未找到唯一缓冲配置: ' + relative)
        text = text.replace(target, old + str(args.content_bytes))
        if relative == 'sdkconfig':
            text = text.replace('# CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN is not set',
                'CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y\n'
                f'CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN={args.content_bytes}\n'
                f'CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN={args.content_bytes}')
        else:
            text += ('\n#define CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN 1\n'
                     f'#define CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN {args.content_bytes}\n'
                     f'#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN {args.content_bytes}\n')
        path.write_text(text)
        replacements.append(relative)
    objects.mkdir(parents=True, exist_ok=True)
    cc = args.toolchain / 'riscv32-esp-elf-gcc'
    ar = args.toolchain / 'riscv32-esp-elf-ar'
    nm = args.toolchain / 'riscv32-esp-elf-nm'

    def flags(sdk):
        return ['@' + str(sdk / 'flags/c_flags'), '@' + str(sdk / 'flags/defines'),
                '-Os', '-I' + str(source / 'library'),
                '-iprefix', str(sdk / 'include') + '/', '@' + str(sdk / 'flags/includes'),
                '-I' + str(sdk / 'dio_qspi/include')]

    # 内容长度不应改变公开或握手结构大小；否则不能混用现有 ESP-TLS 等预编译库。
    abi = objects / 'abi.c'
    abi.write_text('''#include "common.h"
#include "mbedtls/ssl.h"
#include "ssl_misc.h"
_Static_assert(MBEDTLS_SSL_IN_CONTENT_LEN == EXPECTED_RECORD_BYTES, "actual TLS input buffer mismatch");
_Static_assert(MBEDTLS_SSL_OUT_CONTENT_LEN == EXPECTED_RECORD_BYTES, "actual TLS output buffer mismatch");
const char abi_context[sizeof(mbedtls_ssl_context)] = {0};
const char abi_config[sizeof(mbedtls_ssl_config)] = {0};
const char abi_session[sizeof(mbedtls_ssl_session)] = {0};
const char abi_handshake[sizeof(mbedtls_ssl_handshake_params)] = {0};
const char abi_transform[sizeof(mbedtls_ssl_transform)] = {0};
''')
    def get_abi(sdk, name, expected):
        obj = objects / (name + '.o')
        run([cc, *flags(sdk), '-DEXPECTED_RECORD_BYTES=' + str(expected), '-c', abi, '-o', obj])
        lines = subprocess.check_output([str(nm), '-S', '-P', str(obj)], text=True)
        return {parts[0]: int(parts[3], 16) for line in lines.splitlines()
                if (parts := line.split()) and parts[0].startswith('abi_')}
    base_abi = get_abi(original, 'abi-original', 16384)
    new_abi = get_abi(out, 'abi-overlay', args.content_bytes)
    if base_abi != new_abi: raise SystemExit('TLS ABI 结构尺寸变化，停止: ' + str((base_abi, new_abi)))
    library = out / 'lib/libmbedtls_2.a'
    members = subprocess.check_output([str(ar), 't', str(library)], text=True).splitlines()
    run([ar, 'x', library], cwd=objects)
    rebuilt = []
    for member in members:
        if member in ('mbedtls_debug.c.obj', 'esp_platform_time.c.obj', 'net_sockets.c.obj'):
            continue
        candidate = source / 'library' / member.removesuffix('.obj')
        if not candidate.exists():
            # 三个 ESP 移植文件保持原始对象，使用相同版本/相同 ABI。
            if member not in ('mbedtls_debug.c.obj', 'esp_platform_time.c.obj', 'net_sockets.c.obj'):
                raise SystemExit('无法定位归档成员源码: ' + member)
            continue
        run([cc, *flags(out), '-c', candidate, '-o', objects / member])
        rebuilt.append(member)
        print('rebuilt ' + member, flush=True)
    if len(rebuilt) != 17: raise SystemExit('TLS 重编译成员数量不符')
    library.unlink()
    run([ar, 'rcs', library, *[objects / name for name in members]])
    for path in checks:
        if sha(path) != original_hashes[str(path)]:
            raise SystemExit('全局 SDK 出现非预期变化: ' + str(path))
    manifest = {'sourceCommit': SOURCE_COMMIT, 'sourceArchiveSha256': SOURCE_SHA256,
                'baseSdk': str(original), 'contentBytes': args.content_bytes,
                'headersMatched': checked_headers, 'sourceFilesVerified': source_files_verified, 'abiSizes': base_abi,
                'rebuiltMembers': rebuilt, 'originalHashes': original_hashes,
                'overlayLibrarySha256': sha(library), 'sdkconfigFiles': replacements,
                'overlayConfigHashes': {name: sha(out / name) for name in replacements}}
    (out / 'tls-overlay-manifest.json').write_text(json.dumps(manifest, indent=2))
    print('overlay ready: ' + str(out))


if __name__ == '__main__':
    main()
