# 第三方依赖

自有项目代码暂未指定开源许可证，保留相关权利。公开仓库供查看、课程展示和复现参考；其他使用的授权范围需与权利人确认。下列第三方软件按其各自许可证使用，原始声明随文件保留。

| 组件 | 固定版本 | 许可依据 |
| --- | --- | --- |
| Adafruit BusIO | 1.16.1 | lib/Adafruit_BusIO/LICENSE |
| Adafruit GFX | 1.11.11 | lib/Adafruit_GFX/license.txt |
| Adafruit PN532 | 1.3.4 | lib/Adafruit_PN532/license.txt |
| Adafruit SH110X | 2.1.12 | lib/Adafruit_SH110X/license.txt |
| RTClib | 2.1.4 | lib/RTClib/license.txt |
| Arduino ESP32 / ESP-IDF 等工具链组件 | PlatformIO espressif32 7.1.3 解析的版本 | 构建工具下载的各组件原许可证 |
| Python 后台依赖 | admin/requirements-lock.txt | 各 PyPI 包附带的许可证 |

`certs/x509_crt_bundle` 是由 certifi/Mozilla 根证书集合生成的公开信任数据，不含终端私钥或生产证书。生成脚本为 `tools/make_cert_bundle.py`。certifi 的证书分发遵循 MPL-2.0，参见 [certifi 上游说明](https://github.com/certifi/python-certifi)。

第三方源码中的作者名称、上游邮箱和示例地址属于原版权声明，予以保留，不表示本项目部署身份。分发修改后的第三方文件前应检查对应许可证要求。
