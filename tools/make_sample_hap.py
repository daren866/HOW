#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_sample_hap.py - 生成示例 HAP 包 MyFirstDemo.hap
内容：app.json5 / module.json5 / pack.info / pages/index.json / ets/modules.abc
其中 .abc 为真实 Panda 容器头（PACA + 版本号）+ 字符串池 + HOWRT 字节码段，
HOWVM 将真实执行该字节码（onPlus 计数器逻辑）。
"""
import struct, zipfile, os, sys

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "samples", "MyFirstDemo.hap")

def u16(v): return struct.pack('<H', v)
def u32(v): return struct.pack('<I', v)
def f64(v): return struct.pack('<d', v)

APP_JSON5 = '''{
  "app": {
    "bundleName": "com.example.hello",
    "vendor": "how-demo",
    "versionCode": 1000000,
    "versionName": "1.0.0",
    "label": "你好方舟"
  }
}
'''

MODULE_JSON5 = '''{
  "module": {
    "name": "entry",
    "type": "entry",
    "description": "HOW Runtime demo",
    "mainElement": "Index",
    "abilities": [
      { "name": "EntryAbility", "srcEntry": "./ets/entryability/EntryAbility.ets" }
    ],
    "pages": "$profile:main_pages"
  }
}
'''

PACK_INFO = '''{
  "summary": {
    "app": { "label": "你好方舟" }
  }
}
'''

INDEX_JSON = '''{
  "page": { "title": "你好方舟" },
  "state": { "count": 0, "subtitle": "HOWVM 运行中" },
  "root": {
    "type": "column", "padding": 20, "space": 14, "justify": "center",
    "backgroundColor": "#F1F3F5",
    "children": [
      { "type": "text", "text": "Hello ArkUI", "fontSize": 34, "bold": 1,
        "color": "#182431", "textAlign": 1 },
      { "type": "text", "text": "{{subtitle}}", "fontSize": 14,
        "color": "#00A36C", "textAlign": 1 },
      { "type": "text", "text": "当前计数：{{count}}", "fontSize": 20,
        "color": "#E64545", "textAlign": 1 },
      { "type": "row", "space": 12, "justify": "center", "children": [
          { "type": "button", "text": "+ 1", "width": 110, "height": 44,
            "backgroundColor": "#007DFF", "radius": 10,
            "action": { "type": "vmcall", "chunk": "onPlus" } },
          { "type": "button", "text": "重置", "width": 110, "height": 44,
            "backgroundColor": "#E84026", "radius": 10,
            "action": { "type": "set", "var": "count", "value": 0 } }
      ]},
      { "type": "button", "text": "弹出 Toast", "width": 232, "height": 44,
        "backgroundColor": "#5BA854", "radius": 10,
        "action": { "type": "toast", "msg": "你好，来自 HOW Runtime！" } },
      { "type": "spacer", "height": 6 },
      { "type": "text", "text": "HOW Runtime · GDI 软渲染 · .abc 已解析",
        "fontSize": 12, "color": "#99A0A8", "textAlign": 1 }
    ]
  }
}
'''

def build_abc():
    """PACA 头 + 版本 + 元数据字符串池 + HOWRT 字节码段"""
    # 真实 Panda 魔数与版本（参考 arkcompiler_runtime_core file.h）
    data = b'PACA' + bytes([3, 0, 0, 0])
    # 模拟 Panda 字符串池区（utf8 + 终止符）
    strings = [b'entry', b'Index', b'build', b'onPlus', b'aboutToAppear',
               b'console', b'log', b'com.example.hello', b'MyFirstDemo']
    for s in strings:
        data += s + b'\x00'
    # HOWRT 代码段：HOWVM 程序
    # onPlus: LOADG count; PUSHI 1; ADD; STOREG count; HALT
    code = bytes([
        1, 0,                 # OP_LOADG 0
        0, 1, 0, 0, 0,        # OP_PUSHI 1
        3,                    # OP_ADD
        2, 0,                 # OP_STOREG 0
        11,                   # OP_HALT
    ])
    vm = b'HOWRT' + u16(1) + u16(1)          # 标记 + 版本 + 块数
    name = b'onPlus'
    vm += u16(len(name)) + name              # 块名
    vm += u16(1)                             # 全局变量数
    gname = b'count'
    vm += u16(len(gname)) + gname + bytes([1]) + f64(0.0)  # 名 + isNum + 初值
    vm += u32(len(code)) + code              # 字节码
    return data + vm

def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with zipfile.ZipFile(OUT, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('app.json5', APP_JSON5)
        z.writestr('module.json5', MODULE_JSON5)
        z.writestr('pack.info', PACK_INFO)
        z.writestr('pages/index.json', INDEX_JSON.encode('utf-8'))
        z.writestr('ets/modules.abc', build_abc())
        z.writestr('resources/base/media/app_icon.png', b'\x89PNG\r\n\x1a\nHOW-DEMO')
    abc = build_abc()
    print('written:', OUT, os.path.getsize(OUT), 'bytes')
    print('abc magic:', abc[:4], 'version:', list(abc[4:8]))
    print('HOWRT marker at:', abc.find(b'HOWRT'))

if __name__ == '__main__':
    sys.exit(main())
