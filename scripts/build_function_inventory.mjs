import fs from "node:fs/promises";
import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";

const outputDir = "D:/Project/VoltPilot/outputs/function_inventory";
await fs.mkdir(outputDir, { recursive: true });

const workbook = Workbook.create();
const summary = workbook.worksheets.add("概览");
const detail = workbook.worksheets.add("功能清单");
const test = workbook.worksheets.add("硬件验证");
const stc = workbook.worksheets.add("STC8H清单");

const rows = [
  ["启动与基础框架", "ESP32-S3 启动入口与 FreeRTOS 服务初始化", "已实现", "main.c、app_start.c", "已具备", "已接入 board/fault/diag/watchdog/UI/ADC/BMS/STC/display 服务"],
  ["输出控制", "24V/36V/48V EN GPIO 控制与默认关闭", "已实现", "board_service.c", "已具备", "GPIO17/16/15；禁止多路同时打开"],
  ["输出控制", "挡位到 24V/36V/48V 的真实 EN 映射", "未实现", "app_state_service.c", "等待实现", "当前只执行虚拟输出请求，真实 EN 保持关闭"],
  ["挡位输入", "ESP32 直接读取三档开关", "已实现", "gear_service.c、vp_board.h", "可联调", "GPIO27/33/34；公共端接 GND；内部上拉"],
  ["挡位输入", "STC8H 读取三档开关并通过 UART 上报", "部分实现", "stc_service.c、stc_protocol.c", "受硬件限制", "协议和服务已存在，但当前没有 STC8H 实物验证"],
  ["AI 模拟量", "AI_RS485 Modbus RTU 读取 4 路输入寄存器", "已实现", "ai_rs485_service.c", "可联调", "UART1；GPIO37/38；9600 8N1；功能码 04H"],
  ["AI 模拟量", "模拟量原始值换算为工程量", "部分实现", "ai_rs485_service.c", "需完善", "当前保存 raw 值；小数点和电流量程换算尚未接入快照"],
  ["AI 模拟量", "AI 模块作为输出电压反馈", "未实现", "无独立闭环模块", "等待实现", "需明确通道量程、接线和目标值容差"],
  ["BMS", "BMS RS485 请求、缓存、CRC 和协议解析", "已实现", "bms_service.c、bms_protocol.c", "模拟可测", "支持 14S/15S 数据结构；当前默认使用 Mock"],
  ["BMS", "真实 BMS 硬件联调", "受硬件限制", "bms_service.c", "等待硬件", "当前没有 BMS 实物"],
  ["ADC", "ESP32 ADC DMA 采样 5V/24V/36V/48V", "已实现", "adc_service.c", "可做基础测试", "GPIO4/5/6/7；当前分压比例仍为 1:1"],
  ["ADC", "真实高压输出电压校准", "未实现", "vp_board.h", "等待硬件", "需根据实际分压电阻修改比例并用万用表校准"],
  ["人机交互", "按键单击、双击、长按和消抖", "已实现", "ui_service.c", "可验证", "GPIO39；长按执行安全关闭"],
  ["人机交互", "蜂鸣器与红绿状态灯", "已实现", "board_service.c、ui_service.c", "可验证", "GPIO40/41/42"],
  ["显示", "电子墨水屏初始化和四色启动测试图案", "已实现", "display_service.c、epd_2in13.c", "需屏幕硬件", "GPIO9~14；当前只显示启动测试图案"],
  ["显示", "电池、挡位、输出和故障实时界面", "未实现", "display_service.c", "等待实现", "当前没有业务数据显示页面"],
  ["安全", "输出互斥、故障关闭和默认关闭", "已实现", "board_service.c、fault_service.c", "可审查", "故障入口优先关闭全部 EN"],
  ["安全", "STC 物理安全互锁", "部分实现", "app_state_service.c、stc_service.c", "等待接线确认", "未配置互锁 mask 时按失效安全策略拒绝启动"],
  ["诊断", "NVS 双槽、CRC、启动和故障记录", "已实现", "diag_service.c", "可审查", "保存状态、通信错误、故障和看门狗信息"],
  ["看门狗", "任务订阅、喂狗和监督", "已实现", "watchdog_service.c", "可审查", "监控关键 FreeRTOS 任务"],
  ["测试", "协议解析和状态机自动化测试", "未实现", "项目暂无测试目录", "等待实现", "建议补充主机侧单元测试和硬件联调记录"],
  ["STC8H硬件", "STC8H 20脚 MCU 作为挡位采集从机", "设计已定义", "硬件图、STC协议文档", "等待实物", "当前项目已有协议模型，但没有 STC8H 实物和固件"],
  ["STC8H硬件", "P3.2/P3.1/P3.0 对应 24V/36V/48V 挡位", "设计已定义", "STC8H硬件图", "等待实物", "最终应由 STC 采样并编码后上报 ESP32"],
  ["STC8H硬件", "P3.5/P3.4 UART 通信", "设计已定义", "STC8H硬件图", "等待实物", "STC_RX/STC_TX；需确认电平为 3.3V"],
  ["STC8H硬件", "P1.1 运行灯、P1.0 保留", "设计已定义", "STC8H硬件图", "未验证", "可作为 STC 本地状态指示扩展"],
  ["STC8H采集", "三档开关输入采集", "未实现", "STC8H固件缺失", "等待实现", "当前由 ESP32 临时直读 GPIO27/33/34"],
  ["STC8H采集", "挡位 ADC/数字量判定", "协议已定义", "stc_protocol.c", "等待实现", "READ_GEAR 返回 raw_gear、gear_valid、adc_key_raw"],
  ["STC8H采集", "挡位消抖", "协议已定义", "stc_protocol.c、STC协议文档", "等待实现", "READ_GEAR 返回 debounce_ms，具体采样算法需在 STC 固件实现"],
  ["STC8H协议", "Modbus RTU 风格帧结构", "已实现协议模型", "stc_protocol.c", "主机侧可测", "ADDR/FUNC/SEQ/LEN/PAYLOAD/CRC16"],
  ["STC8H协议", "CRC16/MODBUS 计算", "已实现", "stc_protocol.c", "主机侧可测", "低字节先发送"],
  ["STC8H协议", "HEARTBEAT 心跳响应", "ESP32侧已实现", "stc_service.c", "等待 STC 实物", "周期 1000ms；返回 uptime、status_flags、protocol_version"],
  ["STC8H协议", "READ_GEAR 挡位响应", "ESP32侧已实现", "stc_service.c", "等待 STC 实物", "周期 1000ms；返回挡位和消抖信息"],
  ["STC8H协议", "READ_IO_STATUS 输入输出状态", "ESP32侧已实现", "stc_service.c", "等待 STC 实物", "周期 1000ms；返回 io_inputs、io_outputs"],
  ["STC8H协议", "READ_VERSION 版本响应", "ESP32侧已实现", "stc_service.c", "等待 STC 实物", "周期 10000ms；检查协议/固件/硬件主版本"],
  ["STC8H协议", "WRITE_CONTROL 提示控制", "协议已预留", "stc_protocol.c、stc_service.c", "未接入", "可用于运行灯/蜂鸣器提示，不能直接控制 EN"],
  ["STC8H安全", "STC 在线超时", "已实现", "stc_service.c、app_state_service.c", "主机侧可测", "10 秒没有有效帧则进入 STC_TIMEOUT"],
  ["STC8H安全", "STC 版本兼容性检查", "已实现", "app_state_service.c", "主机侧可测", "协议版本、固件主版本、硬件主版本检查"],
  ["STC8H安全", "STC IO 互锁输入检查", "框架已实现", "app_state_service.c、vp_board.h", "等待接线", "mask 为 0 时失效安全拒绝启动"],
  ["STC8H安全", "STC 不直接控制 24V/36V/48V EN", "设计已约束", "STC协议文档、app_state_service.c", "已具备", "ESP32 统一执行输出安全策略"],
];

summary.showGridLines = false;
summary.getRange("A1:F1").merge();
summary.getRange("A1").values = [["VoltPilot ESP32-S3 + STC8H 功能实现状态总览"]];
summary.getRange("A3:B7").values = [
  ["统计项", "数量"],
  ["已实现", null],
  ["部分实现", null],
  ["未实现", null],
  ["受硬件限制", null],
];
summary.getRange("B4:B7").formulas = [["=COUNTIF('功能清单'!$C$2:$C$100, A4)"], ["=COUNTIF('功能清单'!$C$2:$C$100, A5)"], ["=COUNTIF('功能清单'!$C$2:$C$100, A6)"], ["=COUNTIF('功能清单'!$C$2:$C$100, A7)"]];
summary.getRange("D3:F7").values = [
  ["当前结论", "说明", "依据"],
  ["可立即联调", "AI_RS485、三档开关、GPIO 状态机", "当前已有 ESP32-S3、RS485 模块和模拟量模块"],
  ["暂不接高压", "真实 EN 输出保持关闭", "尚无安全功率级和实际电源输出硬件"],
  ["STC 待后续接入", "保留 STC 协议和服务", "当前只有三档开关，没有 STC8H 实物"],
  ["下一优先级", "完善 AI 数值换算和反馈闭环", "当前服务已读取 raw 值"],
];
summary.getRange("A1:F1").format = { font: { bold: true, color: "#000000", size: 16 }, horizontalAlignment: "center", verticalAlignment: "center" };
summary.getRange("A3:B3").format = { font: { bold: true, color: "#000000" } };
summary.getRange("D3:F3").format = { font: { bold: true, color: "#000000" } };
summary.getRange("A3:B7").format.borders = { preset: "all", style: "thin", color: "#B7B7B7" };
summary.getRange("D3:F7").format.borders = { preset: "all", style: "thin", color: "#B7B7B7" };
summary.getRange("D4:F7").format.wrapText = true;
summary.getRange("A1:F1").format.rowHeight = 30;
summary.getRange("A:A").format.columnWidth = 16;
summary.getRange("B:B").format.columnWidth = 10;
summary.getRange("D:D").format.columnWidth = 18;
summary.getRange("E:E").format.columnWidth = 34;
summary.getRange("F:F").format.columnWidth = 48;

detail.showGridLines = false;
detail.getRange("A1:F1").values = [["模块", "功能", "状态", "代码位置", "验证状态", "说明"]];
detail.getRange(`A2:F${rows.length + 1}`).values = rows;
detail.getRange(`A1:F${rows.length + 1}`).format.wrapText = true;
detail.getRange("A1:F1").format = { font: { bold: true, color: "#000000" }, horizontalAlignment: "center", verticalAlignment: "center" };
detail.getRange(`A1:F${rows.length + 1}`).format.borders = { preset: "all", style: "thin", color: "#B7B7B7" };
detail.getRange("A:A").format.columnWidth = 16;
detail.getRange("B:B").format.columnWidth = 34;
detail.getRange("C:C").format.columnWidth = 14;
detail.getRange("D:D").format.columnWidth = 32;
detail.getRange("E:E").format.columnWidth = 18;
detail.getRange("F:F").format.columnWidth = 58;
detail.freezePanes.freezeRows(1);

test.showGridLines = false;
test.getRange("A1:G1").values = [["阶段", "测试目标", "接线/前置条件", "操作", "预期结果", "当前状态", "备注"]];
test.getRange("A2:G7").values = [
  ["1", "三档开关识别", "公共端接 GND；GPIO27/33/34", "分别拨到三个档位并观察串口", "gear=1/2/3，非法组合为 0", "可执行", "不接高压"],
  ["2", "RS485 物理通信", "收发器 TXD/RXD；A/B；共地", "给 AI 模块 6~36V 供电并启动设备", "模块通信灯有响应", "可执行", "确认逻辑电平兼容"],
  ["3", "Modbus 读取", "模块地址 1，9600 8N1", "读取 0x0000 起始的 4 路输入寄存器", "出现 AI RX channels 日志", "可执行", "普通问答模式"],
  ["4", "模拟量换算", "配置安全 0~5V 或 0~10V 量程", "输入已知低压并比对万用表", "raw 值与工程量一致", "待完善", "当前只记录 raw"],
  ["5", "状态机联调", "保持 VP_ENABLE_VIRTUAL_OUTPUT=1", "有效挡位后执行单击/双击/长按", "状态切换，真实 EN 仍关闭", "可执行", "当前无真实功率输出"],
  ["6", "真实输出验证", "需要功率级、电源、保护和反馈", "完成硬件评审后再执行", "对应 EN 和反馈闭环正确", "不可执行", "当前禁止进行"],
];
test.getRange("A1:G1").format = { font: { bold: true, color: "#000000" }, horizontalAlignment: "center", verticalAlignment: "center" };
test.getRange("A1:G7").format.wrapText = true;
test.getRange("A1:G7").format.borders = { preset: "all", style: "thin", color: "#B7B7B7" };
test.getRange("A:A").format.columnWidth = 10;
test.getRange("B:B").format.columnWidth = 24;
test.getRange("C:C").format.columnWidth = 38;
test.getRange("D:D").format.columnWidth = 36;
test.getRange("E:E").format.columnWidth = 34;
test.getRange("F:F").format.columnWidth = 14;
test.getRange("G:G").format.columnWidth = 28;
test.freezePanes.freezeRows(1);

stc.showGridLines = false;
stc.getRange("A1:F1").values = [["子系统", "功能项", "状态", "代码/资料依据", "验证状态", "详细说明"]];
const stcRows = rows.filter((row) => row[0].startsWith("STC8H"));
stc.getRange(`A2:F${stcRows.length + 1}`).values = stcRows;
stc.getRange("A1:F1").format = { font: { bold: true, color: "#000000" }, horizontalAlignment: "center", verticalAlignment: "center" };
stc.getRange(`A1:F${stcRows.length + 1}`).format.wrapText = true;
stc.getRange(`A1:F${stcRows.length + 1}`).format.borders = { preset: "all", style: "thin", color: "#B7B7B7" };
stc.getRange("A:A").format.columnWidth = 16;
stc.getRange("B:B").format.columnWidth = 34;
stc.getRange("C:C").format.columnWidth = 18;
stc.getRange("D:D").format.columnWidth = 30;
stc.getRange("E:E").format.columnWidth = 18;
stc.getRange("F:F").format.columnWidth = 58;
stc.freezePanes.freezeRows(1);

const preview = await workbook.render({ sheetName: "概览", autoCrop: "all", scale: 1, format: "png" });
await fs.writeFile(`${outputDir}/preview.png`, new Uint8Array(await preview.arrayBuffer()));
const detailPreview = await workbook.render({ sheetName: "功能清单", autoCrop: "all", scale: 1, format: "png" });
await fs.writeFile(`${outputDir}/detail_preview.png`, new Uint8Array(await detailPreview.arrayBuffer()));
const testPreview = await workbook.render({ sheetName: "硬件验证", autoCrop: "all", scale: 1, format: "png" });
await fs.writeFile(`${outputDir}/test_preview.png`, new Uint8Array(await testPreview.arrayBuffer()));
const stcPreview = await workbook.render({ sheetName: "STC8H清单", autoCrop: "all", scale: 1, format: "png" });
await fs.writeFile(`${outputDir}/stc_preview.png`, new Uint8Array(await stcPreview.arrayBuffer()));
const xlsx = await SpreadsheetFile.exportXlsx(workbook);
await xlsx.save(`${outputDir}/VoltPilot_ESP32_STC8H_功能实现清单_纯白版.xlsx`);
console.log(`${outputDir}/VoltPilot_ESP32_STC8H_功能实现清单_纯白版.xlsx`);
console.log((await workbook.inspect({ kind: "table", range: "概览!A1:F7", include: "values,formulas", tableMaxRows: 10, tableMaxCols: 8 })).ndjson);
