// Small authoring probe: editable Chinese text + supplied image + native chart.
const fs = require('node:fs');
const path = require('node:path');
const PptxGenJS = require('pptxgenjs');

async function main() {
  const [image, output] = process.argv.slice(2);
  if (!image || !output) throw new Error('Usage: node generate.cjs IMAGE OUTPUT.pptx');
  if (fs.existsSync(output)) throw new Error('Output already exists; choose a new filename');
  const pptx = new PptxGenJS();
  pptx.layout = 'LAYOUT_WIDE';
  pptx.author = 'Codex.cpp';
  pptx.subject = 'PPTX generation experiment';
  pptx.title = 'Codex.cpp 设计能力';
  pptx.lang = 'zh-CN';
  pptx.theme = { headFontFace: 'Arial Unicode MS', bodyFontFace: 'Arial Unicode MS', lang: 'zh-CN' };
  const ink = '202B38', accent = '4368CF', muted = '647184';
  const text = (slide, value, x, y, w, h, size, extra = {}) => slide.addText(value, {
    x, y, w, h, fontSize: size, fontFace: 'Arial Unicode MS', color: ink,
    margin: 0, breakLine: false, valign: 'top', ...extra,
  });
  let slide = pptx.addSlide();
  slide.background = { color: 'F6F5F1' };
  text(slide, 'Codex.cpp 设计能力', .85, 1.35, 11.6, 1, 44);
  text(slide, '本地生成与浏览器预览', .88, 2.65, 11, .7, 27, { color: accent });
  text(slide, 'HTML 用于交互演示\nPPTX 用于可编辑的演示文稿', .9, 4.2, 10.8, 1.4, 24, { color: muted });
  slide.addNotes('本地生成实验。文字应保持为可编辑的原生文本。');

  slide = pptx.addSlide();
  text(slide, '终端工作界面', .7, .55, 12, .65, 34);
  text(slide, '键盘完成主要操作\n独立窗口预览成果', .75, 2.1, 3.15, 2.7, 23);
  const size = await require('sharp')(image).metadata();
  const scale = Math.min(8.55 / size.width, 5.4 / size.height);
  const w = size.width * scale, h = size.height * scale;
  slide.addImage({ path: image, x: 4.05 + (8.55 - w) / 2, y: 1.55 + (5.4 - h) / 2, w, h });
  slide.addNotes('图片来源：用户提供的 Codex 官方终端截图。图片保持独立对象，未将整页转成图片。');

  slide = pptx.addSlide();
  text(slide, '示例数据', .7, .55, 12, .65, 34);
  text(slide, '数值用于检查图表显示，不代表实际测量', .75, 1.35, 11.8, .55, 18, { color: muted });
  slide.addChart(pptx.ChartType.bar, [{ name: '示例数量', labels: ['阶段甲', '阶段乙', '阶段丙'], values: [12, 18, 26] }], {
    x: .75, y: 2.05, w: 11.8, h: 4.75, barDir: 'col', chartColors: [accent],
    showLegend: false, showValue: true, showTitle: false, showCatName: false,
    catAxisLabelFontFace: 'Arial Unicode MS', catAxisLabelFontSize: 20,
    valAxisLabelFontFace: 'Arial Unicode MS', valAxisLabelFontSize: 16,
    dataLabelFormatCode: '0', dataLabelPosition: 'outEnd', dataLabelColor: ink,
    dataLabelBkgrdColor: 'FFFFFF', dataLabelFontFace: 'Arial Unicode MS', dataLabelFontSize: 20,
    valAxisMinVal: 0, valAxisMaxVal: 30, valAxisMajorUnit: 10,
    showBorder: false,
  });
  slide.addNotes('演示数据：12、18、26。图表和数据工作簿应保留可编辑性。');
  fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
  await pptx.writeFile({ fileName: output, compression: true });
  console.log(path.resolve(output));
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
